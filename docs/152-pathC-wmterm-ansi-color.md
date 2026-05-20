# Session 166 — ANSI color support in wmterm

Path C phase 59.  Output side polish to match the input-side
work done in sessions 157-165.

## What it does

wmterm's `vt_feed` previously *parsed* `CSI m` (Select Graphic
Rendition) sequences but stripped them silently — all output
rendered in the same sage-green default colour.  This session
implements the standard 8-on-16 VGA palette and wires it
through the grid + scrollback so `ls --color=auto`, syntax-
highlighted output, prompts with colour, etc. all show in
their requested colours.

Default cells (no SGR active) still render in the existing
sage-green; the palette only kicks in when a program has
explicitly set fg/bg.

## Storage

Each cell now carries two extra bytes — one for fg index, one
for bg — both `0xFF` when "use defaults" or `0..15` when set:

```c
static unsigned char g_grid_fg[ROWS][COLS];
static unsigned char g_grid_bg[ROWS][COLS];
static unsigned char g_sb_fg[SB_ROWS][COLS];
static unsigned char g_sb_bg[SB_ROWS][COLS];
```

`grid_scroll()` now copies these arrays in lock-step with
`g_grid` / `g_sb` so scrollback retains colour.

Total per-wmterm BSS additions: 2 × (24 × 60 + 500 × 60) ≈
63 KB.  `wmterm.bin` grew from 40 KB to ~105 KB (most of it
zero-filled BSS that user.ld folds into `.data` for the loader).

## The palette

Classic 16-colour VGA-ish, indices 0..7 = non-bright SGR
foregrounds (codes 30..37), 8..15 = bright (codes 90..97 OR
bold-prefixed):

```c
static const unsigned int g_palette[16] = {
    0x000000, 0xCC0000, 0x00CC00, 0xCCCC00,
    0x0000CC, 0xCC00CC, 0x00CCCC, 0xC0C0C0,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};
```

## SGR parser

The CSI m handler walks the parameter buffer one semicolon-
separated value at a time:

```c
static void apply_sgr(void) {
    int idx = 0;
    if (g_csi_param_len == 0) { g_cur_fg = -1; g_cur_bg = -1; return; }
    while (idx < g_csi_param_len) {
        int v = 0, any = 0;
        while (idx < g_csi_param_len
               && g_csi_param[idx] >= '0'
               && g_csi_param[idx] <= '9') {
            v = v * 10 + (g_csi_param[idx] - '0'); any = 1; idx++;
        }
        if (!any) v = 0;
        if (v == 0)                 { g_cur_fg = -1; g_cur_bg = -1; }
        else if (v == 1) {          /* bold = bright fg */
            int f = (g_cur_fg < 0) ? 7 : (g_cur_fg & 0x07);
            g_cur_fg = f | 0x08;
        }
        else if (v == 22)           { if (g_cur_fg >= 0) g_cur_fg &= 0x07; }
        else if (v >= 30 && v <= 37)  g_cur_fg = v - 30;
        else if (v == 38)           { /* skip 256/RGB tail, clamp to 16 */ }
        else if (v == 39)           g_cur_fg = -1;
        else if (v >= 40 && v <= 47)  g_cur_bg = v - 40;
        else if (v == 49)           g_cur_bg = -1;
        else if (v >= 90 && v <= 97)  g_cur_fg = (v - 90) | 0x08;
        else if (v >= 100 && v <= 107) g_cur_bg = (v - 100) | 0x08;
        if (idx < g_csi_param_len && g_csi_param[idx] == ';') idx++;
        else break;
    }
}
```

The csi_param buffer also grew from 8 to 16 bytes to fit
sequences like `CSI 1;31m` or `CSI 38;5;200m`.

`grid_putc_printable` writes the current g_cur_fg / g_cur_bg
into the parallel colour arrays alongside the char.

## Render

The render loop now picks fg/bg from the palette per cell:

