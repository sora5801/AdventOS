#!/usr/bin/env python3
"""
Session 143 smoke test: toast notifications.

Boots wmd + wmedit, focuses wmedit, types a short string, hits
Ctrl-S to trigger a save (and the new wm_notify toast).  Within
TOAST_LIFE_FRAMES (~3 s), the toast should be visible in the
bottom-right corner — a dark slate box with a blue border and
white "saved /tmp/foo (N B)" text.

Pixel checks:
  - toast box bg present in the bottom-right corner (dark slate
    pixels ~0x202830 in a 240x36 region above the taskbar)
  - toast border (blue ~0x4080E0) present along its rectangle
  - some white text glyphs inside the toast region
  - wmd top status bar still alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_wmtoast.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s143.log"), "w")
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
        ser.sendall(b"wmd 60 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/toast 50\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Click into wmedit to focus.  wmedit outer (100, 200, 644, 420).
        # Click at surface (50, 60) → screen (150, 260).  Use abs.
        click_x, click_y = 150, 260
        # 1024x768 FB.
        ax = 32767 * click_x // 1023
        ay = 32767 * click_y // 767
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ]})
        time.sleep(1.0)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "hi" then Ctrl-S to trigger save + notify.
        print("[+] type 'hi' then Ctrl-S")
        for k in ["h", "i"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.2)
        # Ctrl-S
        qmp_cmd(q, qbuf, "send-key",
                {"keys": [{"type": "qcode", "data": "ctrl"},
                          {"type": "qcode", "data": "s"}]})
        time.sleep(0.8)

        print("[+] screendump")
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

        # Toast region: bottom-right, above the taskbar.
        # TASKBAR_H=28, TOAST_MARGIN=12, TOAST_W=240, TOAST_H=36.
        # tx = fb_w - TOAST_W - TOAST_MARGIN = 1024 - 240 - 12 = 772
        # ty = fb_h - TASKBAR_H - TOAST_MARGIN - TOAST_H = 768 - 28 - 12 - 36 = 692
        TOAST_W, TOAST_H = 240, 36
        TASKBAR_H, TOAST_MARGIN = 28, 12
        tx = w - TOAST_W - TOAST_MARGIN
        ty = h - TASKBAR_H - TOAST_MARGIN - TOAST_H
        print(f"   toast region: ({tx},{ty}) {TOAST_W}x{TOAST_H}")

        # 1. Dark slate bg pixels inside the toast region.
        toast_bg = 0
        for yy in range(ty + 4, ty + TOAST_H - 4):
            for xx in range(tx + 4, tx + TOAST_W - 4):
                if near(px(xx, yy), (0x20, 0x28, 0x30), tol=25):
                    toast_bg += 1
        print(f"   toast bg pixels: {toast_bg}")

        # 2. Blue border (0x4080E0) along the rectangle edges.
        border_blue = 0
        for xx in range(tx, tx + TOAST_W):
            if near(px(xx, ty), (0x40, 0x80, 0xE0), tol=25): border_blue += 1
            if near(px(xx, ty + TOAST_H - 1), (0x40, 0x80, 0xE0), tol=25):
                border_blue += 1
        for yy in range(ty, ty + TOAST_H):
            if near(px(tx, yy), (0x40, 0x80, 0xE0), tol=25): border_blue += 1
            if near(px(tx + TOAST_W - 1, yy), (0x40, 0x80, 0xE0), tol=25):
                border_blue += 1
        print(f"   border-blue pixels: {border_blue}")

        # 3. White text inside the toast region.
        text_white = 0
        for yy in range(ty + 8, ty + TOAST_H - 8):
            for xx in range(tx + 8, tx + TOAST_W - 8):
                if near(px(xx, yy), (0xE0, 0xF0, 0xFF), tol=30):
                    text_white += 1
        print(f"   text white pixels: {text_white}")

        # 4. wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"toast bg present ({toast_bg} dark-slate px)",
                       toast_bg > 200))
        checks.append((f"toast border ({border_blue} blue px)",
                       border_blue > 60))
        checks.append((f"toast text rendered ({text_white} white px)",
                       text_white > 30))
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
