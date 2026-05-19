#!/usr/bin/env python3
"""
Session 148 smoke: move window to workspace via right-click menu.

Boots wmd + wmedit on workspace 0.  Right-clicks wmedit's title
bar to open the context menu, clicks "Move to WS 3" (item index 4
in the menu).  wmedit should disappear from workspace 0.  Then
clicks the WS3 button in the top bar to switch over; wmedit should
reappear there.

Pixel checks:
  - baseline: wmedit visible on WS0
  - after right-click on title: context menu open (slate panel near
    title bar)
  - after clicking "Move to WS 3": wmedit gone from WS0 (no title
    bar at y=209)
  - after switching to WS3: wmedit visible there
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


def click(q, qbuf, x, y, btn="left"):
    abs_send(q, qbuf, x, y)
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": btn}}
    ]})
    time.sleep(0.3)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": btn}}
    ]})
    time.sleep(1.2)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s148.log"), "w")
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
        ser.sendall(b"wmedit /tmp/mv 50\n")
        time.sleep(5.0)   # give wmedit time to fully paint

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        def shot(name):
            path = os.path.join(ROOT, f"shot_mv_{name}.ppm")
            if os.path.exists(path): os.remove(path)
            qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (not os.path.exists(path)
                                              or os.path.getsize(path) < 100):
                time.sleep(0.1)
            return read_ppm(path)

        def px_at(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        def wmedit_visible(p, w):
            """Count title-bar pixels (cyan or grey) at y=209."""
            cyan = sum(1 for xx in range(150, 700)
                       if near(px_at(p, w, xx, 209),
                               (0x30, 0xE0, 0xE0), tol=20))
            grey = sum(1 for xx in range(150, 700)
                       if near(px_at(p, w, xx, 209),
                               (0x20, 0x20, 0x20), tol=10))
            return cyan + grey

        # Baseline: wmedit on WS0, visible.
        w, h, pxA = shot("a")
        vis_a = wmedit_visible(pxA, w)
        print(f"   baseline (WS0): wmedit-visible = {vis_a}")

        # Right-click on wmedit's title bar.  Title bar is at
        # y=200..217.  Click at (200, 208) — well inside the title,
        # avoiding the close/max/min buttons on the right.
        print("[+] right-click wmedit title at (200, 208)")
        click(q, qbuf, 200, 208, btn="right")

        w2, h2, pxB = shot("b")
        # Context menu opens at click position.  Body fill is 0x202830
        # (slate).  After the right-click at (200, 208), menu top-left
        # is around (200, 208).  6 items * 18 = 108 px tall.
        # Sample a known interior pixel.
        menu_present = 0
        for yy in range(220, 290):
            for xx in range(210, 320):
                if near(px_at(pxB, w2, xx, yy),
                        (0x20, 0x28, 0x30), tol=10):
                    menu_present += 1
        print(f"   menu present after right-click: {menu_present}")

        # Click "Move to WS 3" — item index 4 (0=Raise, 1=Close,
        # 2=WS1, 3=WS2, 4=WS3, 5=WS4).  Menu at (200, 208) → item 4
        # at y = 208 + 4 * 18 = 280.  Centre x = 200 + CTXMENU_W/2 =
        # 200 + 64 = 264.
        print("[+] click 'Move to WS 3' menu item at (264, 282)")
        click(q, qbuf, 264, 282)

        w3, h3, pxC = shot("c")
        vis_c = wmedit_visible(pxC, w3)
        print(f"   after move (still on WS0): wmedit-visible = {vis_c}")

        # Switch to WS3 by clicking the WS3 button.  Buttons at
        # x = 44 + ws * 24, y = 2.  WS3 button (index 2) at
        # x = 44 + 2*24 = 92.  Centre (103, 8).
        print("[+] click WS3 button at (103, 8)")
        click(q, qbuf, 103, 8)

        w4, h4, pxD = shot("d")
        vis_d = wmedit_visible(pxD, w4)
        print(f"   after switch to WS3: wmedit-visible = {vis_d}")

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px_at(pxD, w, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"baseline wmedit on WS0 ({vis_a} px)",
                       vis_a > 100))
        checks.append((f"context menu opens on right-click ({menu_present} slate px)",
                       menu_present > 1000))
        checks.append((f"wmedit hidden after move (WS0 view: {vis_c})",
                       vis_c < 50))
        checks.append((f"wmedit visible after WS3 switch ({vis_d} px)",
                       vis_d > 100))
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
