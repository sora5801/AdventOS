#!/usr/bin/env python3
"""
Session 165 smoke: wmterm select-and-copy + Ctrl-V paste.

The full drag-to-select cycle can't be reliably automated in QEMU
because QEMU's usb-tablet device does NOT emit a fresh report for
position-only events while a mouse button is held — every attempt
at sending "press at A / abs to B / release" produced a release
event at A, not B.  The selection works fine for real users
(they're not driving the tablet through QMP) but the smoke just
verifies what it can without that path:

  - wmterm's verbose-mode log shows the focus click landed inside
    the grid body (proves the click-to-cell math is wired).
  - Ctrl-V sent through QMP fires the PASTE intercept inside
    wmterm — confirming the new keystroke is being captured before
    it goes through key_byte to the PTY.

The wmd → wmterm event-pipeline + selection state + render
highlight code paths have been exercised across the previously
passing smokes (157, 159, 161, 162, 163, 164), all of which still
pass after the session-165 changes.  This narrow smoke + the
existing regression suite is the testable surface.
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
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(1.0)


def send_ctrl(q, qbuf, code):
    qmp_cmd(q, qbuf, "send-key", {"keys": [
        {"type": "qcode", "data": "ctrl"},
        {"type": "qcode", "data": code},
    ]})
    time.sleep(0.3)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-s165.log"), "w")
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
        ser.sendall(b"wmterm -v 60 &\n");   time.sleep(5.0)

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

        # Focus wmterm.  This single click is enough to fire one
        # MOUSE_PRESS event into wmterm's verbose log, which is the
        # only PRESS observation the smoke needs.
        focus_ok = False
        for _ in range(10):
            click(q, qbuf, 300, 350); time.sleep(1.0)
            if "wmterm: FOCUS" in trace():
                focus_ok = True; break

        # Send Ctrl-V repeatedly until we see PASTE in the trace
        # (USB-kbd send-key with combo qcodes is also flaky).
        paste_fired = False
        for _ in range(5):
            send_ctrl(q, qbuf, "v")
            time.sleep(1.5)
            if "wmterm: PASTE" in trace():
                paste_fired = True; break

        # Did any MOUSE_PRESS land on a valid grid cell?
        press_landed_on_grid = False
        for line in trace().split("\n"):
            if "wmterm: PRESS" in line:
                try:
                    parts = line.split()
                    x = int(parts[2].split("=")[1])
                    y = int(parts[3].split("=")[1])
                    if x >= 6 and y >= 24:    # GRID_X / GRID_Y
                        press_landed_on_grid = True; break
                except Exception:
                    pass

        ser_stop.set(); time.sleep(0.3)
        with ser_lock:
            ser_buf = bytes(ser_log)
        with open(os.path.join(ROOT, "select_serial.log"), "w") as f:
            f.write(ser_buf.decode("utf-8", "replace"))

        checks = [
            ("wmterm focused after body click",       focus_ok),
            ("mouse PRESS routed to wmterm grid cell", press_landed_on_grid),
            ("Ctrl-V intercepted as PASTE",            paste_fired),
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
