#!/usr/bin/env python3
"""
Session 113 smoke test: input routing to focused client.

Boot QEMU, start wmd, start wmhello, move the cursor over the
wmhello window, click, take a screendump.  Verify that wmhello
visibly reacted to the focus + press events:

  - bottom border is GREEN (focus established)
  - background is NOT the default first palette entry
    (a click happened → palette advanced)
  - a click marker (yellow during press OR green after release)
    is visible somewhere in the surface

Sanity-checks the wmd + wmhello pixel landmarks too (top bar, blue
band, etc.) so a kernel-level regression that breaks the screendump
is caught early.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4453
SERIAL_PORT = 4454
SHOT_PPM = os.path.join(ROOT, "shot_wmevents.ppm")


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
    log = open(os.path.join(ROOT, "qemu-s113.log"), "w")
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
        print("[+] launching wmhello 15")
        ser.sendall(b"wmhello 15\n")
        time.sleep(1.5)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Cursor starts at FB center (512, 384).  wmhello window at
        # (340, 360); content surface at (341, 378)..(560, 517).
        # Walk cursor to (~450, ~460) so the crosshair lands at
        # surface (108, 82) — below the moving red square's vertical
        # span (y=40..76), so the cursor stays visible regardless
        # of where the square happens to be.
        # 7 events of (-9, +11) gives delta (-63, +77) → (449, 461).
        print("[+] moving cursor onto wmhello (~450, 461)")
        for i in range(7):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -9}},
                {"type": "rel", "data": {"axis": "y", "value": 11}},
            ]})
            qbuf = b""
            time.sleep(0.06)
        time.sleep(0.8)

        # Click — press + release.  Hold time matters: QEMU's PS/2
        # emulation seems to coalesce button events that flip state
        # within ~600ms of each other, so we hold the down state for
        # 1.0s to guarantee both edges produce a packet wmd can see.
        print("[+] click (1.0s hold)")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.0)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.2)

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

        # wmhello surface at (341, 378)..(560, 517) — 220x140.
        sx, sy = 341, 378
        sw, sh = 220, 140
        checks = []

        # 1. wmhello visible at all — top blue band at (sx+30..sx+190, sy+8)
        blue = sum(1 for xx in range(sx + 30, sx + 190)
                   if near(px(xx, sy + 8), (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"wmhello blue band @ y={sy+8} ({blue}/160)",
                       blue > 120))

        # 2. Bottom border GREEN — focus is held (cursor over surface).
        #    border y = sy + sh - 4..sy+sh-1 = sy+136..sy+139.
        border_green = sum(1 for xx in range(sx + 20, sx + 200)
                           if near(px(xx, sy + 137),
                                   (0x30, 0xE0, 0x30), tol=15))
        checks.append((f"focus green border @ y={sy+137} ({border_green}/180)",
                       border_green > 140))

        # 3. Background palette advanced — not the default 0x101030.
        #    Sample several interior points and confirm at least one
        #    differs from the default palette entry.
        bg_palette = [
            (0x10, 0x10, 0x30),  # default
            (0x30, 0x10, 0x30), (0x10, 0x30, 0x30),
            (0x30, 0x30, 0x10), (0x10, 0x30, 0x10),
        ]
        # Find a clean bg sample: in the upper-left of surface, below
        # the blue band, above any red square. (sx+5, sy+30) is safe.
        bg_sample = px(sx + 5, sy + 30)
        in_palette = any(near(bg_sample, p, tol=12) for p in bg_palette)
        not_default = not near(bg_sample, bg_palette[0], tol=12)
        print(f"    bg sample @ ({sx+5},{sy+30}) = {bg_sample}; "
              f"in_palette={in_palette}, advanced={not_default}")
        checks.append(("bg palette advanced (click registered)",
                       not_default and in_palette))

        # 4. Click marker visible — yellow (during press) or green
        #    (after release).  Press-marker color is 0xE0E030, release
        #    is 0x30E030.  Scan surface interior.
        yellow_hits = 0
        green_hits  = 0
        for yy in range(sy + 20, sy + sh - 5):
            for xx in range(sx + 5, sx + sw - 5):
                p = px(xx, yy)
                if near(p, (0xE0, 0xE0, 0x30), tol=15):
                    yellow_hits += 1
                elif near(p, (0x30, 0xE0, 0x30), tol=15):
                    green_hits += 1
        # Note: green_hits will be inflated by the bottom-border
        # green pixels if we don't exclude the border band; we did
        # (yy stops at sy+sh-5 = sy+135, border starts at sy+136).
        print(f"    click marker hits: yellow={yellow_hits}, "
              f"green={green_hits}")
        checks.append((f"click marker present (yellow={yellow_hits} "
                       f"green={green_hits})",
                       yellow_hits + green_hits >= 25))

        # 5. wmd still alive — top status bar present.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"wmd status bar ({sb}/{w-100})",
                       sb > (w - 100) * 0.75))

        # 6. Cursor crosshair inside wmhello (white 11x11 cross drawn
        #    by wmhello itself when it has focus + a known mx/my from
        #    MOUSE_MOVE).  Cursor at screen (449, 461) → surface
        #    (108, 82).  Below the moving red square (y=40..76) so
        #    the crosshair stays visible regardless of sq_x phase.
        cur_white = 0
        for d in range(-5, 6):
            if near(px(449 + d, 461), (0xFF, 0xFF, 0xFF), tol=20):
                cur_white += 1
            if near(px(449, 461 + d), (0xFF, 0xFF, 0xFF), tol=20):
                cur_white += 1
        checks.append((f"cursor crosshair @ (~449, 461): {cur_white}/22",
                       cur_white >= 12))

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
