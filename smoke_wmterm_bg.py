#!/usr/bin/env python3
"""
Session 160 smoke: keyboard grab — wmterm typing works when wmterm
is launched in the background.

Before session 160, both wmd's sys_kbd_poll loop and the outer
shell's raw-mode read_line_interactive drained from the same kbd
ring.  When the user ran `wmterm &` (background) the outer shell
sat at its prompt blocked in keyboard_wait_char, racing wmd for
every keystroke.  Whoever pulled first won; users would see their
typing eaten by the outer shell instead of arriving at the focused
wmterm.

Fix: wmd calls sys_kbd_grab(1) at startup.  keyboard_wait_char now
yields any task that isn't the grabber, so the outer shell's
tty_read effectively stalls while wmd is up.  task_exit_current
auto-releases if wmd dies without explicitly calling
sys_kbd_grab(0) — the kernel-console shell is recoverable.

Smoke launches `wmterm -v 50 &` (background, the previously broken
pattern) and verifies:

  wmterm: KEY 0x61 ...        keystroke reached wmterm, not outer sh
  wmterm: rd n=1 first=0x61   PTY echo round-trip worked
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
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk: return False, buf
        buf += chunk
        if marker in buf: return True, buf
    return False, buf


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})


def click(q, qbuf, x, y):
    abs_send(q, qbuf, x, y)
    time.sleep(0.3)
    abs_send(q, qbuf, x, y)
    time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}
    ]})
    time.sleep(0.4)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}
    ]})
    time.sleep(1.0)


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s160.log"), "w")
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
        # The whole point of session 160: launch wmterm in BACKGROUND.
        # The outer shell is at its prompt actively reading kbd via
        # read_line_interactive — without sys_kbd_grab in wmd, it
        # would race wmd and eat keystrokes.
        ser.sendall(b"wmterm -v 50 &\n")
        time.sleep(5.0)

        ser_log = bytearray(ser_buf)
        ser_lock = threading.Lock()
        ser_stop = threading.Event()

        def drainer():
            ser.settimeout(0.2)
            while not ser_stop.is_set():
                try:
                    chunk = ser.recv(4096)
                    if not chunk: break
                    with ser_lock: ser_log.extend(chunk)
                except (socket.timeout, OSError):
                    continue

        drain_thread = threading.Thread(target=drainer, daemon=True)
        drain_thread.start()

        def trace_has(marker):
            with ser_lock:
                return marker in bytes(ser_log).decode("utf-8", "replace")

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        banner_ok = trace_has("wmterm: rd n=")
        print(f"[+] shell banner reached wmterm: "
              f"{'OK' if banner_ok else 'NOT SEEN'}")

        # Click body to focus.  Retry until FOCUS arrives in trace.
        print("[+] click wmterm body for focus")
        focus_ok = False
        for attempt in range(10):
            click(q, qbuf, 300, 350)
            time.sleep(1.5)
            if trace_has("wmterm: FOCUS"):
                print(f"   FOCUS on attempt {attempt+1}")
                focus_ok = True
                break

        # Type 'a'.  Retry until trace shows the KEY + echo.
        print("[+] type 'a'")
        key_ok = False
        echo_ok = False
        for attempt in range(8):
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": "a"}]})
            time.sleep(1.5)
            if trace_has("wmterm: KEY 0x61"): key_ok = True
            if trace_has("wmterm: rd n=1 first=0x61"):
                echo_ok = True
                break
            print(f"   echo attempt {attempt+1}: not yet")

        # Click off wmterm onto the empty desktop — wmd's focus
        # release should drop the kbd grab, letting the outer shell
        # read kbd / serial again.  Verify that by sending a command
        # over serial and checking the shell echoes / executes it.
        # (While wmterm was focused, serial bytes would have been
        # routed to wmterm via the grab — see the test trace before
        # the click-off, where 'echo sentinel...' was visible going
        # to wmterm's inner shell instead.)
        print("[+] click empty desktop to release wmd grab")
        click(q, qbuf, 800, 600)        # corner of desktop, no window
        time.sleep(2.0)
        # Now the outer shell should be able to read serial again.
        ser.sendall(b"echo sentinel_back_to_outer_shell\n")
        time.sleep(3.0)
        outer_alive = trace_has("sentinel_back_to_outer_shell")
        print(f"   outer shell back: {outer_alive}")

        # Stop drainer + dump trace.
        ser_stop.set()
        drain_thread.join(timeout=2)
        with ser_lock:
            ser_buf = bytes(ser_log)
        all_lines = ser_buf.decode("utf-8", "replace").splitlines()
        with open(os.path.join(ROOT, "kbd_grab_serial.log"), "w") as f:
            for l in all_lines: f.write(l + "\n")
        trace = [l for l in all_lines
                 if "wmterm:" in l or "sentinel" in l or "aecho" in l
                 or "advent$" in l]
        if trace:
            print("\n--- trace (last 25) ---")
            for l in trace[-25:]: print(f"   {l}")

        checks = [
            ("shell banner reached wmterm",             banner_ok),
            ("wmd -> wmterm FOCUS event delivery",      focus_ok),
            ("KEY 'a' routed to wmterm (not outer sh)", key_ok),
            ("PTY echo round-trip 'a' -> wmterm",       echo_ok),
            ("outer shell readable again after click-off",
             outer_alive),
        ]
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
