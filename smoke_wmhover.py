#!/usr/bin/env python3
"""
Session 117 smoke test: HOVER_ENTER / HOVER_LEAVE are distinct from
FOCUS / UNFOCUS.

wmhello reacts to HOVER edges by flipping its bottom border between
green (entered) and grey (left).  Click-focus is separate; FOCUS
events are counted but don't change the border under the new
semantics.

Steps:
  1. boot wmd + wmhello
  2. move cursor INTO wmhello content → border should be GREEN
  3. screendump A
  4. move cursor OUT of wmhello (back to desktop bg)   → border GREY
  5. screendump B

Checks:
  A: wmhello bottom border is GREEN at the expected position
  B: wmhello bottom border is now GREY (HOVER_LEAVE fired)
  B: wmhello window is still painted (didn't crash)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4461
SERIAL_PORT = 4462
SHOT_IN  = os.path.join(ROOT, "shot_hover_in.ppm")
SHOT_OUT = os.path.join(ROOT, "shot_hover_out.ppm")


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


def read_ppm(path):
    with open(path, "rb") as f:
        magic = f.readline()
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=4):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def screendump(q, qbuf, path):
    if os.path.exists(path):
        os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s117.log"), "w")
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
        if not ok:
            print("[!] never saw shell prompt")
            return 1
        ser.sendall(b"wmd 60 &\n")
        time.sleep(2.0)
        ser.sendall(b"wmhello 30\n")
        time.sleep(2.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmhello surface at (341, 378)..(560, 517).  Border at
        # screen y = 514..517 (sy + sh - 4 .. sy + sh - 1, where
        # sy=378, sh=140).
        sy = 378
        sx = 341
        border_y = sy + 140 - 2     # middle row of the 4-px border

        # Step 1: move cursor INTO wmhello.  Center (512, 384) →
        # target (~450, 461).  7 events of (-9, +11) → (449, 461).
        print("[+] moving cursor INTO wmhello")
        for i in range(7):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -9}},
                {"type": "rel", "data": {"axis": "y", "value": 11}},
            ]})
            qbuf = b""
            time.sleep(0.06)
        time.sleep(1.0)  # let wmhello process HOVER_ENTER

        # Screendump A.
        if not screendump(q, qbuf, SHOT_IN):
            print("[!] screendump A failed"); return 1
        print(f"    {SHOT_IN}")

        # Step 2: move cursor OUT of wmhello.  Currently at (449,
        # 461).  Move far up-right to land on desktop bg.
        # Delta needed: at least (+200, -200).  10 events of (+25,
        # -25) → (699, 211).  Should clear wmhello and not be on
        # any of the demo windows (Color-bars: y=360..470; Gradient:
        # y=120..290 → (699, 211) is at y=211 just below Gradient's
        # bottom which is 290, so we're outside that one too;
        # Clock: y=80..190 → (699, 211) is below Clock).  Should
        # land on desktop bg.
        print("[+] moving cursor OUT of wmhello")
        for i in range(10):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 25}},
                {"type": "rel", "data": {"axis": "y", "value": -25}},
            ]})
            qbuf = b""
            time.sleep(0.06)
        time.sleep(1.0)

        if not screendump(q, qbuf, SHOT_OUT):
            print("[!] screendump B failed"); return 1
        print(f"    {SHOT_OUT}")

        # Parse both.
        aw, ah, ap = read_ppm(SHOT_IN)
        bw, bh, bp = read_ppm(SHOT_OUT)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # When cursor is INSIDE wmhello, HOVER_ENTER fired and border
        # is green (wmhello uses HOVER for has_focus).
        in_green = sum(1 for xx in range(sx + 20, sx + 200)
                       if near(px(ap, aw, xx, border_y),
                               (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"IN: green hover border @ y={border_y} ({in_green}/180)",
                       in_green > 140))

        # When cursor is OUTSIDE wmhello, HOVER_LEAVE fired and the
        # border should now be GREY (0x606060).
        out_grey = sum(1 for xx in range(sx + 20, sx + 200)
                       if near(px(bp, bw, xx, border_y),
                               (0x60, 0x60, 0x60), tol=12))
        checks.append((f"OUT: grey hover border @ y={border_y} ({out_grey}/180)",
                       out_grey > 140))

        # And the border is NOT green anymore.
        out_green = sum(1 for xx in range(sx + 20, sx + 200)
                        if near(px(bp, bw, xx, border_y),
                                (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"OUT: no green border ({out_green}/180)",
                       out_green < 20))

        # wmhello is still painted (blue title band visible).
        out_blue = sum(1 for xx in range(sx + 30, sx + 200)
                       if near(px(bp, bw, xx, sy + 8),
                               (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"OUT: wmhello blue band still painted ({out_blue}/170)",
                       out_blue > 120))

        # wmd still running.
        sb = sum(1 for xx in range(50, bw - 50)
                 if near(px(bp, bw, xx, 6),
                         (0x20, 0x20, 0x20), tol=10))
        checks.append((f"OUT: wmd status bar alive ({sb}/{bw-100})",
                       sb > (bw - 100) * 0.70))

        print("\n=== pixel checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed:
                ok_all = False
        return 0 if ok_all else 2
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=3)
        except Exception:
            qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
