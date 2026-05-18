#!/usr/bin/env python3
"""
Session 122 smoke test: per-task VA bump → multi-window-per-client.

Boots wmd + wmpair (one process, two windows).  Verifies both
windows are visible and have their distinctive title-band colors:

  pair-A: blue title band 0x4080E0 at slot-4 position (340, 360)
  pair-B: magenta title band 0xE030E0 at slot-5 position (400, 400)

If the kernel did NOT bump next_wm_va, the second wm_open would
overlap the first (both at 0x60000000) and the test would fail
either at wm_open (corrupted page tables) or at composition (the
second window's pixels overwrite the first's).
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4471
SERIAL_PORT = 4472
SHOT_PPM = os.path.join(ROOT, "shot_pair.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s122.log"), "w")
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
        ser.sendall(b"wmd 30 &\n")
        time.sleep(2.0)
        ser.sendall(b"wmpair 20\n")
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
        def near(a, b, tol=12):
            return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))

        # Window 1 (pair-A) slot 4 → outer (340, 360, 204, 120).
        #   Surface at (341, 378)..(540, 477).
        #   wmpair paints fill_rect(4,4,W1_W-8,12) — band y=4..15
        #   inside surface = screen y=382..393. Color = 0x4080E0.
        # Window 2 (pair-B) slot 5 → outer (400, 400, 204, 120).
        #   Surface at (401, 418)..(600, 517).
        #   Band y=4..15 inside = screen y=422..433. Color = 0xE030E0.
        sx1, sy1 = 341, 378
        sx2, sy2 = 401, 418
        checks = []

        # pair-A blue band.
        blue_a = sum(1 for xx in range(sx1 + 20, sx1 + 180)
                     if near(px(xx, sy1 + 8),
                             (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"pair-A blue band @ y={sy1+8} ({blue_a}/160)",
                       blue_a > 100))

        # pair-A teal background.  pair-B (slot 5) is at (400, 400)
        # so it overlaps pair-A's right half from x>=400.  Sample
        # on the LEFT side of pair-A's content area where pair-B
        # can't be drawn.  (sx1+30, sy1+30) = (371, 408).
        a_bg = px(sx1 + 30, sy1 + 30)
        checks.append((f"pair-A teal bg @ ({sx1+30},{sy1+30}) = {a_bg}",
                       near(a_bg, (0x10, 0x30, 0x40), tol=10)))

        # pair-B magenta band.
        mag_b = sum(1 for xx in range(sx2 + 20, sx2 + 180)
                    if near(px(xx, sy2 + 8),
                            (0xE0, 0x30, 0xE0), tol=12))
        checks.append((f"pair-B magenta band @ y={sy2+8} ({mag_b}/160)",
                       mag_b > 100))

        # pair-B magenta-ish bg in the right of surface (well past
        # any text).
        b_bg = px(sx2 + 180, sy2 + 30)
        checks.append((f"pair-B magenta-bg @ ({sx2+180},{sy2+30}) = {b_bg}",
                       near(b_bg, (0x30, 0x10, 0x30), tol=10)))

        # Both windows have their own button in the taskbar.
        # First client button at x=68..207, second at x=212..351.
        # Sample centers at (138, 753) and (282, 753).
        # Both unfocused (no clicks) so dark slate 0x303848.
        btn_a = px(138, 753)
        btn_b = px(282, 753)
        checks.append((f"pair-A taskbar button @ (138,753) = {btn_a}",
                       near(btn_a, (0x30, 0x38, 0x48), tol=10)))
        checks.append((f"pair-B taskbar button @ (282,753) = {btn_b}",
                       near(btn_b, (0x30, 0x38, 0x48), tol=10)))

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
