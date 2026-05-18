# Session 115 — Path C phase 9: wmclock + wmpaint

**Goal.** Ship two real WM client apps that exercise the protocol
end to end.  Sessions 112–114 built the plumbing; phase 9 proves
it can host non-trivial user programs running side by side.

Status: **done.** Smoke test (`smoke_wmapps.py`, 7/7 pass):

```
=== pixel checks ===
  [OK] wmclock title @ y=380 (170/170)
  [OK] wmclock green time digits (96)
  [OK] wmpaint red swatch (16/16)
  [OK] wmpaint green swatch (16/16)
  [OK] wmpaint white stroke pixels (281)
  [OK] wmpaint untouched bg @ lower-left (3800)
  [OK] wmd status bar (887/924)
```

Both apps are visible **at the same time** alongside wmd's four
demo windows (Clock, Gradient, About, Color bars).  That's eight
overlapping surfaces composited per frame: four daemon-internal +
two client (wmclock, wmpaint) + cursor + status bar.

---

## wmclock — timer-driven render + keyboard toggle

A 260 × 100 window showing `HH:MM:SS` in a 2x-scaled 8x8 font.
The clock polls `sys_time()` every 250 ms and re-renders.  Pressing
`SPACE` while focused toggles between 24-hour and 12-hour mode.

The interesting bit is the 2x scaling.  libgfx ships only the
8x8 font, so wmclock paints each glyph into a 64-pixel scratch
buffer (a stack-allocated `unsigned int[64]` with a synthetic
`gfx_ctx` pointing at it), then expands every set pixel into a
2×2 block in the surface:

```c
unsigned int scratch[64];
struct gfx_ctx s2 = { ..., .fb = (volatile unsigned char *)scratch,
                      .width = 8, .height = 8, .pitch = 32, .bpp = 32 };
gfx_glyph(&s2, 0, 0, c, rgb, GFX_TRANSPARENT);
for (int yy = 0; yy < 8; yy++)
    for (int xx = 0; xx < 8; xx++)
        if (scratch[yy*8 + xx]) {
            wm_put_pixel(w, x + 2*xx,     y + 2*yy,     rgb);
            wm_put_pixel(w, x + 2*xx + 1, y + 2*yy,     rgb);
            wm_put_pixel(w, x + 2*xx,     y + 2*yy + 1, rgb);
            wm_put_pixel(w, x + 2*xx + 1, y + 2*yy + 1, rgb);
        }
```

This pattern (synthetic gfx_ctx into a private buffer) is the
same trick `wmtype` uses to write directly into its surface.
libgfx didn't have to grow a new API.

---

## wmpaint — drag-to-draw canvas

A 400 × 280 window with a 24-pixel toolbar containing 7 color
swatches.  Drag the left mouse button to paint smooth strokes;
release to lift the pen.  Keys 1..7 pick color, `c` clears, `q`
quits, `+`/`-` adjust the brush radius.

Stroke smoothness comes from Bresenham between every two
`MOUSE_MOVE` events:

```c
case WM_EV_MOUSE_MOVE:
    if (drawing && ev.y >= TOOLBAR_H) {
        int x = ev.x, y = ev.y;
        if (last_x < 0) { last_x = x; last_y = y; }
        draw_line(&win, last_x, last_y, x, y,
                  g_brushes[brush_idx], radius);
        last_x = x; last_y = y;
    }
    break;
```

`draw_line` is a tiny Bresenham that plants a filled disk of
`radius` at every step, so even fast drags don't produce dotted
strokes.

The canvas is the surface itself.  `wm_clear(&win, 0x282828)` runs
**once** at startup; subsequent frames only repaint the toolbar
overlay (which lives at `y < TOOLBAR_H`).  This is a deliberate
choice that costs zero per-frame work for the canvas — strokes
persist because the underlying pixels persist.  Compositors that
repaint the whole surface each frame need a separate stroke buffer;
we get the same effect for free.

Click in toolbar / click in canvas is dispatched by checking
`ev.y < TOOLBAR_H`.  No widget framework — the swatch hit-test is
just `(ev.x - 4) / 22` mapped to colour index.

---

## What the smoke test exercises

`smoke_wmapps.py` boots a single QEMU instance, starts:

1. `wmd 60 &` — the compositor (slot 4..7 reserved for clients)
2. `wmclock 30 &` — registers at slot 4, position (340, 360)
3. `wmpaint 30` — registers at slot 5, position (400, 400)

Then drives:

- 17 QMP rel events to walk the cursor to (~563, ~554) — inside
  wmpaint's canvas, below the toolbar.
- A 1.3-second left-button press (hold long enough to survive
  QEMU's PS/2 button-coalescing; see session 113's doc).
- 15 more rel events while held → 15 `WM_EV_MOUSE_MOVE` events
  feeding `draw_line` (white brush by default).
- Release.

Verifies in the screendump:

| check                              | what it proves |
|------------------------------------|----------------|
| wmclock title bar pure dark grey   | wmclock paints background correctly |
| wmclock has green pixels at y≈410  | the 2x-scaled digits actually rendered |
| wmpaint toolbar has red swatch     | wmpaint's static toolbar painted |
| wmpaint toolbar has green swatch   | (color #4) — also visible |
| wmpaint has white stroke pixels    | the drag produced a visible line |
| wmpaint untouched bg still 0x282828| the no-clear-each-frame model works |
| wmd top status bar still alive     | nothing crashed the compositor |

---

## Files touched

- `user/wmclock.c` — new, ~140 lines
- `user/wmpaint.c` — new, ~160 lines
- `build.sh` — both added to `WMCLIENT_PROGS`
- `mkfs.py` — both .elfs + man pages packed
- `fs/man/wmclock`, `fs/man/wmpaint` — new
- `smoke_wmapps.py` — new headless harness, 7 pixel checks
- `docs/102-pathC-apps.md` — this file

kernel.bin: 114864 (no kernel change).
wmclock.bin: 10876 bytes.  wmpaint.bin: 10792 bytes.  Each smaller
than the older `gfx`/`mouse` demos, because they only link libwm
and libgfx (not full libuser-with-everything).

---

## Path C status after session 115

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing + wmtype
- ✅ 115 — wmclock + wmpaint real apps
- ⏳ 116+ — polish (close button shortcut for client-initiated
          close, FOCUS/UNFOCUS on click rather than hover for
          consistent keyboard semantics, multi-window-per-client)

What we now have working end-to-end: a tiled-by-cascade WM with
real-app clients drawing into shared-memory surfaces, receiving
mouse + keyboard events with surface-local coordinates, all
composited 60 fps under a 16-ms frame budget.  The four session-115
apps (wmhello, wmtype, wmclock, wmpaint) run side by side and stay
responsive to clicks and keystrokes through the click-focus model.
