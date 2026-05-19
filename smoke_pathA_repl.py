#!/usr/bin/env python3
"""
Smoke test: REPL polish + papercut fixes.

  - Missing-command no longer hangs the shell
  - Tab completion for command names + env vars
  - `~` tilde expansion -> $HOME
  - `!!` and `!N` history recall
  - Brace expansion `{a,b,c}` and `{N..M}`
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
PORT = 4507
PROMPT = b"$ "


def safe_print(b):
    s = b.decode("ascii", errors="replace")
    print("".join(ch if (ch == "\n" or (32 <= ord(ch) < 127)) else "?" for ch in s))


def read_until(sock, marker, timeout=15):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try: chunk = sock.recv(4096)
        except socket.timeout: continue
        except ConnectionResetError:
            err = TimeoutError("reset"); err.buf = buf; raise err
        if not chunk:
            err = TimeoutError("eof"); err.buf = buf; raise err
        buf += chunk
        if marker in buf: return buf
    err = TimeoutError(f"never saw {marker!r}"); err.buf = buf; raise err


def run(sock, cmd, timeout=15):
    sock.sendall(cmd.encode() + b"\n")
    return read_until(sock, PROMPT, timeout=timeout)


def output_after(slc, cmd):
    needle = cmd.encode()
    pos = slc.find(needle)
    if pos < 0: return slc
    nl = slc.find(b"\n", pos)
    if nl < 0: return slc
    return slc[nl + 1:]


def assert_in(label, slc, needle):
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
    log = open(os.path.join(ROOT, "qemu-repl.log"), "w")
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
        if ser is None: print("[!] no connection"); return 1
        read_until(ser, PROMPT, timeout=60)
        print("[+] shell up")

        # ---- 1. exec-failure no longer hangs ----
        slc = run(ser, "nosuchcmd", timeout=10)
        assert_in("missing-cmd", slc, b"sh: command not found: nosuchcmd")
        # Shell must still respond after the failure.
        slc = run(ser, "echo SURVIVED")
        out = output_after(slc, "echo SURVIVED")
        assert_in("survive", out, b"SURVIVED")
        print("[+] missing command: cleanly fails, shell stays responsive")

        # ---- 2. || recovers from missing command ----
        slc = run(ser, "nosuchcmd || echo RECOVERED")
        out = output_after(slc, "nosuchcmd || echo RECOVERED")
        assert_in("|| recover", out, b"RECOVERED")
        print("[+] `nosuchcmd || echo RECOVERED` runs the recovery")

        # ---- 3. Tilde expansion ----
        # HOME defaults to / per the shell startup.
        slc = run(ser, "echo ~")
        out = output_after(slc, "echo ~")
        # First line after the input echo IS the result. Should be "/".
        # The output line is exactly "/" (one char).
        first_line = out.split(b"\n", 1)[0].rstrip(b"\r")
        if first_line != b"/":
            print(f"[!] `~` -> expected /, got {first_line!r}"); safe_print(out); return 1
        print("[+] `~` -> $HOME (= /)")

        run(ser, "export HOME=/etc")
        slc = run(ser, "echo ~/passwd")
        out = output_after(slc, "echo ~/passwd")
        assert_in("tilde slash", out, b"/etc/passwd")
        print("[+] `~/passwd` -> $HOME/passwd")

        # Single-quoted tilde stays literal.
        slc = run(ser, "echo '~/foo'")
        out = output_after(slc, "echo '~/foo'")
        assert_in("quoted tilde", out, b"~/foo")
        print("[+] `'~/foo'` stays literal under single quotes")

        run(ser, "export HOME=/")  # reset

        # ---- 4. !! history recall ----
        run(ser, "echo HIST_TARGET")
        slc = run(ser, "!!")
        # The recalled line should print itself once + run echo
        out = output_after(slc, "!!")
        # Look for HIST_TARGET in the output AFTER the !!.
        assert_in("!! ran prior cmd", slc[slc.rfind(b"!!"):], b"HIST_TARGET")
        print("[+] `!!` repeats the previous command")

        # ---- 5. !N recall ----
        # Pick a fresh marker, run it, query its history index, recall.
        run(ser, "echo MARKER_RECALL")
        # `history` prints lines like "  N  echo MARKER_RECALL"
        slc = run(ser, "history")
        # Pull the line number for our marker (last occurrence).
        idx = None
        for line in slc.split(b"\n"):
            if b"echo MARKER_RECALL" in line:
                # Skip the "history" output line itself (no leading digits).
                lstrip = line.lstrip()
                digits = b""
                for ch in lstrip:
                    if ch >= ord('0') and ch <= ord('9'): digits += bytes([ch])
                    else: break
                if digits: idx = int(digits)
        if idx is None:
            print("[!] couldn't find MARKER_RECALL in history"); return 1
        slc = run(ser, f"!{idx}")
        assert_in("!N ran cmd", slc[slc.rfind(b"!"):], b"MARKER_RECALL")
        print(f"[+] `!{idx}` recalls history entry")

        # ---- 6. Brace expansion: comma list ----
        slc = run(ser, "echo a-{x,y,z}-b")
        out = output_after(slc, "echo a-{x,y,z}-b")
        # echo joins args with spaces.
        assert_in("brace comma", out, b"a-x-b a-y-b a-z-b")
        print("[+] `{x,y,z}` expands with prefix + suffix")

        # ---- 7. Brace expansion: numeric range ----
        slc = run(ser, "echo {1..5}")
        out = output_after(slc, "echo {1..5}")
        assert_in("brace range", out, b"1 2 3 4 5")
        print("[+] `{1..5}` -> `1 2 3 4 5`")

        # ---- 8. Brace expansion: descending range ----
        slc = run(ser, "echo {3..1}")
        out = output_after(slc, "echo {3..1}")
        assert_in("brace desc range", out, b"3 2 1")
        print("[+] `{3..1}` descending")

        # ---- 9. Brace expansion: cartesian (sequential braces) ----
        slc = run(ser, "echo {a,b}{1,2}")
        out = output_after(slc, "echo {a,b}{1,2}")
        assert_in("brace cartesian", out, b"a1 a2 b1 b2")
        print("[+] `{a,b}{1,2}` -> cartesian product")

        # ---- 10. Brace expansion is a no-op without a comma/range ----
        slc = run(ser, "echo {only}")
        out = output_after(slc, "echo {only}")
        assert_in("brace single", out, b"{only}")
        print("[+] `{only}` stays literal (no `,` or `..`)")

        print("\nALL CHECKS PASSED")
        return 0
    except TimeoutError as e:
        print(f"[!] timeout: {e}")
        b = getattr(e, "buf", b"")
        if b: safe_print(b[-1024:])
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
