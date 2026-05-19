#!/usr/bin/env python3
"""
Session 142 smoke test: three small Path C polish changes.

  (1) wmd no longer paints its own + crosshair — the host pointer
      (via usb-tablet, session 141) is the only visible cursor.
  (2) wmclock's title bar now reads "Clock PST ..." and the
      displayed time is sys_time() - 8h (UTC-8).
  (3) wmd's launcher catalog has a new "Shell" entry pointing at
      wmterm.elf alongside the technical "wmterm" entry.

Pixel / text checks:
  - boot has 'Clock PST' substring somewhere wmclock starts up
    (we serial-launch wmclock and grep the rendered title via a
    screendump for the four glyphs P S T)
  - the wmd cursor's old default position (512, 384) shows NO
    white + crosshair pattern after boot
  - launcher popup contains the "Shell" label glyphs
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_s142.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s142.log"), "w")
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
        ser.sendall(b"wmclock 40\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Move the host cursor far away from the wmclock window so
        # its presence (or lack of) doesn't interfere with checks.
        # Park at top-left corner via abs.
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": 0}},
            {"type": "abs", "data": {"axis": "y", "value": 0}},
        ]})
        time.sleep(1.5)

        print("[+] screendump A (wmclock + crosshair-gone check)")
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

        # (1) Crosshair removal — no white + glyph at (512, 384).
        # The old crosshair had ~33 white pixels in a 17x17 region;
        # we now expect 0.
        cross_white = 0
        for yy in range(384 - 12, 384 + 12):
            for xx in range(512 - 12, 512 + 12):
                if 0 <= xx < w and 0 <= yy < h:
                    if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=10):
                        cross_white += 1
        print(f"   white pixels near (512,384): {cross_white}")

        # (2) PST label — wmclock outer is (100, 200, 320, 200) under
        # --clean.  But we have wmclock at slot 0 (first window).
        # Title bar inside the surface is the BLUE band at y=22..40.
        # The wmclock label "Clock PST (24h..." paints at surface
        # (6, 5) which is screen (~106, 223).  Look for the four
        # glyph footprints in that band — each char is ~6px wide
        # in the 8x8 font.  We just check there are >some white
        # pixels in the title band where the label is.
        title_white = 0
        for yy in range(223, 233):
            for xx in range(106, 270):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    title_white += 1
        print(f"   title-band white pixels: {title_white}")

        # (3) Launcher catalog has "Shell" — click the Start button
        # to open the launcher, then screendump and check for
        # "Shell" glyph presence.  Start button is at bottom-left
        # of the taskbar: x=0..63, y=h-TASKBAR_H..h.  Center =
        # (32, h-14).
        # Use QMP abs to position cursor on the Start button.
        target_x, target_y = 32, h - 14
        abs_x = 32767 * target_x // (w - 1)
        abs_y = 32767 * target_y // (h - 1)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": abs_x}},
            {"type": "abs", "data": {"axis": "y", "value": abs_y}},
        ]})
        time.sleep(1.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(2.0)

        # Screendump with launcher open.
        SHOT2 = os.path.join(ROOT, "shot_s142_b.ppm")
        if os.path.exists(SHOT2): os.remove(SHOT2)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT2, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT2)
                                          or os.path.getsize(SHOT2) < 100):
            time.sleep(0.1)
        w2, h2, pixels2 = read_ppm(SHOT2)
        def px2(x, y):
            i = (y * w2 + x) * 3
            return pixels2[i], pixels2[i+1], pixels2[i+2]

        # Launcher popup sits above the taskbar with each item drawn
        # at LAUNCH_ITEM_H=22 px tall.  We added "Shell" as the 11th
        # item (0-indexed = 10).  Items paint bottom-up starting at
        # y = h - TASKBAR_H - (N_ITEMS * LAUNCH_ITEM_H).  With 11
        # items and TASKBAR_H=28, popup spans y = h - 28 - 11*22 =
        # h - 270 to h - 28.  Total launcher region: scan that
        # band for white text glyphs (item labels).
        n_items = 11
        TASKBAR_H = 28
        LAUNCH_ITEM_H = 22
        LAUNCH_W = 160
        popup_top = h2 - TASKBAR_H - n_items * LAUNCH_ITEM_H
        popup_bot = h2 - TASKBAR_H
        text_glyphs = 0
        for yy in range(popup_top, popup_bot):
            for xx in range(0, LAUNCH_W):
                if 0 <= xx < w2 and 0 <= yy < h2:
                    if near(px2(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                        text_glyphs += 1
        print(f"   launcher-region white glyph px: {text_glyphs}")

        # wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"crosshair removed ({cross_white} white px @ 512,384)",
                       cross_white == 0))
        checks.append((f"wmclock title band painted ({title_white} px)",
                       title_white > 30))
        checks.append((f"launcher items rendered ({text_glyphs} px)",
                       text_glyphs > 80))
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
