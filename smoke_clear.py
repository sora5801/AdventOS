#!/usr/bin/env python3
"""
Smoke test: `clear` builtin and Ctrl-L line-editor binding.

`clear` calls sys_tty_clear() which zeroes the console + homes the
cursor. Hard to verify visually over serial, but we can:
  1. Confirm the builtin runs and returns to prompt (no hang).
  2. Confirm Ctrl-L (0x0C) is accepted by the line editor and
     redraws the prompt + any partially-typed buffer.
  3. Confirm tab completion finds `clear` in the builtin set.
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
PORT = 4508
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


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)
    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-clear.log"), "w")
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

        # ---- 1. `clear` builtin runs and returns to prompt ----
        ser.sendall(b"clear\n")
        read_until(ser, PROMPT, timeout=10)
        # Confirm subsequent commands still work.
        ser.sendall(b"echo AFTER_CLEAR\n")
        buf = read_until(ser, PROMPT, timeout=10)
        if b"AFTER_CLEAR" not in buf:
            print("[!] shell unresponsive after `clear`"); safe_print(buf[-512:]); return 1
        print("[+] `clear` builtin runs, prompt comes back, shell responsive")

        # ---- 2. clear chains via `;` ----
        ser.sendall(b"clear ; echo CHAINED\n")
        buf = read_until(ser, PROMPT, timeout=10)
        if b"CHAINED" not in buf:
            print("[!] `clear ; echo CHAINED` didn't run the trailing echo")
            safe_print(buf[-512:]); return 1
        print("[+] `clear ;` chains cleanly")

        # ---- 3. Tab completion finds `clear` ----
        # Send `cle<TAB>` and expect it to complete to `clear` (only
        # builtin matching). Then send <enter> to run it.
        ser.sendall(b"cle\t")
        # The line editor echoes typing live. Wait a moment for the
        # completion + space to land, then send newline.
        time.sleep(0.3)
        ser.sendall(b"\n")
        buf = read_until(ser, PROMPT, timeout=10)
        # After completion + run, the prompt should be back. We can't
        # directly inspect the echoed line easily (raw mode emits per
        # char), but if `cle\t` completed to anything other than
        # `clear`, the shell would have errored. Confirm no error.
        if b"command not found" in buf or b"error" in buf:
            print("[!] cle<TAB> didn't complete to `clear`"); safe_print(buf[-512:]); return 1
        print("[+] `cle<TAB>` completes to `clear` (no error after enter)")

        # ---- 4. Ctrl-L survives + redraws ----
        # Send Ctrl-L at an empty prompt, then a full command. Sync
        # on the command's OUTPUT (a unique marker), not on `$ ` —
        # Ctrl-L itself emits a fresh prompt which would trip a
        # naive prompt-sync prematurely.
        ser.sendall(b"\x0c")               # Ctrl-L
        time.sleep(0.3)
        ser.sendall(b"echo CTRL_L_OK_MARK\n")
        # Sync on the output line itself (terminated by \r or \n)
        # — the actual prompt comes later after kernel-emitted task
        # exit diagnostics interleave.
        buf = read_until(ser, b"CTRL_L_OK_MARK\r", timeout=10)
        # And then wait for the next prompt to confirm shell returned.
        buf = read_until(ser, PROMPT, timeout=10)
        print("[+] Ctrl-L on empty buffer: shell processes next command")

        # Partial buffer + Ctrl-L preserves the buffer.
        ser.sendall(b"echo PARTIAL_MARK")
        time.sleep(0.3)
        ser.sendall(b"\x0c")               # Ctrl-L
        time.sleep(0.3)
        ser.sendall(b"\n")
        buf = read_until(ser, b"PARTIAL_MARK\r", timeout=10)
        buf = read_until(ser, PROMPT, timeout=10)
        print("[+] Ctrl-L: partial buffer preserved across redraw")

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
