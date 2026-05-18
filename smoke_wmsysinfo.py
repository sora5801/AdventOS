#!/usr/bin/env python3
"""
Session 129 smoke test: wmsysinfo dashboard.

Boots wmd + wmsysinfo with --clean, screendumps once, verifies
the dashboard rendered the expected key-value rows (recognisable
labels in light grey, values in white).
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4481
SERIAL_PORT = 4482
SHOT_PPM = os.path.join(ROOT, "shot_sysinfo.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s129.log"), "w")
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
        ser.sendall(b"wmsysinfo 25\n")
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

        # wmsysinfo at slot 0 (--clean) → outer (100, 200, 404, 280).
        # Surface inside at (sx, sy) = (101, 218), size 400x260.
        sx, sy = 101, 218
        checks = []

        # 1. Header band (unfocused — never clicked).  Sample
        # ABOVE the title text band so the grey strip is pure.
        # Header text starts at surface y=5 (gfx_text default top
        # row), so y=sy+2 is unbroken bg.
        hdr = sum(1 for xx in range(sx + 30, sx + 360)
                  if near(px(xx, sy + 2), (0x40, 0x40, 0x40), tol=8))
        checks.append((f"header band @ y={sy+2} ({hdr}/330)",
                       hdr > 300))

        # 2. Body bg sample.  y=sy+150 well inside the table, sample
        # at x=sx+200 which should land on bg (between labels and
        # values, white text at x>=130 in surface = sx+130..sx+300).
        # Actually sample to the FAR right where no text is.
        bg = px(sx + 380, sy + 150)
        checks.append((f"body bg @ ({sx+380},{sy+150}) = {bg}",
                       near(bg, (0x20, 0x20, 0x20), tol=8)))

        # 3. Label text rendered — light grey (0xC0C0C0) in the
        # label column at x=sx+12..sx+120.  Sample band y=sy+24..40
        # which contains the "pid:" / "current cpu:" labels.
        label_grey = 0
        for yy in range(sy + 24, sy + 50):
            for xx in range(sx + 12, sx + 120):
                if near(px(xx, yy), (0xC0, 0xC0, 0xC0), tol=15):
                    label_grey += 1
        checks.append((f"label text grey pixels ({label_grey})",
                       label_grey > 30))

        # 4. Value text rendered — white at x>=sx+130.  Sample
        # same y-band x=sx+130..sx+250.  Values are numbers.
        value_white = 0
        for yy in range(sy + 24, sy + 50):
            for xx in range(sx + 130, sx + 250):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    value_white += 1
        checks.append((f"value text white pixels ({value_white})",
                       value_white > 15))

        # 5. Footer hint text — light grey at the bottom.
        footer_grey = 0
        for yy in range(sy + 248, sy + 256):
            for xx in range(sx + 8, sx + 350):
                if near(px(xx, yy), (0x80, 0x80, 0x80), tol=20):
                    footer_grey += 1
        checks.append((f"footer hint pixels ({footer_grey})",
                       footer_grey > 30))

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
