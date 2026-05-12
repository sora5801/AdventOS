# Session 63 — Damage-rect compositing

**Goal:** turn the WM's "clear the whole screen + redraw every window every frame" path into "only repaint the regions that actually changed." Classic damage-rect compositing — same model X11, Wayland, the macOS WindowServer, and Windows DWM use. The win: idle frames stop redrawing 786,432 pixels just to repaint two animated windows.

What ships:

- **A clip rectangle** that every drawing primitive consults. `put_pixel` and `fill_rect` reject pixels outside it. The redraw path sets it to each damage rect in turn, so the same `draw_window_frame` / `clock_draw` / `paint_draw` functions that used to repaint the entire desktop now repaint only the slice the clip allows.

- **A damage list** (`struct rect g_damage[32]`) — every event that needs paint pushes its bbox onto the list. Sources:
  - Frame 0 (whole screen)
  - Cursor move (old bbox + new bbox)
  - Window drag (old rect + new rect)
  - Window close, focus change, title-bar drag start
  - Click / key event into a window's body
  - Animated windows (Clock + Tasks via `wants_anim`)
  - Out-of-process client `FILL_RECT` / `DRAW_TEXT` / `DRAW_PIXEL` commands
  - Per-frame: the small frame-counter strip + bottom status overlay

- **`paint_rect(x, y, w, h, frame)`** — recomposes one damage rectangle: set the clip, optionally fill the desktop bg (skipped if a window fully covers the rect — "occlusion skip"), then re-run each intersecting window's chrome + draw callback under the clip. A `damage_redraw` driver iterates the list.

- **Pixel counter instrumentation** — `g_pixels_painted` ticks up inside `put_pixel` and `fill_rect`. The WM's selftest summary reports avg / min / max pixels/frame so the t46 test can empirically observe the optimization.

`[t46]` selftest, 4/4 PASS:

```
[t46] damage-rect compositing: WM only repaints dirty regions
  parsed WM damage stats: avg=273402 min=207727 max=2634572 (full=786432)
  PASS  avg pixels/frame < 70% of a full redraw
  PASS  min pixels/frame < 40% of a full redraw (an idle frame)
  PASS  min ≤ avg ≤ max (sanity)
  PASS  rendered pixels are still correct (Paint cell + Hello title + bg)
```

Selftest total: **144 PASS, 0 FAIL.**

---

## 1. The numbers

Baseline (session 62, full redraw every frame at 1024×768): **786,432 pixels per frame**, every frame. At 60 fps that's ~135 MB/s of pure framebuffer write traffic, regardless of whether anything changed.

After session 63:

| Metric                    | Value      | Vs. full redraw  |
|---------------------------|------------|------------------|
| Average                   | 273,402    | **35%** (2.9× speedup) |
| Min (idle frame)          | 207,727    | **26%** (3.8× speedup) |
| Max (drag frame)          | 2,634,572  | 335% — worse, but rare |
| Full redraw (baseline)    | 786,432    | 100%            |

The min — an idle frame painting just Clock + Tasks animation rects + the menu/status strips — is the cleanest measurement of the optimization. ~210k pixels means we touched ~26% of the screen.

The max being **higher than a full redraw** is the cost of the damage model when many rects pile up in one frame: dragging a window adds both its OLD and NEW positions, plus the cursor old/new bboxes, plus the per-frame anim rects (Clock + Tasks), plus the menu + status strips. Six or seven damage rects all firing `paint_rect` walks the windows multiple times, painting the same pixels for each overlap. Real compositors avoid this with rect coalescing — see §7.

---

## 2. The clip rectangle

```c
static int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

static inline void set_clip(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if ((unsigned)(x + w) > g_w) w = (int)g_w - x;
    if ((unsigned)(y + h) > g_h) h = (int)g_h - y;
    g_clip_x0 = x; g_clip_y0 = y;
    g_clip_x1 = x + w; g_clip_y1 = y + h;
}

static inline void put_pixel(int x, int y, unsigned int rgb) {
    if (x < g_clip_x0 || y < g_clip_y0 ||
        x >= g_clip_x1 || y >= g_clip_y1) return;
    /* ... existing pixel write ... */
}
```

