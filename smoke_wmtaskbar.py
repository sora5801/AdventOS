#!/usr/bin/env python3
"""
Session 118 smoke test: taskbar at the bottom with click-to-focus.

Boot wmd + wmhello, then:
  - confirm the taskbar strip exists at the bottom of the FB
  - confirm wmhello's button is visible inside the taskbar
  - click the button → wmhello becomes focused → button highlights
    in the window's frame_color (CYAN for client windows)

Pixel checks (5):
  - taskbar strip present (dark blue 0x182030 at bottom)
  - wmhello button visible (focused-button-bg sample for BEFORE
    click should be the unfocused dark slate 0x303848)
  - wmhello title text on the button (white pixels)
  - AFTER click: button highlights cyan
  - wmhello window still present (compositor alive)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4463
SERIAL_PORT = 4464
SHOT_BEFORE = os.path.join(ROOT, "shot_taskbar_before.ppm")
SHOT_AFTER  = os.path.join(ROOT, "shot_taskbar_after.ppm")


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


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=4):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def screendump(q, qbuf, path):
    if os.path.exists(path):
        os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s118.log"), "w")
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
            print("[!] never saw shell prompt")
            return 1
        ser.sendall(b"wmd 60 &\n")
        time.sleep(2.5)
        ser.sendall(b"wmhello 30\n")
        time.sleep(3.5)             # give wmhello time to register + paint

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # FB is 1024x768.  Taskbar at y = 768 - 28 .. 768 → 740..767.
        # First client button: x = 64+4 = 68, width 140, y=744..763.
        # Sample point: x = 138 (right side of button, past the
        # 7-char title "wmhello" + a few chars of spacing — well
        # clear of any rendered glyph), y = 753 (vertical centre of
        # button).
        button_cx, button_cy = 138, 753

        # Before-click screendump: button should be DARK SLATE.
        print("[+] before-click screendump")
        if not screendump(q, qbuf, SHOT_BEFORE):
            print("[!] screendump A failed"); return 1
        print(f"    {SHOT_BEFORE}")

        # Move cursor onto the taskbar button.  Start at (512, 384).
        # Target ≈ button center (138, 753).  Delta (-374, +369).
        # 21 events of (-18, +18) → (-378, +378) → (134, 762).
        print("[+] moving cursor to taskbar button")
        for i in range(21):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -18}},
                {"type": "rel", "data": {"axis": "y", "value": 18}},
            ]})
            qbuf = b""
            time.sleep(0.05)
        time.sleep(0.6)

        # Click.
        print("[+] click taskbar button (1.3s hold)")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.0)

        # After-click screendump.
        print("[+] after-click screendump")
        if not screendump(q, qbuf, SHOT_AFTER):
            print("[!] screendump B failed"); return 1
        print(f"    {SHOT_AFTER}")

        aw, ah, ap = read_ppm(SHOT_BEFORE)
        bw, bh, bp = read_ppm(SHOT_AFTER)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # 1. Taskbar strip background visible.  Sample at y=755, x in
        #    the gap between buttons (x = 4 + 140 + 4 ... = 148..151
        #    is padding between button 0 and would-be button 1).
        #    With only one client, the gap is wide — sample x=200.
        tb_bg = sum(1 for xx in range(200, 900)
                    if near(px(ap, aw, xx, 755), (0x18, 0x20, 0x30), tol=10))
        checks.append((f"BEFORE: taskbar strip bg @ y=755 ({tb_bg}/700)",
                       tb_bg > 600))

        # 2. Button bg = dark slate before click (no focus).
        bb = px(ap, aw, button_cx, button_cy)
        checks.append((f"BEFORE: button bg @ ({button_cx},{button_cy}) = {bb}",
                       near(bb, (0x30, 0x38, 0x48), tol=10)))

        # 3. Button has white pixels (title text "wmhello").  Sample
        #    a band y=748..756 inside the button text region.
        white_btn = 0
        for yy in range(748, 757):
            for xx in range(10, 80):
                if near(px(ap, aw, xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white_btn += 1
        checks.append((f"BEFORE: title text on button ({white_btn})",
                       white_btn > 15))

        # 4. AFTER click: button bg becomes CYAN = wmhello.frame_color
        #    (GFX_CYAN = 0x30E0E0).
        ab = px(bp, bw, button_cx, button_cy)
        checks.append((f"AFTER: button bg @ ({button_cx},{button_cy}) = {ab}",
                       near(ab, (0x30, 0xE0, 0xE0), tol=12)))

        # 5. wmhello window still painted (blue band visible at y=386).
        blue = sum(1 for xx in range(371, 540)
                   if near(px(bp, bw, xx, 386),
                           (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"AFTER: wmhello blue band still painted ({blue}/169)",
                       blue > 120))

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
