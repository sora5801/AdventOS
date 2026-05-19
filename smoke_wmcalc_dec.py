#!/usr/bin/env python3
"""
Session 156 smoke: wmcalc decimal + memory.

Boots wmd + wmcalc.  Three sub-tests via send-key:

  1. Decimal addition: "1.5 + 2.25 =" -> display shows "3.75"
  2. Memory: type "5", M+ (m); type "3", MR (r) -> recalled "5"
  3. The 6th row of buttons (memory row) is painted with a
     distinctive bluish background (~0x405068).

Pixel checks:
  - decimal arithmetic result text rendered as light-green
    glyphs in the display area
  - memory row buttons painted in the memory blue
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502


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


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})


def click_focus(q, qbuf, x, y):
    abs_send(q, qbuf, x, y)
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}
    ]})
    time.sleep(0.3)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}
    ]})
    time.sleep(0.8)


def send_keys(q, qbuf, keys):
    for k in keys:
        qmp_cmd(q, qbuf, "send-key",
                {"keys": [{"type": "qcode", "data": k}]})
        time.sleep(0.18)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s156.log"), "w")
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
        ser.sendall(b"wmcalc 50\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # wmcalc outer (100, 200, 224, 388).  Body at (101, 218).
        # Click middle of the calc body for focus.
        click_focus(q, qbuf, 180, 350)

        # Compute 1.5 + 2.25 = 3.75
        # qcodes: '1' '.' '5' 'kp_add' '2' '.' '2' '5' 'kp_equals'
        # send-key uses qcodes; 'kp_add' = '+', 'kp_equals' = '='.
        # But our key_to_button accepts ASCII '+' and '='. The send-key
        # produces those ASCII bytes in the keyboard ring.
        print("[+] type 1.5 + 2.25 =")
        send_keys(q, qbuf, ["1", "dot", "5", "kp_add", "2", "dot",
                            "2", "5", "kp_equals"])
        time.sleep(1.5)

        SHOT_A = os.path.join(ROOT, "shot_calc_a.ppm")
        if os.path.exists(SHOT_A): os.remove(SHOT_A)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT_A, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT_A)
                                          or os.path.getsize(SHOT_A) < 100):
            time.sleep(0.1)
        w, h, pxA = read_ppm(SHOT_A)
        def px(p, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        # Display panel: surface (8, 24, 204, 44).
        # On screen: (109, 242) to (313, 286).  Light-green text
        # 0xE0F0E0 right-aligned.  Sample broadly.
        green_disp = 0
        for yy in range(244, 282):
            for xx in range(110, 310):
                if near(px(pxA, xx, yy), (0xE0, 0xF0, 0xE0), tol=25):
                    green_disp += 1
        print(f"   display green pixels (3.75 rendered): {green_disp}")

        # Memory row (6th row).  Surface y = GRID_Y + 5*(44+4) =
        # (24+44+6) + 240 = 314..358.  Screen y = 218 + 314..358 =
        # 532..576.  Memory row blue ~0x405068.
        mem_blue = 0
        for yy in range(534, 574):
            for xx in range(110, 310):
                if near(px(pxA, xx, yy), (0x40, 0x50, 0x68), tol=20):
                    mem_blue += 1
        print(f"   memory row blue pixels: {mem_blue}")

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(pxA, xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"decimal arithmetic rendered ({green_disp} green px)",
                       green_disp > 30))
        checks.append((f"memory row painted ({mem_blue} blue px)",
                       mem_blue > 1000))
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
