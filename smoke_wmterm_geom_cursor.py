#!/usr/bin/env python3
"""
Session 162 smoke: two interactive fixes.

1. wmterm window grew from 240 -> 270 px tall so all 24 grid rows
   fit inside the surface.  Before this, GRID_Y(24) + ROWS(24) *
   LINE_H(10) = 264 px of content was clipped to 240 px of
   surface — the bottom 2-3 rows were off-screen.  After `ls /`
   scrolled the content up, the new prompt landed in those
   invisible rows.  We verify by checking pixel content at
   screen y > 458 (which would have been outside the old surface).

2. wmd now draws its own arrow cursor sprite at the host pointer
   coordinates.  QEMU's host pointer shape changes when re-entering
   the window from an edge (Windows leaves the resize arrow behind);
   the guest overlay gives a stable visual.  We verify by moving
   the cursor to a known position and checking the bitmap pixels.
"""
import os, socket, json, subprocess, time, sys, threading

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
        try: chunk = sock.recv(4096)
        except socket.timeout: continue
        if not chunk: return False, buf
        buf += chunk
        if marker in buf: return True, buf
    return False, buf


def abs_send(q, qbuf, x, y, fb_w=1024, fb_h=768):
    ax = 32767 * x // (fb_w - 1)
    ay = 32767 * y // (fb_h - 1)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}]})


def click(q, qbuf, x, y):
    abs_send(q, qbuf, x, y); time.sleep(0.3)
    abs_send(q, qbuf, x, y); time.sleep(0.5)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": True, "button": "left"}}]})
    time.sleep(0.4)
    qmp_cmd(q, qbuf, "input-send-event", {"events": [
        {"type": "btn", "data": {"down": False, "button": "left"}}]})
    time.sleep(1.0)


def send_qkey(q, qbuf, code):
    qmp_cmd(q, qbuf, "send-key",
            {"keys": [{"type": "qcode", "data": code}]})
    time.sleep(0.12)


def type_str(q, qbuf, s):
    qmap = {' ': 'spc', '/': 'slash', '.': 'dot', '\n': 'ret'}
    for ch in s:
        if ch in qmap: send_qkey(q, qbuf, qmap[ch])
        elif ch.isalnum(): send_qkey(q, qbuf, ch.lower())


def read_ppm(path):
    with open(path, "rb") as f:
        f.readline()
        line = f.readline()
        while line.startswith(b"#"): line = f.readline()
        w, h = map(int, line.split())
        int(f.readline().strip())
        return w, h, f.read()


def px(p, w, x, y):
    i = (y * w + x) * 3
    return p[i], p[i+1], p[i+2]


def near(a, b, tol=15):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


