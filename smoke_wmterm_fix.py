#!/usr/bin/env python3
"""
Session 157 smoke: wmterm input + close actually work now.

The wmterm bug deferred since session 145 was that the kernel's
FD_PTY_M / FD_PTY_S read paths ignored FD_FL_NONBLOCK.  wmterm's
main loop blocked on sys_read(master, ...) waiting for shell
output, so wm_poll_event never got called and clicks / keystrokes
/ close events accumulated unanswered.

Fix: pty_master_read_avail() peek + early-return-0 in the
non-block branch.

This test is trace-driven, not pixel-driven, because QEMU's
USB-tablet event delivery is flaky enough that visual confirmation
fails 30-40% of the time even when the kernel is doing the right
thing.  We watch wmterm's stderr for five markers, retrying clicks
until each one shows up (or we give up):

    wmterm: rd n=88 first=0xa   shell banner round-trip
    wmterm: FOCUS               wmd → wmterm event delivery alive
    wmterm: KEY 0x61 wr=1       keystroke routed and written to PTY
    wmterm: rd n=1 first=0x61   PTY echo full round-trip
    wmterm: done                CLOSE event handled cleanly

A background thread continuously drains qemu's TCP-backed serial so
the guest's busy-wait `while (!tx_empty()) {}` in kernel/serial.c
never stalls; without that, wmterm's diagnostic printfs can pin the
kernel in serial_putc and freeze the event loop.
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
    # Send the abs coords twice with a settle in between — QEMU's
    # usb-tablet sometimes drops the very first abs report of a
    # session (or right after a long idle window) and we end up
    # clicking at the previous cursor position.
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

    log = open(os.path.join(ROOT, "qemu-s157.log"), "w")
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
        # IMPORTANT: foreground wmterm so the outer shell sits in
        # sys_wait() and doesn't steal keystrokes from the kbd ring.
        # -v enables wmterm's per-event diagnostic prints (off by
        # default to keep launching consoles quiet during normal use).
        ser.sendall(b"wmterm -v 50\n")
        time.sleep(5.0)

        # Background drainer: keep qemu's serial TX moving so the
        # kernel's busy-wait tx-empty loop doesn't pin wmterm in a
        # diagnostic printf.  Captured bytes append to ser_log; the
        # main thread reads ser_log at end-of-test for the trace.
        ser_log = bytearray()
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

        # The banner trace ("rd n=88") should already be in the log
        # from wmterm's first read after sh.elf started.
        banner_ok = trace_has("wmterm: rd n=88")
        print(f"[+] shell banner round-trip: {'OK' if banner_ok else 'NOT SEEN'}")

        # 1. Click body for FOCUS.  Retry until trace shows FOCUS.
        # We click multiple times per attempt to compensate for the
        # USB-tablet abs report sometimes being dropped on quiet idle.
        print("[+] click wmterm body for focus")
        focus_ok = False
        for attempt in range(10):
            click(q, qbuf, 300, 350)
            time.sleep(2.0)
            if trace_has("wmterm: FOCUS"):
                print(f"   FOCUS delivered on attempt {attempt+1}")
                focus_ok = True
                break
            print(f"   focus attempt {attempt+1}: no FOCUS yet, retrying")

        # 2. Type 'a'.  Retry until trace shows wmterm received KEY +
        #    read back the echo.  Re-click body each attempt — focus
        #    can drift if wmd processes intervening mouse-move events.
        print("[+] type 'a'")
        key_ok = False
        echo_ok = False
        for attempt in range(10):
            if not focus_ok or attempt > 0:
                click(q, qbuf, 300, 350)
                time.sleep(1.0)
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": "a"}]})
            time.sleep(2.0)
            if not key_ok and trace_has("wmterm: KEY 0x61"):
                print(f"   KEY 'a' delivered on attempt {attempt+1}")
                key_ok = True
            if trace_has("wmterm: rd n=1 first=0x61"):
                print(f"   echo round-trip seen on attempt {attempt+1}")
                echo_ok = True
                break
            print(f"   echo attempt {attempt+1}: no echo yet, retrying")

        # 3. Close.  Retry until trace shows wmterm exited.
        print("[+] click close X")
        close_ok = False
        for attempt in range(10):
            click(q, qbuf, 635, 209)
            time.sleep(2.0)
            if trace_has("wmterm: done"):
                print(f"   close + exit on attempt {attempt+1}")
                close_ok = True
                break
            print(f"   close attempt {attempt+1}: window still up")

        # Re-check focus_ok at end — FOCUS sometimes arrives after the
        # focus phase exited (the second click in the typing phase
        # finally took).  The fix is what we're verifying; we don't
        # care when in the test it shows up, only that it does.
        if not focus_ok and trace_has("wmterm: FOCUS"):
            focus_ok = True

        # Stop the drainer + extract everything it captured.
        ser_stop.set()
        drain_thread.join(timeout=2)
        with ser_lock:
            ser_buf = bytes(ser_log)
        all_lines = ser_buf.decode("utf-8", errors="replace").splitlines()
        with open(os.path.join(ROOT, "wmterm_serial.log"), "w") as f:
            for l in all_lines: f.write(l + "\n")
        trace = [l for l in all_lines
                 if "wmterm:" in l or "wmd:" in l or "panic" in l.lower()]
        if trace:
            print("\n--- wmterm debug ---")
            for l in trace[-20:]: print(f"   {l}")

        checks = [
            ("shell banner round-trip (rd n=88)", banner_ok),
            ("wmd -> wmterm FOCUS event delivery", focus_ok),
            ("KEY 'a' routed to wmterm",          key_ok),
            ("PTY echo round-trip 'a' -> wmterm", echo_ok),
            ("CLOSE event -> wmterm exits cleanly", close_ok),
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