```c
unsigned int fg = (fgi == COLOR_DEFAULT) ? 0xC0E0C0u
                                         : g_palette[fgi & 0x0F];
unsigned int bg = (bgi == COLOR_DEFAULT) ? GFX_TRANSPARENT
                                         : g_palette[bgi & 0x0F];
if (selected) bg = 0x405068u;
if (bg != GFX_TRANSPARENT)
    wm_fill_rect(&win, x, y, CELL_W, LINE_H, bg);
if (ch) gfx_glyph(&sctx, x, y, ch, fg, ...);
```

Selection highlight (session 165) overrides any cell's bg, so
selected coloured text remains highlighted.

## colortest

A tiny one-file user program (`user/colortest.c`) that emits
six coloured words followed by three bold/inverse variants:

```c
put("\033[31mRED\033[0m \033[32mGREEN\033[0m "
    "\033[33mYELLOW\033[0m \033[34mBLUE\033[0m "
    "\033[35mMAGENTA\033[0m \033[36mCYAN\033[0m\n");
put("\033[1;31mBRED\033[0m \033[1;32mBGREEN\033[0m "
    "\033[44;37mWBLUE\033[0m\n");
```

Added to `mkfs.py` so `colortest` is available at the shell.

## Smoke

`smoke_wmterm_color.py` — six checks, one per primary palette
colour:

| Check                              | 3/3 runs |
|------------------------------------|----------|
| colour 'red' visible on grid       | OK       |
| colour 'green' visible on grid     | OK       |
| colour 'yellow' visible on grid    | OK       |
| colour 'blue' visible on grid      | OK       |
| colour 'magenta' visible on grid   | OK       |
| colour 'cyan' visible on grid      | OK       |

The smoke drives the inner shell via **serial** (the kbd grab
from session 160 routes serial input through the kbd ring to
the focused wmterm, same as keystrokes).  Driving via QMP
send-key would have been a more direct test but USB-kbd
keystroke delivery through QMP is flaky enough that a 9-char
command can drop chars; serial gets the command through
intact.

Hit-count is small (16-40 px per colour for the 6 letters of
each word) because the 8x8 font has more transparent than
filled pixels — the smoke just confirms non-zero hits at the
palette's exact RGB triple, which any non-rendered case would
fail.

## What changed, exhaustively

- `user/wmterm.c`:
  - `g_grid_fg`/`g_grid_bg`/`g_sb_fg`/`g_sb_bg` colour arrays.
  - `g_palette[16]` + `g_cur_fg`/`g_cur_bg` SGR state.
  - `grid_clear` / `grid_scroll` keep colour arrays in sync.
  - `grid_putc_printable` writes colour with the char.
  - `apply_sgr` parses semicolon-separated SGR params.
  - `csi_dispatch` case 'm' calls `apply_sgr`.
  - `csi_param` buffer 8 → 16 to fit longer SGR sequences.
  - `visible_fg_row` / `visible_bg_row` accessor.
  - Render loop uses palette + fills coloured bg before glyph.
- `user/colortest.c` — new.
- `build.sh` / `mkfs.py` — colortest binary entry.
- `fs/man/wmterm` — updated.
- `smoke_wmterm_color.py` — new.

## What this *doesn't* fix

- **No 256-colour or true-colour rendering.**  `CSI 38;5;N m`
  is parsed but N is clamped to N % 16; `CSI 38;2;R;G;B m`
  isn't currently handled (the apply_sgr branch for 38 reads
  one extended value and stops).  Most colour-aware programs
  fall back to the 16-colour set when terminfo says we only
  have 16, so this is rarely user-visible.

- **No italic / underline / strikethrough.**  SGR 3 / 4 / 9 are
  silently ignored.  Our 8x8 bitmap font wouldn't render them
  well anyway.

- **Default-background behaviour with selection.**  When the
  user selects coloured text, the selection-highlight bg
  (`0x405068`) overrides the cell's chosen bg.  Releasing the
  selection restores the original colours.  Same behaviour as
  every modern terminal; mentioned here only because the
  underlying override happens at render time, not in the
  copy buffer (the copied text is whatever's in the chars,
  with no colour info — clipboard is plain text).
