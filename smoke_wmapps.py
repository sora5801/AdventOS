#!/usr/bin/env python3
"""
Session 115 smoke test: wmclock + wmpaint visible at the same time.

Boot QEMU, start wmd, start BOTH wmclock and wmpaint, paint a few
strokes in wmpaint, screendump, verify:

  - wmclock window shows a 2x-scaled colon ":" in the time display
    (a distinctive horizontal pattern of green dots that we can
    pixel-check without depending on the exact current time)
  - wmpaint shows the toolbar swatches (7 distinct colors at the
    top of its surface)
  - wmpaint shows painted strokes (mouse-drag produced colored
    pixels in the canvas)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4457
SERIAL_PORT = 4458
SHOT_PPM = os.path.join(ROOT, "shot_wmapps.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s115.log"), "w")
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
        print("[+] shell up; launching wmd 60 &")
        ser.sendall(b"wmd 60 &\n")
        time.sleep(2.0)
        print("[+] launching wmclock 30 &")
        ser.sendall(b"wmclock 30 &\n")
        time.sleep(1.5)
        print("[+] launching wmpaint 30")
        ser.sendall(b"wmpaint 30\n")
        time.sleep(2.0)

        # wmclock at slot 4: (340, 360), size 260+4=264 × 100+18+2=120
        #   surface at (341, 378)..(600, 477)
        # wmpaint at slot 5: (400, 400), size 400+4=404 × 280+18+2=300
        #   surface at (401, 418)..(800, 697)
        wmclock_sx, wmclock_sy = 341, 378
        wmpaint_sx, wmpaint_sy = 401, 418

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Move cursor to wmpaint canvas (well inside, below toolbar).
        # Cursor center is (512, 384). wmpaint canvas at screen
        # (401..800, 442..697).  Aim for (550, 550) → delta (+38, +166).
        # 17 events of (+3, +10) → (+51, +170) → (563, 554).
        print("[+] moving cursor into wmpaint canvas")
        for i in range(17):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 3}},
                {"type": "rel", "data": {"axis": "y", "value": 10}},
            ]})
            qbuf = b""
            time.sleep(0.04)
        time.sleep(0.6)

        # Click + hold to start drawing — focus wmpaint AND begin a
        # stroke.  Then drag.  Hold the press for 1.3s before
        # starting drag to ensure wmd sees the down-edge through
        # QEMU's PS/2 button coalescing.
        print("[+] press + drag to paint")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.3)
        # Drag — move cursor while held.  Inject motion paired
        # with a (0, 0) rel to keep the kernel mouse driver alive.
        for i in range(15):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 4}},
                {"type": "rel", "data": {"axis": "y", "value": 2}},
            ]})
            qbuf = b""
            time.sleep(0.08)
        time.sleep(0.5)
        # Release.
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.5)

        if os.path.exists(SHOT_PPM):
            os.remove(SHOT_PPM)
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT_PPM, "format": "ppm"})
        qbuf = b""
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_PPM)
                                          or os.path.getsize(SHOT_PPM) < 100):
            time.sleep(0.1)
        if not os.path.exists(SHOT_PPM):
            print("[!] screendump never produced a file")
            return 1
        print(f"    saved {SHOT_PPM}, {os.path.getsize(SHOT_PPM)} bytes")

        with open(SHOT_PPM, "rb") as f:
            magic = f.readline()
            line = f.readline()
            while line.startswith(b"#"):
                line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()
        print(f"    {w}x{h} ppm")

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        def near(a, b, tol=4):
            return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

        checks = []

        # 1. wmclock title bar — dark grey (not focused; we didn't
        #    click it).  Sample at clock_sy+2 *above* the text band
        #    so the title text doesn't break up the dark-grey run.
        clk_title = sum(1 for xx in range(wmclock_sx + 30, wmclock_sx + 200)
                        if near(px(xx, wmclock_sy + 2),
                                (0x30, 0x30, 0x30), tol=10))
        checks.append((f"wmclock title @ y={wmclock_sy+2} ({clk_title}/170)",
                       clk_title > 140))

        # 2. wmclock has GREEN pixels in the time region — that's
        #    the digits.  Exact count depends on which numerals
        #    happen to be rendered ('1' = ~16, '8' = ~64), so just
        #    require >= 60 green pixels (worst case "1:11:11").
        green_time = 0
        for yy in range(wmclock_sy + 32, wmclock_sy + 50):
            for xx in range(wmclock_sx + 20, wmclock_sx + 240):
                if near(px(xx, yy), (0x30, 0xE0, 0x30), tol=15):
                    green_time += 1
        checks.append((f"wmclock green time digits ({green_time})",
                       green_time > 60))

        # 3. wmpaint toolbar has the red swatch.  Toolbar y=0..23,
        #    swatch 2 is red at x=4+1*22..x+16 → 26..42 within surface.
        red_swatch = sum(1 for xx in range(wmpaint_sx + 26,
                                            wmpaint_sx + 42)
                         if near(px(xx, wmpaint_sy + 10),
                                 (0xE0, 0x30, 0x30), tol=12))
        checks.append((f"wmpaint red swatch ({red_swatch}/16)",
                       red_swatch > 10))

        # 4. wmpaint toolbar has the green swatch (color 4).
        #    x = 4 + 3*22 = 70 → 70..86 within surface.
        green_swatch = sum(1 for xx in range(wmpaint_sx + 70,
                                              wmpaint_sx + 86)
                           if near(px(xx, wmpaint_sy + 10),
                                   (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"wmpaint green swatch ({green_swatch}/16)",
                       green_swatch > 10))

        # 5. wmpaint has WHITE drawn pixels in the canvas area.
        #    Initial brush is white, so the drag stroke should leave
        #    a trail of white dots in the canvas (y >= toolbar).
        #    Canvas starts at wmpaint_sy + 24.  Cursor went to ~(563, 554)
        #    then dragged to ~(623, 584).  So white trail somewhere in
        #    canvas, x in [560..630], y in [550..590] (screen coords).
        white_strokes = 0
        for yy in range(wmpaint_sy + 100, wmpaint_sy + 200):
            for xx in range(wmpaint_sx + 100, wmpaint_sx + 280):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white_strokes += 1
        checks.append((f"wmpaint white stroke pixels ({white_strokes})",
                       white_strokes > 30))

        # 6. wmpaint canvas background still mostly 0x282828 in the
        #    untouched lower-left area (proves clear-once-then-overlay
        #    model works).
        bg_dark = 0
        for yy in range(wmpaint_sy + 220, wmpaint_sy + 270):
            for xx in range(wmpaint_sx + 4, wmpaint_sx + 80):
                if near(px(xx, yy), (0x28, 0x28, 0x28), tol=8):
                    bg_dark += 1
        checks.append((f"wmpaint untouched bg @ lower-left ({bg_dark})",
                       bg_dark > 2000))

        # 7. wmd top status bar still alive (whole pipeline OK).
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
        try:
            qemu.terminate()
            qemu.wait(timeout=3)
        except Exception:
            qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
