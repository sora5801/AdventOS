#!/usr/bin/env python3
"""
Smoke test: `cd ..` walks up to parent + path normalization.

Verifies that the userspace path normalizer in cmd_cd correctly
resolves `.` and `..` segments before calling sys_chdir (which only
understands one-shot named directories).

Cases:
  /         cd mnt          /mnt
  /mnt      cd ..           /
  /etc/ssl  cd ..           /etc
  /         cd /etc/../mnt  /mnt
  /etc      cd ./../mnt     /mnt
  /mnt      cd ../..        /
  /         cd .            /
  /etc      cd nonexistent  /etc (unchanged + error)
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
PORT = 4504
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


def run(sock, cmd, timeout=10):
    sock.sendall(cmd.encode() + b"\n")
    return read_until(sock, PROMPT, timeout=timeout)


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)
    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-cd.log"), "w")
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
            print("[!] no connection"); return 1
        read_until(ser, PROMPT, timeout=60)
        print("[+] shell up")

        cases = [
            # (start_cwd, command, expected_cwd, label)
            ("/",        "cd mnt",          "/mnt",     "cd mnt"),
            ("/mnt",     "cd ..",           "/",        "cd .. from /mnt"),
            ("/",        "cd /etc/ssl",     "/etc/ssl", "cd /etc/ssl"),
            ("/etc/ssl", "cd ..",           "/etc",     "cd .. from /etc/ssl"),
            ("/etc",     "cd /etc/../mnt",  "/mnt",     "cd /etc/../mnt"),
            ("/mnt",     "cd .",            "/mnt",     "cd ."),
            ("/mnt",     "cd ../..",        "/",        "cd ../.. from /mnt"),
            ("/",        "cd /etc",         "/etc",     "absolute cd /etc"),
            ("/etc",     "cd ./../mnt",     "/mnt",     "cd ./../mnt"),
        ]
        rc = 0
        for start, cmd, expected, label in cases:
            # First, force-set cwd to `start` so each case is hermetic.
            run(ser, "cd /")
            if start != "/":
                run(ser, "cd " + start)
            # Run the test command and verify pwd output.
            slc = run(ser, cmd)
            slc += run(ser, "pwd")
            # pwd output appears on its own line as the literal path.
            # Look for `\r\n<path>\r\n` or just <path> surrounded by linebreaks.
            target = b"\n" + expected.encode() + b"\r"
            target_alt = b"\n" + expected.encode() + b"\n"
            if target not in slc and target_alt not in slc:
                print(f"[!] {label}: expected pwd='{expected}'")
                safe_print(slc[-512:])
                rc = 1
                continue
            print(f"[+] {label} -> {expected}")

        # Negative case: cd nonexistent should fail, leave cwd alone.
        run(ser, "cd /etc")
        slc = run(ser, "cd nonexistent")
        if b"no such directory" not in slc:
            print("[!] cd nonexistent: missing error message")
            safe_print(slc[-512:])
            rc = 1
        else:
            slc = run(ser, "pwd")
            if b"\n/etc\r" not in slc and b"\n/etc\n" not in slc:
                print("[!] cd nonexistent: cwd changed unexpectedly")
                safe_print(slc[-512:])
                rc = 1
            else:
                print("[+] cd nonexistent -> error, cwd unchanged")

        if rc == 0: print("\nALL CHECKS PASSED")
        return rc
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
