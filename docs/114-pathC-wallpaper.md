# Session 127 — Path C phase 20: procedural desktop wallpaper

**Goal.** Replace wmd's flat-blue desktop background with
something that looks less like a placeholder.  Should stay cheap
(60-fps budget) and shouldn't break any existing smoke test that
samples "desktop bg" colour.

Status: **done.** New smoke test (`smoke_wmwallpaper.py`, 4/4
pass):

```
band 0 @ (800,48)  = (8, 20, 36)
band 4 @ (800,430) = (10, 24, 40)
band 7 @ (800,720) = (11, 27, 43)
stars found: 336

=== pixel checks ===
  [OK] band 0 darker than band 4 ((8, 20, 36) < (10, 24, 40))
  [OK] band 4 darker than band 7 ((10, 24, 40) < (11, 27, 43))
  [OK] band 4 matches legacy 0x0A1828
  [OK] some wallpaper stars present (336)
```

Plus regression: every prior smoke test that sampled the desktop
bg (`smoke_wmd`, `smoke_wmclean`, `smoke_wmctxmenu`, …) still
passes — the band-4 colour matches the legacy `0x0A1828` exactly,
so any test with `tol >= 4` accepts it.

---

## The pattern

```c
static void paint_wallpaper(struct gfx_ctx *ctx) {
    int fb_w = ctx->width, fb_h = ctx->height;

    /* 8 vertical bands, centred on the legacy 0x0A1828 colour. */
    int bands = 8, band_h = fb_h / bands;
    for (int i = 0; i < bands; i++) {
        int t = i - bands/2;          /* -4..+3 */
        int r = 0x0A + t / 2;
        int g = 0x18 + t;
        int b = 0x28 + t;
        gfx_fill_rect(ctx, 0, i * band_h, fb_w,
                      (i == bands - 1 ? fb_h - i * band_h : band_h),
                      (r << 16) | (g << 8) | b);
    }

    /* Sparse deterministic "stars" — one dot per 16×16 cell, kept
     * only if its hash falls under the 32/256 threshold. */
    for (int y = 24; y < fb_h - 32; y += 16) {
        for (int x = 12; x < fb_w; x += 16) {
            unsigned int h = (x * 73u + y * 197u) & 0xFFu;
            if (h < 32) {
                unsigned int v = 0x40 + h;
                gfx_put_pixel(ctx, x, y,
                              (v << 16) | (v << 8) | (v + 32));
            }
        }
    }
}
```

Two layers:

- **Vertical gradient.**  8 bands × 96 px at 768 tall.  Band 4
  (centre) is exactly the legacy `0x0A1828`, so any code or test
  that samples the bg at tol≥4 still sees what it used to.  Bands
  1–3 are slightly darker, bands 5–7 slightly lighter.  Whole
  gradient is centred on the legacy colour so backwards-compat
  is symmetric.

- **Stars.**  ~336 distinct dots in a 16×16 grid, deterministic
  (no rng), gated by a coordinate hash so only ~12 % of grid
  positions emit a dot.  Each dot is a single `gfx_put_pixel` of
  a value brighter than the underlying band — visible without
  being noisy.

Both layers are deterministic, so smoke tests get a stable image.

---

## Why this is fast

`gfx_fill_rect` is the cheap path in libgfx — for 24-bpp it
writes 3 bytes per pixel in a tight inner loop.  8 bands × 1024
× 96 = ~786 K pixels, ~2.3 MB written.  That's the same data
volume `gfx_clear` was writing before, just split across 8 calls
that each prepare their colour once.

The stars cost 336 `put_packed` calls — negligible.

End-to-end frame time on the QEMU box: unchanged within
measurement noise.  The compositor was already memcpy-bound by
`gfx_present` anyway (session 110).

---

## Files touched

- `user/wmd.c` — `paint_wallpaper` helper; one call-site change
  in the main loop replacing `gfx_clear(&ctx, 0x0A1828u)` with
  `paint_wallpaper(&ctx)`
- `smoke_wmwallpaper.py` — new harness, 4 pixel checks
- `docs/114-pathC-wallpaper.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 19284 → 19604 (+320 bytes for the wallpaper helper).

---

## Path C status after session 127

- ✅ 107..126 — see prior session docs
- ✅ 127 — procedural wallpaper
- ⏳ 128+ — LIBC_TABLE-aware gc-sections (session 125's other
          blocker), file manager (wmfiles), window resize via
          drag handle
