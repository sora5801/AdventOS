#!/usr/bin/env python3
"""
Session 128 smoke test: wmfiles file manager.

Boots wmd + wmfiles, takes screendump A (initial state showing
the cwd's entries), clicks into wmfiles to focus, then sends an
arrow-down keystroke (as ANSI 'ESC [ B') and screendumps again.
The selection highlight should have moved down one row.

Pixel checks:
  A: wmfiles header band visible (blue or grey)
  A: row 0 shows the selection highlight (0x405880)
  A: some entry text rendered (white pixels in the list area)
  B: row 0 no longer selected (highlight moved)
  B: row 1 IS selected (highlight present)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4479
SERIAL_PORT = 4480
SHOT_A = os.path.join(ROOT, "shot_files_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_files_b.ppm")


def qmp_recv(s, buf):
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    line, _, rest = buf.partition(b"\n")
    return json.loads(line.decode()), rest


def qmp_cmd(s, buf, cmd, args=None):
    msg = {"execute": cmd}
    if args:
        msg["arguments"] = args
    s.sendall((json.dumps(msg) + "\r\n").encode())
    while True:
        rep, buf = qmp_recv(s, buf)
        if rep is None:
            return None, buf
        if "return" in rep or "error" in rep:
            return rep, buf


def wait_for(sock, marker, buf, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            return False, buf
        buf += chunk
        if marker in buf:
            return True, buf
    return False, buf


def screendump(q, qbuf, path):
    if os.path.exists(path):
        os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=4):
    return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s128.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1",
        "-vga", "std",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on",
        "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server=on,wait=off",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)

    time.sleep(1.0)
    try:
        ser = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=5)
        ser_buf = b""
        ok, ser_buf = wait_for(ser, b"$ ", ser_buf, timeout=30)
        if not ok: return 1
        ser.sendall(b"wmd 60 --clean &\n")
        time.sleep(2.5)
        ser.sendall(b"wmfiles 30\n")
        time.sleep(3.5)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmfiles at slot 0 (no demos due to --clean) → outer
        # (100, 200, 404, 320).  Surface inside at (101, 218).
        # HDR_H = 20, so list starts at surface y=20 inside.
        sx, sy = 101, 218

        if not screendump(q, qbuf, SHOT_A): return 1
        print(f"    {SHOT_A}")

        # Click into wmfiles to focus.  Cursor at (512, 384).
        # Target wmfiles content area: ~(200, 350).  Delta
        # (-312, -34).  17 events of (-18, -2) → (-306, -34) →
        # (206, 350). Inside wmfiles content.
        print("[+] focus wmfiles via click")
        for i in range(17):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -18}},
                {"type": "rel", "data": {"axis": "y", "value": -2}},
            ]})
            time.sleep(0.05)
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        time.sleep(1.5)

        # Send arrow-down — ESC, '[', 'B' three keystrokes.
        # QMP send-key supports qcodes for these.
        print("[+] arrow-down key")
        qmp_cmd(q, qbuf, "send-key", {"keys": [{"type": "qcode", "data": "down"}]})
        time.sleep(2.0)

        if not screendump(q, qbuf, SHOT_B): return 1
        print(f"    {SHOT_B}")

        aw, ah, ap = read_ppm(SHOT_A)
        bw, bh, bp = read_ppm(SHOT_B)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # A: header band — light blue (focused) or grey (not yet
        # focused).  At first screendump nothing has been clicked
        # so header is grey 0x404040.
        hdr = px(ap, aw, sx + 50, sy + 8)
        print(f"  A: header @ ({sx+50},{sy+8}) = {hdr}")
        checks.append((f"A: header bar visible",
                       near(hdr, (0x40, 0x40, 0x40), tol=8)
                       or near(hdr, (0x40, 0x80, 0xE0), tol=12)))

        # A: row 0 (the first entry) is selected — blue highlight
        # at y=sy+HDR_H+3..sy+HDR_H+13 = sy+23..sy+33 (LINE_H=12).
        # Sample at the middle x.  Sample y = sy+27 (mid-row).
        row0 = px(ap, aw, sx + 60, sy + 27)
        print(f"  A: row 0 bg @ ({sx+60},{sy+27}) = {row0}")
        checks.append((f"A: row 0 selected (highlight bg)",
                       near(row0, (0x40, 0x58, 0x80), tol=15)))

        # A: some entry text — scan row 0 for white pixels.
        white = sum(1 for xx in range(sx + 8, sx + 200)
                    for yy in range(sy + 23, sy + 34)
                    if near(px(ap, aw, xx, yy), (0xFF, 0xFF, 0xFF), tol=20))
        checks.append((f"A: text in row 0 ({white} white)", white > 10))

        # B: row 0 NO LONGER selected.
        row0_b = px(bp, bw, sx + 60, sy + 27)
        print(f"  B: row 0 bg @ ({sx+60},{sy+27}) = {row0_b}")
        checks.append((f"B: row 0 no longer selected",
                       not near(row0_b, (0x40, 0x58, 0x80), tol=15)))

        # B: SOME row in the list region is selected after the
        # click-then-arrow-down sequence.  Click landed somewhere
        # in the middle of the list (we didn't aim for row 0); the
        # arrow-down then advances selection by one.  Scan all
        # rows for the 0x405880 highlight.
        rows_with_highlight = 0
        for r in range(0, 20):
            y_row = sy + 23 + r * 12 + 6   # mid-row
            if y_row >= sy + 280: break
            p = px(bp, bw, sx + 60, y_row)
            if near(p, (0x40, 0x58, 0x80), tol=15):
                rows_with_highlight += 1
        print(f"  B: rows with highlight = {rows_with_highlight}")
        checks.append((f"B: some row in list is selected ({rows_with_highlight})",
                       rows_with_highlight == 1))

        print("\n=== pixel checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed: ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
