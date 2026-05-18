#!/usr/bin/env python3
"""
Session 135 — smoke for the FILE * write path used by tcc -c.
Boots QEMU, runs /fwtest.elf, prints whatever serial output it gets.
"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4455


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
    log = open(os.path.join(ROOT, "qemu-fwtest.log"), "w")
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
        if ser is None: return 1
        buf = b""
        ok, buf = wait_for(ser, b"advent$ ", buf, timeout=60)
        if not ok: return 1
        buf = b""
        time.sleep(0.3)
        ser.sendall(b"/fwtest.elf\n")
        ok, buf = wait_for(ser, b"[fwtest] DONE", buf, timeout=60)
        if not ok:
            print("[!] fwtest did not reach DONE")
        print("\n--- output ---")
        safe_print(buf[-3000:])
        return 0 if ok else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