def main():
    subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                   capture_output=True)
    time.sleep(0.5)
    log = open(os.path.join(ROOT, "qemu-s162.log"), "w")
    qemu = subprocess.Popen([
        "qemu-system-i386", "-drive", f"format=raw,file={OS_IMG}",
        "-m", "32", "-smp", "1", "-vga", "std", "-display", "none",
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
        ser.sendall(b"wmd 60 --clean &\n"); time.sleep(2.0)
        ser.sendall(b"wmterm -v 90 &\n");   time.sleep(5.0)

        ser_log = bytearray(ser_buf)
        ser_lock = threading.Lock()
        ser_stop = threading.Event()
        def drainer():
            ser.settimeout(0.2)
            while not ser_stop.is_set():
                try:
                    c = ser.recv(4096)
                    if not c: break
                    with ser_lock: ser_log.extend(c)
                except (socket.timeout, OSError): continue
        threading.Thread(target=drainer, daemon=True).start()

        def trace():
            with ser_lock:
                return bytes(ser_log).decode("utf-8", "replace")

        q = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=5)
        qbuf = b""
        _, qbuf = qmp_recv(q, qbuf)
        qmp_cmd(q, qbuf, "qmp_capabilities"); qbuf = b""

        def shot(name):
            path = os.path.join(ROOT, f"shot_geom_{name}.ppm")
            if os.path.exists(path): os.remove(path)
            qmp_cmd(q, qbuf, "screendump", {"filename": path, "format": "ppm"})
            deadline = time.time() + 5
            while time.time() < deadline and (not os.path.exists(path)
                                              or os.path.getsize(path) < 100):
                time.sleep(0.1)
            return read_ppm(path)

        # ---- Cursor sprite test ----
        # Move the mouse to a known position far from any window so
        # the cursor sits on the wallpaper alone.  Verify the wmd-drawn
        # arrow's distinctive black outline + white fill bitmap is
        # present there.  The hot-spot is the top-left tip, so the
        # bitmap occupies (cursor_x, cursor_y) -> (cursor_x+11, cursor_y+15).
        # Retry the abs-send a few times — QEMU's USB tablet
        # occasionally drops the first event of a session and the
        # cursor stays at the old position.  We re-send and re-shot
        # until the cursor bitmap appears where we asked, or we give
        # up.
        print("[+] cursor sprite test")
        cur_x, cur_y = 700, 100
        outline_hit = False
        fill_hit = False
        for attempt in range(5):
            abs_send(q, qbuf, cur_x, cur_y); time.sleep(0.4)
            abs_send(q, qbuf, cur_x, cur_y); time.sleep(0.8)
            w, h, pxc = shot("cursor")
            outline_hit = False
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    p = px(pxc, w, cur_x + dx, cur_y + dy)
                    if near(p, (0, 0, 0), tol=15):
                        outline_hit = True; break
                if outline_hit: break
            fill_hit = False
            for dy in range(3, 10):
                for dx in range(1, 6):
                    p = px(pxc, w, cur_x + dx, cur_y + dy)
                    if near(p, (0xFF, 0xFF, 0xFF), tol=15):
                        fill_hit = True; break
                if fill_hit: break
            if outline_hit and fill_hit:
                print(f"   cursor sprite found on attempt {attempt+1}")
                break
            print(f"   attempt {attempt+1}: outline={outline_hit} "
                  f"fill={fill_hit}, retrying abs_send")
        print(f"   cursor outline pixel near tip: {outline_hit}")
        print(f"   cursor white-fill pixel in body: {fill_hit}")

        # ---- wmterm geometry test ----
        # Focus wmterm and run `ls /` so output overflows the visible
        # grid.  The bottom of the wmterm body should now show the
        # `advent$` prompt (rendered after ls's scroll-up); before
        # session 162 it would have been clipped off-screen.  We
        # check by sampling pixels at the new bottom row.
        print("[+] focus wmterm + run ls /")
        for _ in range(10):
            click(q, qbuf, 300, 350); time.sleep(1.0)
            if "wmterm: FOCUS" in trace(): break
        type_str(q, qbuf, "ls /")
        send_qkey(q, qbuf, "ret"); time.sleep(3.5)

        # wmterm body color is 0x080808.  Old surface ended at screen
        # y = 200 + 240 = 440 (window y + WIN_H).  New surface ends at
        # screen y = 200 + 18 + 270 = 488.  Sample at y=465 (well past
        # old end, well inside new surface) inside the wmterm window
        # column range (x = 110..640).
        w, h, pxg = shot("geom")
        # Count "wmterm body or text" pixels at the row where we used
        # to have nothing (off-surface).  Body is dark grey 0x080808;
        # rendered text is sage green 0xC0E0C0.
        body_or_text = 0
        for x in range(110, 640):
            p = px(pxg, w, x, 465)
            if near(p, (0x08, 0x08, 0x08), tol=12) or \
               near(p, (0xC0, 0xE0, 0xC0), tol=25):
                body_or_text += 1
        print(f"   wmterm-content px at y=465 (was clipped): {body_or_text}")
        bottom_visible = body_or_text > 400  # ~530 px wide if fully present

        ser_stop.set(); time.sleep(0.3)
        with ser_lock:
            ser_buf = bytes(ser_log)
        with open(os.path.join(ROOT, "geom_serial.log"), "w") as f:
            f.write(ser_buf.decode("utf-8", "replace"))

        checks = [
            ("wmd cursor arrow outline visible",       outline_hit),
            ("wmd cursor arrow white fill visible",    fill_hit),
            ("wmterm bottom rows now on-screen",       bottom_visible),
        ]
        print("\n=== checks ===")
        ok_all = True
        for n, p_ in checks:
            print(f"  [{'OK' if p_ else 'FAIL'}] {n}")
            if not p_: ok_all = False
        return 0 if ok_all else 2
    finally:
        try: qemu.terminate(); qemu.wait(timeout=3)
        except: qemu.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
