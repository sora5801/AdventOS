#!/usr/bin/env python3
"""
Session 146 smoke: resize from any edge / corner.

Boots wmd + wmedit at a known position.  Drags the LEFT edge of
the wmedit window to the right; the window's left side should
move while the right side stays put.  Then drags the BOTTOM edge
down; the window grows taller.

Pixel checks:
  - baseline: title bar present at the original x range
  - after W-drag right: title bar's LEFT edge is further right
    than before, RIGHT edge is unchanged
  - after S-drag down: title bar still at same y (top didn't
    move), but the bottom of the window is lower

The resize math under test: when grabbing the W edge and moving
the mouse right by ~30 px, the kernel/wmd should set the new
window x to (anchor_x + 30) and new w to (anchor_w - 30).
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


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s146.log"), "w")
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
        ser.sendall(b"wmedit /tmp/r 50\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        def shot(name):
            path = os.path.join(ROOT, f"shot_resize_{name}.ppm")
            if os.path.exists(path): os.remove(path)
            qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (not os.path.exists(path)
                                              or os.path.getsize(path) < 100):
                time.sleep(0.1)
            return read_ppm(path)

        # Baseline: wmedit at outer (100, 200, 644, 420).
        # Title bar at y=200..217.  Sample row y=209 — title bar is
        # either cyan (focused frame_color) or dark grey (unfocused).
        # Either way it differs from the wallpaper, so we detect the
        # title edges by "any non-wallpaper pixel."
        w, h, pxA = shot("a")
        def px_at(p, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]
        def title_edges(p):
            # Detect the title bar by finding the contiguous run of
            # pixels at y=209 that are CYAN (focused) OR DARK GREY
            # (unfocused).  Either is the title bar.
            lo, hi = -1, -1
            for x in range(0, w):
                c = px_at(p, x, 209)
                is_cyan = near(c, (0x30, 0xE0, 0xE0), tol=20)
                is_grey = near(c, (0x20, 0x20, 0x20), tol=10)
                if is_cyan or is_grey:
                    if lo == -1: lo = x
                    hi = x
            return lo, hi
        lo_a, hi_a = title_edges(pxA)
        print(f"   baseline title x range: [{lo_a}, {hi_a}]")

        # Click + drag the LEFT edge.  W zone is rx in [0, 6) of the
        # window, and y outside the title bar (ry >= TITLE_H = 18).
        # wmedit at (100, 200), so click at (102, 250) — left edge,
        # below the title.  Drag the mouse right by ~50 px.
        print("[+] press on LEFT edge of wmedit at (102, 250)")
        abs_send(q, qbuf, 102, 250)
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        print("[+] drag right to (152, 250)")
        abs_send(q, qbuf, 152, 250)
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        _, _, pxB = shot("b")
        lo_b, hi_b = title_edges(pxB)
        print(f"   after W-drag title cyan x range: [{lo_b}, {hi_b}]")

        # Sample bottom of window before BOTTOM edge drag.  Window
        # bottom = y + h - 1.  Look for the cyan/blue frame at the
        # bottom row by scanning for last row that has wmedit's
        # content (light-green text or cyan/blue border).
        def find_bottom_row(p):
            # Look for last row containing the wmedit's frame_color
            # cyan in the title bar area (won't work, title is at top)
            # — instead look for the wmd outer frame which paints at
            # window edges.  Try y bottom from h going up: find rows
            # that have many wmedit-body pixels (light-green text
            # background 0x0A0A14 or so).
            for y in range(h - 1, 0, -1):
                # Sample at the middle of the window's x range.
                mid_x = (lo_b + hi_b) // 2 if hi_b > 0 else w // 2
                c = px_at(p, mid_x, y)
                # wmedit body bg is dark (~0x0A0A14), frame border is
                # white.  Look for white frame outline.
                if near(c, (0xFF, 0xFF, 0xFF), tol=10):
                    return y
            return -1
        bot_b = find_bottom_row(pxB)
        print(f"   bottom edge after W-drag: y={bot_b}")

        # Click + drag the BOTTOM edge.  S zone is at the bottom
        # 6 px of the window.  Window bottom-y ≈ bot_b.  Click 2 px
        # above the bottom edge.
        if bot_b > 0:
            click_y = bot_b - 2
            mid_x = (lo_b + hi_b) // 2 if hi_b > 0 else w // 2
            print(f"[+] press on BOTTOM edge at ({mid_x}, {click_y})")
            abs_send(q, qbuf, mid_x, click_y)
            time.sleep(0.5)
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "btn", "data": {"down": True, "button": "left"}}
            ]})
            time.sleep(0.3)
            print(f"[+] drag down to ({mid_x}, {click_y + 40})")
            abs_send(q, qbuf, mid_x, click_y + 40)
            time.sleep(0.5)
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "btn", "data": {"down": False, "button": "left"}}
            ]})
            time.sleep(1.0)

            _, _, pxC = shot("c")
            bot_c = find_bottom_row(pxC)
            print(f"   bottom edge after S-drag: y={bot_c}")
        else:
            bot_c = bot_b

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px_at(pxB, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        # W edge drag moved left edge right.
        checks.append((f"W-edge drag: left edge moved right "
                       f"({lo_a} -> {lo_b}, diff {lo_b - lo_a})",
                       lo_b > lo_a + 20))
        # W edge drag kept right edge approximately the same.
        checks.append((f"W-edge drag: right edge ~unchanged "
                       f"({hi_a} -> {hi_b}, diff {hi_b - hi_a})",
                       abs(hi_b - hi_a) < 10))
        # S edge drag moved bottom down.
        checks.append((f"S-edge drag: bottom moved down "
                       f"({bot_b} -> {bot_c}, diff {bot_c - bot_b})",
                       bot_c > bot_b + 20))
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
