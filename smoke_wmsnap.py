#!/usr/bin/env python3
"""
Session 138 smoke test: snap-to-edge + drop shadows.

The drop-shadow gradient is fully visual — straightforward to
verify by sampling the strip of pixels right of any window's
right edge.  The shadow palette is a fixed 6-step table in
paint_window (sh[]); we read it back directly from the FB and
match values within tolerance.

Snap-to-edge is mouse-driven and hard to drive deterministically
through QEMU's PS/2 rel-event stream (cursor accel/scaling drift
across QEMU versions, see comments in earlier smokes).  We send
a best-effort drag burst and report the post-drag wmedit position
as informational, but only fail when the WM itself stops painting
(crash/freeze).  Interactive snap behaviour is verified by hand:
launch wmd, drag any window title bar to a screen edge, release.

Pixel checks:
  - Shadow right-strip: 6 darkish pixels at x=window_right+i, y=mid
  - Shadow bottom-strip: 6 darkish pixels at y=window_bottom+i, x=mid
  - wmd top status bar still alive after a chaotic drag burst
  - wmedit title bar still visible somewhere on screen
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT_A = os.path.join(ROOT, "shot_wmsnap_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_wmsnap_b.ppm")


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


def pxer(w, pixels):
    def px(x, y):
        i = (y * w + x) * 3
        return pixels[i], pixels[i+1], pixels[i+2]
    return px


def near(a, b, tol=15):
    return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))


def is_darkish(c, max_b=0x60):
    return c[0] <= max_b and c[1] <= max_b and c[2] <= max_b


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s138.log"), "w")
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
            print("[!] no shell prompt"); return 1
        ser.sendall(b"wmd 60 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/smk 50\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # ---- (a) shadow check at rest -----------------------------
        print("[+] screendump A (shadow check)")
        if os.path.exists(SHOT_A): os.remove(SHOT_A)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_A, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_A)
                                          or os.path.getsize(SHOT_A) < 100):
            time.sleep(0.1)
        w, h, pixels = read_ppm(SHOT_A)
        px = pxer(w, pixels)

        # wmedit outer (100, 200, 644, 420).  Right edge x=744.
        # Shadow strips at x=744..749, offset by y+(2..7).  Sample
        # at y=400 (mid-body, no chrome).
        shadow_pixels = [px(744 + i, 400) for i in range(6)]
        n_right = sum(1 for c in shadow_pixels if is_darkish(c))
        print(f"   right-shadow @ x=744..749, y=400:")
        for i, c in enumerate(shadow_pixels):
            print(f"     [{i}] {c}")

        # Bottom shadow: y=620..625 should be darkish, at x=400
        # (middle of window width).
        bottom_shadow = [px(400, 620 + i) for i in range(6)]
        n_bottom = sum(1 for c in bottom_shadow if is_darkish(c))
        print(f"   bottom-shadow @ x=400, y=620..625:")
        for i, c in enumerate(bottom_shadow):
            print(f"     [{i}] {c}")

        # ---- (b) drag burst — best-effort snap attempt -----------
        # We send a chaotic burst to verify the drag/snap code path
        # doesn't crash.  Don't fail on the snap outcome since PS/2
        # rel-mouse cursor scaling is unreliable.
        print("[+] mouse burst (best-effort snap attempt)")
        for _ in range(30):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -30}},
                {"type": "rel", "data": {"axis": "y", "value": -30}},
            ]})
            time.sleep(0.04)
        time.sleep(0.4)
        for _ in range(10):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 25}},
                {"type": "rel", "data": {"axis": "y", "value": 22}},
            ]})
            time.sleep(0.05)
        time.sleep(0.6)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.5)
        for _ in range(25):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "y", "value": -30}},
            ]})
            time.sleep(0.04)
        time.sleep(0.6)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.5)

        print("[+] screendump B (post-drag survival check)")
        if os.path.exists(SHOT_B): os.remove(SHOT_B)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_B, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_B)
                                          or os.path.getsize(SHOT_B) < 100):
            time.sleep(0.1)
        w, h, pixels = read_ppm(SHOT_B)
        px = pxer(w, pixels)

        # wmd top status bar — dark band y=6 at any x in [50, w-50].
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        # wmedit still has SOME title bar visible (a row of either
        # CYAN/dark-grey/blue spanning >= 200 pixels at some y in
        # [18, 600]).  We look for any of the three colour signatures
        # since after the drag the window might be focused or not,
        # snapped or not.
        title_candidates = [
            (0x30, 0xE0, 0xE0),   # cyan (focused frame_color)
            (0x40, 0x40, 0x40),   # dark grey (unfocused outer)
            (0x40, 0x80, 0xE0),   # blue (wmedit inner header)
        ]
        title_seen = False
        best_y = -1
        best_run = 0
        for yy in range(18, 700, 2):
            for tc in title_candidates:
                run = sum(1 for xx in range(20, w-20, 4)
                          if near(px(xx, yy), tc, tol=20))
                if run > best_run:
                    best_run, best_y = run, yy
                if run > 50:    # 50 of (w-40)/4 samples = ~200 px
                    title_seen = True
                    break
        print(f"   strongest title-bar row: y={best_y}, run={best_run}")

        checks = []
        checks.append((f"right shadow strip darkish ({n_right}/6)",
                       n_right >= 4))
        checks.append((f"bottom shadow strip darkish ({n_bottom}/6)",
                       n_bottom >= 4))
        checks.append((f"wmd status bar alive ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))
        checks.append(("wmedit title bar still painted",
                       title_seen))

        print("\n=== pixel checks ===")
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
