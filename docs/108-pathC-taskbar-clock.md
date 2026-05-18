# Session 121 — Path C phase 15: taskbar system clock

**Goal.** A glance at the time without launching wmclock.  The
compositor's own status chrome should answer "what time is it?".

Status: **done.** Smoke test (`smoke_wmclock_taskbar.py`, 4/4 pass):

```
=== pixel checks ===
  [OK] clock digits white pixels (476)
  [OK] 2x2 white blocks in clock (5)
  [OK] taskbar bg between Start and clock (85/85)
  [OK] Start button green (32/48)
```

Plus regression: all prior smoke tests (110-120) still green (the
two failures while iterating were a sample-coordinate issue in
`smoke_wmtaskbar.py` that the original test got lucky with).

What we verified:
- 476 white pixels live in the right-most 132 px of the bottom
  strip, in the y-band the 2x font produces.
- At least 5 of those whites sit inside a 2×2 block, confirming
  the scale-by-2 emission rather than a single 8-pixel-tall row.
- The middle of the taskbar is still empty taskbar background
  (no spurious client buttons).
- The Start button is still painted on the left.

---

## Layout

```
┌──────────────────────────────────────────────────────────────────┐
│ wmd - AdventOS Path C session 111                                │   ← top status bar
│                                                                  │
│  (windows live here)                                             │
│                                                                  │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ Start │ (window buttons) ... gap ...                  │ HH:MM   │   ← taskbar
└──────────────────────────────────────────────────────────────────┘
  64 px      stops at fb_w - 132                          132 px
```

`clock_w = 132` is wide enough for `HH:MM` at 2x (5 chars × 16 px
= 80 px) plus 4 px of left/right padding plus a 48-px cushion
that catches `space-padded` variants if we ever switch.

---

## Code

The new chunk is small and lives entirely inside `paint_taskbar`:

```c
int clock_w = 132;
int clock_x = fb_w - clock_w;
unsigned int ts = sys_time();
unsigned int min = (ts / 60u) % 60u;
unsigned int hr  = (ts / 3600u) % 24u;
char buf[6];
buf[0] = '0' + (hr / 10) % 10; buf[1] = '0' + hr % 10;
buf[2] = ':';
buf[3] = '0' + min / 10;       buf[4] = '0' + min % 10;
buf[5] = 0;
gfx_text_n(ctx, clock_x + 4, y + 6, buf, 2,
           GFX_WHITE, GFX_TRANSPARENT);
```

The button paint loop and the taskbar hit-test both got a matching
`btn_right_limit = clock_x - TASKBAR_BTN_PAD = 888` cap so window
buttons can't reach the clock region.  This caps the visible
window list at 5 buttons (= `(888 - 68) / 144`), which exceeds the
4 client slots `WM_MAX_WINDOWS` permits, so no real overflow.

---

## What gfx_text_n bought us

Session 120 shipped `gfx_text_n(ctx, x, y, s, scale, fg, bg)`.
This is its first use in wmd itself: a 2x clock without scratch
buffers or per-character open-coded expansion.  Adding the
feature was 8 lines.

If we ever want 3x for a weather-readout digit display or a
notification badge, that's the same call site with `scale = 3`.

---

## Other minor changes

- `smoke_wmtaskbar.py` — sample point moved from `(74, 753)` to
  `(138, 753)` (button centre, well clear of the title text).
  The old coord was *inside* the 'w' glyph at row 4; that the
  test happened to pass before was effectively luck — a different
  client title would have failed it.  Sampling the actual button
  centre is robust against window-title length.

---

## Files touched

- `user/wmd.c` — `paint_taskbar` grows the clock-render block and
  the `btn_right_limit` clamp; `taskbar_hit` mirrors the clamp.
- `smoke_wmclock_taskbar.py` — new harness, 4 pixel checks.
- `smoke_wmtaskbar.py` — sample-point correction (still 5/5 pass).
- `docs/108-pathC-taskbar-clock.md` — this file.

kernel.bin: 114864 (unchanged — userspace only).
wmd.bin: 18260 → 18452 (+192 bytes for the clock-render block +
the layout clamp).

---

## Path C status after session 121

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ✅ 117 — hover vs focus separation
- ✅ 118 — taskbar with click-to-focus
- ✅ 119 — Start button + launcher popup
- ✅ 120 — scalable fonts (gfx_text_n)
- ✅ 121 — taskbar clock
- ⏳ 122+ — multi-window-per-client (kernel-side per-task VA
          bumping), --gc-sections for user-prog links, desktop
          wallpaper

The compositor chrome is now feature-complete for normal use:
top status bar, draggable + closable + focusable windows with
mouse + keyboard routing, taskbar with one button per client and
a Start menu, system clock on the right.  The next sessions
move into structural changes inside the kernel (multi-window per
client) and shrinkers (user-program gc-sections).
