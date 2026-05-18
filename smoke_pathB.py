#!/usr/bin/env python3
"""
Session 121 smoke test: Path B Phase 4 capstone.

Boots QEMU headless, runs `cc /capstone.c -o /cap.elf` to compile the
session-121 sample, then `/cap.elf` to run it. Verifies the output
matches the expected lines that exercise SBV returns, static/extern,
and the function-pointer typedef syntax.
"""
import os, socket, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4451


def safe_print(b):
    """Print bytes safely on Windows consoles. Strip anything that's not
    plain printable ASCII so cp1252 / chcp 65001 / piped output all work."""
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
    log = open(os.path.join(ROOT, "qemu-s112.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1",
        "-display", "none",
        # wait=on makes QEMU block on the serial accept until we connect,
        # so the boot banner isn't lost before the client attaches.
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)

    time.sleep(2.0)
    try:
        # Retry connecting — QEMU sometimes takes a moment to bind the
        # serial socket on slow hosts.
        ser = None
        for attempt in range(10):
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
            print("[!] never saw shell prompt; last ~2KB of serial:")
            safe_print(buf[-2048:])
            return 1
        print("[+] shell up; compiling capstone.c")
        time.sleep(0.3)   # let the shell settle before sending
        ser.sendall(b"cc /capstone.c -o /cap.elf\n")
        # cc is a 300 KiB binary that has to be paged in from the ATA disk
        # for its first invocation; subsequent runs are warm. 120s allows
        # plenty of headroom for the cold path.
        ok, buf = wait_for(ser, b"cc: wrote /cap.elf", buf, timeout=120)
        if not ok:
            print("[!] cc never reported success; QEMU alive?",
                  qemu.poll() is None)
            print("    last 2KB of serial:")
            safe_print(buf[-2048:])
            return 1
        print("[+] cc reported success; running /cap.elf")
        buf = b""
        ser.sendall(b"/cap.elf\n")
        ok, buf = wait_for(ser, b"op=s_mul: op(3,5) = 15", buf, timeout=30)
        if not ok:
            print("[!] capstone never finished; last 2KB of serial:")
            safe_print(buf[-2048:])
            return 1
        # Measure cap.elf size to track codegen-quality regressions.
        time.sleep(0.3)
        size_buf = b""
        ser.sendall(b"wc -c /cap.elf\n")
        # Wait for `wc`'s output. The shell prompt after will be "advent$"
        # which appears AFTER the wc output line. Allow some flexibility.
        deadline = time.time() + 8
        while time.time() < deadline:
            try:
                ser.settimeout(0.5)
                chunk = ser.recv(4096)
                if chunk:
                    size_buf += chunk
                else:
                    break
            except (socket.timeout, ConnectionResetError):
                if b"advent$" in size_buf and b"cap.elf" in size_buf:
                    break

        out = buf.decode("ascii", errors="replace") + size_buf.decode("ascii", errors="replace")
        print("\n=== capstone output ===")
        for line in out.splitlines():
            line = line.strip()
            if line and (line.startswith("make_point")
                         or line.startswith("shift")
                         or line.startswith("p untouched")
                         or line.startswith("s_module_counter")
                         or line.startswith("squared")
                         or line.startswith("op=")):
                print(f"  {line}")

        # Extract cap.elf size from the `wc -c /cap.elf` output.
        # wc.elf typically prints "<num> /cap.elf" or just "<num>".
        import re
        size_text = size_buf.decode("ascii", errors="replace")
        m = re.search(r"\b(\d{3,})\b\s*/?cap\.elf", size_text)
        if not m:
            m = re.search(r"\n\s*(\d{3,})\b", size_text)
        if m:
            print(f"\n  cap.elf size = {m.group(1)} bytes")
        else:
            print("\n  (could not parse cap.elf size from wc output)")

        expectations = [
            "make_point(3,4) = (3, 4)",
            "shift(p,10,20)  = (13, 24)",
            "p untouched     = (3, 4)",
            "shift_x_only    = (103, 4)",
            "s_module_counter = 45",
            "squared(7)       = 49",
            "op=s_add: op(3,5) = 8",
            "op=s_mul: op(3,5) = 15",
        ]
        print("\n=== expected lines ===")
        all_ok = True
        for line in expectations:
            present = line in out
            print(f"  [{'OK' if present else 'FAIL'}] {line}")
            if not present:
                all_ok = False

        return 0 if all_ok else 2
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=3)
        except Exception:
            qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
