#!/usr/bin/env python3
"""
Session 112 smoke test: external WM client over the shared-surface
protocol.

Boots QEMU headless, starts `wmd 30` from the in-guest shell, gives
it a moment to bind, then forks a `wmhello 12` in the background.
Lets the demo run a few seconds, takes a QMP screendump, and verifies:

  - the four wmd internal demo windows are still painted (untouched
    from session 111)
  - the wmhello client window appears at the expected slot offset
    (100, 200) of size 224x158 = 220+4 wide + 18-title + 2 borders +
    140 high
  - the client surface content actually shows up — animated square
    + green bottom border that are wmhello's defining elements

This proves the cross-process shared-memory blit path works end to
end.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4451
SERIAL_PORT = 4452
SHOT_PPM = os.path.join(ROOT, "shot_wmclient.ppm")


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
    log = open(os.path.join(ROOT, "qemu-s112.log"), "w")
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
        print("[+] shell up; launching wmd 30 &")
        ser.sendall(b"wmd 30 &\n")
        time.sleep(2.0)

        # Now spawn the client. Need a second shell because the first
        # is now hosting wmd's stdio. Actually wmd is backgrounded, so
        # the prompt should still be reachable — but the prompt is
        # mute (output is going to the framebuffer). Let me just send
        # the command blind.
        print("[+] launching wmhello 12")
        ser.sendall(b"wmhello 12\n")
        time.sleep(3.0)  # let wmhello bind, paint a few frames

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

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
        print(f"    {w}x{h} ppm, {len(pixels)} bytes")

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        def near(a, b, tol=4):
            return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

        checks = []

        # Where does wmd place the first client window?
        # init_demo_windows fills slots 0..3; the new client lands at
        # slot 4, which drain_wm_messages places at x=100+4*60=340,
        # y=200+4*40=360.  Size = 220+4=224 wide × 140+18+2=160 tall.
        # The client surface (220x140) lives inside: x=341..560,
        # y=378..517 (1 px frame + 18 px title bar offset).
        cx0, cy0 = 340, 360
        cw, ch = 224, 160
        sx, sy = cx0 + 1, cy0 + 18           # surface top-left in screen coords
        # wmhello paints:
        #   - background gradient (0x101030 + small ramp)
        #   - title-ish band y=0..17 inside surface: x=sx..sx+220, y=sy..sy+17 = blue 0x4080E0
        #   - moving red square at (sq_x, 40)..(sq_x+36, 76) inside surface
        #   - green bottom border y=h-4..h-1 = green 0x30E030

        # 1. wmd is still showing its top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"wmd top status bar @ y=6 ({sb}/{w-100})",
                       sb > (w - 100) * 0.80))

        # 2. Client window cyan title bar at wmd's (cx0, cy0)..(cx0+cw, cy0+TITLE_H).
        #    Cyan in libgfx = 0x30E0E0. With no focus, wmd paints title bar
        #    in DARK_GREY 0x202020; on focus, in frame_color=CYAN.
        #    Initial focused = -1, so dark grey.
        ct = sum(1 for xx in range(cx0 + 30, cx0 + cw - 30)
                 if near(px(xx, cy0 + 9), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"client title bar dark @ y={cy0+9} ({ct}/{cw-60})",
                       ct > (cw - 60) * 0.7))

        # 3. Client surface: blue band y=sy..sy+17 inside the client.
        #    Sample at y=sy+8, x in middle of band.
        cb = sum(1 for xx in range(sx + 30, sx + 200)
                 if near(px(xx, sy + 8), (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"client blue band @ y={sy+8} ({cb}/170)",
                       cb > 120))

        # 4. Client surface: green bottom border y=sy+136..sy+139.
        gb = sum(1 for xx in range(sx + 20, sx + 200)
                 if near(px(xx, sy + 137), (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"client green bottom @ y={sy+137} ({gb}/180)",
                       gb > 100))

        # 5. Client surface: dark gradient bg sample at known-clear
        #    interior. y=sy+30 is between blue band (y=0..17) and red
        #    square (y=40+). x=sx+5 should be background.
        bgp = px(sx + 5, sy + 30)
        bg_ok = (bgp[0] < 0x40) and (bgp[1] < 0x40) and (bgp[2] >= 0x20 and bgp[2] <= 0x60)
        checks.append((f"client bg-gradient @ ({sx+5},{sy+30}) = {bgp}", bg_ok))

        # 6. Red square: it moves, so its X is variable. Look for
        #    any red pixel cluster in the band y=40..75 of the
        #    client surface (= sy+40..sy+75).
        red_hits = 0
        red_xs = []
        for xx in range(sx, sx + 220):
            if near(px(xx, sy + 55), (0xE0, 0x30, 0x30), tol=20):
                red_hits += 1
                red_xs.append(xx)
        if red_xs:
            print(f"    red square pixels at y={sy+55}: {red_hits}; "
                  f"x range {min(red_xs)}..{max(red_xs)}")
        checks.append((f"client red square pixels @ y={sy+55}: {red_hits}",
                       red_hits >= 5))

        # 7. wmd's clock window (slot 0 in init_demo_windows) still
        #    paints — at (80, 80) w=260 h=110.  Sample its content
        #    bg color 0x102030 at x=200, y=130.
        clk = sum(1 for yy in range(105, 185)
                  if near(px(200, yy), (0x10, 0x20, 0x30), tol=8))
        checks.append((f"wmd Clock content @ x=200 ({clk}/80)",
                       clk > 50))

        # 8. Color bars window (the Yellow one) at (540, 360, 320, 110)
        #    — red bar still where we expect it.
        cb2 = sum(1 for xx in range(555, 588)
                  if near(px(xx, 420), (0xE0, 0x30, 0x30), tol=12))
        checks.append((f"wmd Color-bar 0 RED @ y=420 ({cb2}/33)",
                       cb2 > 20))

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