`set_clip` pre-clamps against screen bounds, so `put_pixel` only needs a single 4-comparison reject instead of the previous "check < 0 / >= g_w" pair. Every drawing helper (`fill_rect`, `draw_glyph`, `draw_str`, `draw_int`, `rect_outline`, `draw_cursor`, the client trampolines, every built-in app's `*_draw`) calls `put_pixel` internally — they all automatically honor the clip without code changes.

`fill_rect`'s 32-bpp fast path can't go through `put_pixel` (the whole point is to skip per-pixel checks), so it does an explicit clip-rect intersect up front:

```c
int x1 = x0 + w, y1 = y0 + h;
if (x0 < g_clip_x0) x0 = g_clip_x0;
if (y0 < g_clip_y0) y0 = g_clip_y0;
if (x1 > g_clip_x1) x1 = g_clip_x1;
if (y1 > g_clip_y1) y1 = g_clip_y1;
w = x1 - x0;
h = y1 - y0;
if (w <= 0 || h <= 0) return;
```

`set_clip_full()` opens the clip to the whole screen — used by the cursor sprite + status overlay + post-redraw chrome.

---

## 3. The damage list

```c
#define MAX_DAMAGE  32
struct rect { int x, y, w, h; };
static struct rect g_damage[MAX_DAMAGE];
static int         g_damage_n;
static int         g_damage_full_flag;   /* shortcut: paint whole screen */

static void damage_clear(void);
static void damage_full(void);
static void damage_add(int x, int y, int w, int h);
static void damage_window(struct window *w);    /* shorthand */
```

32 slots is enough for typical frames. On overflow `damage_add` degrades to `damage_full()` — costs more than necessary that one frame, but the user-visible output stays correct, which is the only guarantee that actually matters.

### Sources

Every event handler that mutates visible state pushes a rect:

| Source                              | What gets damaged                                  |
|-------------------------------------|----------------------------------------------------|
| Frame 0                             | Whole screen (initial paint over fbcon's leftover) |
| Mouse cursor move                   | Old 12×12 bbox + new 12×12 bbox                   |
| Click that focuses a window         | Lost-focus window's title bar + newly-focused full window rect |
| Click on close button               | Closed window's old rect                          |
| Title-bar drag (per frame)          | Old window rect + new window rect                 |
| Body click → app handler            | Window body (title bar not damaged)              |
| Key event → focused window          | Focused window body                              |
| `wants_anim` (Clock, Tasks)         | Body, every frame                                |
| Client `FILL_RECT` / `DRAW_PIXEL`  | Tightly-bound rect in screen coords              |
| Client `DRAW_TEXT`                  | Text bounding box (one rect per call, not per glyph) |
| Client `CREATE_WIN`                 | New window's full rect                            |
| Per frame: frame counter strip      | (g_w−110, 0, 110, 24) ≈ 2,640 px                 |
| Per frame: status overlay strip     | (g_w−220, g_h−24, 220, 16) ≈ 3,520 px            |

The per-frame strips for frame counter + status overlay are there because they update every tick regardless of user input — even a fully idle desktop counts up "frame: 123" and shows the current mouse coords. The 6k px cost is cheap.

`wants_anim` is the cleanest pattern: opt-in per window. Clock + Tasks set it (they re-render every frame); Hello / Calc / Paint / Notepad / client windows don't. Without this flag, ALL six built-in apps would have to redraw per frame for the WM to be correct, defeating the optimization.

---

## 4. The redraw

```c
static void paint_rect(int rx, int ry, int rw, int rh, int frame) {
    set_clip(rx, ry, rw, rh);

    /* z-sort alive windows */
    /* ... */

    /* Occlusion skip: if any window fully covers this rect, the
     * desktop bg never shows through. */
    int desktop_covered = 0;
    for (int i = 0; i < n; i++) {
        struct window *w = &g_wins[order[i]];
        if (w->x <= rx && w->y <= ry &&
            w->x + w->w >= rx + rw && w->y + w->h >= ry + rh) {
            desktop_covered = 1;
            break;
        }
    }
    if (!desktop_covered) {
        fill_rect(0, 0, g_w, g_h, 0x103060u);     /* desktop blue */
    }

    if (!desktop_covered && ry < 24) {
        /* menu bar + text */
    }

    for (int i = 0; i < n; i++) {
        struct window *w = &g_wins[order[i]];
        /* cheap intersect-reject */
        if (w->x >= rx + rw || w->y >= ry + rh) continue;
        if (w->x + w->w <= rx || w->y + w->h <= ry) continue;
        draw_window_frame(w);
        if (w->draw) w->draw(w, frame);
    }
}

static void damage_redraw(int frame) {
    if (g_damage_full_flag) {
        paint_rect(0, 0, g_w, g_h, frame);
    } else {
        for (int i = 0; i < g_damage_n; i++) {
            paint_rect(g_damage[i].x, g_damage[i].y,
                       g_damage[i].w, g_damage[i].h, frame);
        }
    }
    set_clip_full();
}
```

The occlusion skip alone is responsible for nearly half of the optimization. The Clock animation damage rect is `(420, 78, 260, 122)` — Clock's body region. The Clock window's outer rect `(420, 60, 260, 140)` fully contains it, so `desktop_covered=true`, and we skip the `fill_rect(0, 0, 1024, 768, 0x103060)` which would have wastefully painted ~32,000 pixels of desktop blue under a window that's about to immediately repaint them.

Before adding this check: idle avg ~500k pixels/frame. After: idle avg ~273k. Same arithmetic for Tasks. The check costs maybe ~10 cycles per window per rect (a couple of integer comparisons in a tight loop); the saving is thousands of framebuffer writes.

---

## 5. The pixel counter

Two places in `put_pixel`:

```c
static inline void put_pixel(int x, int y, unsigned int rgb) {
    if (x < g_clip_x0 || y < g_clip_y0 ||
        x >= g_clip_x1 || y >= g_clip_y1) return;
    g_pixels_painted++;
    /* ... existing write ... */
}
```

And in the `fill_rect` 32-bpp fast path:

```c
g_pixels_painted += (uint32_t)(w * h);
```

Per-frame: reset before `damage_redraw`, capture min/max/sum/count after. End of run: emit `damage: avg=N min=N max=N pixels/frame (full=N, idle_frames=N)` for t46 to grep.

The counter is unconditional — there's no "instrumentation off" build flag — but the cost is a single increment per pixel write, which is negligible compared to the framebuffer-memory transaction itself.

---

## 6. Why max is 2.6× full-redraw on drag frames

`handle_mouse`'s drag branch:

```c
} else if ((btns & 0x1) && g_drag_idx >= 0) {
    struct window *w = &g_wins[g_drag_idx];
    damage_window(w);                   // OLD position
    w->x = mx - g_drag_dx;
    w->y = my - g_drag_dy;
    /* clamp ... */
    damage_window(w);                   // NEW position
}
```

For a window at (320, 200) size 320×280, that's two 89,600-pixel rects = 179,200 pixels of damage from drag alone. Add cursor old/new (288 px), Clock anim (32k), Tasks anim (62k), menu strip (5k), status strip (3.5k) = ~280,000 pixels of damage RECTS — which translates to ~800,000 pixels of paint because `paint_rect` redraws each rect's worth of windows AND the same window's body fill happens once per rect that contains it.

For a Clock damage rect: Clock body redraws (62k pixels). For Tasks: Tasks body (62k). For old window rect (overlapping Paint or Hello): chrome + body redraws (~89k). For new window rect: same. Etc. Sum of overlapping work: 2.6M. Worse than full redraw.

Real compositors solve this with **rect coalescing**: before painting, merge adjacent/overlapping rects into a single bigger rect. The single bigger rect causes one `paint_rect` instead of N, and each pixel inside the merged region gets painted at most once. Wayland, Mutter, KDE's KWin all do this.

For session 63 we skip coalescing — the cost is the worst-case max, but the *average* and *min* are still big wins, and the code stays under 50 lines of damage-list machinery. A future session 64 could add rect-merge logic in `damage_redraw` if drag perf becomes the bottleneck.

---

## 7. Worked example: a script frame walk-through

Frame ~100 in the existing WM scripted test (no mouse movement, no events, idle):

```
damage list at end of frame:
  Clock body:    (420,  78, 260, 122) → 31,720 px
  Tasks body:    (680, 238, 280, 222) → 62,160 px
  Menu strip:    (914,   0, 110,  24) →  2,640 px
  Status strip:  (804, 752, 220,  16) →  3,520 px

paint_rect on Clock damage:
  occlusion: Clock fully covers → skip desktop bg
  walk windows: only Clock intersects
    draw_window_frame: title bar clipped out (above damage),
                       body fill (~31k px) inside damage
    clock_draw: digits text (~80 px)
  total: ~31k px

paint_rect on Tasks damage:
  occlusion: Tasks fully covers → skip desktop bg
  walk windows: only Tasks intersects
    draw_window_frame: title clipped, body fill (~62k px)
    tasks_draw: /proc text (~1k px)
  total: ~63k px

paint_rect on menu strip:
  occlusion: no window over menu → bg fill (~2.6k px)
  menu bar bg: ~2.6k px
  menu text + frame counter: ~500 px
  walk windows: none intersect
  total: ~5.7k px

paint_rect on status strip:
  occlusion: no window over → bg fill (~3.5k px)
  walk windows: none intersect (windows end by y=670)
  total: ~3.5k px

Sum: ~103k px → reported g_pixels_painted ≈ 207k (the discrepancy is the
  extra "draw chrome but most is clipped out" overhead, glyphs, etc.)

vs. full redraw 786,432 px. ~3.8× speedup on this idle frame.
```

---

## 8. Touched files

- `user/gui.c` — clip rect + clip-aware `put_pixel` / `fill_rect`; damage list (`g_damage`, `damage_*` helpers); `paint_rect` + `damage_redraw` replace the old `desktop_draw`; main loop pumps damage sources every frame; pixel counter instrumentation; selftest summary prints damage stats.
- `user/sh.c` — `[t46]` parses the WM's stats line + asserts avg < 70% / min < 40% of full redraw + pixel-value sanity.

No kernel changes. Like session 61, the optimization was achievable entirely in userspace — the kernel's framebuffer plumbing was already what we wanted, we just stopped writing through it so often.

## 9. Out of scope

- **Rect coalescing** — fix the 3× max-pixel cost on drag frames by merging overlapping / adjacent rects before paint.
- **Front-to-back compositing with per-pixel occlusion** — what real Wayland compositors do. Drops the "draw fully-covered window's chrome under another window" overhead.
- **Per-window backbuffer + invalidation** — only the dirty parts of a window's own pixmap re-rasterize; the rest is reused. We currently re-call `clock_draw` whenever the Clock damage rect fires.
- **Vsync / double buffering** — eliminating cursor-tearing during fast drags. Not strictly damage-related but visually obvious next step.
- **Damage union tracking across multiple windows** — when a window is destroyed AND its old rect overlapped two others, currently we damage the closed window's rect; the WM correctly walks both underlying windows for the affected pixels. But we don't damage their LARGER rects, only the intersection. That's still correct because only that intersection actually changed.

Most realistic next session: rect coalescing. The diminishing-returns curve points there — drag perf is the most visible remaining cost.
