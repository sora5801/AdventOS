# Session 114 — Path C phase 8: keyboard input + wmtype demo

**Goal.** Get keystrokes to the focused window so a client can act
as a real text-input surface.  Session 113 routed mouse events;
phase 8 routes keys and ships the first WM client that does
something useful with them.

Status: **done.** Smoke test (`smoke_wmtype.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] wmtype focused title @ y=386 (215/280)
  [OK] close X red @ y=386 (9/12)
  [OK] focused bg @ (541,528) = (16, 24, 32)
  [OK] text white pixels in 'hey' band (95)
  [OK] footer 'chars=3' gray pixels (111)
  [OK] wmd status bar (887/924)
```

The flow: QMP injects mouse motion+click to focus wmtype, then 3
`send-key` events for "hey".  Screendump shows wmtype's status bar
in focused-blue, the three letters rendered via the 8x8 font, and
the "chars=3" footer — all evidence that keys flowed through the
kernel ring → wmd → SYS_WM_EVENT_PUSH → wmtype's queue → render.

---

## Where keys come from

The kernel already had `SYS_KBD_POLL` (session ~57) — non-blocking,
returns the next ASCII byte from the global keyboard ring (drained
by `usb_hid` or `keyboard.c` depending on path).  wmd just consumes
that ring each tick and forwards anything that arrived:

```c
for (int drained = 0; drained < 32; drained++) {
    int c = sys_kbd_poll();
    if (c <= 0) break;
    if (focused < 0
        || focused >= g_window_count
        || g_windows[focused].kind != KIND_CLIENT) continue;
    struct sys_wm_event ev = {0};
    ev.type    = WM_EV_KEY;
    ev.keycode = (unsigned int)c;
    sys_wm_event_push(g_windows[focused].client_id, &ev);
}
```

Routing decision: keystrokes go to `focused` (the most-recently
clicked client window), not to `target` (the hover window).
Click-to-focus matches user expectation — type doesn't follow the
mouse.

If `focused` is one of the demo windows (Clock / Gradient / About /
Color bars) or `-1`, keystrokes are dropped.  No `focused`-stealing
yet; the WM itself doesn't react to keys.

---

## The `wmtype` app

A 320 × 200 text scratchpad.  Whatever you type appears.

```c
struct wm_event ev;
while (wm_poll_event(&win, &ev)) {
    if (ev.type == WM_EV_KEY) {
        if (ev.keycode == 0x08)            { if (len > 0) len--; }
        else if (ev.keycode == '\n')       { buf[len++] = '\n'; }
        else if (ev.keycode >= 0x20
              && ev.keycode <= 0x7E)       { buf[len++] = ev.keycode; }
    }
}
```

Rendering uses libgfx by pointing a *synthetic* `gfx_ctx` at the
shared surface:

```c
static void make_surface_ctx(struct gfx_ctx *ctx, struct wm_window *w) {
    ctx->fb       = (volatile unsigned char *)w->pixels;
    ctx->fb_real  = ctx->fb;
    ctx->back     = 0;
    ctx->width    = w->w;
    ctx->height   = w->h;
    ctx->pitch    = w->w * 4;
    ctx->bpp      = 32;
    ctx->fb_size  = w->w * w->h * 4;
}
```

…then call `gfx_text(&sctx, x, y, ...)` to print into the surface.
libgfx clips against `ctx->width/height` so out-of-bounds writes
get dropped naturally.  The 8x8 font ships with libgfx; no extra
font asset needed.

`wmtype` also responds to a left click on the red "X" in the top
right by clearing the buffer — first concrete interaction between
mouse and keyboard input in the same app.

---

## Two QEMU input quirks the smoke test had to work around

1. **PS/2 button coalescing** (carried over from session 113).
   Click hold ≥ 0.8s before release, or the down-edge vanishes
   from the polled byte stream.

2. **USB-HID key pacing**.  `send-key` events fired faster than
   ~0.5s apart get partly dropped at the USB host-controller polling
   rate.  At 0.5s per key, the 3-letter "hey" sequence delivers
   reliably.  At 0.2s, 4 of 5 keys went missing.  Real users type
   at ~200ms inter-key intervals — too fast.  In the real-hardware
   path the USB HID polling rate is 10ms, so it shouldn't matter
   there; this is a QEMU quirk specifically.

---

## Files touched

- `user/wmd.c` — keyboard poll + forward in main loop (~15 lines)
- `user/wmtype.c` — new, ~150 lines (text scratchpad + close button)
- `build.sh` — `wmtype` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmtype.elf` + man page in the image
- `fs/man/wmtype` — new
- `smoke_wmtype.py` — new headless harness, 6 pixel checks
- `docs/101-pathC-keyboard.md` — this file

kernel.bin: 114864 (unchanged — pure userspace change).
wmd.bin: 14448 → 14608 (+160 B for the keyboard drain loop).
wmtype.bin: new, 11580 bytes.

---

## Path C status after session 114

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing + first text-input client
- ⏳ 115 — more apps (wmclock with display modes, wmpaint with
          mouse-drag drawing)
- ⏳ 116+ — multi-window per client, damage rects, real WM polish

Session 115 will land two more concrete apps:
- **wmclock**: a digital clock that responds to space-bar to toggle
  12/24-hour mode (combines keyboard + the timer-driven render
  pattern).
- **wmpaint**: a drag-to-draw canvas (combines MOUSE_MOVE while
  pressed + dragging + per-pixel writes).

Both are small but they prove the WM client protocol can host
non-trivial userland programs.
