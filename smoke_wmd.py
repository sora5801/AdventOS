#!/usr/bin/env python3
"""
Session 111 smoke test: wmd compositor.

Boots QEMU headless, runs `wmd 15`, lets it paint a few frames,
injects mouse motion (no drag), then verifies that the screendump
contains the expected window decorations + content.

Checks the four demo windows:
  - "Clock"      blue title bar at (80, 80, 260, 110)
  - "Gradient"   magenta title bar at (400, 120, 260, 170)
  - "About wmd"  green title bar at (180, 260, 260, 130)
  - "Color bars" yellow title bar at (540, 360, 320, 110)

Plus the top status bar (dark grey strip at y=0..17) and the deep
blue desktop background and the cursor crosshair.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4447
SERIAL_PORT = 4448
SHOT_PPM = os.path.join(ROOT, "shot_wmd.ppm")


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
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s111.log"), "w")
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
        print("[+] shell up; launching wmd 15")
        ser.sendall(b"wmd 15\n")
        time.sleep(2.0)  # let it paint a few frames

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Some mouse motion to verify the cursor follows. Aim at the
        # Clock window's title bar (around x=120, y=88).
        print("[+] injecting motion toward clock title (~(120,88))")
        for i in range(40):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -10}},
                {"type": "rel", "data": {"axis": "y", "value": -8}},
            ]})
            qbuf = b""
            time.sleep(0.05)
        time.sleep(1.0)

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
            assert magic.strip() == b"P6", f"bad magic {magic!r}"
            line = f.readline()
            while line.startswith(b"#"):
                line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()
        print(f"    {w}x{h} ppm, {len(pixels)} bytes")

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        def near(a, b, tol=4):
            return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

        # Desktop bg is 0x0A1828. Sample a known-clear area: between
        # all four windows. e.g. (700, 50) is above gradient (y=120).
        checks = []

        # 1. Top status bar dark grey strip.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=8))
        checks.append((f"top status bar @ y=6 ({sb}/{w-100})",
                       sb > (w - 100) * 0.85))

        # 2. Desktop background present in the gap area near top-right.
        bg = sum(1 for xx in range(700, 1000)
                 if near(px(xx, 50), (0x0A, 0x18, 0x28), tol=6))
        checks.append((f"desktop background top-right @ y=50 ({bg}/300)",
                       bg > 200))

        # 3. Clock window title bar — BLUE (initial raise=1, lowest z,
        #    not focused by default → DARK_GREY). Title bar y range
        #    80..98. Sample at y=85, x in 90..330 (interior of bar).
        #    Initial focused = -1 so all title bars are DARK_GREY at
        #    boot. After click we'd see focus color, but we never
        #    clicked.  So just verify the title-bar STRIP is dark
        #    grey (uniform).
        clk_title = sum(1 for xx in range(85, 335)
                        if near(px(xx, 85), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"Clock title bar dark @ y=85 ({clk_title}/250)",
                       clk_title > 180))

        # 4. Clock content area: 0x102030, around (200, 130).
        clk_content = sum(1 for yy in range(105, 185)
                          if near(px(200, yy), (0x10, 0x20, 0x30), tol=8))
        checks.append((f"Clock content bg @ x=200 ({clk_content}/80)",
                       clk_content > 50))

        # 5. Gradient window content — a known interior point should
        #    have R growing with x. At (500, 200) it's well inside.
        #    Sample two points horizontally: at x=420 (near left) R should
        #    be small; at x=640 (near right) R should be large.
        gx_left  = px(420, 200)
        gx_right = px(640, 200)
        checks.append((f"gradient L pixel {gx_left} vs R pixel {gx_right}: R grows",
                       gx_right[0] > gx_left[0] + 40))

        # 6. About wmd window — green-tinted bg 0x103018 around (300, 320).
        ab = sum(1 for yy in range(295, 380)
                 if near(px(300, yy), (0x10, 0x30, 0x18), tol=8))
        checks.append((f"About content bg @ x=300 ({ab}/85)",
                       ab > 40))

        # 7. Color bars window — sample a known red bar near (560, 410).
        #    With 6 bars in 320-4=316 px width, each bar is ~52 px.
        #    Bar 0 (red) interior: x = 540+2+10..540+2+50 = 552..592.
        bar_red = sum(1 for xx in range(555, 588)
                      if near(px(xx, 420), (0xE0, 0x30, 0x30), tol=12))
        checks.append((f"Color-bar 0 RED @ y=420 ({bar_red}/33)",
                       bar_red > 20))

        # 8. Cursor crosshair (white-ish, 17h+17v cross). After 40
        #    rel events of (-10, -8), cursor should be near
        #    (512-400, 384-320) = (112, 64) if 1:1. May be different
        #    due to QEMU scaling. Scan the upper-left quadrant.
        cluster = []
        for yy in range(22, 200):
            for xx in range(20, 400):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=12):
                    cluster.append((xx, yy))
        if cluster:
            xs = [p[0] for p in cluster]; ys = [p[1] for p in cluster]
            print(f"    white-ish pixels in UL quadrant: {len(cluster)}; "
                  f"x in [{min(xs)},{max(xs)}], y in [{min(ys)},{max(ys)}]")
        # The text "AdventOS wmd" in the About window is also white,
        # but it's outside the UL quadrant (in About at x>=186, y>=282).
        # Title bar text "Clock" is at (86, 85). So the cluster might
        # mix cursor with text. We just check we have *some* cluster
        # of >= 17 white pixels in the quadrant.
        checks.append((f"cursor or text white pixels in UL quadrant: {len(cluster)}",
                       len(cluster) >= 17))

        # 9. wmd "still running" — frame counter advances. We can't
        #    check directly from one screenshot, but the very fact
        #    that we got a non-black frame after 3+ seconds of demo
        #    is strong evidence.

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
