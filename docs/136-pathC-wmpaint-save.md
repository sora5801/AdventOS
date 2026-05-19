# Session 150 — Path C phase 43: wmpaint Ctrl-S save to PPM

**Goal.** wmpaint draws but couldn't save what you drew.  Session
149's wmview can open PPM images.  Closing the loop: Ctrl-S in
wmpaint writes the canvas to `/tmp/paint.ppm` as a binary P6 PPM,
which `wmview /tmp/paint.ppm` then opens.

Status: **done.**  Smoke `smoke_wmpaint_save.py` (3/3):

```
=== checks ===
  [OK] save toast visible (5716 slate px)
  [OK] wmview opened the saved file (98750 content px)
  [OK] wmd status bar alive (842/924)
```

Boot wmd + wmpaint, draw a stroke, hit Ctrl-S — a toast appears
in the bottom-right confirming the save.  Quit wmpaint, run
`wmview /tmp/paint.ppm` — wmview's window paints with 98,750
non-wallpaper pixels (the canvas content + frame chrome).

---

## How save works

```c
static void save_canvas(struct wm_window *win) {
    int fd = sys_open_w("/tmp/paint.ppm");
    if (fd < 0) {
        wm_notify("wmpaint: save failed");
        return;
    }
    int canvas_w = WIN_W;
    int canvas_h = WIN_H - TOOLBAR_H;

    /* Header: "P6\n<w> <h>\n255\n" */
    char hdr[32]; int hn = 0;
    /* ...build ASCII header into hdr[]... */
    sys_write(fd, hdr, hn);

    /* Stream one row at a time so we never need a 300 KB scratch
     * buffer in the wmpaint binary. */
    unsigned int *pix   = win->pixels;
    unsigned int  pitch = win->pitch_px;
    unsigned char row[WIN_W * 3];          /* 1200 bytes on stack */
    for (int y = TOOLBAR_H; y < WIN_H; y++) {
        for (int x = 0; x < WIN_W; x++) {
            unsigned int c = pix[y * pitch + x];
            row[x*3 + 0] = (c >> 16) & 0xFF;   /* R */
            row[x*3 + 1] = (c >>  8) & 0xFF;   /* G */
            row[x*3 + 2] =  c        & 0xFF;   /* B */
        }
        sys_write(fd, row, canvas_w * 3);
    }
    sys_close(fd);
    wm_notify("saved /tmp/paint.ppm (NNNN B)");
}
```

Key design choices:

- **`sys_open_w` + streaming `sys_write`** instead of one big
  `sys_fs_write`.  A 400×256 RGB888 image is 307,200 bytes; a
  static scratch buffer that size would inflate `wmpaint.bin` by
  300 KB because userspace BSS gets emitted as zero-bytes in the
  binary blob.  Streaming row-by-row keeps wmpaint.bin tiny
  (5 KB) — only a 1200-byte stack buffer for the current row.
- **Tmpfs** as the target.  `/tmp/paint.ppm` is a tmpfs file —
  `tmpfs_write` grows the backing buffer by doubling on demand,
  so 300+ KB writes work fine.  No disk traffic.
- **Toast confirmation** via `wm_notify` (session 143) so the
  user has feedback that the save fired, including the byte
  count.

Canvas dimensions: 400 wide × `WIN_H - TOOLBAR_H` = 400 × 256
tall.  The 24-px toolbar at the top is excluded from the save —
PPM contains only the user's drawing, not the UI chrome.

---

## The round-trip

1. `wmpaint &` — start the paint window
2. Click in canvas, drag to draw something
3. `Ctrl-S` — toast: "saved /tmp/paint.ppm (307215 B)"
4. `q` to quit wmpaint (or use the X close button)
5. `wmview /tmp/paint.ppm` — opens with the saved canvas centred
   in a 400×300 wmview window.  Since the PPM is exactly 400×256
   and wmview's content area is 400×~278, the image fills the
   width and centres vertically.

The bundled `/sample.ppm` (session 149) is 64×48; the saved
paint canvas is 400×256.  Both work — wmview's centring code
handles either size as long as they fit in the content area.

---

## What stays out of scope

- **Custom save path.**  Hard-coded `/tmp/paint.ppm`.  No "save
  as" dialog (no file picker exists yet).  Workaround: copy
  from the shell with `cp /tmp/paint.ppm /tmp/my-drawing.ppm`.
- **Save toolbar area.**  Only the canvas (below the toolbar)
  is saved, not the 7 color swatches or hint text.
- **Lossy compression.**  Pure RGB888 dump, ~300 KB per save.
  No PNG, no JPEG.
- **Auto-save.**  Save fires on Ctrl-S only.  Closing wmpaint
  without saving loses the canvas.

---

## Files touched

- `user/wmpaint.c`:
  - `save_canvas()` helper — streams rows through
    `sys_open_w` + `sys_write`
  - Ctrl-S (keycode 0x13) in the key handler calls
    `save_canvas`
  - Toolbar hint updated to mention "Ctrl-S save"
- `fs/man/wmpaint` — documents the new Ctrl-S shortcut +
  round-trip example with wmview
- `smoke_wmpaint_save.py` — new harness, 3 checks
- `docs/136-pathC-wmpaint-save.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged)
- wmpaint.bin: 4352 → 5260 (+908 B for the PPM save path)

---

## Path C status after session 150

- ✅ 107..149 — see prior docs
- ✅ 150 — wmpaint Ctrl-S save → wmview round-trip
- ⚠️  wmterm input + close — still deferred

The desktop now has a working creative loop: draw in wmpaint →
Ctrl-S → open in wmview.  Files persist in tmpfs across wmd
restarts (until reboot).
