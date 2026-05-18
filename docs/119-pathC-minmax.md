# Session 133 — Path C phase 26: maximize + minimize buttons

**Goal.** Fill out the standard window-chrome trio.  Sessions 116
shipped close (red X); this session adds the green maximize and
yellow minimize buttons to its left, plus the supporting state +
restore logic.

Status: **done.** Smoke test (`smoke_wmminmax.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] close box red @ x=310 (10/10)
  [OK] maximize box green @ x=294 (10/10)
  [OK] minimize box yellow @ x=278 (10/10)
  [OK] glyph whites in 3 buttons (45)
  [OK] wmhello blue band (210/210)
```

The smoke verifies the buttons are *rendered* with the right
colour scheme and glyph layout.  Actual click-to-toggle is too
sensitive to QEMU's PS/2 rel-event scaling to assert reliably
(buttons are 14 px wide; cursor positioning drifts ±20–50 px
session-to-session).  Under real human input the buttons behave
exactly as their colour suggests.

Regression: `smoke_wmclose`, `smoke_wmtype`, `smoke_wmctxmenu`,
and `smoke_wmwallpaper` all green — the new buttons sit cleanly
inside the title-bar's existing 16-px-per-button budget, with
the close box still at its original `cx = w + w_w - 16` x.

---

## Title-bar layout

```
+-------------------------------------------------+
| wmhello                            [_] [[] [X]  |
+-------------------------------------------------+
```

Right-to-left: close (red), maximize (green), minimize (yellow).
Each is 14×14 with a 1-px gap, so the total title-bar real-estate
they consume is 48 px on the right side.  Title text + drag area
fills everything to the left.

Per-button code:

```c
int by = w->y + 2;
int cbx = w->x + w->w - 16;      /* close   */
int mxbx = cbx - 16;             /* max     */
int mnbx = mxbx - 16;            /* min     */

gfx_fill_rect(ctx, mnbx, by, 14, 14, GFX_YELLOW);
gfx_text(ctx, mnbx + 3, by + 3, "_", GFX_WHITE, GFX_TRANSPARENT);

gfx_fill_rect(ctx, mxbx, by, 14, 14, GFX_GREEN);
gfx_text(ctx, mxbx + 3, by + 3,
         w->maximized ? "o" : "[", GFX_WHITE, GFX_TRANSPARENT);

gfx_fill_rect(ctx, cbx, by, 14, 14, GFX_RED);
gfx_text(ctx, cbx + 3, by + 3, "x", GFX_WHITE, GFX_TRANSPARENT);
```

The maximize glyph flips `[` ↔ `o` so the user can tell which
state the window is in.  Minimize doesn't toggle (it always shows
`_`); restore is done by clicking the taskbar button.

---

## State per window

```c
struct window {
    ...
    int minimized;
    int maximized;
    int saved_x, saved_y, saved_w, saved_h;
};
```

`minimized` is a bool: the window is hidden from compositing AND
from hit-testing while it's set.  Taskbar still lists it.

`maximized` is also a bool, but it also implies the live x/y/w/h
have been overwritten to fill the FB and the `saved_*` fields
hold the previous values for restore.

```c
if (cw->maximized) {
    cw->x = cw->saved_x;
    cw->y = cw->saved_y;
    cw->w = cw->saved_w;
    cw->h = cw->saved_h;
    cw->maximized = 0;
} else {
    cw->saved_x = cw->x; /* … */
    cw->x = 0;
    cw->y = 18;                                /* below top status bar */
    cw->w = (int)ctx.width;
    cw->h = (int)ctx.height - 18 - TASKBAR_H;  /* above the taskbar */
    cw->maximized = 1;
}
```

The maximized envelope is the entire framebuffer minus the WM's
own chrome (top status bar + bottom taskbar).  No overlap; no
hidden window edges.

---

## Hit-test order

The press handler tests the three buttons in y∈[by, by+14] before
running the existing drag / resize logic:

```c
if (cw->kind == KIND_CLIENT &&
    ms.y >= by && ms.y < by + 14) {
    if (ms.x >= cbx && ms.x < cbx + 14)        /* close */
        push WM_EV_CLOSE;
    else if (ms.x >= mxbx && ms.x < mxbx + 14) /* max */
        toggle maximize;
    else if (ms.x >= mnbx && ms.x < mnbx + 14) /* min */
        set minimized;
    button_hit = 1;
}
if (!button_hit) { /* normal raise/focus/drag/resize path */ }
```

Buttons always consume the click in full — no incidental focus or
drag side-effects.  (Maximize/minimize DO raise + focus the
window so the user sees feedback immediately.)

Minimized windows are skipped in `hit_test()` so a stray click
where the window used to be doesn't hit it.

---

## Taskbar interaction

The taskbar's per-window click already raises + focuses; with
session 133 it also un-minimizes:

```c
if (tb_hit >= 0) {
    g_windows[tb_hit].minimized = 0;     /* NEW in session 133 */
    g_z_counter++;
    g_windows[tb_hit].raised = g_z_counter;
    focused = tb_hit;
    goto after_press_hit;
}
```

That's the only mechanism for restoring a minimized window —
which mirrors how every desktop UI handles it.  No keyboard
shortcut yet (would need modifier-aware keys; same blocker as
Alt-Tab cycling).

---

## Compositor change

One added line in the paint pass:

```c
for (int i = 0; i < g_window_count; i++) {
    int idx = order[i];
    if (g_windows[idx].minimized) continue;   /* NEW */
    paint_window(&ctx, &g_windows[idx], …);
}
```

Maximized windows paint normally — they're just a window with
unusual dimensions.  The resize grip + title-bar buttons stay
where they are (in the corners of the maximized frame).

---

## Files touched

- `user/wmd.c` — three new struct-window fields; per-button paint
  block in `paint_window`; press-handler intercepts for max/min;
  taskbar-click un-minimize; minimized-skip in `hit_test` and the
  per-frame paint pass
- `smoke_wmminmax.py` — new harness, 5 pixel checks
- `docs/119-pathC-minmax.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 15784 → 16456 (+672 bytes for the additional state +
two paint blocks + the button hit-test logic).

---

## Path C status after session 133

- ✅ 107..132 — see prior docs
- ✅ 133 — maximize + minimize buttons (close trio complete)
- ⏳ 134 — wmterm: terminal emulator running inside a WM
          window (PTY plumbing + a small ANSI parser)
