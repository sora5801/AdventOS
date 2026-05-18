#!/usr/bin/env python3
"""
Session 137 smoke test: wmedit text editor.

Boots wmd + wmedit pointing at a fresh tmp path.  Focuses
wmedit via mouse, types a short line, checks that the text
appears in the grid + the status bar shows the cursor position
and the unsaved-changes marker.

Pixel checks (5):
  - wmedit title bar painted (focused-blue or unfocused-grey)
  - typed text appears in the grid as light-green characters
  - status bar at the bottom painted in dark slate
  - status-bar text (path / position) rendered
  - wmd top status bar still alive (compositor not stuck)
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_wmedit.ppm")


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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s137.log"), "w")
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
        if not ok: return 1
        ser.sendall(b"wmd 40 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/smk 30\n")
        time.sleep(3.0)
        # Park the shell so it doesn't compete for the keyboard.
        # (wmedit isn't backgrounded — it'll run for 30s in fg, which
        # naturally blocks the shell.)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmedit at slot 0 (--clean): outer (100, 200, 644, 420).
        # Click in the text body to give it focus.  Center of body
        # would be (~422, ~410).  Cursor starts at FB centre
        # (512, 384).  Delta to (300, 400) = (-212, +16).  10
        # events of (-21, +2) → (-210, +20) → (302, 404).
        print("[+] click into wmedit body")
        for i in range(10):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -21}},
                {"type": "rel", "data": {"axis": "y", "value": 2}},
            ]})
            time.sleep(0.05)
        time.sleep(0.4)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(1.0)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "abc".
        print("[+] type 'abc'")
        for key in ["a", "b", "c"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": key}]})
            time.sleep(0.5)
        time.sleep(1.0)

        print("[+] screendump")
        if os.path.exists(SHOT): os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)

        with open(SHOT, "rb") as f:
            f.readline()
            line = f.readline()
            while line.startswith(b"#"): line = f.readline()
            w, h = map(int, line.split())
            int(f.readline().strip())
            pixels = f.read()

        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]
        def near(a, b, tol=15):
            return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))

        # wmedit outer (100, 200, 644, 420).  Title bar y=200..217;
        # body starts at y=218.  GRID_Y inside surface = HDR_H+4 =
        # 22 → screen y=240.  Footer at y=600..613 (WIN_H-FOOTER_H
        # = 386 inside surface = screen 604).
        checks = []

        # 1. wmd's outer title bar is CYAN (wmhello-style
        #    frame_color) when wmedit is focused.  wmedit's own
        #    inner header band sits below that and is the blue
        #    0x4080E0 — sample both: cyan at y=209, blue at y=222.
        title_cyan = sum(1 for xx in range(110, 700)
                         if near(px(xx, 209), (0x30, 0xE0, 0xE0), tol=15))
        title_blue = sum(1 for xx in range(110, 700)
                         if near(px(xx, 222), (0x40, 0x80, 0xE0), tol=15))
        checks.append((f"wmd-side title cyan @ y=209 ({title_cyan}/590)",
                       title_cyan > 300))
        checks.append((f"wmedit header blue @ y=222 ({title_blue}/590)",
                       title_blue > 300))

        # 2. Typed "abc" — 3 light-green glyphs at grid origin
        #    (107, 240).  Scan a band y=240..251 for green text.
        green_text = 0
        for yy in range(240, 252):
            for xx in range(106, 140):
                if near(px(xx, yy), (0xC0, 0xE0, 0xC0), tol=25):
                    green_text += 1
        print(f"   green text in 'abc' band: {green_text}")
        checks.append((f"'abc' rendered ({green_text} green px)",
                       green_text > 10))

        # 3. Status-bar background — dark slate 0x202830 at the
        #    bottom of the window.  Surface y=386 (FOOTER y) ->
        #    screen y=604.  Sample at far-right where there's no
        #    text.
        status_bg = px(600, 612)
        checks.append((f"status bar bg @ (600,612) = {status_bg}",
                       near(status_bg, (0x20, 0x28, 0x30), tol=10)))

        # 4. Status bar has text — sample the band y=604..615 for
        #    any non-bg pixel (path text, [*] marker, etc.).
        sb_text = 0
        for yy in range(604, 614):
            for xx in range(106, 500):
                c = px(xx, yy)
                if not near(c, (0x20, 0x28, 0x30), tol=10):
                    sb_text += 1
        checks.append((f"status bar text rendered ({sb_text})",
                       sb_text > 30))

        # 5. wmd top status bar alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))
        checks.append((f"wmd status bar ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))

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
