#!/usr/bin/env python3
"""
Session 127 smoke test: procedural desktop wallpaper.

Boots wmd with --clean (no demo windows to obscure the bg) and
verifies the new wallpaper structure:
  - top of screen (y=24): the lightest band 0 colour
  - middle of screen (y=380): a mid-band colour
  - bottom-of-bg area (y=735): the darkest band 7 colour

Plus a sanity check that the legacy color 0x0A1828 sample still
passes within tolerance (so all existing smoke tests that look
for "desktop bg" with tol≥4 keep working).
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4477
SERIAL_PORT = 4478
SHOT_PPM = os.path.join(ROOT, "shot_wp.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s127.log"), "w")
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
        ser.sendall(b"wmd 30 --clean\n")
        time.sleep(3.0)

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

        # Band layout: fb_h=768, band_h = 768/8 = 96.
        #   band 0: y= 0..95  → t=-4 → (r,g,b) = (0x08, 0x14, 0x24)
        #   band 1: y=96..191 → t=-3 → (0x09, 0x15, 0x25)
        #   band 2: y=192..287 → t=-2 → (0x09, 0x16, 0x26)
        #   band 3: y=288..383 → t=-1 → (0x0a, 0x17, 0x27)
        #   band 4: y=384..479 → t= 0 → (0x0a, 0x18, 0x28)   ← legacy
        #   band 5: y=480..575 → t=+1 → (0x0a, 0x19, 0x29)
        #   band 6: y=576..671 → t=+2 → (0x0b, 0x1a, 0x2a)
        #   band 7: y=672..767 → t=+3 → (0x0b, 0x1b, 0x2b)
        # Sample at the middle of each band — pick x=800 to avoid
        # interference from the "stars" (which are at x=12+k*16).
        checks = []

        b0 = px(800, 48)    # band 0 middle
        b4 = px(800, 430)   # band 4 middle (the legacy 0x0A1828)
        b7 = px(800, 720)   # band 7 middle
        print(f"  band 0 @ (800,48)  = {b0}")
        print(f"  band 4 @ (800,430) = {b4}")
        print(f"  band 7 @ (800,720) = {b7}")

        checks.append((f"band 0 darker than band 4 ({b0} < {b4})",
                       b0[2] < b4[2]))
        checks.append((f"band 4 darker than band 7 ({b4} < {b7})",
                       b4[2] < b7[2]))
        # Band 4 should be very close to the legacy 0x0A1828 so
        # existing smokes that sample bg at tol=4..8 keep passing.
        checks.append((f"band 4 matches legacy 0x0A1828",
                       near(b4, (0x0A, 0x18, 0x28), tol=2)))

        # Look for at least a few "stars" — pixels at the 16-grid
        # locations that are brighter than the base.
        stars = 0
        for y in range(40, 700, 16):
            for x in range(12, 1024, 16):
                p = px(x, y)
                if p[0] >= 0x40 and p[1] >= 0x40 and p[2] > p[0]:
                    stars += 1
        print(f"  stars found: {stars}")
        checks.append((f"some wallpaper stars present ({stars})",
                       stars > 50))

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
