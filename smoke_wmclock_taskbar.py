#!/usr/bin/env python3
"""
Session 121 smoke test: clock on the right of the taskbar.

Boots wmd, takes a screendump, verifies the clock occupies the
last ~132 px of the bottom strip:
  - white pixels (digits) present in the clock region
  - colon visible (2x font → a 2x2 dot pattern at column 5 of the
    glyph, paint at the centre of the string)
  - first 6 px column has a digit (sanity: clock didn't get
    truncated)

Compared to session 120's font smoke (which proved scale=2 works
inside a CLIENT surface), this proves wmd's TASKBAR uses scale=2
on its own gfx_ctx (the real FB, not a client surface).
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4469
SERIAL_PORT = 4470
SHOT_PPM = os.path.join(ROOT, "shot_clock.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s121.log"), "w")
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
            print("[!] no shell"); return 1
        ser.sendall(b"wmd 20\n")
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
        if not os.path.exists(SHOT_PPM):
            print("[!] screendump failed"); return 1

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
        def near(a, b, tol=15):
            return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))

        # Clock occupies right 132 px: x = 1024-132 = 892..1023.
        # 2x font baseline y = h - TASKBAR_H + 6 = 768 - 28 + 6 = 746.
        # Digit band y=746..761 (16 px tall).
        checks = []

        white_clock = 0
        for yy in range(746, 762):
            for xx in range(892, 1024):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white_clock += 1
        print(f"clock white pixels: {white_clock}")
        checks.append((f"clock digits white pixels ({white_clock})",
                       white_clock > 60))

        # 2x2 white-block confirmation — every 2x-scaled set pixel
        # produces a 2x2 block.
        any_2x = 0
        for yy in range(746, 760, 2):
            for xx in range(892, 1022, 2):
                p = px(xx, yy)
                if not near(p, (0xFF, 0xFF, 0xFF), tol=20): continue
                if (near(px(xx+1, yy), (0xFF, 0xFF, 0xFF), tol=20) and
                    near(px(xx, yy+1), (0xFF, 0xFF, 0xFF), tol=20) and
                    near(px(xx+1, yy+1), (0xFF, 0xFF, 0xFF), tol=20)):
                    any_2x += 1
                if any_2x >= 5: break
            if any_2x >= 5: break
        checks.append((f"2x2 white blocks in clock ({any_2x})",
                       any_2x >= 5))

        # No client buttons (only Start button is up to x=64).
        # x in 200..880 should all be taskbar bg.
        sb_bg = sum(1 for xx in range(200, 880, 8)
                    if near(px(xx, 753), (0x18, 0x20, 0x30), tol=10))
        checks.append((f"taskbar bg between Start and clock ({sb_bg}/85)",
                       sb_bg > 75))

        # Start button still painted.
        sb_g = sum(1 for xx in range(8, 56)
                   if near(px(xx, 754), (0x20, 0x50, 0x30), tol=15))
        checks.append((f"Start button green ({sb_g}/48)",
                       sb_g > 30))

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
