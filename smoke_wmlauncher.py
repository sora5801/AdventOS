#!/usr/bin/env python3
"""
Session 119 smoke test: Start button + launcher popup + spawn via
fork+exec from inside wmd.

Steps:
  1. boot wmd (NO clients running)
  2. screendump A — taskbar exists, Start button visible, no client
     buttons
  3. click Start button → popup appears
  4. screendump B — popup is on screen (white border + items)
  5. click first popup item (wmhello) → wmd fork+execs it
  6. wait for wmhello to register
  7. screendump C — wmhello window and its taskbar button present

Pixel checks:
  A: Start button green-ish (0x205030); no client taskbar button
  B: launcher popup body visible (0x202830 above taskbar)
  C: wmhello title-band blue visible; wmhello's button in taskbar
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4465
SERIAL_PORT = 4466
SHOT_A = os.path.join(ROOT, "shot_launch_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_launch_b.ppm")
SHOT_C = os.path.join(ROOT, "shot_launch_c.ppm")


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


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=4):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def screendump(q, qbuf, path):
    if os.path.exists(path):
        os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def click_qmp(q, qbuf, dx_rel_events, dy_per_event, n_events,
              hold_s=1.3, settle_s=1.0):
    """Move cursor by (dx_rel_events*n, dy_per_event*n), click, release."""
    for i in range(n_events):
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": dx_rel_events}},
            {"type": "rel", "data": {"axis": "y", "value": dy_per_event}},
        ]})
        time.sleep(0.05)
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}},
    ]})
    time.sleep(hold_s)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]})
    time.sleep(settle_s)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-s119.log"), "w")
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
            print("[!] never saw shell prompt"); return 1
        ser.sendall(b"wmd 60\n")     # foreground; we won't run more shell cmds
        time.sleep(3.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # A — initial state. Cursor at (512, 384), no clients yet.
        print("[+] screendump A (initial)")
        if not screendump(q, qbuf, SHOT_A):
            print("[!] screendump A failed"); return 1

        # Click the Start button. Start button center is roughly
        # (32, 754).  Cursor at (512, 384). Delta (-480, +370).
        # 27 events of (-18, +14) → (-486, +378) → (26, 762).
        print("[+] clicking Start")
        click_qmp(q, qbuf, -18, 14, 27)

        # B — popup should be open.
        print("[+] screendump B (popup open)")
        if not screendump(q, qbuf, SHOT_B):
            print("[!] screendump B failed"); return 1

        # Click the first launcher item (wmhello).  Items go from
        # y = h - TASKBAR_H - N_LAUNCH_ITEMS * LAUNCH_ITEM_H - 4
        # = 768 - 28 - 4*22 - 4 = 648.  Item 0: y=648..669, center
        # y=659.  x = 4..164, center x=84.
        # Cursor at (26, 762).  Delta to (84, 659): (+58, -103).
        # 10 events of (+6, -10) → (+60, -100) → (86, 662). Close.
        print("[+] clicking 'wmhello' item")
        click_qmp(q, qbuf, 6, -10, 10)

        # Give wmhello time to fork+exec+register+paint.
        time.sleep(4.0)

        # C — wmhello should be open.
        print("[+] screendump C (wmhello spawned)")
        if not screendump(q, qbuf, SHOT_C):
            print("[!] screendump C failed"); return 1

        aw, ah, ap = read_ppm(SHOT_A)
        bw, bh, bp = read_ppm(SHOT_B)
        cw, ch, cp = read_ppm(SHOT_C)

        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # A: Start button is greenish 0x205030 at center y=754.
        sb_color = sum(1 for xx in range(8, 56)
                       if near(px(ap, aw, xx, 754),
                               (0x20, 0x50, 0x30), tol=15))
        checks.append((f"A: Start button green @ y=754 ({sb_color}/48)",
                       sb_color > 30))

        # A: NO client taskbar button (nothing past the Start
        # button in the taskbar strip).  Sample at x in 200..900,
        # y=753 — should all be the taskbar bg 0x182030.
        no_client = sum(1 for xx in range(200, 900, 8)
                        if near(px(ap, aw, xx, 753),
                                (0x18, 0x20, 0x30), tol=8))
        checks.append((f"A: no client taskbar buttons ({no_client}/88)",
                       no_client > 80))

        # B: launcher popup body — at y=659 (middle of item 0)
        # except where the text is.  Sample column x=150 (right
        # edge of popup body, well past any item text).
        body = sum(1 for yy in range(650, 720)
                   if near(px(bp, bw, 150, yy),
                           (0x20, 0x28, 0x30), tol=10))
        checks.append((f"B: launcher body @ x=150 ({body}/70)",
                       body > 50))

        # B: launcher popup has white text — sample text band of
        # item 0.
        b_text = 0
        for yy in range(655, 666):
            for xx in range(10, 80):
                if near(px(bp, bw, xx, yy), (0xFF, 0xFF, 0xFF), tol=20):
                    b_text += 1
        checks.append((f"B: launcher item text ({b_text})",
                       b_text > 15))

        # C: wmhello window opened.  Default position for slot 4
        # (no other clients) → (340, 360), surface (341, 378+).
        # Blue title band at y=386.
        blue = sum(1 for xx in range(371, 540)
                   if near(px(cp, cw, xx, 386),
                           (0x40, 0x80, 0xE0), tol=12))
        checks.append((f"C: wmhello blue band painted ({blue}/169)",
                       blue > 120))

        # C: wmhello's button in the taskbar (dark slate by
        # default since the launch click closed the popup but
        # didn't focus the new window).
        # First client button starts at x = START_BTN_W + PAD = 68.
        # Width 140 → x=68..207.  Sample center x=138, y=753.
        tb_btn = px(cp, cw, 138, 753)
        checks.append((f"C: wmhello taskbar button @ (138,753) = {tb_btn}",
                       near(tb_btn, (0x30, 0x38, 0x48), tol=10)
                       or near(tb_btn, (0x30, 0xE0, 0xE0), tol=15)))

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
