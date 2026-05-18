#!/usr/bin/env python3
"""
Session 116 smoke test: WM-managed close buttons + WM_EV_CLOSE
delivery.

Boots wmd + wmhello, takes a "before" screendump confirming wmhello
is present (red close box visible in its title bar), then walks
the cursor to the close box and clicks.  Takes an "after"
screendump; the wmhello window should be gone.

Pixel checks:
  before:
    [OK] wmhello red close box visible at expected coords
    [OK] wmhello body painted (blue title band etc.)
  after:
    [OK] no red close box in that location
    [OK] no blue title band where wmhello was — desktop bg restored
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4459
SERIAL_PORT = 4460
SHOT_BEFORE = os.path.join(ROOT, "shot_close_before.ppm")
SHOT_AFTER  = os.path.join(ROOT, "shot_close_after.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s116.log"), "w")
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
        print("[+] shell up; launching wmd 60 &")
        ser.sendall(b"wmd 60 &\n")
        time.sleep(2.0)
        print("[+] launching wmhello 30")
        ser.sendall(b"wmhello 30\n")
        time.sleep(2.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmhello at slot 4 = outer (340, 360), w=224 h=160.
        # Close box at (w->x + w->w - 16, w->y + 2) = (340+208, 362)
        # = (548, 362), 14x14.  Center at (555, 369).
        close_x, close_y = 555, 369

        # Take "before" screendump.
        print("[+] before screendump")
        for path in [SHOT_BEFORE, SHOT_AFTER]:
            if os.path.exists(path):
                os.remove(path)
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT_BEFORE, "format": "ppm"})
        qbuf = b""
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_BEFORE)
                                          or os.path.getsize(SHOT_BEFORE) < 100):
            time.sleep(0.1)

        # Move cursor onto close box.  Center (555, 369) → delta
        # (+43, -15).  3 events of (+15, -5) → (557, 369).
        print("[+] moving cursor onto close box")
        for i in range(3):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 15}},
                {"type": "rel", "data": {"axis": "y", "value": -5}},
            ]})
            qbuf = b""
            time.sleep(0.1)
        time.sleep(0.6)

        # Click — 1.3s hold to survive QEMU PS/2 coalescing.
        print("[+] click close box (1.3s hold)")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        # Give wmhello time to process WM_EV_CLOSE, exit, and wmd
        # time to pop the destroy and update the screen.
        time.sleep(2.0)

        # Take "after" screendump.
        print("[+] after screendump")
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT_AFTER, "format": "ppm"})
        qbuf = b""
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_AFTER)
                                          or os.path.getsize(SHOT_AFTER) < 100):
            time.sleep(0.1)

        if not (os.path.exists(SHOT_BEFORE) and os.path.exists(SHOT_AFTER)):
            print("[!] missing screendump file")
            return 1

        # Parse both.
        bw, bh, bp = read_ppm(SHOT_BEFORE)
        aw, ah, ap = read_ppm(SHOT_AFTER)

        def px(pixels, w, x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        checks = []

        # Before: close box red.  Sample at the BOTTOM ROW of the
        # 14x14 box (y = close_y + 5 = 374) where the white X glyph
        # doesn't overlap — `gfx_text` placed 'x' at (bx+3, by+3) =
        # (551, 365)..(559, 373), so y >= 374 is pure red fill.
        red_y = close_y + 5
        red_before = sum(1 for d in range(-5, 6)
                         if near(px(bp, bw, close_x + d, red_y),
                                 (0xE0, 0x30, 0x30), tol=15))
        checks.append((f"BEFORE: close box red @ y={red_y} ({red_before}/11)",
                       red_before >= 8))

        # Before: wmhello blue band visible at (sx, sy+8) = (446, 386).
        # wmhello surface at (sx, sy) = (341, 378), top band y=0..17.
        blue_before = sum(1 for xx in range(371, 540)
                          if near(px(bp, bw, xx, 386),
                                  (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"BEFORE: wmhello blue band ({blue_before}/169)",
                       blue_before > 100))

        # After: close box position should NOT be red anymore.
        red_after = sum(1 for d in range(-5, 6)
                        if near(px(ap, aw, close_x + d, red_y),
                                (0xE0, 0x30, 0x30), tol=15))
        checks.append((f"AFTER: no red box @ y={red_y} ({red_after}/11)",
                       red_after <= 1))

        # After: wmhello blue band gone.
        blue_after = sum(1 for xx in range(371, 540)
                         if near(px(ap, aw, xx, 386),
                                 (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"AFTER: no wmhello blue band ({blue_after}/169)",
                       blue_after < 20))

        # After: desktop bg visible at close-box location (the
        # desktop is 0x0A1828, but the demo windows could be
        # over it; the close box at (555, 369) is in the area
        # that *was* wmhello's title bar.  After close, the
        # underlying pixels should be desktop bg if wmhello was
        # bottom-of-z, but here wmhello was top so behind it could
        # be Color-bars (which extends to x=540..860, y=360..470).
        # So (555, 369) might land on Color-bars title bar or
        # gradient.  Just verify it's NOT red anymore — covered
        # above — and that wmd's status bar still paints (=
        # compositor still alive).
        sb_after = sum(1 for xx in range(50, aw - 50)
                       if near(px(ap, aw, xx, 6),
                               (0x20, 0x20, 0x20), tol=10))
        checks.append((f"AFTER: wmd status bar still alive ({sb_after}/{aw-100})",
                       sb_after > (aw - 100) * 0.70))

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
