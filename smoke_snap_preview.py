#!/usr/bin/env python3
"""
Session 155 smoke: snap-to-edge ghost preview.

Boots wmd + wmedit.  Click+drag the wmedit title bar toward the
top edge so the cursor lands within SNAP_PX=8 of the top.  Hold
the button (do NOT release) and screendump.  Verify a thick cyan
outline (~0x60D0F0) frames the top-snap target (full FB minus
top status bar and bottom taskbar).
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

    log = open(os.path.join(ROOT, "qemu-s155.log"), "w")
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
        ser.sendall(b"wmedit /tmp/snap 50 &\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmedit outer (100, 200, 644, 420).  Title bar y=200..217.
        # Click in the title bar away from buttons: (200, 208).
        # Drag UP to (200, 20) which is in the top snap zone
        # (ms.y < 18 + 8 = 26).  HOLD the button (don't release).
        print("[+] press in wmedit title bar")
        abs_send(q, qbuf, 200, 208)
        time.sleep(0.8)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.4)

        # Move cursor to the top snap zone.  ms.y must be < 26.
        # Send abs to (200, 20).  Use multiple intermediate steps
        # so the WM gets MOUSE_MOVE events.
        print("[+] drag toward top edge")
        for y in range(180, 19, -20):
            abs_send(q, qbuf, 200, y)
            time.sleep(0.05)
        abs_send(q, qbuf, 200, 20)
        time.sleep(1.0)

        # Screendump WHILE the button is still held (preview should
        # be visible).
        SHOT = os.path.join(ROOT, "shot_snap_preview.ppm")
        if os.path.exists(SHOT): os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)

        # Release the button (snap will fire).
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})

        w, h, pixels = read_ppm(SHOT)
        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        # Top-snap target: (0, 18, fb_w, fb_h - TASKBAR_H - 18).
        # = (0, 18, 1024, 768 - 28 - 18) = (0, 18, 1024, 722).
        # So outline rect is (0, 18) to (1023, 739).  3 px thick.
        # Top edge of outline: y in {18, 19, 20}.  Sample y=18 across.
        cyan_top = sum(1 for xx in range(20, w - 20)
                       if near(px(xx, 18), (0x60, 0xD0, 0xF0), tol=25))
        # Bottom edge of outline: y = 18 + 722 - 1 = 739.
        cyan_bot = sum(1 for xx in range(20, w - 20)
                       if near(px(xx, 739), (0x60, 0xD0, 0xF0), tol=25))
        # Left edge: x in {0, 1, 2}.  Sample x=0, y range 30..730.
        cyan_left = sum(1 for yy in range(30, 730)
                        if near(px(0, yy), (0x60, 0xD0, 0xF0), tol=25))
        # Right edge: x = 1023.
        cyan_right = sum(1 for yy in range(30, 730)
                         if near(px(1023, yy), (0x60, 0xD0, 0xF0), tol=25))
        print(f"   cyan outline top={cyan_top} bot={cyan_bot} "
              f"left={cyan_left} right={cyan_right}")

        # wmd top status bar — preview doesn't overlap it; the bar
        # at y=6 is above the snap target (which starts at y=18).
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"top outline ({cyan_top} cyan px)",
                       cyan_top > 500))
        checks.append((f"bottom outline ({cyan_bot} cyan px)",
                       cyan_bot > 500))
        checks.append((f"left outline ({cyan_left} cyan px)",
                       cyan_left > 400))
        checks.append((f"right outline ({cyan_right} cyan px)",
                       cyan_right > 400))
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
