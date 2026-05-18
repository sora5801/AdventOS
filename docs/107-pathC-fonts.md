# Session 120 — Path C phase 14: scalable fonts

**Goal.** Bigger text without per-app scratch buffers.  Session 115
proved wmclock could render at 2x by hand-rolling a scratch-buffer
expansion; phase 14 moves that capability into libgfx so every
client gets it.

Status: **done.** New smoke test (`smoke_wmfonts.py`, 4/4 pass):

```
=== pixel checks ===
  [OK] 2x font: TOP band has green (368)
  [OK] 2x font: BOTTOM band has green (268)
  [OK] 2x2 green blocks present (5)
  [OK] wmclock title bar @ y=380 (168/169)
```

Verifies the new `gfx_text_n` at scale=2 emits a 16-pixel-tall
character: both the top half (y=410..417, source rows 0..3
doubled) and the bottom half (y=418..425, source rows 4..7
doubled) carry green digit pixels — the old 8x8 renderer could
only fill ONE band at the same y.  And every green pixel sits
inside a 2×2 block, proving the per-source-pixel doubling.

Regression: 11 existing smoke tests (sessions 110–119) all pass.
The behavioural surface change is purely additive.

---

## API

Two new entry points in `libgfx`:

```c
void gfx_glyph_n(struct gfx_ctx *ctx, int x, int y, char c,
                 int scale, unsigned int fg, unsigned int bg);
void gfx_text_n (struct gfx_ctx *ctx, int x, int y, const char *s,
                 int scale, unsigned int fg, unsigned int bg);
```

`scale` semantics:
- `0` or negative → no-op.
- `1` → identical to existing `gfx_glyph` / `gfx_text` (in fact,
  the implementation forwards to them for scale=1).
- `2` → 16×16 cell, 16 px per character advance.
- `3` → 24×24 cell, 24 px per character advance.
- Higher integer scales work too; clipping is handled by the
  underlying `put_packed` so out-of-bounds writes are dropped.

`fg` / `bg` follow the usual `0xRRGGBB` / `GFX_TRANSPARENT`
convention.  The `bg` fill at scale=N covers the full N×N pixel
block per source pixel, not just every Nth row — so transparent
glyphs leave no awkward stripes through the body.

---

## Implementation

The 8x8 source glyph data lives in `font8x8[]`.  The inner loop
walks each set/cleared source pixel and emits a `scale × scale`
filled block:

```c
for (int r = 0; r < FONT_H; r++) {
    uint8_t bits = glyph[r];
    for (int col = 0; col < FONT_W; col++) {
        unsigned int p;
        if (bits & (1u << col)) p = pfg;
        else if (do_bg)         p = pbg;
        else                    continue;
        int x0 = x + col * scale, y0 = y + r * scale;
        for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++)
                put_packed(ctx, x0 + dx, y0 + dy, p);
    }
}
```

Two design notes:

1. **`put_packed` per pixel, not `gfx_fill_rect`.**  `fill_rect`
   would be cleaner for the N×N block but it re-packs the colour
   each call and has its own clipping pass.  Inline `put_packed`
   reuses the pre-packed `pfg` / `pbg` and runs in one pass.
   Measured cost at scale=2 for a HH:MM:SS string: well under
   the per-frame budget on the QEMU box.

2. **`scale == 1` short-circuit.**  Forwards to the original
   `gfx_glyph` to keep the per-frame text cost identical for
   apps that never opt in.

---

## What got smaller in wmclock

Before:

```c
unsigned int scratch[64];
struct gfx_ctx s2 = { .fb = (volatile unsigned char *)scratch,
                      .width = 8, .height = 8, .pitch = 32, .bpp = 32 };
for (int i = 0; i < 64; i++) scratch[i] = 0;
gfx_glyph(&s2, 0, 0, c, rgb, GFX_TRANSPARENT);
for (int yy = 0; yy < 8; yy++)
    for (int xx = 0; xx < 8; xx++)
        if (scratch[yy*8 + xx])
            wm_put_pixel(w, x + 2*xx,     y + 2*yy,     rgb),
            wm_put_pixel(w, x + 2*xx + 1, y + 2*yy,     rgb),
            wm_put_pixel(w, x + 2*xx,     y + 2*yy + 1, rgb),
            wm_put_pixel(w, x + 2*xx + 1, y + 2*yy + 1, rgb);
```

After:

```c
gfx_text_n(&sctx, x, y, tbuf, 2, GFX_GREEN, GFX_TRANSPARENT);
```

`draw_char_2x` and `draw_text_2x` both gone — about 40 lines of
client code replaced by one call into the library.  Future apps
that want 2x or 3x text just call the library; they don't
re-invent the scratch trick.

---

## libgfx size cost

`libgfx.o`: 4400 → 5221 bytes (+821 B for the two new functions).

This nets out roughly:
- per-client overhead +800 B since user programs link libgfx in
  its entirety (no `--gc-sections` on user-prog links yet).
- wmclock specifically saves ~270 B of its own code that used to
  do the scratch dance, so its `.bin` grows by net +352 (10876 →
  11228).
- wmd grows +704 (it doesn't *use* gfx_text_n but it links the
  bigger libgfx).

Enabling `--gc-sections` for user programs is the obvious next
shrinker — same pattern that worked for the kernel in session
112.  Filed as a future polish task; not part of session 120.

---

## Files touched

- `libgfx/libgfx.h` — declarations for `gfx_glyph_n` / `gfx_text_n`
- `libgfx/libgfx.c` — implementations (~40 lines)
- `user/wmclock.c` — drop `draw_char_2x` / `draw_text_2x`,
  replace call site with `gfx_text_n(..., 2, ...)`
- `smoke_wmfonts.py` — new harness, 4 pixel checks proving the
  scale really doubles each dimension
- `docs/107-pathC-fonts.md` — this file

kernel.bin: 114864 (unchanged).
libgfx.o: 4400 → 5221.
wmclock.bin: 10876 → 11228 (+352).
wmd.bin: 17556 → 18260 (+704, library pull-in).

---

## Path C status after session 120

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
- ⏳ 121+ — multi-window per client, system-tray clock, user-prog
          `--gc-sections`, real wallpapers

libgfx now offers everything a client needs for non-trivial UIs:
clear, fill_rect, put_pixel, line, rect, glyph, text, scaled
glyph, scaled text.  Apps don't have to roll their own font
handling for any size that's a sensible integer multiple of 8.
