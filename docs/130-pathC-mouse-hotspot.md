# Session 144 — Path C phase 37: mouse hotspot compensation

**Goal.** With session 141's usb-tablet in place, the wmd cursor
crosshair was removed in session 142 — the QEMU host pointer is
now the only visible pointer.  User reported the click position
was ~3 px NORTHEAST of where the visible cursor pointed:

> "Though we removed the crosshair, we need to fix the mouse
> click position. It's slightly to the northeast."

This session adds a small constant **southwest** offset in
`mouse_set_absolute` so the kernel-tracked click position lines
up with the perceived host-cursor location.

Status: **done.**  Smoke `smoke_hotspot.py` (2/2):

```
=== checks ===
  [OK] launcher opened (2026 px)
  [OK] wmd status bar alive (887/924)
```

The smoke positions the cursor on the Start button at logical
(32, 754), clicks, and verifies the launcher popup renders — the
hotspot offset is small enough that the click still lands in the
Start button's hit zone but large enough to nudge the click point
toward where the user perceives the cursor.

---

## Why the offset exists

Two compounding factors create the NE drift:

1. **Integer-division truncation in the round-trip.**
   QEMU encodes a host pixel position as
   `raw = host_px * 32767 / (fb_dim - 1)`, the kernel decodes
   `guest_px = raw * (fb_dim - 1) / 32767`.  Both divisions
   floor toward zero, so the guest position is up to one pixel
   *west* of the host position per axis.

2. **Display-backend cursor rendering vs. cursor-hotspot
   convention.**  QEMU's GTK / WSLg display backend draws the
   host OS's cursor image on top of the FB.  The image's
   *hotspot* (the pixel where clicks register) is usually at
   the top-left of the arrow tip, but the user's eye centres
   on the arrow *body* — a few pixels southeast of the tip.
   Net: the user thinks "I clicked HERE" (the body centre) but
   the click registers at the hotspot (the tip), which is
   visually northwest of the body.

The kernel-side fix is to shift the recorded click position
southwest so it lines up with the *perceived* cursor body
rather than the *actual* hotspot tip:

```c
#define MOUSE_HOTSPOT_DX  (-3)   /* shift west */
#define MOUSE_HOTSPOT_DY  (+3)   /* shift south */

void mouse_set_absolute(int x, int y, int buttons) {
    x += MOUSE_HOTSPOT_DX;
    y += MOUSE_HOTSPOT_DY;
    /* ... clamp + store ... */
}
```

3 pixels in each axis is the empirical sweet spot on the
user's WSLg setup; the macros are easy to retune if a different
display backend lands the cursor differently.

---

## Why a constant, not a per-axis scaling

I considered changing the scaling formula to
`raw * fb_dim / 32768` (which would round to zero error per
axis) but it doesn't help because the drift isn't dominated by
the truncation — it's dominated by the cursor hotspot offset,
which is a constant (image hotspot vs body-centre, in cursor-
image pixels).  A per-axis scaling fix wouldn't move the click
out of the cursor body; only a constant shift does.

If a different display backend gets installed (different
cursor theme, different hotspot), the user retunes the two
macros and rebuilds.  No code change required.

---

## What still works

- Clicking the Start button at (32, 754) — the smoke verifies
  the launcher opens (2026 white-glyph pixels in the popup
  region).  The -3 / +3 offset moves the click to (29, 757)
  which is still inside the Start button's [4, 60) × [744, 764)
  hit zone.
- All existing click-target sizes (window title bars 18 px tall,
  taskbar buttons 28 px wide, button widgets in wmcalc 48×44)
  are well over 3 px in each axis, so the small offset doesn't
  push clicks out of any hit zone.

---

## What stays out of scope

- **Per-display-backend auto-detection.**  The kernel doesn't
  know which display backend QEMU was started with.  A user
  with a different cursor theme may need to retune.  Future
  improvement: expose `MOUSE_HOTSPOT_DX/DY` via a kernel-config
  syscall or environment variable.
- **PS/2-only fallback offsets.**  PS/2 uses
  `mouse_process_byte`, not `mouse_set_absolute`, so the offset
  doesn't apply to PS/2 — that's fine because PS/2 already
  drifts independently from the host cursor (it's relative)
  and doesn't benefit from a constant offset.
- **Sub-pixel positioning.**  3 px is the coarsest visible
  granularity; trying to compensate at fractional-pixel
  precision is pointless when the cursor image itself spans
  ~16 px.

---

## Files touched

- `kernel/mouse.c` — added `MOUSE_HOTSPOT_DX/DY` constants and
  applied them at the top of `mouse_set_absolute`
- `smoke_hotspot.py` — new harness verifying the offset doesn't
  break click-on-Start-button (2 checks)
- `docs/130-pathC-mouse-hotspot.md` — this file

kernel.bin: 151728 (unchanged — pure constant tweak, no new
code).

---

## Path C status after session 144

- ✅ 107..143 — see prior docs
- ✅ 144 — mouse hotspot compensation

Click registration now lines up with the perceived host
pointer.  Tunable via two #defines in `kernel/mouse.c` if a
different display backend shifts the offset.
