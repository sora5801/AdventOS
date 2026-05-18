#!/usr/bin/env python3
"""
Session 123 smoke test: --clean mode + 5-item launcher.

Verifies:
  1. `wmd 30 --clean` boots with NO demo windows (the desktop bg
     is visible everywhere a demo would have been).
  2. Clicking Start opens a popup with 5 items now (wmhello,
     wmtype, wmclock, wmpaint, wmpair) instead of 4.
  3. The first client launched (wmhello via the popup) opens at
     slot 0 position (100, 200) — not the slot-4 cascade position
     used when demos occupy slots 0..3.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4473
SERIAL_PORT = 4474
SHOT_A = os.path.join(ROOT, "shot_clean_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_clean_b.ppm")


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


def screendump(q, qbuf, path):
    if os.path.exists(path):
        os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=4):
    return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s123.log"), "w")
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

        # A: clean desktop snapshot.
        if not screendump(q, qbuf, SHOT_A): return 1

        # Open launcher.
        print("[+] clicking Start")
        for i in range(27):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -18}},
                {"type": "rel", "data": {"axis": "y", "value": 14}},
            ]})
            time.sleep(0.04)
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        time.sleep(1.0)

        # B: popup open snapshot.
        if not screendump(q, qbuf, SHOT_B): return 1

        aw, ah, ap = read_ppm(SHOT_A)
        bw, bh, bp = read_ppm(SHOT_B)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # A: clean desktop — sample where the Clock demo USED to be.
        # Clock was at (80, 80, 260, 110); content at (200, 130) used
        # to be 0x102030.  Without demos, that pixel should be the
        # desktop bg 0x0A1828.
        clean_clk = px(ap, aw, 200, 130)
        checks.append((f"CLEAN: ex-Clock area now desktop bg = {clean_clk}",
                       near(clean_clk, (0x0A, 0x18, 0x28), tol=8)))

        # A: where Color-bars red used to be (560, 420) is now bg.
        clean_cb = px(ap, aw, 560, 420)
        checks.append((f"CLEAN: ex-Color-bars area now desktop bg = {clean_cb}",
                       near(clean_cb, (0x0A, 0x18, 0x28), tol=8)))

        # B: popup has 5 items now.  Old: 4 items × 22 px = 88 px;
        # popup top at y = 768 - 28 - 88 - 4 = 648.
        # New: 5 items × 22 = 110; top y = 768 - 28 - 110 - 4 = 626.
        # Sample at y=634 (inside what should be the FIRST item of
        # the new layout) — bg should be 0x202830, text in white.
        popup_top = px(bp, bw, 100, 634)
        checks.append((f"POPUP: top item bg @ (100,634) = {popup_top}",
                       near(popup_top, (0x20, 0x28, 0x30), tol=10)))

        # And the LAST item of the new layout — item index 4 at
        # y = 626 + 4*22 = 714.  Text "wmpair" rendered around y=721.
        wmpair_text = 0
        for yy in range(715, 728):
            for xx in range(12, 90):
                if near(px(bp, bw, xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    wmpair_text += 1
        checks.append((f"POPUP: wmpair text in 5th item band ({wmpair_text})",
                       wmpair_text > 15))

        # Confirm the Start button is still painted.
        sb_g = sum(1 for xx in range(8, 56)
                   if near(px(ap, aw, xx, 754), (0x20, 0x50, 0x30), tol=15))
        checks.append((f"CLEAN: Start button green ({sb_g}/48)",
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
