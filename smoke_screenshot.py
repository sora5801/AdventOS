#!/usr/bin/env python3
"""
Session 151 smoke: Alt+P screenshot -> wmview round-trip.

Boots wmd + wmedit, types a short string so the FB has visible
content, presses Alt+P to trigger a screenshot, then opens
wmview /tmp/screen.ppm and verifies the snapshot renders.

Pixel checks:
  - after Alt+P: toast appears in bottom-right (the "screenshot:
    /tmp/screen.ppm (NNN B)" message)
  - wmview /tmp/screen.ppm opens successfully — the saved image
    is the previous frame's full FB, so wmview shows a tiny copy
    of the desktop including wmedit
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502


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


def near(a, b, tol=15):
    return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s151.log"), "w")
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
        ser.sendall(b"wmd 80 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/scr 60 &\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Send Alt+P to trigger screenshot.
        print("[+] Alt+P -> screenshot")
        qmp_cmd(q, qbuf, "send-key",
                {"keys": [{"type": "qcode", "data": "alt"},
                          {"type": "qcode", "data": "p"}]})
        time.sleep(2.0)

        SHOT_A = os.path.join(ROOT, "shot_screenshot_a.ppm")
        if os.path.exists(SHOT_A): os.remove(SHOT_A)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_A, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_A)
                                          or os.path.getsize(SHOT_A) < 100):
            time.sleep(0.1)
        w, h, pxA = read_ppm(SHOT_A)
        def px_at(p, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        # Toast in bottom-right: TOAST_W=240, TOAST_H=36, anchored
        # 12 px from each edge.  tx = 1024 - 240 - 12 = 772.
        # ty = 768 - 28 - 12 - 36 = 692.  Sample interior for dark
        # slate (0x202830).
        toast_bg = 0
        for yy in range(694, 720):
            for xx in range(780, 1000):
                if near(px_at(pxA, xx, yy), (0x20, 0x28, 0x30), tol=25):
                    toast_bg += 1
        print(f"   screenshot toast bg pixels: {toast_bg}")

        # Open wmview /tmp/screen.ppm.  wmedit is still alive on
        # WS0; the new wmview opens as slot 1 (cascade offset
        # +60, +40 -> outer (160, 240, 404, 320)).
        print("[+] wmview /tmp/screen.ppm")
        ser.sendall(b"wmview /tmp/screen.ppm 30 &\n")
        time.sleep(4.0)

        SHOT_B = os.path.join(ROOT, "shot_screenshot_b.ppm")
        if os.path.exists(SHOT_B): os.remove(SHOT_B)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_B, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_B)
                                          or os.path.getsize(SHOT_B) < 100):
            time.sleep(0.1)
        w2, h2, pxB = read_ppm(SHOT_B)

        # wmview window opens at slot 1 (wmedit is slot 0).
        # Outer (160, 240, 404, 320); surface at (161, 258).
        # The screenshot is 1024x768 — much larger than the
        # 400x278 content area, so it gets CLIPPED at the top-left.
        # Sample content pixels in wmview's body: should see content
        # from the screenshot (not pure wallpaper).
        content = 0
        for yy in range(260, 540):
            for xx in range(165, 555):
                c = px_at(pxB, xx, yy)
                if not near(c, (0x09, 0x16, 0x26), tol=10):  # not wallpaper
                    if not near(c, (0x10, 0x10, 0x10), tol=8):  # not wmview bg
                        content += 1
        print(f"   wmview content (non-wallpaper, non-bg): {content}")

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px_at(pxB, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"screenshot toast visible ({toast_bg} slate px)",
                       toast_bg > 200))
        checks.append((f"wmview shows screenshot content ({content} px)",
                       content > 5000))
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
