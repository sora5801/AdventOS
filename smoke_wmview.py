#!/usr/bin/env python3
"""
Session 149 smoke: wmview image viewer.

Boots wmd + wmview /sample.ppm.  The sample is a 64x48 four-
colored-quadrant test pattern with a diagonal brightness gradient
(generated procedurally in mkfs.py:gen_sample_ppm).

Pixel checks:
  - wmview title bar painted
  - image rendered: red-ish in TL quadrant, green-ish in TR,
    blue-ish in BL, yellow-ish in BR
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_wmview.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s149.log"), "w")
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
        ser.sendall(b"wmd 40 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmview /sample.ppm 30\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        if os.path.exists(SHOT): os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)
        w, h, pixels = read_ppm(SHOT)
        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        # wmview window outer (100, 200, 404, 320).  Client surface
        # is blitted at (window.x + 1, window.y + TITLE_H) = (101, 218).
        # Image (64x48) is centred inside the surface at (168, 135),
        # so on-screen at (269, 353) to (333, 401).  Quadrants are
        # 32x24 each; midpoints:
        #   TL (red):    (285, 365)
        #   TR (green):  (317, 365)
        #   BL (blue):   (285, 389)
        #   BR (yellow): (317, 389)
        tl = px(285, 365)
        tr = px(317, 365)
        bl = px(285, 389)
        br = px(317, 389)
        print(f"   TL (red)    @ (285,365) = {tl}")
        print(f"   TR (green)  @ (317,365) = {tr}")
        print(f"   BL (blue)   @ (285,389) = {bl}")
        print(f"   BR (yellow) @ (317,389) = {br}")

        # wmview title bar — wmd outer at y=200..217.  At baseline
        # (no clicks), unfocused: dark grey ~0x202020.
        title = px(200, 209)
        print(f"   title bar @ (200,209) = {title}")

        # wmd top status bar alive.
        def near(a, b, tol=15):
            return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        # Quadrant colour tests: dominant channel + significant
        # difference from the other channels.
        def is_red(c):    return c[0] > 100 and c[0] > c[1] + 50 and c[0] > c[2] + 50
        def is_green(c):  return c[1] > 100 and c[1] > c[0] + 50 and c[1] > c[2] + 50
        def is_blue(c):   return c[2] > 100 and c[2] > c[0] + 50 and c[2] > c[1] + 50
        def is_yellow(c): return c[0] > 100 and c[1] > 100 and c[0] > c[2] + 50 and c[1] > c[2] + 50
        checks.append((f"TL quadrant red {tl}", is_red(tl)))
        checks.append((f"TR quadrant green {tr}", is_green(tr)))
        checks.append((f"BL quadrant blue {bl}", is_blue(bl)))
        checks.append((f"BR quadrant yellow {br}", is_yellow(br)))
        checks.append((f"wmview title painted ({title})",
                       near(title, (0x20, 0x20, 0x20), tol=15)
                       or near(title, (0x30, 0xE0, 0xE0), tol=20)))
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
