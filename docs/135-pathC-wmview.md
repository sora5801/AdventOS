# Session 149 — Path C phase 42: wmview image viewer

**Goal.** Another visible launcher app, this time one that
*reads* a file and renders it pixel-for-pixel into a window.
Smallest reasonable image format: **P6 PPM** (binary RGB888, no
compression).  Adds wmview to the Start menu plus a bundled
`/sample.ppm` test pattern.

Status: **done.**  Smoke `smoke_wmview.py` (6/6 pass):

```
=== checks ===
  [OK] TL quadrant red (255, 0, 0)
  [OK] TR quadrant green (57, 255, 57)
  [OK] BL quadrant blue (42, 42, 255)
  [OK] BR quadrant yellow (255, 255, 100)
  [OK] wmview title painted ((32, 32, 32))
  [OK] wmd status bar alive (842/924)
```

`wmview /sample.ppm` opens a 400×300 window with the 64×48 test
pattern centred — four colored quadrants (red / green / blue /
yellow) overlaid with a diagonal brightness gradient.  Pixel
checks sample each quadrant's midpoint and confirm the right
hue.

---

## What the viewer does

1. **Load** the file at PATH into a 32 KiB scratch buffer
   (`sys_open` + `sys_read` + `sys_close`).  Cap is documented
   below.
2. **Parse** the P6 header: "P6" magic, ASCII width / height /
   maxval separated by whitespace (with `#`-comment lines
   tolerated per spec), then one whitespace byte before the
   binary pixel data.
3. **Open** a 400×300 wmview window via libwm.
4. **Blit** the pixels directly into the WM-mapped shared
   surface (`win.pixels`).  RGB triplets are packed into
   `0x00RRGGBB` 32-bit values and stored to `win.pixels[oy * pitch + ox]`.
   No per-pixel `wm_put_pixel` syscall — the surface is mapped
   into the client's address space, so a plain pointer write
   suffices.
5. **Repaint** the title bar each frame (focus-colour tracks
   FOCUS / UNFOCUS events); image is static and doesn't need
   re-blitting.

```c
unsigned int *pix   = win.pixels;
unsigned int  pitch = win.pitch_px;
for (int y = 0; y < draw_h; y++) {
    for (int x = 0; x < draw_w; x++) {
        int idx = data_pos + (y * img_w + x) * 3;
        if (idx + 2 >= n) break;
        unsigned int r = g_buf[idx + 0];
        unsigned int g = g_buf[idx + 1];
        unsigned int b = g_buf[idx + 2];
        pix[(oy + y) * pitch + (ox + x)] = (r << 16) | (g << 8) | b;
    }
}
```

---

## Why 32 KiB cap

Userspace BSS is emitted as zero bytes in the .bin blob — the
build uses `-fno-zero-initialized-in-bss` so the ELF loader can
skip the bss-zero step.  A 256 KiB scratch buffer would inflate
the binary to ~270 KiB on disk.  32 KiB is the sweet spot:

- 32 KiB ≥ a 100×100 RGB888 image (= 30 K of pixels + ~20 B header)
- wmview.bin ends up at 37 KiB instead of 270 KiB
- The bundled sample is 64×48 = 9 KiB; comfortably fits

Larger images would need either heap allocation or streaming —
future polish.

---

## Sample image generator

`mkfs.py:gen_sample_ppm()` synthesises the 64×48 test pattern at
build time.  Four colored quadrants — TL red, TR green, BL blue,
BR yellow — modulated by a diagonal brightness gradient
`shade = (x + y) * 200 / (W + H)` so the image has visual interest
beyond flat colours.  Output: 9229 bytes (15 bytes of header + 64
× 48 × 3 = 9216 bytes of pixel data).

The file lives at `/sample.ppm` in the AdventFS image; `wmview
/sample.ppm` is the canonical first invocation.

---

## What stays out of scope

- **PNG / JPEG / BMP.**  Only P6 PPM (and only maxval 255).
  These need decompression libs that don't exist yet.
- **Scaling / zoom.**  Images larger than the 400×~278 content
  area get clipped at the top-left.  No fit-to-window, no
  zoom-in, no pan.
- **Format autodetect.**  Strict P6 — anything else prints an
  error and exits.  No PPM "P3" (ASCII) fallback.
- **Animation / multi-image.**  No GIF, no slideshow.
- **Heap-backed buffer.**  32 KiB fixed cap; a larger image
  errors with "truncated."
- **Save / export.**  Read-only.

---

## Files touched

- `user/wmview.c` — new ~200-line viewer
- `build.sh` — wmview joins `WMCLIENT_PROGS`
- `mkfs.py`:
  - `gen_sample_ppm()` synthesises the test pattern
  - `wmview.elf` + man page registered
  - `sample.ppm` added to `DATA_FILES`
- `fs/man/wmview` — new
- `user/wmd.c` — wmview added to Start-menu launcher catalog
  (now 12 items)
- `smoke_wmview.py` — new harness, 6 pixel checks
- `docs/135-pathC-wmview.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged — pure userspace)
- wmview.bin: new 37372 bytes (~32 K of which is the scratch
  buffer in BSS-as-data)
- wmd.bin: 19424 → 19456 (+32 B for the launcher catalog entry)
- sample.ppm: 9229 bytes in the FS image

---

## Path C status after session 149

- ✅ 107..148 — see prior docs
- ✅ 149 — wmview image viewer
- ⚠️  wmterm input + close — still deferred

The Start menu has 12 items now: wmhello, wmtype, wmclock,
wmpaint, wmpair, wmfiles, sysinfo, wmps, wmterm, wmedit, wmcalc,
wmview.  Path C has crossed the "actually a usable desktop"
threshold — multiple apps, persistent text editor, terminal,
calculator, image viewer, workspaces, snap-to-edge, drop
shadows, toast notifications.
