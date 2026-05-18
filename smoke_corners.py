#!/usr/bin/env python3
"""
Session 125 smoke test: cc language-corners batch.

Boots QEMU, compiles /corners.c with cc, runs the resulting binary,
and checks that each new language feature produced the expected
output line. Each new commit on the language-corners branch adds a
new feature and a new check here.
"""
import os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
SERIAL_PORT = 4453


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


# Lines we expect /corners.elf to print. Each feature contributes one
# (or more) anchor lines; the smoke fails if any are missing. New
# features add new lines as commits go in.
EXPECTED = [
    "comma            = 30",
    "b_and_eq         = 6",
    "b_or_eq          = 22",
    "b_xor_eq         = 4",
    "b_shl_eq         = 16",
    "b_shr_eq         = 2",
    "do_while_sum     = 15",
    "break_continue   = 19",
    "union_as_int     = 42",
    "union_as_str     = ok",
    "sizeof_x         = 4",
    "sizeof_view      = 4",
    "sizeof_u         = 4",
    "goto_sum         = 6",
    "assign_expr_a    = 10",
    "assign_expr_b    = 7",
    "assign_expr_c    = 10",
    "switch_total     = 122",
    "array2d_sum      = 21",
]


def main():
    subprocess.run(["taskkill", "/IM", "qemu-system-i386.exe", "/F"],
                   capture_output=True)
    time.sleep(0.5)

    print("[+] starting QEMU...")
    log = open(os.path.join(ROOT, "qemu-corners.log"), "w")
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

        print("[+] compiling /corners.c")
        time.sleep(0.3)
        ser.sendall(b"cc /corners.c -o /cor.elf\n")
        ok, buf = wait_for(ser, b"cc: wrote /cor.elf", buf, timeout=120)
        if not ok:
            print("[!] cc never reported success; last 2KB:")
            safe_print(buf[-2048:])
            return 1

        print("[+] running /cor.elf")
        buf = b""
        ser.sendall(b"/cor.elf\n")
        # Wait for the FINAL expected line (latest commit's last check).
        last = EXPECTED[-1].encode("ascii")
        ok, buf = wait_for(ser, last, buf, timeout=30)
        if not ok:
            print("[!] /cor.elf never reached the final expected line")
            safe_print(buf[-2048:])
            return 1

        # Measure cor.elf size to track codegen quality.
        time.sleep(0.3)
        size_buf = b""
        ser.sendall(b"wc -c /cor.elf\n")
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
                if b"advent$" in size_buf and b"cor.elf" in size_buf:
                    break

        out = buf.decode("ascii", errors="replace")
        size_text = size_buf.decode("ascii", errors="replace")
        m = re.search(r"\b(\d{3,})\b\s*/?cor\.elf", size_text)
        if not m:
            m = re.search(r"\n\s*(\d{3,})\b", size_text)
        size_str = m.group(1) if m else "?"

        print(f"\n  cor.elf size = {size_str} bytes\n")
        print("=== expected lines ===")
        all_ok = True
        for line in EXPECTED:
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
