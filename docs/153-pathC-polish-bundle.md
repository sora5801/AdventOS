# Session 167 — five polish items in one session

Path C phase 60.  Knocking out polish items the previous docs
called out as deferred:

| # | Item                                            | Source doc |
|---|-------------------------------------------------|------------|
| 1 | Title-bar "move" cursor                         | docs/150  |
| 2 | Window seam (1-2 px black band at frame edge)   | docs/150  |
| 3 | Double-click word select                        | docs/151  |
| 4 | 256-color SGR sequences                         | docs/152  |
| 5 | Italic / underline / strikethrough              | docs/152  |

## 1. Title-bar move cursor

`enum cursor_kind` got a new variant `CUR_MOVE` — a black-
outlined plus sign with white-fill arrowheads, 13×11, hot at
(5, 6).  The cursor-selection logic at the end of each wmd
frame now checks `in_titlebar` after `in_resize_zone` and
picks the move cursor when the pointer is over a title bar:

```c
} else {
    int hit = hit_test(ms.x, ms.y);
    if (hit >= 0 && g_windows[hit].kind == KIND_CLIENT) {
        int zone = in_resize_zone(&g_windows[hit], ms.x, ms.y);
        if (zone != RES_NONE)             ck = cursor_for_zone(zone);
        else if (in_titlebar(&g_windows[hit], ms.x, ms.y))
            ck = CUR_MOVE;
    }
}
```

If a drag is actively in progress (`g_drag_idx >= 0`) the move
cursor stays visible even when the cursor strays off the
title bar — same persistence we already had for the resize
cursors.

## 2. Window seam fix

wmd's `drain_wm_messages` opened CLIENT windows with
`total_w = m.w + 4` and `total_h = m.h + TITLE_H + 2`.  The
extra 2 px width + 1 px height that wasn't covered by the
client surface got filled with `content_color` (GFX_BLACK for
CLIENT) — a visible black seam on the right and bottom edges
of every wmterm.  Fix:

```c
int total_w = (int)m.w + 2;             /* 1-px frame each side */
int total_h = (int)m.h + TITLE_H + 1;
```

`paint_window`'s existing fill — `w->w - 2` wide, `w->h - TITLE_H - 1`
tall — now matches the client surface size exactly.  No seam.

Smoke samples pixels at `x = 641, 642` at `y = 300` (where the
band used to be pure black) and asserts they're no longer
`0x000000`.

## 3. Double-click word select

A second `MOUSE_PRESS` within 10 wmterm ticks (≈ 330 ms at the
30 fps frame rate) and ±1 column of the previous press
triggers word selection.  "Word" = run of printable
non-whitespace chars; we scan outward from the click column
until we hit a space or the column boundary:

```c
const char *rowdata = get_abs_row(r);
int sc = c, ec = c;
while (sc > 0       && rowdata[sc-1] > ' ' && rowdata[sc-1] < 0x7F) sc--;
while (ec < COLS-1  && rowdata[ec+1] > ' ' && rowdata[ec+1] < 0x7F) ec++;
g_sel_anchor_row = r; g_sel_anchor_col = sc;
g_sel_head_row   = r; g_sel_head_col   = ec;
```

Word select auto-copies to the clipboard the same way a
drag-then-release does — the user gets the word in the
clipboard without an extra gesture.

Smoke can't reliably automate double-click in QEMU (the
usb-tablet flake from session 165 affects this path too), so
this is verified by code review + interactive testing.

## 4. 256-color SGR

`palette256(int i)` returns the RGB triple for SGR's 256-color
table:

- 0..15: existing 16-color VGA palette.
- 16..231: 6×6×6 RGB cube.  `r = (i-16)/36 % 6`, `g = (i-16)/6 % 6`,
  `b = (i-16) % 6`; each channel maps to `0` when 0 or
  `55 + 40*n` when 1..5 — xterm's exact formula.
- 232..255: 24-step grayscale at `v = 8 + 10*(i - 232)`.

Index 255 collides with our `COLOR_DEFAULT` sentinel (`0xFF`),
so 256-color requests for 255 get clamped to 254.  The user
can hit the same shade via bright white (palette[15]) anyway.

The SGR parser's `38;5;N` and `48;5;N` branches now store
`N & 0xFE` instead of `N & 0x0F`.  `38;2;R;G;B` (truecolor)
maps to the nearest 6×6×6 cube point:

