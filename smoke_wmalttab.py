#!/usr/bin/env python3
"""
Session 135 smoke test: Alt-Tab cycles focus through client
windows.

Boots wmd --clean (no demo windows), launches two `wmhello`
instances back-to-back (so wmd's client list has exactly two
client slots), takes a baseline screendump, then sends Alt+Tab
via QMP send-key, then a second screendump.

After the Alt+Tab the second taskbar button should be the
focused one (highlighted in the window's frame_color = CYAN).
Before, the most-recently-clicked window had focus — but since
neither window was clicked in the smoke, baseline focus is the
default (-1, no focused button highlighted in CYAN).
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4497
SERIAL_PORT = 4498
SHOT_A = os.path.join(ROOT, "shot_alttab_a.ppm")
SHOT_B = os.path.join(ROOT, "shot_alttab_b.ppm")


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


def screendump(q, qbuf, path):
    if os.path.exists(path): os.remove(path)
    qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
    deadline = time.time() + 5
    while time.time() < deadline and (not os.path.exists(path)
                                      or os.path.getsize(path) < 100):
        time.sleep(0.1)
    return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=12):
    return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s135.log"), "w")
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
        ser.sendall(b"wmhello 35 &\n")
        time.sleep(2.5)
        ser.sendall(b"wmhello 35 &\n")
        time.sleep(2.5)
        # Park the shell in sys_sleep_ms so it stops draining the
        # keyboard ring (otherwise it competes with wmd for the
        # Alt+Tab keystroke and we get a race).
        ser.sendall(b"sleep 30\n")
        time.sleep(1.5)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # First wmhello at slot 0 (--clean): outer (100, 200, 224, 158)
        # Second wmhello at slot 1: outer (160, 240, 224, 158)
        # Taskbar buttons (after START_BTN_W = 64):
        #   button 0: x = 64+4..64+4+140 = 68..207, y = 744..763
        #     centre: (138, 753)
        #   button 1: x = 68+140+4 = 212..212+140 = 212..351, y = 744..763
        #     centre: (282, 753)

        # A: baseline.  Neither taskbar button is focused yet
        # (default focused = -1; both windows are CLIENT but neither
        # was clicked).  Both buttons show unfocused dark slate.
        print("[+] screendump A (baseline, two wmhello, no focus)")
        if not screendump(q, qbuf, SHOT_A): return 1

        # Send Alt+Tab.  QMP `send-key` supports modifier+key pairs.
        print("[+] Alt+Tab")
        qmp_cmd(q, qbuf, "send-key", {
            "keys": [
                {"type": "qcode", "data": "alt"},
                {"type": "qcode", "data": "tab"},
            ]
        })
        # Give wmd a tick or two to process the keystroke.
        time.sleep(2.0)

        print("[+] screendump B (after Alt+Tab)")
        if not screendump(q, qbuf, SHOT_B): return 1

        aw, ah, ap = read_ppm(SHOT_A)
        bw, bh, bp = read_ppm(SHOT_B)
        def px(p, w, x, y):
            i = (y * w + x) * 3
            return p[i], p[i+1], p[i+2]

        checks = []

        # 1. A: button 0 = dark slate (unfocused).
        a_b0 = px(ap, aw, 138, 753)
        checks.append((f"A: button 0 unfocused = {a_b0}",
                       near(a_b0, (0x30, 0x38, 0x48), tol=10)))

        # 2. A: button 1 = dark slate (also unfocused).
        a_b1 = px(ap, aw, 282, 753)
        checks.append((f"A: button 1 unfocused = {a_b1}",
                       near(a_b1, (0x30, 0x38, 0x48), tol=10)))

        # 3. B: after Alt+Tab, ONE of the two buttons is CYAN.
        b_b0 = px(bp, bw, 138, 753)
        b_b1 = px(bp, bw, 282, 753)
        cyan_b0 = near(b_b0, (0x30, 0xE0, 0xE0), tol=15)
        cyan_b1 = near(b_b1, (0x30, 0xE0, 0xE0), tol=15)
        print(f"   B: button 0 = {b_b0}  cyan={cyan_b0}")
        print(f"   B: button 1 = {b_b1}  cyan={cyan_b1}")
        checks.append((f"B: Alt+Tab focused one window (b0_cyan={cyan_b0}, b1_cyan={cyan_b1})",
                       cyan_b0 or cyan_b1))

        # 4. B: wmd status bar still alive.
        sb = sum(1 for xx in range(50, bw - 50)
                 if near(px(bp, bw, xx, 6),
                         (0x20, 0x20, 0x20), tol=10))
        checks.append((f"B: wmd status bar ({sb}/{bw-100})",
                       sb > (bw - 100) * 0.70))

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
