#!/usr/bin/env python3
"""
Session 141 smoke test: USB tablet (absolute mouse) driver.

Boots QEMU with `-device usb-tablet` attached.  Verifies:

  1. Kernel enumerates the tablet and logs "HID tablet registered"
     followed by "HID tablet polling task started".
  2. Sending an absolute QMP `input-send-event` of type=abs to a
     known on-screen point teleports the wmd cursor (a + glyph) to
     that point — the cursor pixels appear near the commanded
     position, NOT at the PS/2 starting centre (512, 384).

Pixel checks:
  - boot log contains the tablet-init lines
  - after issuing abs (x=200, y=300), white cursor pixels appear in
    a small window around (200, 300) in the framebuffer
  - wmd top status bar still alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_usbtablet.ppm")


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

    log_path = os.path.join(ROOT, "qemu-s141.log")
    log = open(log_path, "w")
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
        if not ok:
            print("[!] no shell prompt"); return 1
        ser_text = ser_buf.decode("utf-8", errors="replace")

        tablet_registered = "HID tablet registered" in ser_text
        tablet_polling    = "HID tablet polling task started" in ser_text
        print(f"   boot has 'tablet registered': {tablet_registered}")
        print(f"   boot has 'tablet polling':    {tablet_polling}")

        # Launch wmd so we get a visible cursor.
        ser.sendall(b"wmd 40 --clean &\n")
        time.sleep(2.5)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Teleport the cursor to absolute (200, 300) using abs-axis
        # events.  QMP abs values are in [0, 32767]; QEMU forwards
        # those verbatim to the tablet, which sends them in the
        # report payload bytes 1..4.  Our kernel then scales 0..32767
        # to FB pixel space → x = 200, y = 300 for a 1024x768 FB:
        #    32767 * 200 / 1023 = 6411
        #    32767 * 300 / 767  = 12814
        # Send those abs values.
        target_x = 200
        target_y = 300
        abs_x = 32767 * target_x // 1023
        abs_y = 32767 * target_y // 767
        print(f"[+] teleport cursor to ({target_x}, {target_y}) "
              f"via abs ({abs_x}, {abs_y})")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": abs_x}},
            {"type": "abs", "data": {"axis": "y", "value": abs_y}},
        ]})
        # The tablet polling task runs every 15 ms but the device
        # only emits a report when input state changes (a stream of
        # NAKs precedes the abs-event burst).  ~1.5 s gives the task
        # time to pick up the report and gives wmd several frames to
        # repaint.
        time.sleep(1.5)

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

        # Crosshair is a white + glyph centred at the cursor.  Look
        # for white pixels in a 24x24 region around (target_x, target_y).
        cur_pixels = 0
        for yy in range(target_y - 12, target_y + 12):
            for xx in range(target_x - 12, target_x + 12):
                if 0 <= xx < w and 0 <= yy < h:
                    if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=10):
                        cur_pixels += 1
        print(f"   white pixels near ({target_x},{target_y}): {cur_pixels}")

        # Sanity: there should be very few (ideally zero) white pixels
        # at the OLD centre (512, 384) — proving the cursor moved.
        center_pixels = 0
        for yy in range(384 - 12, 384 + 12):
            for xx in range(512 - 12, 512 + 12):
                if 0 <= xx < w and 0 <= yy < h:
                    if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=10):
                        center_pixels += 1
        print(f"   white pixels at old centre (512,384):    {center_pixels}")

        # wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"boot enumerated tablet", tablet_registered))
        checks.append((f"boot started tablet polling task", tablet_polling))
        checks.append((f"cursor at target ({cur_pixels} white px in 24x24)",
                       cur_pixels >= 8))
        checks.append((f"cursor moved away from default centre "
                       f"({center_pixels} white px there)",
                       center_pixels < cur_pixels))
        checks.append((f"wmd status bar alive ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))

        print("\n=== checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed: ok_all = False

        # Drain any pending serial output for diagnostics — pick up
        # USB_HID_TRACE lines emitted between boot and now.
        ser.settimeout(0.5)
        try:
            while True:
                more = ser.recv(4096)
                if not more: break
                ser_buf += more
        except socket.timeout:
            pass
        trace = [l for l in ser_buf.decode("utf-8", errors="replace").splitlines()
                 if "tablet" in l or "[usb-hid]" in l]
        if trace:
            print("\n--- last 10 tablet/usb-hid trace lines ---")
            for l in trace[-10:]: print(f"   {l}")

        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
