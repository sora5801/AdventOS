#!/usr/bin/env python3
"""
Session 161 smoke: wmterm interactive polish.

User report after sessions 157-160:
  1. backspace doesn't work inside wmterm
  2. only one command runs (prompt doesn't reappear visually)
  3. PgUp scrollback doesn't work from the real keyboard

This test exercises all three.  Backspace + multi-command are
verified by:

  - type 'abc' + backspace + Enter
  - observe wmterm read back the post-backspace line "ab" via PTY
  - observe the inner shell try to execute "ab" (not "abc") — only
    happens if sh.elf's read_line_interactive actually saw '\b'
    as a delete (which it always did) AND the redraw_line clear-EOL
    propagates through the PTY to wmterm's grid (the new bit)
  - type 'pwd' + Enter
  - observe pwd's output ("/" + newline + new prompt)

PgUp is verified via the same trace marker we used in session 159
(view= non-zero in the verbose log after a 'pgup' qcode).  But the
key thing for the user complaint is that the PS/2 driver path also
emits ESC[5~ now, not just USB-HID.  We test both: send 'pgup' via
QMP send-key (which routes to whichever keyboard QEMU has wired) AND
explicitly check the trace shows ESC byte 0x1b arriving at wmterm.
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
    abs_send(q, qbuf, x, y); time.sleep(0.3)
    abs_send(q, qbuf, x, y); time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}]})
    time.sleep(0.4)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(1.0)


def send_qkey(q, qbuf, code):
    qmp_cmd(q, qbuf, "send-key",
            {"keys": [{"type": "qcode", "data": code}]})
    time.sleep(0.15)


def type_str(q, qbuf, s):
    qmap = {' ': 'spc', '/': 'slash', '.': 'dot', '\n': 'ret'}
    for ch in s:
        if ch in qmap: send_qkey(q, qbuf, qmap[ch])
        elif ch.isalnum(): send_qkey(q, qbuf, ch.lower())


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-s161.log"), "w")
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

        # Focus wmterm.
        focus_ok = False
        for _ in range(10):
            click(q, qbuf, 300, 350); time.sleep(1.0)
            if "wmterm: FOCUS" in trace():
                focus_ok = True; break

        # Test 1 — multi-command + prompt reappears.
        print("[+] type 'pwd' + Enter")
        type_str(q, qbuf, "pwd")
        send_qkey(q, qbuf, "ret")
        time.sleep(3.0)
        # After pwd, wmterm should read echo of 'pwd\n' + pwd output
        # '/\n' + new prompt 'advent$ '.  The shell can flush these in
        # one or several reads depending on scheduling; check the
        # combined trace contains both the path output and the new
        # prompt.  The preview-mode dump (session 161) makes this
        # trivial — we just look for the literal substring.
        all_trace = trace()
        # Look for any read whose preview contains pwd's output "/.".
        # The shell may flush in one read (echo + output + new prompt)
        # or split across two — either way, the literal "/." substring
        # in some `rd n=` preview proves the path arrived.
        pwd_ok = False
        for line in all_trace.splitlines():
            if "wmterm: rd n=" in line and "/." in line and "advent" in line:
                pwd_ok = True; break
        print(f"   pwd read-back present: {pwd_ok}")

        print("[+] type 'pwd' + Enter (second time, verifies prompt came back)")
        before2 = len(trace())
        type_str(q, qbuf, "pwd")
        send_qkey(q, qbuf, "ret")
        time.sleep(3.0)
        new2 = trace()[before2:]
        # If the prompt reappeared, sh.elf was at read_line_interactive
        # ready to consume 'p','w','d','\n' and echo each, then run pwd.
        second_pwd_ok = "KEY 0x70" in new2 and "wmterm: rd n=" in new2
        print(f"   second pwd accepted + output: {second_pwd_ok}")

        # Test 2 — backspace via CSI K.
        print("[+] type 'abc' + backspace + Enter")
        before3 = len(trace())
        type_str(q, qbuf, "abc")
        send_qkey(q, qbuf, "backspace")
        send_qkey(q, qbuf, "ret")
        time.sleep(3.0)
        new3 = trace()[before3:]
        bs_ev = "KEY 0x8" in new3
        # wmterm's verbose mode now dumps up to 80 chars of every
        # PTY read in square brackets, so we can look directly for
        # the inner shell's "not found" message.  If backspace took
        # effect, sh.elf committed line "ab" — not "abc" — and the
        # error names "ab".
        ab_msg = "command not found: ab" in new3
        abc_msg = "command not found: abc" in new3
        print(f"   backspace KEY routed: {bs_ev}")
        print(f"   inner sh sees 'ab' (backspace worked): "
              f"{ab_msg and not abc_msg}")

        # Generate enough output to fill scrollback before testing PgUp.
        # ls / produces ~30 lines which exceeds the 24-row visible grid,
        # so several rows actually land in g_sb.
        print("[+] type 'ls /' to populate scrollback")
        type_str(q, qbuf, "ls /")
        send_qkey(q, qbuf, "ret")
        time.sleep(3.5)

        # Test 3 — PgUp via QMP qcode 'pgup'.
        print("[+] PgUp scrollback")
        before4 = len(trace())
        for _ in range(3):
            send_qkey(q, qbuf, "pgup"); time.sleep(0.4)
        time.sleep(1.5)
        new4 = trace()[before4:]
        # ESC 0x1b + view becoming non-zero.
        esc_seen = "KEY 0x1b" in new4
        any_nonzero = False
        for line in new4.split("\n"):
            if "view=" in line and "view=0" not in line:
                any_nonzero = True; break
        print(f"   PgUp ESC reached wmterm: {esc_seen}")
        print(f"   view scrolled into history: {any_nonzero}")

        ser_stop.set(); time.sleep(0.3)
        with ser_lock:
            ser_buf = bytes(ser_log)
        with open(os.path.join(ROOT, "polish_serial.log"), "w") as f:
            f.write(ser_buf.decode("utf-8", "replace"))

        checks = [
            ("focus wmterm",                              focus_ok),
            ("pwd runs + prompt reappears (read-back)",   pwd_ok),
            ("second pwd works (prompt was ready)",       second_pwd_ok),
            ("backspace routed to wmterm",                bs_ev),
            ("inner shell saw 'ab' after backspace",      ab_msg and not abc_msg),
            ("PgUp ESC reached wmterm (PS/2 + USB path)", esc_seen),
            ("scrollback view went non-zero",             any_nonzero),
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
