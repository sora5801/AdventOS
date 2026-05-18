#!/usr/bin/env python3
"""
Session 131 smoke test: bottom-right resize grip on client windows.

Verifies the grip is RENDERED.  Actual drag behaviour is too
flaky to assert through QEMU's PS/2 emulation (rel-event scaling
varies session-to-session); the grip-rendering check confirms the
feature is wired into paint_window in the right z-order (on top of
the content fill).

Pixel checks (5):
  - wmhello window at expected position (left edge white)
  - grip background = wmhello's frame_color (CYAN 0x30E0E0)
  - grip diagonal stripe — at least one white pixel inside grip
  - WINDOW content (blue header band) still visible
  - wmd compositor alive (top status bar)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4491
SERIAL_PORT = 4492
SHOT = os.path.join(ROOT, "shot_resize.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s131.log"), "w")
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
        ser.sendall(b"wmd 30 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmhello 25\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        if os.path.exists(SHOT):
            os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)

        with open(SHOT, "rb") as f:
            f.readline()
            line = f.readline()
            while line.startswith(b"#"): line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]
        def near(a, b, tol=4):
            return all(abs(int(x)-int(y)) <= tol for x,y in zip(a, b))

        # wmhello at slot 0: outer (100, 200, 224, 158).  Right
        # outer x=323, bottom outer y=357.  Resize grip occupies
        # (x=312..323, y=346..357) — 12x12 in the corner.
        checks = []

        # 1. wmhello left edge white at x=100, y=280.
        left_white = near(px(100, 280), (0xFF, 0xFF, 0xFF), tol=20)
        checks.append((f"wmhello left edge white @ (100, 280)",
                       left_white))

        # 2. Grip background = CYAN 0x30E0E0.  Sample (315, 350) —
        #    clearly inside the 12x12 grip box.
        gb = px(315, 350)
        print(f"   grip bg @ (315, 350) = {gb}")
        checks.append((f"grip bg = CYAN ({gb})",
                       near(gb, (0x30, 0xE0, 0xE0), tol=15)))

        # 3. Grip has at least a few white pixels (the diagonal
        #    stripe pattern).  Count whites in the grip box.
        grip_whites = 0
        for yy in range(346, 358):
            for xx in range(312, 324):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    grip_whites += 1
        print(f"   grip white pixels: {grip_whites}")
        checks.append((f"grip diagonal stripe ({grip_whites} white px)",
                       grip_whites >= 5))

        # 4. wmhello blue band still painted at y=218..235.
        blue = sum(1 for xx in range(131, 300)
                   if near(px(xx, 226), (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"wmhello blue band painted ({blue}/169)",
                       blue > 130))

        # 5. wmd status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"wmd status bar ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))

        print("\n=== pixel checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed:
                ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
