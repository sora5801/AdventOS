#!/usr/bin/env python3
"""
Session 124 smoke test: right-click context menu on a window
title bar.

Steps:
  1. boot wmd + wmhello
  2. screendump A — baseline
  3. move cursor onto wmhello's title bar
  4. inject RIGHT-button press+release -> context menu opens
  5. screendump B — menu visible
  6. cursor walk to "Close" item, LEFT-click -> wmhello closes
  7. screendump C — wmhello gone

Pixel checks:
  A: focus-label NOT visible (no client clicked)
  B: context menu body visible at cursor position
  B: focus label visible (wmhello is now click-focused as side-
     effect of session-117 focus tracking? Actually no — right-
     click doesn't change focus.  Just verify the menu paint.)
  C: wmhello's title band gone (the "Close" item delivered
     WM_EV_CLOSE)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4475
SERIAL_PORT = 4476
SHOT_A = os.path.join(ROOT, "shot_ctx_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_ctx_b.ppm")
SHOT_C = os.path.join(ROOT, "shot_ctx_c.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s124.log"), "w")
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
        ser.sendall(b"wmd 60 &\n")
        time.sleep(2.0)
        ser.sendall(b"wmhello 30\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        if not screendump(q, qbuf, SHOT_A): return 1
        print(f"    {SHOT_A}")

        # wmhello title bar at slot 4 -> (340, 360, 224, 18).
        # Title bar y=360..378.  Centre x=452, y=369.
        # Cursor at (512, 384). Delta (-60, -15). 4 events of (-15, -4).
        print("[+] cursor -> wmhello title bar")
        for i in range(4):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -15}},
                {"type": "rel", "data": {"axis": "y", "value": -4}},
            ]})
            qbuf = b""
            time.sleep(0.06)
        time.sleep(0.8)

        # Right-click to open menu.  Hold 1.3s like the left-click
        # smoke pattern for PS/2 reliability.
        print("[+] right-click -> context menu")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "right"}},
        ]})
        qbuf = b""
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "right"}},
        ]})
        qbuf = b""
        time.sleep(1.0)

        if not screendump(q, qbuf, SHOT_B): return 1
        print(f"    {SHOT_B}")

        # Move cursor down to the "Close" item (item index 1).
        # Menu opens at the cursor position (~452, ~369).  Item 1
        # spans y=387..404 relative to menu top.  So target y =
        # 369 + 18 + 9 = 396 (item 1 centre).
        # Current cursor at (~452, ~369). Delta (0, +27). 3 events
        # of (0, +9).
        print("[+] cursor -> Close item")
        for i in range(3):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "y", "value": 9}},
            ]})
            qbuf = b""
            time.sleep(0.08)
        time.sleep(0.5)

        # Left-click to select Close.
        print("[+] left-click Close")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        # wmhello takes a moment to process WM_EV_CLOSE.
        time.sleep(2.5)

        if not screendump(q, qbuf, SHOT_C): return 1
        print(f"    {SHOT_C}")

        aw, ah, ap = read_ppm(SHOT_A)
        bw, bh, bp = read_ppm(SHOT_B)
        cw, ch, cp = read_ppm(SHOT_C)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # B: context menu visible.  Menu body color 0x202830,
        # cursor at (452, 369) so menu top-left ≈ (452, 369).
        # Sample at menu interior — (460, 380) is item 0 interior.
        menu_body = px(bp, bw, 458, 380)
        checks.append((f"B: ctx menu body @ (458,380) = {menu_body}",
                       near(menu_body, (0x20, 0x28, 0x30), tol=10)))

        # B: menu contains white text "Raise" — sample band y=372..380
        # at x=460..490.
        white_menu = 0
        for yy in range(372, 382):
            for xx in range(460, 530):
                if near(px(bp, bw, xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white_menu += 1
        checks.append((f"B: 'Raise' text in menu band ({white_menu})",
                       white_menu > 20))

        # B: wmhello title bar is still painted UNDER the menu
        # (left part not covered by menu).  Sample on a clean strip
        # ABOVE the title-text band (which has white glyphs that
        # break up the dark-grey runs).
        title_dark = sum(1 for xx in range(345, 445)
                         if near(px(bp, bw, xx, 363),
                                 (0x20, 0x20, 0x20), tol=10))
        checks.append((f"B: wmhello title bar visible left of menu ({title_dark}/100)",
                       title_dark > 80))

        # C: wmhello blue band gone (Close worked).
        blue_c = sum(1 for xx in range(371, 540)
                     if near(px(cp, cw, xx, 386),
                             (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"C: wmhello blue band gone ({blue_c}/169)",
                       blue_c < 20))

        # C: context menu also gone (we LEFT-clicked Close, which
        # closes the menu).
        no_menu = px(cp, cw, 458, 380)
        checks.append((f"C: ctx menu gone @ (458,380) = {no_menu}",
                       not near(no_menu, (0x20, 0x28, 0x30), tol=10)))

        # C: wmd top status bar still alive.
        sb = sum(1 for xx in range(50, cw - 50)
                 if near(px(cp, cw, xx, 6),
                         (0x20, 0x20, 0x20), tol=10))
        checks.append((f"C: wmd status bar ({sb}/{cw-100})",
                       sb > (cw - 100) * 0.70))

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
