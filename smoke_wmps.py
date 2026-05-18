#!/usr/bin/env python3
"""
Session 130 smoke test: wmps process viewer.

Boots wmd (--clean) + wmps, screendumps, verifies:
  - wmps window painted with header + column labels
  - at least one process row visible (PID + STATE + NAME)
  - row 0 selection highlight present
  - footer "rows=N" text visible
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4483
SERIAL_PORT = 4484
SHOT_PPM = os.path.join(ROOT, "shot_wmps.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s130.log"), "w")
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
        time.sleep(2.5)
        ser.sendall(b"wmps 25\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        if os.path.exists(SHOT_PPM):
            os.remove(SHOT_PPM)
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT_PPM, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_PPM)
                                          or os.path.getsize(SHOT_PPM) < 100):
            time.sleep(0.1)

        with open(SHOT_PPM, "rb") as f:
            f.readline()
            line = f.readline()
            while line.startswith(b"#"): line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]
        def near(a, b, tol=4):
            return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))

        # wmps at slot 0 (--clean) → outer (100, 200, 424, 340).
        # Surface inside at (sx, sy) = (101, 218), size 420x320.
        sx, sy = 101, 218
        checks = []

        # 1. Header band — grey (unfocused).
        hdr = sum(1 for xx in range(sx + 30, sx + 380)
                  if near(px(xx, sy + 2), (0x40, 0x40, 0x40), tol=8))
        checks.append((f"header band @ y={sy+2} ({hdr}/350)",
                       hdr > 300))

        # 2. Column headers — "PID STATE NAME" in light grey
        # (0xA0A0A0) at y=sy+24..sy+33.
        col_grey = 0
        for yy in range(sy + 24, sy + 34):
            for xx in range(sx + 8, sx + 200):
                if near(px(xx, yy), (0xA0, 0xA0, 0xA0), tol=20):
                    col_grey += 1
        checks.append((f"column header text ({col_grey})",
                       col_grey > 40))

        # 3. Row 0 selection highlight — 0x405880 at y around
        # row_y0 + 0 = HDR_H + 4 + LINE_H + 6 = 20+4+12+6 = 42
        # in surface → screen y = sy+42 = 260.
        # Sample column where no text is — between PID and STATE
        # (after pid digits, before "STATE"). x=40 in surface.
        hl = px(sx + 40, sy + 42)
        print(f"  row 0 highlight @ ({sx+40},{sy+42}) = {hl}")
        checks.append((f"row 0 selection highlight",
                       near(hl, (0x40, 0x58, 0x80), tol=15)))

        # 4. Some row text rendered — white pixels in the row 0
        # band.
        white = 0
        for yy in range(sy + 42, sy + 52):
            for xx in range(sx + 8, sx + 300):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white += 1
        checks.append((f"row 0 text white pixels ({white})",
                       white > 30))

        # 5. Footer "rows=N" — light grey at bottom.
        footer = 0
        for yy in range(sy + 305, sy + 315):
            for xx in range(sx + 8, sx + 100):
                if near(px(xx, yy), (0x80, 0x80, 0x80), tol=20):
                    footer += 1
        checks.append((f"footer rows=N pixels ({footer})",
                       footer > 15))

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