```c
int r6 = (R * 5 + 127) / 255;
int g6 = (G * 5 + 127) / 255;
int b6 = (B * 5 + 127) / 255;
g_cur_fg = 16 + 36*r6 + 6*g6 + b6;
```

Lossy but good enough — most truecolor output is in shades
the cube approximates well.

## 5. Italic / underline / strikethrough

A new `g_grid_attr` (and parallel `g_sb_attr`) byte per cell
holds a bitmask:

| Bit | SGR set / cancel | Effect                           |
|-----|------------------|----------------------------------|
|  0  | 4 / 24           | Underline — 1-px line at bottom  |
|  1  | 9 / 29           | Strikethrough — 1-px line center |
|  2  | 3 / 23           | Italic — stored, not rendered    |

The 8x8 bitmap font has no italic glyphs, so SGR 3 is honored
in the *state machine* but not visualised — same approach
gnome-terminal takes when the configured font has no italic
variant.

Render adds two `wm_fill_rect` calls per cell when the
respective bit is set, drawing in the cell's foreground
colour (so colored underline / strikethrough works naturally).

`colortest` was extended to emit one line of each variant +
a line of 256-color cube samples, so visual / smoke
verification covers everything in one binary.

## Storage cost

wmterm now has 4 parallel arrays mirroring the char grid:
chars + fg + bg + attr.  4 × (24 × 60 + 500 × 60) = 126 KB
BSS in total.  `wmterm.bin` grew 40 KB → 139 KB (most of which
is zero-filled BSS folded into `.data` by `user.ld`).  Well
within the kernel ELF loader's per-task budget.

## Smoke

`smoke_wmterm_color.py` grew from 6 checks to 10:

| Check                                              | 3/3 runs |
|----------------------------------------------------|----------|
| colour 'red' visible on grid                       | OK       |
| colour 'green' visible on grid                     | OK       |
| colour 'yellow' visible on grid                    | OK       |
| colour 'blue' visible on grid                      | OK       |
| colour 'magenta' visible on grid                   | OK       |
| colour 'cyan' visible on grid                      | OK       |
| 256-colour 'c196' visible on grid                  | OK       |
| 256-colour 'c46' visible on grid                   | OK       |
| right-edge seam no longer pure black               | OK       |
| underline/strikethrough horizontal line drawn      | OK       |

All previous wmd/wmterm smokes (157, 162, 164, 166) still
pass after the changes.

## What changed, exhaustively

- `user/wmd.c`:
  - `total_w` / `total_h` for new CLIENT windows shrunk by 2/1
    so the surface fills the frame exactly.
  - `enum cursor_kind` gained `CUR_MOVE`.
  - `g_cursors[CUR_MOVE]` sprite (13×11 cross).
  - cursor-selection picks `CUR_MOVE` on title-bar hover or
    during an active title-bar drag.
- `user/wmterm.c`:
  - `palette256()` function (16-cube-grayscale model).
  - `g_grid_attr` / `g_sb_attr` per-cell attribute byte +
    `g_cur_attr` SGR state.
  - SGR parser handles 3/4/9 + 23/24/29 + `38;5;N` /
    `48;5;N` / `38;2;R;G;B` / `48;2;R;G;B`.
  - Render uses `palette256` + draws underline / strike.
  - Double-click word select with auto-copy on
    `WM_EV_MOUSE_PRESS`.
  - `xy_to_cell` indentation warnings cleaned up.
- `user/colortest.c` — added underline / strikethrough /
  italic line + 256-color cube line.
- `smoke_wmterm_color.py` — 4 new checks.

## What this *doesn't* fix

- **Italic isn't rendered.**  See storage table above; the
  bitmap font has no italic glyphs and synthesising italic
  by sheering would be a separate font-engine project.

- **Underline / strikethrough don't span cell gaps.**  Each
  cell paints its own 1-px line so adjacent underlined cells
  produce a continuous line; that's how it should look.
  Mentioned only because the implementation could theoretically
  leave 1-px gaps if `CELL_W != 8` (it doesn't).

- **No clipboard format for color.**  Selection-copied text
  is plain bytes; the recipient program gets the chars
  without SGR codes.  Matches every modern terminal's
  primary-selection behaviour.
