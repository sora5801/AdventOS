#!/usr/bin/env python3
"""
Session 144 smoke test: tablet hotspot compensation.

The kernel applies a -3 px X / +3 px Y offset in mouse_set_absolute
to fix the NE drift between QEMU's visible host cursor and the
guest's click position.  We verify by:

  1. Sending a QMP abs event to logical (target_x, target_y).
  2. Asking wmd to draw something at the kernel cursor position
     — we can't directly do that, so we use the wmedit click
     hit-test as a proxy: launch wmd + wmedit, click a known
     spot, and observe where the wmedit text cursor lands.

Easier in practice: just look at where the kernel reports
mouse position via SYS_MOUSE_POLL.  We don't have a userspace
binary that prints that, so we use an indirect check — boot wmd
WITHOUT --clean so demo windows are visible, position the cursor
at the start-button area, and confirm clicks register where we
expect (i.e. opening the launcher).

For the smoke we just verify the kernel boots cleanly with the
offset applied — interactive verification of "click lands where
I see the cursor" is by hand on user's hardware.

Pixel checks:
  - boot still completes
  - wmd renders normally (top status bar alive)
  - tablet still drives the cursor (clicking the Start button
    opens the launcher, same path the s142 smoke verified)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_hotspot.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s144.log"), "w")
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

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Send abs to (200, 300).  With the new -3/+3 hotspot offset,
        # the kernel cursor will land at (197, 303).  This is still
        # within reach of any decent click target, just shifted.
        # Verify clicks at this position work — we use the Start
        # button (x=0..63 in the bottom-left taskbar) for a simpler
        # test.  Position cursor over Start, click, check launcher.
        # Tablet abs (in QMP) is in 0..32767 over the display.
        target_x, target_y = 32, 768 - 14
        ax = 32767 * target_x // 1023
        ay = 32767 * target_y // 767
        print(f"[+] abs to start-button ({target_x},{target_y})")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ]})
        time.sleep(1.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.4)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.8)

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

        # Launcher popup rendered after the Start click — scan its
        # region for white glyph pixels.
        n_items = 12  # Sh + 11 originals
        TASKBAR_H = 28
        LAUNCH_ITEM_H = 22
        LAUNCH_W = 160
        popup_top = h - TASKBAR_H - n_items * LAUNCH_ITEM_H
        popup_bot = h - TASKBAR_H
        text_glyphs = 0
        for yy in range(popup_top, popup_bot):
            for xx in range(0, LAUNCH_W):
                if 0 <= xx < w and 0 <= yy < h:
                    if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                        text_glyphs += 1
        print(f"   launcher-region white glyph px: {text_glyphs}")

        # wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        # The click landed within the Start button despite the -3/+3
        # offset; launcher opened.  (target=32 - 3 = 29, still inside
        # the 0..63 Start button region.  target_y=754 + 3 = 757,
        # within 740..768 taskbar.)
        checks.append((f"launcher opened ({text_glyphs} px)",
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
