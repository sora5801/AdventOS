#!/usr/bin/env python3
"""
Session 134 smoke test: tcc.elf running inside AdventOS.

Boots QEMU, tries /tcc.elf -v to confirm the binary loads and prints
its version banner. If that works, tries `tcc /hello.c -o /thello.elf`
and reports the result.

This is an aspirational smoke — there's a good chance tcc dies before
the -v banner (any number of libuser interactions could trip). The
script captures whatever happens for diagnosis.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4454


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

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-tcc.log"), "w")
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
            safe_print(buf[-2048:])
            return 1

        # Step 1: tcc -v should print its self-version
        print("[+] /tcc.elf -v")
        time.sleep(0.3)
        ser.sendall(b"/tcc.elf -v\n")
        ok, buf = wait_for(ser, b"tcc version", buf, timeout=30)
        if not ok:
            print("[!] tcc -v didn't print its version banner")
            safe_print(buf[-4096:])
            return 2
        print("[OK] tcc reports its version banner (binary loads + runs)")
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=10)
        buf = b""  # reset for next command

        # Step 2: try -h to make sure tcc's normal startup path works
        # beyond the trivial -v branch.
        print("[+] /tcc.elf -h (sanity)")
        time.sleep(0.3)
        ser.sendall(b"/tcc.elf -h\n")
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=30)
        text_h = buf.decode("ascii", errors="replace")
        if "Usage" in text_h or "usage" in text_h or "options" in text_h:
            print("[OK] tcc -h printed usage")
        else:
            print("[!] tcc -h didn't print expected usage; last 1KB:")
            safe_print(buf[-1024:])
        buf = b""

        # Step 3a: -E preprocess only — lightest possible compile path
        print("[+] /tcc.elf -E /thello.c (preprocess only)")
        time.sleep(0.3)
        ser.sendall(b"/tcc.elf -E /thello.c\n")
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=60)
        text = buf.decode("ascii", errors="replace")
        print(f"[{'OK' if ok else 'TIMEOUT'}] -E returned in time")
        buf = b""

        # Step 3b: try -c (compile to object file)
        print("[+] /tcc.elf -c /thello.c -o /thello.o")
        time.sleep(0.3)
        ser.sendall(b"/tcc.elf -c /thello.c -o /thello.o\n")
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=180)
        text = buf.decode("ascii", errors="replace")

        print("\n--- last 4KB of full buffer ---")
        safe_print(buf[-4096:])
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
