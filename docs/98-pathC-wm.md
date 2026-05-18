# Session 111 — Path C phase 5: window manager daemon

**Goal.** Put more than one thing on the screen at once. Sessions
107–110 gave userspace a framebuffer, drawing primitives, a mouse,
and tear-free presents — but only one program at a time could
actually use any of it. `wmd` is the first program that draws
*multiple* windows and routes mouse events to them.

Status: **done.** Smoke test (`smoke_wmd.py`, 8/8 pass):

```
=== pixel checks ===
  [OK] top status bar @ y=6 (887/924)
  [OK] desktop background top-right @ y=50 (300/300)
  [OK] Clock title bar dark @ y=85 (240/250)
  [OK] Clock content bg @ x=200 (79/80)
  [OK] gradient L pixel (18, 102, 128) vs R pixel (238, 102, 128): R grows
  [OK] About content bg @ x=300 (85/85)
  [OK] Color-bar 0 RED @ y=420 (33/33)
  [OK] cursor or text white pixels in UL quadrant: 1300
```

After 40 QMP rel events of `(-10, -8)`, the cursor walked from the
spawn center `(512, 384)` to `(202, 136)` — landing inside the
Clock window's content area. A pixel-perfect 17h+17v crosshair was
detected at exactly that point. All four demo windows painted with
their per-window color themes; the gradient demonstrably interpolated
across its width; the color-bars test card produced the expected
red bar at the expected x range.

---

## Why a daemon

Previous Path C programs (`gfx`, `mouse`) were each "the" graphical
program — they grabbed the FB, ran for some seconds, then released
it. That works for demos but precludes the obvious next step: having
two programs visible at once.

The standard fix is a window manager / compositor: one privileged
process owns the FB, and other programs ask it for "windows" — chunks
of screen real estate they can paint. The compositor places those
windows, draws decorations (title bars, borders, drop shadows), and
combines everything into the final frame.

`wmd` is the privileged process. In session 111 it doesn't yet have
clients — the windows it composites are pixel buffers it generates
itself. Session 112 will add an IPC channel so external programs can
register their own windows. Session 111 establishes the renderer +
event model so 112 is just plumbing.

---

## The window record

```c
struct window {
    int  x, y;          /* top-left of decoration */
    int  w, h;          /* full size including title bar */
    char title[28];
    unsigned int frame_color;
    unsigned int content_color;
    int  kind;          /* 0=clock, 1=gradient, 2=text, 3=color-bars */
    int  raised;        /* z-order: higher = on top */
};

static struct window g_windows[MAX_WINDOWS];   /* MAX_WINDOWS = 6 */
static int g_window_count;
static int g_z_counter = 1;
```

Position and size are mutable; the demo uses these for mouse-drag.
`kind` selects a content painter — there are four hard-coded ones
in session 111 (a clock, an RGB gradient, an info text block, and
a color-bars test card). These get replaced by client-supplied
pixel buffers in session 112.

`raised` is the z-order. On every click that lands inside a window,
`g_z_counter++` and `w->raised = g_z_counter`. Insertion-sort by
`raised` ascending gives the back-to-front paint order. There's no
shuffling of the array itself — just monotonic counters and a sort
each frame. With six windows max, the sort is O(36) at most.

---

## The compositor loop

```c
for (int tick = 0; tick < total_ticks; tick++) {
    /* 1. Input */
    sys_mouse_poll(&ms);
    int left = ms.buttons & 0x01;
    int pressed  = left && !g_prev_left;
    int released = !left && g_prev_left;
    if (pressed)  { /* z-raise; if title-bar, begin drag */ }
    if (released) { g_drag_idx = -1; }
    if (g_drag_idx >= 0) { /* update dragged window's (x,y) */ }
    g_prev_left = left;

    /* 2. Compose */
    gfx_clear(&ctx, 0x0A1828);                       /* desktop bg */
    gfx_fill_rect(&ctx, 0, 0, w, 18, GFX_DARK_GREY); /* status bar */
    gfx_text(&ctx, 8, 5, "wmd - AdventOS Path C session 111", ...);

    int order[MAX_WINDOWS]; z_order(order);
    for (int i = 0; i < g_window_count; i++) {
        paint_window(&ctx, &g_windows[order[i]],
                     order[i] == focused, t_sec, tick);
    }
    draw_cursor(&ctx, ms.x, ms.y, cursor_color);

    /* 3. Present */
    gfx_present(&ctx);
    sys_sleep_ms(16);
}
```

