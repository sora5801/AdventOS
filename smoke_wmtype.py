#!/usr/bin/env python3
"""
Session 114 smoke test: keyboard events to focused client.

Boot QEMU, start wmd, start wmtype, click into the wmtype window to
get focus, type some characters via the in-guest USB-HID keyboard
(actually via QMP `send-key`), screendump, verify:

  - wmtype's title bar reflects "click to type" (focus established)
  - the typed text appears rendered in the 8x8 font inside the
    window
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4455
SERIAL_PORT = 4456
SHOT_PPM = os.path.join(ROOT, "shot_wmtype.ppm")


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
    log = open(os.path.join(ROOT, "qemu-s114.log"), "w")
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
        print("[+] launching wmtype 30")
        ser.sendall(b"wmtype 30\n")
        time.sleep(2.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmtype window appears at wmd slot 4 (after the 4 demo
        # windows) at (340, 360); its content surface is 320x200
        # but wmd's wmhello layout uses (220+4, 140+18+2) = 224x160
        # for the *outer* size.  wmtype is bigger: surface 320x200
        # → outer 324x220.  Content at (sx, sy) = (341, 378).
        # The new window position is the same deterministic
        # (100+slot*60, 200+slot*40) = (340, 360).

        # Move cursor onto the wmtype TITLE BAR to click-focus it
        # (cursor at title bar = focused, but no drag because
        # clicking on title would START a drag; click on CONTENT
        # focuses without drag).
        # Cursor starts at (512, 384).  Click content area ~(450, 450).
        # delta: (-62, +66).  7 events of (-9, +10) -> (449, 454).
        print("[+] moving cursor into wmtype content (~450, 450)")
        for i in range(7):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -9}},
                {"type": "rel", "data": {"axis": "y", "value": 10}},
            ]})
            qbuf = b""
            time.sleep(0.06)
        time.sleep(0.8)

        # Click to give wmtype focus (wmd's click-focus model).
        print("[+] click to focus wmtype")
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.0)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}},
        ]})
        qbuf = b""
        time.sleep(1.0)

        # Now type "hey" — lowercase ASCII from QMP qcode events.
        # Three letters are enough to verify text rendering across a
        # range of glyph shapes, and pacing each at 0.5s avoids the
        # USB-HID polling layer dropping any (observed: 0.2s drops
        # most of a 5-key burst, 0.5s reliably delivers all of them).
        print("[+] typing 'hey'")
        for k in ["h", "e", "y"]:
            qmp_cmd(q, qbuf, "send-key", {"keys": [{"type": "qcode", "data": k}]})
            qbuf = b""
            time.sleep(0.5)
        time.sleep(1.5)

        # Take screendump.
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

        # wmtype window outer at (340, 360) size 324x220.  Content
        # surface at (341, 378)..(660, 577).  status bar in surface
        # is y=0..17 (screen y=378..395).
        sx, sy = 341, 378
        sw, sh = 320, 200

        checks = []

        # 1. wmtype focused status bar (blue 0x4080E0) — confirms
        #    the click click-focused wmtype.
        focus_blue = sum(1 for xx in range(sx + 10, sx + sw - 30)
                         if near(px(xx, sy + 8),
                                 (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"wmtype focused title @ y={sy+8} ({focus_blue}/280)",
                       focus_blue > 200))

        # 2. Close-button red square at (sx + sw - 18, sy + 2, 14, 14).
        close_red = sum(1 for xx in range(sx + sw - 16, sx + sw - 4)
                        if near(px(xx, sy + 8),
                                (0xE0, 0x30, 0x30), tol=15))
        checks.append((f"close X red @ y={sy+8} ({close_red}/12)",
                       close_red > 8))

        # 3. Body background — focused color 0x101820 sampled at a
        #    clean point (no text, no caret).  (sx+200, sy+150) is
        #    below the text area used by HELLO.
        bg = px(sx + 200, sy + 150)
        checks.append((f"focused bg @ ({sx+200},{sy+150}) = {bg}",
                       near(bg, (0x10, 0x18, 0x20), tol=12)))

        # 4. White pixels in the text-rendering region — 3 letters
        #    'h','e','y' rendered with 8x8 font, GFX_WHITE, transparent
        #    bg.  Scan a wider band so we're robust to vertical
        #    placement drift.
        white_text = 0
        for yy in range(sy + 22, sy + 36):
            for xx in range(sx + 4, sx + 4 + 3 * 8 + 4):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    white_text += 1
        checks.append((f"text white pixels in 'hey' band ({white_text})",
                       white_text > 20))

        # 5. Footer "chars=3" — gray (0x808080) text near bottom.
        footer_gray = 0
        for yy in range(sy + sh - 14, sy + sh - 6):
            for xx in range(sx + 4, sx + 100):
                if near(px(xx, yy), (0x80, 0x80, 0x80), tol=20):
                    footer_gray += 1
        checks.append((f"footer 'chars=3' gray pixels ({footer_gray})",
                       footer_gray > 15))

        # 6. wmd top status bar still painted (overall pipeline alive).
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
