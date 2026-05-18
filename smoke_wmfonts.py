#!/usr/bin/env python3
"""
Session 120 smoke test: confirm gfx_text_n at scale=2 actually
draws a 16-pixel-tall character (not just 8).

Boots wmd + wmclock; the clock uses gfx_text_n(scale=2) to render
HH:MM:SS at 2x size.  Verify that the green digits occupy roughly
16 vertical pixels at the same X position, which is impossible to
produce with the old 8x8 single-scale renderer.

Checks:
  - wmclock has a green pixel block extending across BOTH the
    top half (y in [32, 39]) AND the bottom half (y in [40, 47])
    of the same 2x-tall digit cell.  Old single-scale rendering
    would only paint one of the bands at a time.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4467
SERIAL_PORT = 4468
SHOT_PPM = os.path.join(ROOT, "shot_fonts.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s120.log"), "w")
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
            print("[!] no shell prompt"); return 1
        ser.sendall(b"wmd 30 &\n")
        time.sleep(2.0)
        ser.sendall(b"wmclock 25\n")
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

        def near(a, b, tol=12):
            return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

        # wmclock at slot 4 → outer (340, 360), surface starts at
        # (341, 378).  Digits at y_off=32 inside surface (sy=410)
        # at 2x size (16 px tall) → screen y range 410..425.
        # Top band y=410..417 = source-row 0..3 doubled.
        # Bottom band y=418..425 = source-row 4..7 doubled.
        # For a TRULY 2x glyph, both bands should have green hits.
        checks = []

        top_band = 0
        bot_band = 0
        for yy in range(410, 418):
            for xx in range(361, 600):
                if near(px(xx, yy), (0x30, 0xE0, 0x30), tol=15):
                    top_band += 1
        for yy in range(418, 426):
            for xx in range(361, 600):
                if near(px(xx, yy), (0x30, 0xE0, 0x30), tol=15):
                    bot_band += 1
        print(f"green pixels: top band={top_band}, bot band={bot_band}")
        checks.append((f"2x font: TOP band has green ({top_band})",
                       top_band > 30))
        checks.append((f"2x font: BOTTOM band has green ({bot_band})",
                       bot_band > 30))

        # Each 2x digit pixel should produce a 2x2 block.  Find a
        # green pixel and confirm the pixel diagonally next to it
        # is ALSO green (this is the bottom-right of the 2x2 block).
        any_2x_block = 0
        for yy in range(410, 425, 2):
            for xx in range(361, 600, 2):
                p = px(xx, yy)
                if not near(p, (0x30, 0xE0, 0x30), tol=15): continue
                # neighbors
                neighbors = [
                    px(xx+1, yy), px(xx, yy+1), px(xx+1, yy+1)
                ]
                if all(near(n, (0x30, 0xE0, 0x30), tol=15) for n in neighbors):
                    any_2x_block += 1
                if any_2x_block >= 5:
                    break
            if any_2x_block >= 5: break
        checks.append((f"2x2 green blocks present ({any_2x_block})",
                       any_2x_block >= 5))

        # And wmclock title bar still where expected (sanity).
        clk_title = sum(1 for xx in range(371, 540)
                        if near(px(xx, 380), (0x30, 0x30, 0x30), tol=10))
        checks.append((f"wmclock title bar @ y=380 ({clk_title}/169)",
                       clk_title > 130))

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