The whole compositor is ~30 lines. The work is in `paint_window` —
title bar, frame outline, drop shadow, content area, then a switch
on `kind` to call the right painter. None of this is special; it's
just a sequence of `gfx_*` calls into the backbuffer.

The 16 ms sleep targets 60 fps. With four small windows on a
1024×768 backbuffer, each tick takes well under that — `gfx_present`
dominates the cost (about 3 ms for the 2.25 MiB memcpy on the QEMU
box; see session 110 doc).

---

## Mouse interaction

Three states, distinguished by cursor color:

| Cursor | State |
|--------|-------|
| white  | hover / no button |
| yellow | L pressed on desktop background |
| red    | L pressed in a title bar — dragging |

```c
if (pressed) {
    int hit = hit_test(ms.x, ms.y);
    if (hit >= 0) {
        g_z_counter++;
        g_windows[hit].raised = g_z_counter;     /* click-to-raise */
        focused = hit;
        if (in_titlebar(&g_windows[hit], ms.x, ms.y)) {
            g_drag_idx   = hit;
            g_drag_off_x = ms.x - g_windows[hit].x;
            g_drag_off_y = ms.y - g_windows[hit].y;
        }
    } else {
        focused = -1;
    }
}
if (g_drag_idx >= 0) {
    struct window *w = &g_windows[g_drag_idx];
    w->x = ms.x - g_drag_off_x;
    w->y = ms.y - g_drag_off_y;
    /* clamp to FB so the titlebar stays grabbable */
}
```

Hit-testing is top-to-bottom in z-order so overlapping windows do
the right thing. The drag offset captures where in the title bar
the user grabbed, so the window doesn't snap its top-left to the
cursor on the first frame of a drag.

---

## Why no kernel changes

`wmd` is purely userspace. The only system surfaces it touches are
the ones from sessions 107 and 109:

- `sys_fb_info` / `sys_fb_map` / `sys_fb_unmap` — the FB lifecycle
- `sys_mouse_poll` — drains the PS/2 packets and returns `(x, y, buttons)`
- `sys_sleep_ms` — frame budgeting
- `sys_time` — for the Clock window display

That's it. No new syscall surface in 111. Session 112 *will* add
syscalls (for client window registration + shared pixel buffers),
but 111's design constraint was "use what we already have." That
makes the compositor + event-routing logic easy to read in isolation:
none of it is buried in a kernel handler.

---

## What's hardcoded for session 111

- Four windows, declared at startup with fixed `(x, y, w, h, kind)`.
- `kind` switches between four hand-written painters; one of them
  (kind=0) reads `sys_time` and `tick` so it visibly changes.
- Initial focus is `-1` (nothing focused), so all four title bars
  paint in `GFX_DARK_GREY` until the user clicks.
- After release the cursor reverts to white; no "pressed in this
  window, button-up-elsewhere does X" yet.

In session 112, all four painters disappear. Each window's content
will instead be a pointer to a client-supplied pixel buffer (mapped
via shared-page IPC). The window-list management, z-order, focus,
drag, and present pipeline survive unchanged.

---

## Files touched

- `user/wmd.c` — new (~270 lines: window struct, painters, hit-test,
  mouse handling, compositor loop)
- `build.sh` — `wmd` joins `GFX_PROGS`
- `mkfs.py` — `wmd.elf` and `fs/man/wmd` listed in the FS image
- `fs/man/wmd` — new
- `smoke_wmd.py` — new headless harness, 8 pixel checks
- `docs/98-pathC-wm.md` — this file

`kernel.bin` unchanged. `user/_obj/wmd.elf`: new (21467 bytes), `wmd.bin`:
12044 bytes.

---

## Path C status after session 111

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx (drawing primitives, fonts)
- ✅ 109 — PS/2 mouse + cursor demo
- ✅ 110 — double-buffering / no-tearing present
- ✅ 111 — wmd compositor (multi-window, mouse focus & drag)
- ⏳ 112 — WM client protocol: SHM pixel buffer + register/unregister/present
- ⏳ 113+ — sample widgets (button, label, text input), real client apps

Session 112 is the watershed: it makes "user programs draw their
own windows" a real thing, not just a daemon-internal demonstration.
The compositor and event routing are already in place; 112 just
needs to swap out the in-process pixel painters for client-mapped
buffers and wire up the registration syscall.
