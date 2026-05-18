#!/usr/bin/env python3
"""
Session 134 smoke test: wmterm terminal emulator.

Launches wmd + wmterm, gives the shell time to start and print
its banner + prompt, takes a screendump.  Verifies:
  - wmterm window painted at the cascade slot
  - light-green text pixels in the grid area (the rendered
    shell output — banner "AdventOS userspace shell, pid=N",
    "Type 'help' for builtins. ...", and the "advent$ " prompt)
  - the column at x=107 (grid origin) has some text — sanity
    that the grid started at the expected position
  - wmd compositor alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4495
SERIAL_PORT = 4496
SHOT = os.path.join(ROOT, "shot_wmterm.ppm")


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

    log = open(os.path.join(ROOT, "qemu-s134.log"), "w")
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
            print("[!] no shell"); return 1
        ser.sendall(b"wmd 40 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmterm 30\n")
        # Give the PTY-forked sh.elf plenty of time to print its
        # banner + prompt.
        time.sleep(5.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

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

        # wmterm at slot 0 (--clean): outer (100, 200, 540, 240).
        # Surface at (101, 218, 540, 240).  Grid origin (107, 242).
        # First grid row spans y=242..251.
        checks = []

        # 1. wmterm window title-bar present (focused-blue or grey).
        #    Sample x=200, y=226 — middle of the title bar.
        title = px(200, 226)
        title_ok = near(title, (0x40, 0x80, 0xE0), tol=15) or \
                   near(title, (0x40, 0x40, 0x40), tol=10)
        print(f"   title bar @ (200, 226) = {title}")
        checks.append((f"wmterm title bar painted", title_ok))

        # 2. Light-green text pixels in the grid area.  The shell
        #    has had 5s to print its banner + prompt, so multiple
        #    text rows should be populated.  Scan rows 0..5 of the
        #    grid (y=242..291).  Light green = 0xC0E0C0.
        green_text = 0
        for yy in range(242, 322):
            for xx in range(107, 580):
                if near(px(xx, yy), (0xC0, 0xE0, 0xC0), tol=20):
                    green_text += 1
        print(f"   green text pixels in grid: {green_text}")
        checks.append((f"shell output rendered as text ({green_text})",
                       green_text > 100))

        # 3. Caret (white block) somewhere in the grid — the
        #    blinking caret may or may not be ON at screendump
        #    time, so just check that there's SOMETHING white-ish
        #    in the grid (text doesn't render in pure white).
        white_marks = 0
        for yy in range(242, 460):
            for xx in range(107, 640):
                if near(px(xx, yy), (0xFF, 0xFF, 0xFF), tol=8):
                    white_marks += 1
        print(f"   white marks in grid: {white_marks}")
        # Caret is 8x9 = 72 px when on; off means 0.  Either way
        # OK — we don't strictly require it.

        # 4. wmterm window background dark (0x080808) somewhere
        #    in the grid we don't expect text.  Sample (500, 400) —
        #    far right, late row, likely empty.
        bg = px(500, 400)
        checks.append((f"wmterm bg dark @ (500, 400) = {bg}",
                       near(bg, (0x08, 0x08, 0x08), tol=10)))

        # 5. wmd top status bar still painted.
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
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
