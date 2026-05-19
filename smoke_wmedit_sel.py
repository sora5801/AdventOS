#!/usr/bin/env python3
"""
Session 140 smoke test: wmedit caret + drag-selection.

Two visible polish features:
  - caret is now a 2-px vertical white line that blinks (was a
    full-cell block in session 137)
  - mouse drag selects a range; selected bytes get a blue
    (0x305078) background fill under each glyph

We boot wmd + wmedit, focus by clicking, type "abcdef", then
perform a drag from the left edge of the text body across a few
columns to select roughly the first 3 chars.  Three screendumps
spaced 300 ms apart capture the caret across at least one blink
phase.

Pixel checks:
  - typed text is rendered (light-green glyphs)
  - selection highlight pixels exist (blue ~0x305078) in the
    expected band
  - caret white pixel (2-px vertical) appears in at least one
    of the three samples after typing (informational on the
    selection sample since the caret is suppressed while a
    selection is active)
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOTS = [os.path.join(ROOT, f"shot_wmedit_sel_{i}.ppm") for i in range(3)]


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

    log = open(os.path.join(ROOT, "qemu-s140.log"), "w")
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
        ser.sendall(b"wmedit /tmp/sel 50\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmedit outer at slot 0 (--clean): (100, 200, 644, 420).
        # Body grid starts at surface (GRID_X=6, HDR_H+4=22).  Click
        # somewhere safely inside body — surface (50, 60) → screen
        # (150, 260).  From cursor start (512, 384) delta = (-362,
        # -124).  14 events of (-26, -9) = (-364, -126).
        print("[+] click into wmedit body for focus")
        for _ in range(14):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -26}},
                {"type": "rel", "data": {"axis": "y", "value": -9}},
            ]})
            time.sleep(0.04)
        time.sleep(0.4)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "abcdef".
        print("[+] type abcdef")
        for k in ["a", "b", "c", "d", "e", "f"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.2)
        time.sleep(0.8)

        # Take a screendump now (caret should be visible somewhere).
        print("[+] screendump A (caret after typing)")
        if os.path.exists(SHOTS[0]): os.remove(SHOTS[0])
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOTS[0], "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOTS[0])
                                          or os.path.getsize(SHOTS[0]) < 100):
            time.sleep(0.1)

        # Reposition cursor near the start of the typed text and
        # do a small drag selection.  Surface (6, 22) is the body's
        # top-left; the typed text begins there.  Screen ≈ (106,
        # 222).  We're currently at ~(150, 260) so delta to (106,
        # 230) is (-44, -30).  Use 6 events of (-7, -5).  Then
        # press, drag right ~32 px (4 cells), release.
        print("[+] reposition + drag select")
        for _ in range(6):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -7}},
                {"type": "rel", "data": {"axis": "y", "value": -5}},
            ]})
            time.sleep(0.04)
        time.sleep(0.4)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        for _ in range(6):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 6}},
            ]})
            time.sleep(0.05)
        time.sleep(0.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(0.6)

        # Take two more screendumps to capture selection (and
        # provide caret-blink samples).
        for i in (1, 2):
            print(f"[+] screendump {chr(ord('A')+i)} (after drag, sample {i})")
            if os.path.exists(SHOTS[i]): os.remove(SHOTS[i])
            qmp_cmd(q, qbuf, "screendump",
                    {"filename": SHOTS[i], "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (
                not os.path.exists(SHOTS[i])
                or os.path.getsize(SHOTS[i]) < 100):
                time.sleep(0.1)
            time.sleep(0.4)

        # ---- analyse ----
        # wmedit outer (100, 200, 644, 420).  Body surface y=22
        # -> screen y=222 (with HDR_H=18 + 4-pad).  GRID_X=6 ->
        # screen x=106.  CELL_W=8, LINE_H=10.
        w, h, pixels0 = read_ppm(SHOTS[0])
        def pxer(pixels):
            def px(x, y):
                i = (y * w + x) * 3
                return pixels[i], pixels[i+1], pixels[i+2]
            return px

        # (1) Typed text "abcdef" — 6 light-green glyphs.  The
        # glyph has data on some pixel rows but not all (8x8 bitmap
        # font), so scan a wide 2-D band rather than single rows.
        px = pxer(pixels0)
        green_pixels = 0
        for yy in range(222, 580):
            for xx in range(106, 350):
                if near(px(xx, yy), (0xC0, 0xE0, 0xC0), tol=25):
                    green_pixels += 1

        # (2) Caret white-line check — across the 3 screendumps,
        # look for a 2-px-wide vertical white run somewhere in the
        # text band.  Caret is 2 px wide × LINE_H-1=9 tall, white
        # 0xFFFFFF.  Scan all rows where caret might be.
        caret_seen = False
        for shot in SHOTS:
            try:
                _, _, p = read_ppm(shot)
            except Exception:
                continue
            px2 = pxer(p)
            for yy in range(222, 580, 10):
                for xx in range(106, 400):
                    # 2-px wide white at (xx, yy)+(xx+1, yy)?
                    if (near(px2(xx, yy + 2), (0xFF, 0xFF, 0xFF), tol=10)
                        and near(px2(xx + 1, yy + 2), (0xFF, 0xFF, 0xFF), tol=10)
                        and not near(px2(xx + 4, yy + 2), (0xFF, 0xFF, 0xFF), tol=10)):
                        caret_seen = True
                        break
                if caret_seen: break
            if caret_seen: break

        # (3) Selection highlight — blue ~0x305078 somewhere in
        # the body band across either sample 1 or 2.
        sel_seen = 0
        for shot in SHOTS[1:]:
            _, _, p = read_ppm(shot)
            px3 = pxer(p)
            count = 0
            for yy in range(222, 580):
                for xx in range(106, 350):
                    if near(px3(xx, yy), (0x30, 0x50, 0x78), tol=20):
                        count += 1
            sel_seen = max(sel_seen, count)
        print(f"   selection-blue pixels (max across drag shots): {sel_seen}")

        # (4) wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"typed text 'abcdef' rendered ({green_pixels} green px)",
                       green_pixels > 30))
        checks.append((f"2-px vertical caret seen", caret_seen))
        checks.append((f"selection highlight visible ({sel_seen} blue px)",
                       sel_seen > 20))
        checks.append((f"wmd status bar alive ({sb}/{w-100})",
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
