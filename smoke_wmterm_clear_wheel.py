#!/usr/bin/env python3
"""
Session 163 smoke: wmterm clear + mouse-wheel scrollback.

clear:
  - run `ls /` so the grid has content
  - run `clear`
  - verify wmterm's CSI 2J + CSI H handlers wiped the grid back
    to empty (no green text pixels anywhere in the body)

mouse wheel:
  - run `ls /` again so g_sb has rows
  - send QMP input-send-event wheel "up" deltas
  - verify wmterm's WHEEL handler stepped g_view_offset off zero
    (trace shows view= non-zero in the verbose log)
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
    """ Session 169 — bundle abs + btn-down in ONE event so QEMU's
    usb-tablet emits a fresh report at this position with the click.
    Separate abs / btn events occasionally get dropped when the
    button bit hasn't changed between reports. """
    fb_w, fb_h = 1024, 768
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    for _ in range(2):
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax}},
            {"type": "abs", "data": {"axis": "y", "value": ay}},
        ]})
        time.sleep(0.3)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"down": True, "button": "left"}},
    ]})
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]})
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


def wheel(q, qbuf, up):
    """ Send a single mouse-wheel click.  QEMU encodes scroll as
    a btn event with name "wheel-up" or "wheel-down" — there's no
    "value", just a press (and an implicit release on the next
    poll).  `up` truthy = up (scroll back into history). """
    name = "wheel-up" if up else "wheel-down"
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": name}}
    ]})
    time.sleep(0.05)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": name}}
    ]})
    time.sleep(0.15)


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        return w, h, f.read()


def near(a, b, tol=15):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-s163.log"), "w")
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
        ser.sendall(b"wmterm -v 90 &\n");   time.sleep(5.0)

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

        def shot(name):
            path = os.path.join(ROOT, f"shot_clrwh_{name}.ppm")
            if os.path.exists(path): os.remove(path)
            qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (not os.path.exists(path)
                                              or os.path.getsize(path) < 100):
                time.sleep(0.1)
            return read_ppm(path)

        def green_in_body(p, w):
            # Sample the wmterm body region (screen y 242..480, x 110..640)
            # for sage-green text pixels (0xC0E0C0).
            count = 0
            for y in range(242, 480, 2):
                for x in range(110, 640, 4):
                    i = (y * w + x) * 3
                    if near((p[i], p[i+1], p[i+2]),
                            (0xC0, 0xE0, 0xC0), tol=25):
                        count += 1
            return count

        # Focus wmterm.
        focus_ok = False
        for _ in range(10):
            click(q, qbuf, 300, 350); time.sleep(1.0)
            if "wmterm: FOCUS" in trace():
                focus_ok = True; break

        # --- clear test ---
        # Session 169 — drive typing via serial-injected bytes.
        # wmd's kbd-grab (session 160) routes serial into the focused
        # wmterm via the kbd ring, identical to physical keys but
        # without the QMP send-key drop-per-char flake.
        print("[+] run 'ls /' to fill the grid (serial)")
        ser.sendall(b"ls /\n"); time.sleep(3.5)
        w, h, px_before = shot("before_clear")
        green_before = green_in_body(px_before, w)
        print(f"   green text px before clear: {green_before}")

        print("[+] run 'clear' (serial)")
        ser.sendall(b"clear\n"); time.sleep(4.0)
        w2, h2, px_after = shot("after_clear")
        green_after = green_in_body(px_after, w2)
        print(f"   green text px after clear: {green_after}")
        # After `clear`, sh re-prints `advent$ ` (one prompt line) and
        # the cursor block.  Empirically that lands at 100-450 green
        # pixels depending on cursor blink phase and font sub-pixel
        # alignment.  The wall-of-text from `ls /` is ~1500+ pixels,
        # so a >3x reduction is the real signal — the absolute floor
        # of 600 is just a sanity bound that a "no clear at all"
        # failure (~1500 px) would blow through.
        clear_ok = green_after < green_before / 3 and green_after < 600

        # --- mouse wheel test ---
        # Re-fill the grid so there's content in scrollback.
        print("[+] run 'ls /' again to populate scrollback ring (serial)")
        ser.sendall(b"ls /\n"); time.sleep(3.5)

        # Position the cursor over wmterm body, then send wheel deltas.
        # USB-tablet wheel reports require a button or movement to
        # flush; we wiggle the cursor a hair to coax QEMU into sending.
        print("[+] mouse-wheel up")
        before_w = len(trace())
        abs_send(q, qbuf, 300, 350); time.sleep(0.5)
        for i in range(8):
            wheel(q, qbuf, True)
            # Tickle the cursor a hair after each wheel — usb-tablet
            # reports are coalesced, so the report containing the
            # wheel byte goes out on the next position-changing event.
            abs_send(q, qbuf, 300 + (i % 2), 350); time.sleep(0.2)
        time.sleep(2.0)
        new_w = trace()[before_w:]
        wheel_event = "WHEEL" in new_w
        any_view_nonzero = False
        for line in new_w.split("\n"):
            if ("view=" in line and "view=0" not in line) or \
               ("WHEEL" in line and "view=" in line and "view=0" not in line):
                any_view_nonzero = True; break
        print(f"   WHEEL event reached wmterm: {wheel_event}")
        print(f"   view scrolled non-zero from wheel: {any_view_nonzero}")

        ser_stop.set(); time.sleep(0.3)
        with ser_lock:
            ser_buf = bytes(ser_log)
        with open(os.path.join(ROOT, "clrwh_serial.log"), "w") as f:
            f.write(ser_buf.decode("utf-8", "replace"))

        checks = [
            ("focus wmterm",                              focus_ok),
            ("ls / filled the grid (lots of green text)", green_before > 200),
            ("clear emptied the grid (green text gone)",  clear_ok),
            ("mouse wheel event reached wmterm",          wheel_event),
            ("wheel scrolled view into history",          any_view_nonzero),
        ]
        print("\n=== checks ===")
        ok_all = True
        for n, p in checks:
            print(f"  [{'OK' if p else 'FAIL'}] {n}")
            if not p: ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
