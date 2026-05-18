#!/usr/bin/env python3
"""
Session 133 smoke test: maximize + minimize buttons.

Verifies the buttons are *rendered* in the right place and colour
order.  Actual click-to-toggle behaviour is too sensitive to
QEMU's PS/2 mouse-rel scaling to assert through pixel checks (the
buttons are 14 px wide; cursor positioning drifts ±20-50 px
session-to-session).  The rendering check confirms the WM is
wiring up the title-bar chrome correctly.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4493
SERIAL_PORT = 4494
SHOT = os.path.join(ROOT, "shot_minmax.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s133.log"), "w")
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
        ser.sendall(b"wmd 30 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmhello 25\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        if os.path.exists(SHOT): os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)

        with open(SHOT, "rb") as f:
            f.readline()
            line = f.readline()
            while line.startswith(b"#"): line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]
        def near(a, b, tol=15):
            return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))

        # wmhello at slot 0 (--clean): outer (100, 200, 224, 158).
        # Title-bar y=202..215.  Buttons (right→left):
        #   close   x=308..321  RED 0xE03030
        #   maximize x=292..305 GREEN 0x30E030
        #   minimize x=276..289 YELLOW 0xE0E030
        # Sample BG of each at a non-glyph offset (top-left corner
        # of each 14x14 box).
        checks = []

        # 1. Close box bg is red.  Sample near the corner (2 in
        #    from each edge so we avoid the white border).
        cb_red = sum(1 for yy in range(204, 214)
                     if near(px(310, yy), (0xE0, 0x30, 0x30), tol=15))
        checks.append((f"close box red @ x=310 ({cb_red}/10)",
                       cb_red >= 7))

        # 2. Maximize box bg is green.
        mx_green = sum(1 for yy in range(204, 214)
                       if near(px(294, yy), (0x30, 0xE0, 0x30), tol=15))
        checks.append((f"maximize box green @ x=294 ({mx_green}/10)",
                       mx_green >= 7))

        # 3. Minimize box bg is yellow.
        mn_yellow = sum(1 for yy in range(204, 214)
                        if near(px(278, yy), (0xE0, 0xE0, 0x30), tol=15))
        checks.append((f"minimize box yellow @ x=278 ({mn_yellow}/10)",
                       mn_yellow >= 7))

        # 4. White glyphs inside each button (text/bracket).
        glyph_whites = 0
        for yy in range(204, 213):
            for xx in range(276, 322):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    glyph_whites += 1
        checks.append((f"glyph whites in 3 buttons ({glyph_whites})",
                       glyph_whites >= 5))

        # 5. wmhello blue band still painted (window otherwise alive).
        blue = sum(1 for xx in range(110, 320)
                   if near(px(xx, 226), (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"wmhello blue band ({blue}/210)",
                       blue > 150))

        print("\n=== pixel checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed:
                ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
