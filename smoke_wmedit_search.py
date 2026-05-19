#!/usr/bin/env python3
"""
Session 152 smoke: wmedit Ctrl-F search.

Boots wmd + wmedit on a pre-populated file containing
"abcdef abcdef abcdef" so we have 3 known matches of "abc".
Click into wmedit, press Ctrl-F, type "abc"; verify the search
bar appears in the footer (mustard yellow band) and that match
positions in the body are highlighted (yellow cells under the
'a' 'b' 'c' characters).

Pixel checks:
  - search-mode footer is mustard (dark yellow ~0x403820)
  - at least one yellow match-highlight cell in the body
  - wmd top status bar alive
"""
import os, socket, json, subprocess, time, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OS_IMG = os.path.join(ROOT, "os.img")
QMP_PORT = 4501
SERIAL_PORT = 4502


def qmp_recv(s, buf):
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk: return None, buf
        buf += chunk
    line, _, rest = buf.partition(b"\n")
    return json.loads(line.decode()), rest


def qmp_cmd(s, buf, cmd, args=None):
    msg = {"execute": cmd}
    if args: msg["arguments"] = args
    s.sendall((json.dumps(msg) + "\r\n").encode())
    while True:
        rep, buf = qmp_recv(s, buf)
        if rep is None: return None, buf
        if "return" in rep or "error" in rep: return rep, buf


def wait_for(sock, marker, buf, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk: return False, buf
        buf += chunk
        if marker in buf: return True, buf
    return False, buf


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        pixels = f.read()
    return w, h, pixels


def near(a, b, tol=15):
    return all(abs(int(x)-int(y)) <= tol for x, y in zip(a, b))


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)

    log = open(os.path.join(ROOT, "qemu-s152.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1",
        "-vga", "std",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=on",
        "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server=on,wait=off",
        "-device", "piix3-usb-uhci,id=usb0",
        "-device", "usb-kbd,bus=usb0.0",
        "-device", "usb-tablet,bus=usb0.0",
    ], stdout=log, stderr=subprocess.STDOUT)

    time.sleep(1.0)
    try:
        ser = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=5)
        ser_buf = b""
        ok, ser_buf = wait_for(ser, b"$ ", ser_buf, timeout=30)
        if not ok: return 1
        ser.sendall(b"wmd 60 --clean &\n")
        time.sleep(2.0)
        ser.sendall(b"wmedit /tmp/find 50\n")
        time.sleep(4.0)

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities")
        qbuf = b""

        # Click in wmedit body to focus.  wmedit outer (100, 200,
        # 644, 420); surface at (101, 218).  Click at (300, 280).
        print("[+] click wmedit body for focus")
        abs_send(q, qbuf, 300, 280)
        time.sleep(0.8)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}
        ]})
        time.sleep(0.3)
        qmp_cmd(q, qbuf, "input-send-event", {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}
        ]})
        time.sleep(1.0)

        # Type "abcdef abcdef abcdef" — 3 known matches of "abc".
        print("[+] type 'abcdef abcdef abcdef'")
        for k in list("abcdef") + ["spc"] + list("abcdef") + ["spc"] + list("abcdef"):
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.12)
        time.sleep(1.0)

        # Press Ctrl-F to open search.
        print("[+] Ctrl-F")
        qmp_cmd(q, qbuf, "send-key",
                {"keys": [{"type": "qcode", "data": "ctrl"},
                          {"type": "qcode", "data": "f"}]})
        time.sleep(1.0)

        # Type "abc" — should highlight 3 matches.
        for k in ["a", "b", "c"]:
            qmp_cmd(q, qbuf, "send-key",
                    {"keys": [{"type": "qcode", "data": k}]})
            time.sleep(0.3)
        time.sleep(1.0)

        SHOT = os.path.join(ROOT, "shot_search.ppm")
        if os.path.exists(SHOT): os.remove(SHOT)
        qmp_cmd(q, qbuf, "screendump", {"filename": SHOT, "format": "ppm"})
        deadline = time.time() + 5
        while time.time() < deadline and (not os.path.exists(SHOT)
                                          or os.path.getsize(SHOT) < 100):
            time.sleep(0.1)
        w, h, pixels = read_ppm(SHOT)
        def px(x, y):
            i = (y * w + x) * 3
            return pixels[i], pixels[i+1], pixels[i+2]

        # Footer of wmedit window: outer y range 200..200+420-1=619.
        # FOOTER_H=14, so footer is at surface y = WIN_H - FOOTER_H ..
        # WIN_H - 1 = 386..399.  Screen: 200 + TITLE_H_wmd-applied ...
        # Actually wmd's title TITLE_H=18 overwrites surface y=0..17,
        # then wmedit's body runs y=18..(420-1).  Surface fills the
        # whole 640x420 window.  The wmedit footer is at surface y =
        # 400 - 14 = 386..399.  Screen y = 200 + TITLE_H + 386...
        # wait — wmedit's WIN_W=640, WIN_H=400.  But the window
        # struct in wmd uses m.w/m.h + chrome, so outer = (640+4,
        # 400+18+2) = (644, 420).
        # Surface position on screen: (101, 218).  Surface y=386..399
        # -> screen y = 218 + 386 = 604.
        # Footer mustard band at screen y=604..617.  Sample
        # interior.
        mustard = 0
        for yy in range(606, 615):
            for xx in range(108, 600):
                if near(px(xx, yy), (0x40, 0x38, 0x20), tol=20):
                    mustard += 1
        print(f"   search-mode footer mustard pixels: {mustard}")

        # Match highlight cells.  After typing 3 matches of "abc"
        # in the body, with cursor jumped to the LAST match
        # (after typing "abc" again to refine the search, cursor
        # jumps forward).  Matches are at byte 0, 7, 14.
        # On the displayed row, "abc" is the first 3 characters,
        # then space, "abc" again at col 7, etc.  Match highlight
        # is 0x807030 (yellow mustard).  Body row 0 is at surface
        # y=22..31 (HDR_H=18 + 4); screen y=240..249.  Cell width
        # CELL_W=8, grid x = 6 -> screen x = 107.  "abc" highlight
        # spans 24 px wide.
        match_yellow = 0
        for yy in range(240, 252):
            for xx in range(107, 240):
                if near(px(xx, yy), (0x80, 0x70, 0x30), tol=25):
                    match_yellow += 1
        print(f"   match highlight yellow pixels: {match_yellow}")

        # wmd top status bar.
        sb = sum(1 for xx in range(50, w - 50)
                 if near(px(xx, 6), (0x20, 0x20, 0x20), tol=10))

        checks = []
        checks.append((f"search bar mustard footer ({mustard} px)",
                       mustard > 200))
        checks.append((f"match highlights in body ({match_yellow} yellow px)",
                       match_yellow > 30))
        checks.append((f"wmd status bar alive ({sb}/{w-100})",
                       sb > (w - 100) * 0.70))

        print("\n=== checks ===")
        ok_all = True
        for name, passed in checks:
            print(f"  [{'OK' if passed else 'FAIL'}] {name}")
            if not passed: ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except Exception: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
