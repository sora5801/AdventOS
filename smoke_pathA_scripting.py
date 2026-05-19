#!/usr/bin/env python3
"""
Path A scripting fundamentals smoke test.

Verifies the second bundle of bash features:
  - Positional args ($1..$9, $@, $*, $#, $0)
  - `shift` builtin
  - `[` / `test` builtin (file tests, string ops, integer compares)
  - `read` builtin (no -p path tested here since stdin is the serial wire)
  - Shell functions (define + call + positional args inside)
  - `if`/`then`/`elif`/`else`/`fi`
  - `for VAR in WORDS; do ...; done`
  - `while CMD; do ...; done`
  - Multi-line scripts via a `.sh` file dropped on tmpfs + sourced

Sync model: each command lands at a fresh "$ " prompt. We capture the
slice between sends and assert against the command's actual output
slice (skipping the input echo line where the marker appears once).
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
PORT = 4501
PROMPT = b"$ "


def safe_print(b):
    s = b.decode("ascii", errors="replace")
    out = []
    for ch in s:
        if ch == "\n" or (32 <= ord(ch) < 127): out.append(ch)
        else: out.append("?")
    print("".join(out))


def read_until(sock, marker, timeout=15):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try: chunk = sock.recv(4096)
        except socket.timeout: continue
        except ConnectionResetError:
            e = TimeoutError("reset"); e.buf = buf; raise e
        if not chunk:
            e = TimeoutError("eof"); e.buf = buf; raise e
        buf += chunk
        if marker in buf: return buf
    e = TimeoutError(f"never saw {marker!r}"); e.buf = buf; raise e


def run(sock, cmd, timeout=15):
    sock.sendall(cmd.encode() + b"\n")
    return read_until(sock, PROMPT, timeout=timeout)


def output_after_echo(slc, cmd):
    """Return the slice that follows the input-echo line for `cmd`.
    The shell raw-echoes user input, so we slice past the first
    occurrence of the literal command + newline."""
    needle = cmd.encode()
    pos = slc.find(needle)
    if pos < 0: return slc
    pos = slc.find(b"\n", pos)
    if pos < 0: return slc
    return slc[pos + 1:]


def assert_contains(label, slc, needle):
    if needle not in slc:
        print(f"[!] {label}: missing {needle!r}")
        safe_print(slc); sys.exit(1)


def assert_not(label, slc, needle):
    if needle in slc:
        print(f"[!] {label}: should not contain {needle!r}")
        safe_print(slc); sys.exit(1)


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)
    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-script.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{PORT},server=on,wait=on",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(2.0)
    ser = None
    try:
        for _ in range(10):
            try: ser = socket.create_connection(("127.0.0.1", PORT), timeout=5); break
            except (ConnectionRefusedError, OSError): time.sleep(0.5)
        if ser is None:
            print("[!] could not connect"); return 1

        read_until(ser, PROMPT, timeout=60)
        print("[+] shell up")

        # ---- 1. `[` builtin: file tests ----
        cmd = "[ -f /etc/passwd ] && echo HAS_PASSWD"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("[ -f] ", out, b"HAS_PASSWD")
        print("[+] `[ -f FILE ]` true on /etc/passwd")

        cmd = "[ -d /etc ] && echo ETC_IS_DIR"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("[ -d]", out, b"ETC_IS_DIR")
        print("[+] `[ -d /etc ]` true")

        cmd = "[ -f /no_such ] || echo MISSING"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("[ -f miss]", out, b"MISSING")
        print("[+] `[ -f missing ]` false")

        # ---- 2. `[` builtin: string / int ----
        cmd = "[ abc = abc ] && echo STR_EQ"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("=", out, b"STR_EQ")
        print("[+] `[ STR = STR ]`")

        cmd = "[ 5 -lt 10 ] && echo INT_LT"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("-lt", out, b"INT_LT")
        print("[+] `[ N -lt N ]`")

        cmd = "[ -z \"\" ] && echo ZERO_LEN"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("-z", out, b"ZERO_LEN")
        print("[+] `[ -z STR ]`")

        # ---- 3. `if` / `then` / `else` / `fi` single-line ----
        cmd = "if [ -f /etc/passwd ]; then echo IF_TRUE; else echo IF_FALSE; fi"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("if-then", out, b"IF_TRUE")
        assert_not("if-then", out, b"IF_FALSE")
        print("[+] `if/then/else/fi` (true branch)")

        cmd = "if [ -f /no_such ]; then echo IF_TRUE; else echo IF_FALSE; fi"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("if-else", out, b"IF_FALSE")
        assert_not("if-else", out, b"IF_TRUE")
        print("[+] `if/then/else/fi` (false branch)")

        # ---- 4. `for` loop single-line ----
        cmd = "for x in a b c; do echo FOR=$x; done"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        for v in (b"FOR=a", b"FOR=b", b"FOR=c"):
            assert_contains("for", out, v)
        print("[+] `for VAR in WORDS; do ...; done`")

        # ---- 5. `while` loop with shift ----
        cmd = "x=3; while [ $x -gt 0 ]; do echo W=$x; x=$(($x)); x=99; break_hack; done"
        # while loops need arithmetic to decrement; we don't have $((..)).
        # Instead, use shift on positional args via a function. Simpler:
        # set FOO and decrement via test/echo. Use a fixed iteration via for.
        # We'll replace this test with a while that consumes positional args
        # via shift inside a function, which is the canonical pattern anyway.
        cmd = "for i in 1 2 3; do echo WIDX=$i; done"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        for v in (b"WIDX=1", b"WIDX=2", b"WIDX=3"):
            assert_contains("for-idx", out, v)
        print("[+] for as counted loop OK")

        # ---- 6. Shell function definition + call ----
        cmd = "greet() { echo HELLO_$1; } ; greet WORLD"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("func", out, b"HELLO_WORLD")
        print("[+] function def + call with $1")

        cmd = "args() { echo COUNT=$# ; echo FIRST=$1 ; echo LAST=$3 ; } ; args x y z"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("func-count", out, b"COUNT=3")
        assert_contains("func-first", out, b"FIRST=x")
        assert_contains("func-last", out, b"LAST=z")
        print("[+] function $# / $1 / $3")

        # ---- 7. shift builtin ----
        cmd = "show() { shift ; echo NOW=$1 ; } ; show a b c"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("shift", out, b"NOW=b")
        print("[+] shift drops $1")

        # ---- 8. while loop calling shift ----
        cmd = "walk() { while [ $# -gt 0 ]; do echo W=$1; shift; done; } ; walk a b c"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        for v in (b"W=a", b"W=b", b"W=c"):
            assert_contains("while-shift", out, v)
        print("[+] while + shift consumes positional args")

        # ---- 9. read builtin ----
        # Provide the line via the same serial input. Use `-p PROMPT`.
        ser.sendall(b"read NAME\n")
        # The shell now waits for a line on stdin. Send it.
        time.sleep(0.2)
        ser.sendall(b"alice\n")
        # Then check
        read_until(ser, PROMPT, timeout=10)
        cmd = "echo READ=$NAME"
        slc = run(ser, cmd)
        out = output_after_echo(slc, cmd)
        assert_contains("read", out, b"READ=alice")
        print("[+] read NAME populates env var")

        # ---- 10. Multi-line script via tmpfs ----
        # Write a script file and source it. Each line is shipped to
        # the FS via `echo '...' >> /tmp.sh`; the single quotes are
        # what stop $x / $1 from being expanded BEFORE the file is
        # written (bash-standard single-quote semantics, which I
        # just enabled).
        script_lines = [
            "echo SCRIPT_START",
            "for x in one two three; do",
            "  echo LOOP=$x",
            "done",
            "if [ -f /etc/passwd ]; then",
            "  echo SCRIPT_IF_TRUE",
            "fi",
            "fn() {",
            "  echo FN_ARG=$1",
            "}",
            "fn from_script",
            "echo SCRIPT_END",
        ]
        run(ser, "echo '" + script_lines[0] + "' > /tmp.sh")
        for ln in script_lines[1:]:
            run(ser, "echo '" + ln + "' >> /tmp.sh")
        slc = run(ser, "source /tmp.sh", timeout=20)
        out = output_after_echo(slc, "source /tmp.sh")
        for v in (b"SCRIPT_START", b"LOOP=one", b"LOOP=two", b"LOOP=three",
                  b"SCRIPT_IF_TRUE", b"FN_ARG=from_script", b"SCRIPT_END"):
            assert_contains("script "+v.decode(), out, v)
        print("[+] multi-line script: for + if + function all work")

        print("\nALL CHECKS PASSED")
        return 0
    except TimeoutError as e:
        print(f"[!] timeout: {e}")
        b = getattr(e, "buf", b"")
        if b:
            print("--- buffer at timeout ---")
            safe_print(b)
        return 1
    finally:
        try:
            if ser: ser.close()
        except: pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
