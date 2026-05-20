#!/usr/bin/env python3
"""
Session 166 smoke: wmterm honors ANSI CSI m (SGR) sequences.

Flow:
  1. Focus wmterm.
  2. Type `colortest` — a tiny user binary that prints six
     coloured words followed by three bright/inverse variants.
  3. Screenshot the result and sample the visible grid at the
     expected positions of each colour word.  For each, count
     pixels close to the palette entry for that colour.  If
     the SGR parser is wired right, RED comes back as 0xCC0000,
     GREEN as 0x00CC00, etc.

Default cells (no SGR active) still render with the existing
sage-green 0xC0E0C0 fg, so the banner / prompt / unchanged
output is visually identical to pre-session-166.
"""
import os, socket, json, subprocess, time, sys, threading

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
        try: chunk = sock.recv(4096)
        except socket.timeout: continue
        if not chunk: return False, buf
        buf += chunk
        if marker in buf: return True, buf
    return False, buf


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]})


def click(q, qbuf, x, y):
    abs_send(q, qbuf, x, y); time.sleep(0.4)
    abs_send(q, qbuf, x, y); time.sleep(0.6)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}]})
    time.sleep(0.4)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(1.0)


def send_qkey(q, qbuf, code):
    qmp_cmd(q, qbuf, "send-key",
            {"keys": [{"type": "qcode", "data": code}]})
    time.sleep(0.12)


def type_str(q, qbuf, s):
    qmap = {' ': 'spc', '/': 'slash', '.': 'dot', '\n': 'ret'}
    for ch in s:
        if ch in qmap: send_qkey(q, qbuf, qmap[ch])
        elif ch.isalnum(): send_qkey(q, qbuf, ch.lower())


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        return w, h, f.read()


def near(a, b, tol=20):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-s166.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386", "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1", "-vga", "std", "-display", "none",
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
        ser.sendall(b"wmd 60 --clean &\n"); time.sleep(2.0)
        ser.sendall(b"wmterm -v 60 &\n"); time.sleep(5.0)

        ser_log = bytearray(ser_buf)
        ser_lock = threading.Lock()
        ser_stop = threading.Event()
        def drainer():
            ser.settimeout(0.2)
            while not ser_stop.is_set():
                try:
                    c = ser.recv(4096)
                    if not c: break
                    with ser_lock: ser_log.extend(c)
                except (socket.timeout, OSError): continue
        threading.Thread(target=drainer, daemon=True).start()

        def trace():
            with ser_lock:
                return bytes(ser_log).decode("utf-8", "replace")

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities"); qbuf = b""

        # Focus wmterm.  Once focused, wmd's kbd grab (session 160)
        # routes ALL kbd-ring input — including bytes injected via
        # the serial port — to wmterm instead of the outer shell.
        # That lets us drive wmterm's inner sh.elf by sending text
        # over the serial socket, which is far more reliable than
        # USB-kbd qcode-per-char via QMP.
        focused = False
        for attempt in range(15):
            click(q, qbuf, 300, 350); time.sleep(1.0)
            if "wmterm: FOCUS" in trace():
                print(f"   focused on attempt {attempt+1}")
                focused = True; break
        if not focused:
            print("   [!] focus never took, aborting smoke")
            return 1

        # Send "colortest\n" via serial.  wmd's kbd path picks up the
        # injected bytes; since wmterm has focus, they go to wmterm's
        # inner sh.elf, which runs colortest, which emits the SGR-
        # coloured words back through the PTY to wmterm.
        print("[+] send 'colortest' over serial (kbd-grabbed)")
        ser.sendall(b"colortest\n")
        time.sleep(5.0)

        def shot(name):
            path = os.path.join(ROOT, f"shot_color_{name}.ppm")
            if os.path.exists(path): os.remove(path)
            qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (not os.path.exists(path)
                                              or os.path.getsize(path) < 100):
                time.sleep(0.1)
            return read_ppm(path)

        w, h, p = shot("colortest")

        # The colortest output starts with the first word "RED" at
        # grid col 0.  Each word is then followed by a space then
        # the next word, in the order RED GREEN YELLOW BLUE MAGENTA
        # CYAN.  Exact x positions depend on prompt + echo offsets;
        # rather than hard-code, we sample a horizontal band across
        # the row where the colored output landed and count palette
        # hits per colour.
        #
        # Body row math: wmterm surface starts at screen y=218,
        # GRID_Y = 24 (header), LINE_H = 10.  Banner + echo + cmd
        # echo + prompt fill the first ~5-6 rows; colortest's first
        # output line lands around row 6 or 7.  We scan rows 5..15
        # for any pixels close to each palette entry and pick the
        # row with the highest count.
        palette = {
            "red":     (0xCC, 0x00, 0x00),
            "green":   (0x00, 0xCC, 0x00),
            "yellow":  (0xCC, 0xCC, 0x00),
            "blue":    (0x00, 0x00, 0xCC),
            "magenta": (0xCC, 0x00, 0xCC),
            "cyan":    (0x00, 0xCC, 0xCC),
        }

        def count_color(row_y, color):
            n = 0
            for x in range(108, 640):
                i = (row_y * w + x) * 3
                if near((p[i], p[i+1], p[i+2]), color, tol=20):
                    n += 1
            return n

        # Body screen y range for the grid: 218 + 24 .. 218 + 24 + 24*10
        # = 242 .. 482.
        print("\n=== colour hit counts (best row per palette entry) ===")
        results = {}
        for name, color in palette.items():
            best = 0
            for y in range(242, 482, 2):
                n = count_color(y, color)
                if n > best: best = n
            results[name] = best
            print(f"   {name:<8} = {best} px")

        # We expect each colour name to appear once on the colortest
        # output line — that's ~6 chars × 8px = ~48 colored pixels
        # roughly, given the 8x8 font.  Threshold low because some
        # glyphs are sparse and the body fill counts background, not
        # glyph fg.  Want non-zero per colour.
        checks = []
        for name, _ in palette.items():
            checks.append((f"colour '{name}' visible on grid",
                           results[name] >= 5))

        ser_stop.set(); time.sleep(0.3)
        with ser_lock:
            ser_buf = bytes(ser_log)
        with open(os.path.join(ROOT, "color_serial.log"), "w") as f:
            f.write(ser_buf.decode("utf-8", "replace"))

        print("\n=== checks ===")
        ok_all = True
        for n, p_ in checks:
            print(f"  [{'OK' if p_ else 'FAIL'}] {n}")
            if not p_: ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
