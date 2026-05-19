#!/usr/bin/env python3
"""
Path A polish smoke test: dynamic prompt reflects the cwd.

Boots QEMU headless, then drives the shell over serial to verify:
  /        -> "advent$ "
  /mnt     -> "advent/mnt$ "      (after `cd mnt`)
  /        -> "advent$ "          (after `cd ..`)
  /etc     -> "advent/etc$ "      (after `cd /etc`)

The change being verified: user/sh.c:current_prompt() now builds a
dynamic "advent<cwd>$ " when $PS1 is unset (the default), so the prompt
tracks `cd`. PS1 override still wins if set.
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4497


def safe_print(b):
    s = b.decode("ascii", errors="replace")
    out = []
    for ch in s:
        if ch == "\n" or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append("?")
    print("".join(out))


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
    log = open(os.path.join(ROOT, "qemu-prompt.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)

    time.sleep(2.0)
    rc = 1
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
            print("[!] never saw initial prompt at root")
            safe_print(buf[-2048:])
            return 1
        print("[+] root prompt OK: 'advent$ '")
        time.sleep(0.3)

        # cd mnt -> expect "advent/mnt$ "
        ser.sendall(b"cd mnt\n")
        ok, buf = wait_for(ser, b"advent/mnt$ ", buf, timeout=10)
        if not ok:
            print("[!] expected 'advent/mnt$ ' after 'cd mnt'; last 1KB:")
            safe_print(buf[-1024:])
            return 1
        print("[+] cd mnt    -> 'advent/mnt$ ' OK")

        # cd .. -> expect "advent$ " again (without the /mnt suffix)
        time.sleep(0.3)
        ser.sendall(b"cd ..\n")
        # We need to see a fresh "advent$ " emitted AFTER our command. The
        # easiest reliable marker is a newline-anchored prompt.
        marker = b"\nadvent$ "
        ok, buf = wait_for(ser, marker, buf, timeout=10)
        if not ok:
            print("[!] expected '\\nadvent$ ' after 'cd ..'; last 1KB:")
            safe_print(buf[-1024:])
            return 1
        print("[+] cd ..     -> 'advent$ ' OK")

        # cd /etc -> expect "advent/etc$ "
        time.sleep(0.3)
        ser.sendall(b"cd /etc\n")
        ok, buf = wait_for(ser, b"advent/etc$ ", buf, timeout=10)
        if not ok:
            print("[!] expected 'advent/etc$ ' after 'cd /etc'; last 1KB:")
            safe_print(buf[-1024:])
            return 1
        print("[+] cd /etc   -> 'advent/etc$ ' OK")

        # PS1 override still wins.
        time.sleep(0.3)
        ser.sendall(b"export PS1=fixed$ \n")
        ok, buf = wait_for(ser, b"fixed$ ", buf, timeout=10)
        if not ok:
            print("[!] PS1 override never took effect; last 1KB:")
            safe_print(buf[-1024:])
            return 1
        print("[+] PS1 override still works")

        rc = 0
        print("\nALL CHECKS PASSED")
    finally:
        try: ser.close()
        except: pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except: qemu.kill()
        log.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
