#!/usr/bin/env python3
"""
Smoke test: arithmetic, parameter expansion, break/continue/return.

  - `$((expr))` substitutes the result into a token
  - `((expr))` as a command sets $? based on result (0 != 0 -> 0)
  - `${var:-default}` / `${#var}` / `${var%suf}` / `${var/old/new}`
  - `break` / `continue` in for and while loops
  - `return [N]` from a function (with status propagation)
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
PORT = 4505
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


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)
    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-arith.log"), "w")
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

        # ---- Arithmetic ----
        cmd = "echo A=$((2+3*4))"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("arith +/*",  out, b"A=14")
        print("[+] $((2+3*4)) -> 14")

        cmd = "echo A=$((10/3))"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("arith /",  out, b"A=3")
        print("[+] $((10/3)) -> 3")

        cmd = "echo A=$(((1+2)*4))"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("arith parens",  out, b"A=12")
        print("[+] $(((1+2)*4)) -> 12")

        run(ser, "x=5")
        cmd = "echo A=$((x*x))"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("arith var",  out, b"A=25")
        print("[+] $((x*x)) (x=5) -> 25")

        cmd = "((y=7+3)) ; echo Y=$y"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("((assign))",  out, b"Y=10")
        print("[+] ((y=7+3)) sets y -> 10")

        cmd = "(( 5 < 10 )) && echo LT_OK"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("(( < ))",  out, b"LT_OK")
        print("[+] (( 5 < 10 )) true")

        cmd = "(( 10 < 5 )) || echo GE_OK"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("(( >= via || ))",  out, b"GE_OK")
        print("[+] (( 10 < 5 )) false")

        # ---- Parameter expansion ----
        run(ser, "FOO=hello")
        cmd = "echo L=${#FOO}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${#var}",  out, b"L=5")
        print("[+] ${#FOO} -> 5")

        run(ser, "unset BAR")
        cmd = "echo X=${BAR:-fallback}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var:-}",  out, b"X=fallback")
        print("[+] ${BAR:-fallback} on unset -> fallback")

        run(ser, "unset BAR")
        run(ser, "echo X=${BAR:=initialized}")
        cmd = "echo BAR_NOW=$BAR"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var:=}",  out, b"BAR_NOW=initialized")
        print("[+] ${BAR:=initialized} also assigns")

        run(ser, "P=hello.c")
        cmd = "echo S=${P%.c}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var%suf}",  out, b"S=hello")
        print("[+] ${P%.c} strips .c")

        run(ser, "P=/usr/bin/ls")
        cmd = "echo S=${P#/usr/}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var#pre}",  out, b"S=bin/ls")
        print("[+] ${P#/usr/} strips prefix")

        run(ser, "Q='one two one'")
        cmd = "echo R=${Q/one/X}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var/old/new}",  out, b"R=X two one")
        print("[+] ${Q/one/X} replaces first")

        cmd = "echo R=${Q//one/X}"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("${var//old/new}",  out, b"R=X two X")
        print("[+] ${Q//one/X} replaces all")

        # ---- break ----
        cmd = "for x in a b c d ; do echo I=$x ; if [ $x = c ] ; then break ; fi ; done ; echo DONE"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        for v in (b"I=a", b"I=b", b"I=c", b"DONE"):
            assert_in("break "+v.decode(), out, v)
        if b"I=d" in out:
            print("[!] break: I=d appeared (loop didn't break)")
            safe_print(out); return 1
        print("[+] break exits for-loop")

        # ---- continue ----
        cmd = "for x in 1 2 3 4 ; do if [ $x = 2 ] ; then continue ; fi ; echo K=$x ; done ; echo DONE"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        for v in (b"K=1", b"K=3", b"K=4", b"DONE"):
            assert_in("continue "+v.decode(), out, v)
        if b"K=2" in out:
            print("[!] continue: K=2 leaked through")
            safe_print(out); return 1
        print("[+] continue skips iteration")

        # ---- return from function ----
        cmd = "early() { echo IN_FN ; return 7 ; echo NEVER ; } ; early ; echo RC=$?"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("return body", out, b"IN_FN")
        assert_in("return status", out, b"RC=7")
        if b"NEVER" in out:
            print("[!] return: code after return ran"); safe_print(out); return 1
        print("[+] return exits function with status 7")

        # ---- break from while ----
        run(ser, "i=0")
        cmd = "while (( i < 100 )) ; do (( i = i + 1 )) ; if (( i == 4 )) ; then break ; fi ; done ; echo FINAL=$i"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("while+break", out, b"FINAL=4")
        print("[+] while + arithmetic + break: i stops at 4")

        # ---- nested break N ----
        cmd = "for a in 1 2 ; do for b in x y ; do echo OUTER_$a INNER_$b ; if [ $b = x ] ; then break 2 ; fi ; done ; done ; echo TAIL"
        slc = run(ser, cmd)
        out = output_after(slc, cmd)
        assert_in("break 2", out, b"OUTER_1 INNER_x")
        assert_in("break 2 tail", out, b"TAIL")
        if b"OUTER_2" in out:
            print("[!] break 2: outer loop continued"); safe_print(out); return 1
        print("[+] break 2 exits both loops")

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
