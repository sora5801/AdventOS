#!/usr/bin/env python3
"""
Session 137 — polished tcc UX smoke.

Verifies that `tcc /hello.c -o /myhello.elf` works with no flags on
a stock hello-world source that uses #include <stdio.h> + printf.
Then runs /myhello.elf and checks it prints the expected line.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4457


def safe_print(b):
    s = b.decode("ascii", errors="replace")
    print("".join(ch if ch == "\n" or 32 <= ord(ch) < 127 else "?" for ch in s))


def wait_for(sock, marker, buf, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except ConnectionResetError:
            return False, buf
        if not chunk:
            return False, buf
        buf += chunk
        if marker in buf:
            return True, buf
    return False, buf


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-tcc-polish.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "64", "-smp", "1",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(2.0)
    try:
        ser = None
        for _ in range(10):
            try:
                ser = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=5)
                break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.5)
        if ser is None:
            print("[!] could not connect to QEMU serial socket")
            return 1
        buf = b""
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=60)
        if not ok:
            print("[!] no shell prompt")
            return 1

        def run_cmd(cmd, timeout):
            time.sleep(0.3)
            ser.sendall(cmd + b"\n")
            local = b""
            okk, local = wait_for(ser, b"exited code=", local, timeout=timeout)
            try:
                ser.settimeout(0.4)
                while True:
                    chunk = ser.recv(4096)
                    if not chunk:
                        break
                    local += chunk
            except (socket.timeout, ConnectionResetError):
                pass
            return okk, local

        # Step 1 — verify /tcc/include/stdio.h is reachable
        ok, b1 = run_cmd(b"wc -c /tcc/include/stdio.h", 15)
        print(f"[{'OK' if ok else 'FAIL'}] /tcc/include/stdio.h on FS")

        # Step 2 — verify wrapper is at /tcc.elf and forwards version
        ok, b2 = run_cmd(b"/tcc.elf -v", 30)
        if not ok or b"tcc version" not in b2:
            print("[!] /tcc.elf -v didn't print version")
            safe_print(b2[-1024:])
            return 2
        print("[OK] /tcc.elf forwards -v to /tccraw.elf")

        # Step 3 — compile stock /hello.c (no flags) with the wrapper
        print("[+] /tcc.elf /hello.c -o /myhello.elf")
        ok, b3 = run_cmd(b"/tcc.elf /hello.c -o /myhello.elf", 120)
        if not ok:
            print("[!] tcc didn't return")
            safe_print(b3[-1024:])
            return 3
        if b"error" in b3.lower():
            print("[!] tcc reported errors:")
            safe_print(b3[-1024:])
            return 4

        # Step 4 — run the new binary
        ok, b4 = run_cmd(b"/myhello.elf", 20)
        if not ok:
            print("[!] /myhello.elf didn't exit")
            safe_print(b4[-1024:])
            return 5
        if b"hi from tcc" not in b4:
            print("[!] /myhello.elf didn't print the expected message:")
            safe_print(b4[-1024:])
            return 6

        print("[OK] tcc-compiled /myhello.elf ran and printed expected output")
        print("\n--- run output ---")
        safe_print(b4[-500:])
        return 0
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=3)
        except Exception:
            qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
