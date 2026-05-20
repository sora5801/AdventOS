#!/usr/bin/env python3
"""
Session 158 smoke: wmterm close X delivers SIGHUP to the child sh.elf
and the shell exits cleanly instead of spinning on read-returns-0.

Without this fix, closing wmterm dropped master_refs to 0, sh.elf's
sys_read on the slave returned 0 (EOF), and the shell's
read_line_interactive loop hit `if (n <= 0) continue;` and burned
CPU forever doing zero-byte reads.  The kernel now sends SIGHUP to
the slave's fg_pgrp on last-master-close (POSIX carrier-loss
semantics), and sh.elf's read loop additionally exits on EOF as a
fallback.

Verification (all via serial trace):

  wmterm: id=N                  wmterm opened
  wmterm: rd n=N first=0xa      shell banner reached wmterm
  wmterm: done                  CLOSE event handled, wmterm exited
  [sig] pid=X terminated...     kernel delivered SIGHUP to sh.elf
                                (default action terminates the task)

The kernel-log marker is the key signal — if SIGHUP doesn't fire,
the shell stays alive and the marker never prints.
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


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s158.log"), "w")
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
        ser.sendall(b"wmterm -v 50\n")
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

        # Confirm the inner sh.elf actually came up before we close.
        banner_ok = trace_has("wmterm: rd n=")
        print(f"[+] wmterm banner round-trip: {'OK' if banner_ok else 'NOT SEEN'}")

        # Body click first — QEMU's USB tablet sometimes drops the
        # very first abs report after idle, so we warm up the cursor
        # before the close X click that this test actually cares
        # about.  We don't gate on this taking; it's just to give the
        # cursor + wmd's hover state somewhere to land.
        for _ in range(2):
            click(q, qbuf, 300, 350)
            time.sleep(1.0)
            if trace_has("wmterm: FOCUS"): break

        # Snapshot the kernel-log up to now so the SIGHUP detection
        # below can distinguish "freshly delivered" from any pre-test
        # `[sig]` lines (boot-time signal noise, if any).
        with ser_lock:
            pre_close = bytes(ser_log)

        # Click close X.  Up to 10 retries — QEMU's USB tablet
        # occasionally drops the first abs report and lands the click
        # at the previous cursor coords.
        print("[+] click close X")
        close_ok = False
        for attempt in range(10):
            click(q, qbuf, 635, 209)
            time.sleep(2.0)
            if trace_has("wmterm: done"):
                print(f"   wmterm exited on attempt {attempt+1}")
                close_ok = True
                break
            print(f"   close attempt {attempt+1}: wmterm still up")

        # Give the kernel a beat to deliver SIGHUP + tear down sh.elf.
        time.sleep(2.5)

        with ser_lock:
            post_close = bytes(ser_log)
        new_bytes = post_close[len(pre_close):].decode("utf-8", "replace")
        sighup_ok = (
            "[sig]" in new_bytes
            and ("signal 1" in new_bytes or "SIGHUP" in new_bytes)
        )
        print(f"[+] SIGHUP delivered to sh.elf: "
              f"{'OK' if sighup_ok else 'NOT SEEN'}")

        # The outer shell (the one that ran `wmterm -v 50`) should be
        # back at its own prompt now because its foreground child
        # (wmterm) has exited.  Send a tiny echo to verify it's alive.
        ser.sendall(b"echo s158_alive\n")
        time.sleep(2.0)
        outer_ok = trace_has("s158_alive")
        print(f"[+] outer shell still alive after wmterm exit: "
              f"{'OK' if outer_ok else 'NOT SEEN'}")

        # Stop the drainer + dump the trace for forensics.
        ser_stop.set()
        drain_thread.join(timeout=2)
        with ser_lock:
            ser_buf = bytes(ser_log)
        all_lines = ser_buf.decode("utf-8", "replace").splitlines()
        with open(os.path.join(ROOT, "sighup_serial.log"), "w") as f:
            for l in all_lines: f.write(l + "\n")
        trace = [l for l in all_lines
                 if "wmterm:" in l or "[sig]" in l or "s158_alive" in l]
        if trace:
            print("\n--- relevant trace ---")
            for l in trace[-15:]: print(f"   {l}")

        checks = [
            ("shell banner reached wmterm",          banner_ok),
            ("CLOSE event -> wmterm exits cleanly",  close_ok),
            ("kernel delivers SIGHUP on master close", sighup_ok),
            ("outer shell survives wmterm exit",     outer_ok),
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
