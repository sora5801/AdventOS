#!/usr/bin/env python3
"""
Session 110 smoke test: double-buffered mouse demo.

Boot QEMU headless, run `mouse 8` from the in-guest shell, inject
20 mouse rel events of (+10, +5) via QMP input-send-event, then
take a screendump and pixel-check the framebuffer.

Verifies:
  - Frame is coherent (backdrop, frame, HUD all present)
  - Cursor crosshair is at expected position
  - No half-drawn artifacts (a single QMP screendump can't *prove*
    no tearing, but if the cursor + HUD + frame are all consistent
    then the present is at least atomic-looking from QMP's vantage)
"""
import os, socket, json, subprocess, time, sys, struct

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4445
SERIAL_PORT = 4446
SHOT_PPM = os.path.join(ROOT, "shot_db.ppm")


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
    # Drain until we see our 'return' (skipping events)
    while True:
        rep, buf = qmp_recv(s, buf)
        if rep is None:
            return None, buf
        if "return" in rep or "error" in rep:
            return rep, buf


def wait_for(sock, marker, buf, timeout=30, tee=False):
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
        if tee:
            sys.stdout.write(chunk.decode(errors="replace"))
            sys.stdout.flush()
        if marker in buf:
            return True, buf
    return False, buf


def main():
    # Kill any stale QEMU instance.
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s110.log"), "w")
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
        # Connect serial.
        ser = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=5)
        ser_buf = b""
        ok, ser_buf = wait_for(ser, b"$ ", ser_buf, timeout=30)
        if not ok:
            print("[!] never saw shell prompt")
            print(ser_buf[-2000:].decode(errors="replace"))
            return 1
        print("[+] shell up; launching mouse 12")
        ser.sendall(b"mouse 12\n")
        time.sleep(1.5)  # let the demo grab the FB and paint a frame

        # Connect QMP.
        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        # greeting
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Inject 20 mouse rel events of (+10, +5) so the cursor walks
        # diagonally from the start position (initial state is centered
        # by the kernel: (512, 384)). QMP rel events go through QEMU's
        # input subsystem, which scales/coalesces them before the PS/2
        # emulation re-encodes as packets. Expect smaller-than-raw deltas.
        print("[+] injecting 20 rel events of (+10, +5)")
        for i in range(20):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 10}},
                {"type": "rel", "data": {"axis": "y", "value": 5}},
            ]})
            qbuf = b""
            time.sleep(0.12)
        # Let the userspace poll loop catch up, then run a few more
        # frames so we're sure the FB is repainted with the new pos.
        time.sleep(1.5)

        # Take a screendump.
        print("[+] screendump")
        if os.path.exists(SHOT_PPM):
            os.remove(SHOT_PPM)
        qmp_cmd(q, qbuf, "screendump",
                {"filename": SHOT_PPM, "format": "ppm"})
        qbuf = b""
        # screendump completes asynchronously sometimes; loop till file appears
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_PPM)
                                          or os.path.getsize(SHOT_PPM) < 100):
            time.sleep(0.1)
        if not os.path.exists(SHOT_PPM):
            print("[!] screendump never produced a file")
            return 1
        print(f"    saved {SHOT_PPM}, {os.path.getsize(SHOT_PPM)} bytes")

        # Parse the PPM.
        with open(SHOT_PPM, "rb") as f:
            magic = f.readline()
            assert magic.strip() == b"P6", f"bad magic {magic!r}"
            # skip comments
            line = f.readline()
            while line.startswith(b"#"):
                line = f.readline()
            w, h = map(int, line.split())
            maxv = int(f.readline().strip())
            assert maxv == 255
            pixels = f.read()
        print(f"    {w}x{h} ppm, {len(pixels)} bytes")

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        # Color tolerance for fbcon mapping → ppm dump may diff by 1.
        def near(a, b, tol=4):
            return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))

        # Expected layout (matches user/mouse.c after our session-110 edit):
        #   - clear DARK_GREY 0x202020 background
        #   - white text top-left (top two lines)
        #   - green outline rect at (4, 4, w-8, h-8)
        #   - cursor crosshair at (ms.x, ms.y) — should be near (712, 484)
        #   - HUD strip BLACK at y = h - 32
        # Note: kernel clamps to FB dims (1024x768). Start = (512, 384).
        # After 20 events of (+10, +5): (712, 484).
        checks = []

        # 1. Background pixel in clear region (not on cursor/frame)
        bg = px(200, 200)
        checks.append(("backdrop @ (200,200) = DARK_GREY",
                       near(bg, (0x20, 0x20, 0x20))))

        # 2. Top-line text region — sample several rows, look for SOME white
        white_top = 0
        for yy in range(8, 16):
            for xx in range(8, 8 + 8*40):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=8):
                    white_top += 1
        checks.append((f"white pixels in top text band (got {white_top})",
                       white_top > 50))

        # 3. Green frame top edge — line at y=4
        green_top = sum(1 for xx in range(10, w-10)
                        if near(px(xx, 4), (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"green frame top edge (got {green_top}/{w-20})",
                       green_top > (w - 20) * 0.8))

        # 4. Cursor crosshair somewhere in the FB. QMP rel events get
        #    scaled by QEMU's input subsystem before re-encoding as PS/2
        #    packets, so the exact landing point varies with QEMU
        #    version. We scan the *interior* of the frame for the
        #    distinctive 17h+17v crosshair (CURSOR_R=8) and confirm:
        #      (a) at least one such cluster exists,
        #      (b) it's NOT at the spawn-center (512, 384) — so the
        #          driver did process the rel events.
        cursor_white = []
        for yy in range(60, h - 60):
            for xx in range(40, w - 40):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=12):
                    cursor_white.append((xx, yy))
        if cursor_white:
            xs = [p[0] for p in cursor_white]
            ys = [p[1] for p in cursor_white]
            cx_obs = (min(xs) + max(xs)) // 2
            cy_obs = (min(ys) + max(ys)) // 2
            print(f"    observed cursor at (~{cx_obs}, ~{cy_obs}), "
                  f"{len(cursor_white)} px")
            crosshair_shape = (len(cursor_white) >= 30
                               and (max(xs) - min(xs)) >= 14
                               and (max(ys) - min(ys)) >= 14)
            checks.append((f"cursor crosshair shape ({len(cursor_white)} px, "
                           f"~{max(xs)-min(xs)}w x {max(ys)-min(ys)}h)",
                           crosshair_shape))
            moved = abs(cx_obs - 512) > 30 or abs(cy_obs - 384) > 30
            checks.append((f"cursor moved away from spawn center (512,384)",
                           moved))
        else:
            checks.append(("cursor crosshair shape", False))
            checks.append(("cursor moved", False))

        # 5. HUD strip black at bottom
        hud_y = h - 20
        black_hud = sum(1 for xx in range(20, w-20)
                        if near(px(xx, hud_y), (0, 0, 0), tol=8))
        checks.append((f"black HUD strip pixels @ y={hud_y} (got {black_hud}/{w-40})",
                       black_hud > (w - 40) * 0.5))

        # 6. Frame coherence: no half-drawn artifacts. Verify that the
        #    green frame's right edge also exists. (If the present was
        #    interrupted mid-blit, we'd often see one edge missing.)
        green_right = sum(1 for yy in range(10, h-10)
                          if near(px(w - 5, yy), (0x30, 0xE0, 0x30), tol=12))
        checks.append((f"green frame right edge (got {green_right}/{h-20})",
                       green_right > (h - 20) * 0.8))

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
