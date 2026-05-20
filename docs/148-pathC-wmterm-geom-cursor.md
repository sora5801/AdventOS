# Session 162 — wmterm geometry + wmd cursor sprite

Path C phase 55.  Two more interactive polish fixes from user
reports after sessions 157-161.

## Bug 1 — wmterm bottom rows clipped

> "The bottom of the wmterm screen gets cut off when I input ls."

Cause: arithmetic mistake in `user/wmterm.c`:

```c
#define WIN_H        240    /* 240 = 24 * 10 (line h) + 4 px margin */
```

The comment forgot the 24-pixel internal header (`GRID_Y = HDR_H +
4`).  Real space needed:

```
GRID_Y + ROWS * LINE_H = 24 + 24 * 10 = 264 px content
+ a couple of pixels of bottom margin                = 270 px total
```

With `WIN_H = 240`, only the first 21 rows (y=24..233) actually
land on the surface.  Rows 21-23 — including the one sh.elf
draws the *new prompt* on after `ls /` scrolls everything up —
landed past the surface bottom and got clipped.

That's why the user saw `ls /` work but couldn't see the next
`advent$` prompt: it was rendering at `surface_y = 254` on a
240-px-tall surface.  Off-screen, but the underlying shell loop
was happy and ready for the next command.

### Fix

```c
#define WIN_H        270    /* 270 = 24 (header) + 24*10 (rows) + 6 margin */
```

Build cost: 30 extra pixel rows of shared-surface memory per
wmterm = 30 * 540 * 4 bytes = ~63 KB more BSS per terminal.
Trivial.  Smoke samples the wmterm body color at screen `y=465`
(well past the old surface bottom at `y=440`, well inside the
new surface that runs to `y=488`) — 530 of 530 sampled pixels
are wmterm content after the fix, which confirms the bottom
rows now render on-screen.

## Bug 2 — host cursor stale after re-entering QEMU

> "If my mouse leaves the QEMU window and I place it back in, the
> mouse cursor changes... it turns into a left pointing arrow."

Cause: session 142 removed wmd's drawn cursor sprite on the
theory that QEMU's `usb-tablet` device keeps the host pointer
locked to guest coordinates 1:1, so a separate drawn cursor was
"redundant".

That was true for the **position** but not for the **shape**.
When the host pointer leaves the QEMU window and re-enters at an
edge, Windows briefly hands the cursor over as the *resize arrow*
for that edge (`<-` for the left edge, `->` for the right, etc.).
With no guest-drawn overlay, that's what the user sees inside the
QEMU window — a stale resize cursor that doesn't update until the
host issues another SetCursor (typically on a click).

### Fix

Bring back a guest-drawn cursor.  This time, a proper 12×16 arrow
sprite (not the crosshair from session 111), painted at the very
end of every wmd frame so it sits on top of every window:

```c
static const char * const g_cursor_art[16] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#####.....# ",
    "    #..#    ",
    "    #..#    ",
    "     #..#   ",
    "     #..#   ",
    "      ##    ",
};

static void draw_cursor(struct gfx_ctx *ctx, int cx, int cy) {
    for (int dy = 0; dy < 16; dy++) {
        const char *row = g_cursor_art[dy];
        for (int dx = 0; row[dx]; dx++) {
            char p = row[dx];
            if      (p == '#') gfx_put_pixel(ctx, cx + dx, cy + dy, GFX_BLACK);
            else if (p == '.') gfx_put_pixel(ctx, cx + dx, cy + dy, GFX_WHITE);
        }
    }
}
```

`#` = black outline, `.` = white fill, ` ` = transparent.  The
hot-spot is the top-left tip (the very first `#`), matching where
`ms.x` / `ms.y` report.  Drawn AFTER `gfx_present`'s back-buffer
commit point, so it overrides whatever's underneath.

The host's cursor is still drawn underneath by Windows when it
shows through — but a black-outlined white arrow on top is a
stable, consistent visual anchor regardless of what the host
left behind on the last enter event.

## Smoke

`smoke_wmterm_geom_cursor.py` — three checks:

| Check                                  | Method                       |
|----------------------------------------|------------------------------|
| wmd cursor outline visible             | sample black px at hot-spot  |
| wmd cursor white fill visible          | sample white px in body      |
| wmterm bottom rows now on-screen       | sample at y=465 (was clipped)|

4/4 fresh-QEMU runs pass (the cursor check retries the abs-send
because QEMU's USB-tablet occasionally drops the first event of
a session — same flake we worked around in session 160's bg
smoke; not a fix issue).

Sessions 157, 158, 159, 161 smokes all still pass — no regression.

## What changed, exhaustively

- `user/wmterm.c` — `WIN_H` 240 → 270; comment corrected to
  account for `GRID_Y`.
- `user/wmd.c` — restored `draw_cursor()` (12×16 arrow bitmap)
  + paint hook at the end of every frame.
- `smoke_wmterm_geom_cursor.py` — new.

## What this *doesn't* fix

- **The host cursor is still drawn underneath.**  If the user
  cares enough about the doubled cursor, the QEMU side has
  options (`-display sdl,window_close=on` doesn't help; some
  display backends support a `cursor-hide=on`-style option but
  it's host-OS-dependent).  Today the guest overlay is the
  *primary* visual; the host's stale shape is occasionally
  visible as a secondary, but at least the user can see where
  their click will land.

- **No cursor variants** (resize arrows, I-beam, hand).  All
  windows show the same arrow.  Resize-zone-aware cursor
  changes would be a natural follow-up — wmd already detects
  resize zones for click handling, so the data is right there;
  the renderer just needs a small palette of sprites.
