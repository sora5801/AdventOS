#!/usr/bin/env python3
"""
Session 153 smoke: wmedit Ctrl-Z undo.

Boots wmd + wmedit.  Click into the body, type "hello", screendump
(baseline with text).  Press Ctrl-Z (undo coalesced typing burst),
screendump again.  Verify the second shot has the same window
chrome (title, status bar) but NO green text glyphs in the row
where "hello" was — the entire 5-char run was undone in one step
thanks to coalescing.

Pixel checks:
  - baseline: text is rendered (green glyph pixels in the body)
  - after Ctrl-Z: no green text in the body (line is empty)
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502


def qmp_recv(s, buf):
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk: return None, buf
        buf += chunk
    line, _, rest = buf.partition(b"\n")
    return json.loads(line.decode()), rest


def qmp_cmd(s, buf, cmd, args=None):
    msg = {"execute": cmd}
    if args: msg["arguments"] = args
    s.sendall((json.dumps(msg) + "\r\n").encode())
    while True:
        rep, buf = qmp_recv(s, buf)
        if rep is None: return None, buf
        if "return" in rep or "error" in rep: return rep, buf


def wait_for(sock, marker, buf, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk: return False, buf
        buf += chunk
        if marker in buf: return True, buf
    return False, buf


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=15):
    return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s153.log"), "w")
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
        "-device", "usb-tablet,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)

    time.sleep(1.0)
    try:
        ser = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=5)
        ser_buf = b""
        ok, ser_buf = wait_for(ser, b"$ ", ser_buf, timeout=30)
        if not ok: return 1
        ser.sendall(b"wmd 60 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/undo 50\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Click in wmedit body for focus. wmedit outer (100, 200,
        # 644, 420); surface at (101, 218).  Body interior at
        # screen (101+6, 218+22) = (107, 240).  Click somewhere
        # benign in the body.
        print("[+] click wmedit body for focus")
        abs_send(q, qbuf, 300, 280)
        time.sleep(0.8)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "hello".
        print("[+] type 'hello'")
        for k in ["h", "e", "l", "l", "o"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.15)
        time.sleep(1.0)

        # Baseline screendump (text present).
        SHOT_A = os.path.join(ROOT, "shot_undo_a.ppm")
        if os.path.exists(SHOT_A): os.remove(SHOT_A)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_A, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_A)
                                          or os.path.getsize(SHOT_A) < 100):
            time.sleep(0.1)
        w, h, pxA = read_ppm(SHOT_A)
        def px_at(p, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        def count_green(p):
            # Body region rows ~240..400.  Text colour 0xC0E0C0.
            n = 0
            for yy in range(240, 410):
                for xx in range(107, 400):
                    if near(px_at(p, xx, yy), (0xC0, 0xE0, 0xC0), tol=25):
                        n += 1
            return n

        green_a = count_green(pxA)
        print(f"   baseline (hello typed): {green_a} green px")

        # Send Ctrl-Z.
        print("[+] Ctrl-Z (undo)")
        qmp_cmd(q, qbuf, "send-key",
                {"keys": [{"type": "qcode", "data": "ctrl"},
                          {"type": "qcode", "data": "z"}]})
        time.sleep(1.5)

        SHOT_B = os.path.join(ROOT, "shot_undo_b.ppm")
        if os.path.exists(SHOT_B): os.remove(SHOT_B)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_B, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_B)
                                          or os.path.getsize(SHOT_B) < 100):
            time.sleep(0.1)
        w2, h2, pxB = read_ppm(SHOT_B)
        green_b = count_green(pxB)
        print(f"   after Ctrl-Z: {green_b} green px")

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px_at(pxB, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"baseline has typed text ({green_a} green px)",
                       green_a > 50))
        checks.append((f"Ctrl-Z removed text ({green_b} green px remaining)",
                       green_b < green_a / 4))
        checks.append((f"wmd status bar alive ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))

        print("\n=== checks ===")
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
