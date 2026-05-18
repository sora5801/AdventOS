#!/usr/bin/env python3
"""
Session 139 smoke test: wmcalc calculator.

Boots wmd + wmcalc, types a short expression on the keyboard
("123+456="), screendumps, and checks:

  - wmcalc window painted (cyan outer frame when focused, blue
    inner title bar)
  - display panel painted in deep blue at the top of the body
  - button grid has the expected colour palette (red C, green =,
    grey/slate digits/ops)
  - wmd top status bar still alive

We don't try to assert "579 appears on the display" by OCR.  We
verify the calculator is visible and the click/key path doesn't
crash.  Manual interactive verification via -display gtk confirms
the actual arithmetic.
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502
SHOT = os.path.join(ROOT, "shot_wmcalc.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s139.log"), "w")
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
        ser.sendall(b"wmd 40 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmcalc 30\n")
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmcalc at slot 0 (--clean): outer (100, 200, 220, 320).
        # Click the body to give it focus.  Cursor starts at FB
        # centre (512, 384).  Target (150, 240) — somewhere on the
        # button grid.  Delta from (512, 384) to (150, 240) =
        # (-362, -144).  Use 14 events of (-26, -11) = (-364, -154).
        print("[+] click into wmcalc body for focus")
        for _ in range(14):
            qmp_cmd(q, qbuf, "input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -26}},
                {"type": "rel", "data": {"axis": "y", "value": -11}},
            ]})
            time.sleep(0.04)
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.5)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "12+34=".  Using qemu send-key qcodes.
        print("[+] type 12+34=")
        for k in ["1", "2", "kp_add", "3", "4", "kp_equals"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.3)
        time.sleep(0.8)

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

        # 1. Outer wmd frame around wmcalc — cyan title at y=209.
        title_cyan = sum(1 for xx in range(110, 310)
                         if near(px(xx, 209), (0x30, 0xE0, 0xE0), tol=15))

        # 2. wmcalc's own header band — blue 0x4080E0 at y=222.
        title_blue = sum(1 for xx in range(110, 310)
                         if near(px(xx, 222), (0x40, 0x80, 0xE0), tol=15))

        # 3. Display panel — deep blue 0x081018 at (108..212, 244..)
        #    wmcalc outer y=218; surface y=DISP_Y=24 -> screen 242.
        #    Inside the panel at (200, 250): bg should be 0x081018.
        disp_bg = px(200, 250)
        print(f"   display bg @ (200,250) = {disp_bg}")

        # 4. The red Clear button at row 0, col 0.  Surface coords:
        #    GRID_X=8, GRID_Y=DISP_Y+DISP_H+6 = 24+44+6 = 74.  Cell
        #    centre = (8+24, 74+22) = (32, 96).  Screen coords:
        #    (100+32, 218+96) = (132, 314).  Sample to find a red
        #    button.
        clear_btn = px(132, 314)
        print(f"   clear btn centre = {clear_btn}")

        # 5. The green = button at row 4, col 2.  Surface coords:
        #    GRID_X + 2*(BTN_W+GAP) = 8 + 2*52 = 112.  GRID_Y +
        #    4*(BTN_H+GAP) = 74 + 4*48 = 266.  Cell centre x =
        #    112+24=136, y=266+22=288.  Screen: (100+136, 218+288)
        #    = (236, 506).
        eq_btn = px(236, 506)
        print(f"   = btn centre = {eq_btn}")

        # 6. wmd top status bar still alive.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"wmd-side title cyan @ y=209 ({title_cyan}/200)",
                       title_cyan > 100))
        checks.append((f"wmcalc header blue @ y=222 ({title_blue}/200)",
                       title_blue > 100))
        checks.append((f"display panel deep-blue {disp_bg}",
                       near(disp_bg, (0x08, 0x10, 0x18), tol=15)))
        checks.append((f"Clear btn red-ish {clear_btn}",
                       clear_btn[0] > 0x80 and clear_btn[0] > clear_btn[1] + 30
                       and clear_btn[0] > clear_btn[2] + 30))
        checks.append((f"= btn green-ish {eq_btn}",
                       eq_btn[1] > 0x80 and eq_btn[1] > eq_btn[0] + 20
                       and eq_btn[1] > eq_btn[2] + 30))
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
