#!/usr/bin/env python3
"""
Session 147 smoke: workspaces / virtual desktops.

Boots wmd + wmedit at outer (100, 200, 644, 420) on workspace 0.
Then clicks the workspace-2 button in the top status bar — wmedit
should disappear because it's on workspace 0 and we're now viewing
workspace 1.  Clicks workspace 1 button to return; wmedit reappears.

Pixel checks:
  - baseline: wmedit title bar visible at y=209
  - workspace-1 button cyan in top bar at startup
    (workspace 0 = first button, index 0)
  - after switching to workspace 1: wmedit title GONE; workspace-2
    button now cyan
  - after switching back to workspace 0: wmedit reappears
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


def click(q, qbuf, x, y):
    abs_send(q, qbuf, x, y)
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}
    ]})
    time.sleep(0.3)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}
    ]})
    time.sleep(1.2)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s147.log"), "w")
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
        ser.sendall(b"wmedit /tmp/ws 50\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        def shot(name):
            path = os.path.join(ROOT, f"shot_ws_{name}.ppm")
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

        # Baseline: workspace 0 selected; wmedit visible.
        # Top-bar buttons at y=2..15, x=44..139 (4 buttons of width 22
        # each + 2 px gap = 24-cell stride).
        # Workspace 0 button: x=44..65, should be CYAN (active)
        # Workspace 1 button: x=68..89, should be slate (inactive)
        w, h, pxA = shot("a")
        ws0_active_a = sum(1 for xx in range(44, 66)
                           if near(px_at(pxA, w, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        ws1_active_a = sum(1 for xx in range(68, 90)
                           if near(px_at(pxA, w, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        # wmedit title bar: cyan at y=209 around x=200..700
        wmedit_a = sum(1 for xx in range(150, 700)
                       if near(px_at(pxA, w, xx, 209),
                               (0x30, 0xE0, 0xE0), tol=20))
        wmedit_grey_a = sum(1 for xx in range(150, 700)
                           if near(px_at(pxA, w, xx, 209),
                                   (0x20, 0x20, 0x20), tol=10))
        print(f"   baseline: ws0-cyan={ws0_active_a}  ws1-cyan={ws1_active_a}  "
              f"wmedit-cyan={wmedit_a}  wmedit-grey={wmedit_grey_a}")

        # Click workspace 2 button (x range 68..89, y=2..15 → centre
        # (78, 8)).
        print("[+] click workspace-2 button at (78, 8)")
        click(q, qbuf, 78, 8)

        w2, h2, pxB = shot("b")
        ws0_active_b = sum(1 for xx in range(44, 66)
                           if near(px_at(pxB, w2, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        ws1_active_b = sum(1 for xx in range(68, 90)
                           if near(px_at(pxB, w2, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        wmedit_b = sum(1 for xx in range(150, 700)
                       if near(px_at(pxB, w2, xx, 209),
                               (0x30, 0xE0, 0xE0), tol=20))
        wmedit_grey_b = sum(1 for xx in range(150, 700)
                           if near(px_at(pxB, w2, xx, 209),
                                   (0x20, 0x20, 0x20), tol=10))
        print(f"   after WS2: ws0-cyan={ws0_active_b}  ws1-cyan={ws1_active_b}  "
              f"wmedit-cyan={wmedit_b}  wmedit-grey={wmedit_grey_b}")

        # Click workspace 1 button to go back.
        print("[+] click workspace-1 button at (54, 8)")
        click(q, qbuf, 54, 8)

        w3, h3, pxC = shot("c")
        ws0_active_c = sum(1 for xx in range(44, 66)
                           if near(px_at(pxC, w3, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        ws1_active_c = sum(1 for xx in range(68, 90)
                           if near(px_at(pxC, w3, xx, 8),
                                   (0x30, 0xE0, 0xE0), tol=20))
        wmedit_c = sum(1 for xx in range(150, 700)
                       if near(px_at(pxC, w3, xx, 209),
                               (0x30, 0xE0, 0xE0), tol=20))
        wmedit_grey_c = sum(1 for xx in range(150, 700)
                           if near(px_at(pxC, w3, xx, 209),
                                   (0x20, 0x20, 0x20), tol=10))
        print(f"   after back to WS1: ws0-cyan={ws0_active_c}  ws1-cyan={ws1_active_c}  "
              f"wmedit-cyan={wmedit_c}  wmedit-grey={wmedit_grey_c}")

        # wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px_at(pxC, w, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        # Combined wmedit visibility = cyan + grey (focused or not).
        wmedit_vis_a = wmedit_a + wmedit_grey_a
        wmedit_vis_b = wmedit_b + wmedit_grey_b
        wmedit_vis_c = wmedit_c + wmedit_grey_c
        checks.append((f"WS1 selected at boot (ws0-cyan {ws0_active_a})",
                       ws0_active_a > 10))
        checks.append((f"wmedit visible on WS1 ({wmedit_vis_a} px)",
                       wmedit_vis_a > 100))
        checks.append((f"WS2 selected after click (ws1-cyan {ws1_active_b})",
                       ws1_active_b > 10))
        checks.append((f"wmedit HIDDEN on WS2 ({wmedit_vis_b} px)",
                       wmedit_vis_b < 50))
        checks.append((f"wmedit reappears on WS1 return ({wmedit_vis_c} px)",
                       wmedit_vis_c > 100))
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
