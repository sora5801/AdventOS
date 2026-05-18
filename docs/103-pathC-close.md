# Session 116 — Path C phase 10: close buttons + WM_EV_CLOSE

**Goal.** Make windows closable.  Sessions 112–115 let clients
open and paint windows; phase 10 gives the user a way to ask them
to go away.

Status: **done.** Smoke test (`smoke_wmclose.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] BEFORE: close box red @ y=374 (11/11)
  [OK] BEFORE: wmhello blue band (168/169)
  [OK] AFTER: no red box @ y=374 (0/11)
  [OK] AFTER: no wmhello blue band (0/169)
  [OK] AFTER: wmd status bar still alive (887/924)
```

What the test verifies:
- before: wmhello is up — the WM-painted 14×14 red close box is
  visible at the top-right of its title bar.
- click sequence: cursor walks to (555, 369), 1.3-second left
  press, release.
- after: the close box AND the entire wmhello window are gone.
  The compositor is still running.

End-to-end path: user clicks → wmd's hit-test catches the close
box → wmd pushes `WM_EV_CLOSE` to the client → wmhello sees it
and breaks its main loop → calls `wm_close` → kernel processes
SYS_WM_DESTROY → wmd's next `drain_wm_messages` pops the op=2
destroy and removes the slot.

---

## The close-button rectangle

Every CLIENT window gets a 14×14 red box at the top-right of its
title bar.  Painted by wmd, not the client:

```c
if (w->kind == KIND_CLIENT) {
    int bx = w->x + w->w - 16;
    int by = w->y + 2;
    gfx_fill_rect(ctx, bx, by, 14, 14, GFX_RED);
    gfx_text(ctx, bx + 3, by + 3, "x", GFX_WHITE, GFX_TRANSPARENT);
}
```

That's it — `fill_rect` + a single lowercase `x` glyph centred in
the box.  Visible against any title-bar background (focused or
unfocused).

---

## Click intercept

wmd's existing press handler runs hit-test first, then decides
whether to raise/drag/focus.  Session 116 adds an early-out for
the close box:

```c
if (pressed) {
    int hit = hit_test(ms.x, ms.y);
    if (hit >= 0) {
        struct window *cw = &g_windows[hit];
        int close_hit = 0;
        if (cw->kind == KIND_CLIENT) {
            int bx = cw->x + cw->w - 16;
            int by = cw->y + 2;
            if (ms.x >= bx && ms.x < bx + 14 &&
                ms.y >= by && ms.y < by + 14) {
                close_hit = 1;
                struct sys_wm_event ev = {0};
                ev.type = WM_EV_CLOSE;
                sys_wm_event_push(cw->client_id, &ev);
            }
        }
        if (!close_hit) {
            /* normal raise / drag / focus path */
        }
    }
}
```

When `close_hit` is true:
- A `WM_EV_CLOSE` event lands in the client's queue.
- The click does NOT raise the window, change focus, or start a
  drag.  Closing should feel atomic.

---

## Client side: honor WM_EV_CLOSE

All four sample apps (wmhello, wmtype, wmclock, wmpaint) got the
same minimal change: a `closed`/`quit` flag that the main loop's
condition checks, plus a `WM_EV_CLOSE` case in the event switch:

```c
int closed = 0;
for (int tick = 0; tick < total_ticks && !closed; tick++) {
    struct wm_event ev;
    while (wm_poll_event(&win, &ev)) {
        switch (ev.type) {
            /* ... existing handlers ... */
            case WM_EV_CLOSE: closed = 1; break;
        }
    }
    /* ... paint frame ... */
}
wm_close(&win);
```

`wmpaint` reuses its `quit` flag (already set by the 'q' key) so
WM_EV_CLOSE just sets `quit = 1`.  Same exit path, no duplicate
shutdown.

---

## Why the close box, not "kill the process"?

Two reasons:

1. **Cooperative shutdown.**  A client may want to save state,
   stop a background timer, or release resources before exiting.
   Forcing a kernel-level kill on close would deny that.
2. **The WM doesn't own client lifecycle.**  Clients fork/exec
   from the shell; their exit code goes to whoever `wait`s for
   them.  If wmd were the one to terminate a client, the parent
   would see a confusing exit status.

If a client *ignores* `WM_EV_CLOSE`, the window stays painted.
That's a client bug — the WM's job is to deliver the message,
not enforce compliance.  A future "force-close" path (kill -9
equivalent) is a separate feature for unresponsive windows.

---

## What about wmtype's internal X widget?

wmtype already had a red X inside its content area that clears
the buffer on click — a different feature.  Session 116 leaves
it in place; it's now visibly distinct from the WM's close
button because the WM's lives in the title bar, wmtype's lives in
the surface content.  The comment in wmtype.c was updated to
clarify the distinction.

---

## Files touched

- `user/wmd.c` — close-box paint (~7 lines) + click-intercept
  (~12 lines inside the press handler)
- `user/wmhello.c`, `user/wmtype.c`, `user/wmclock.c`,
  `user/wmpaint.c` — `closed`/`quit` flag + WM_EV_CLOSE case
- `smoke_wmclose.py` — new harness with before/after screendumps
- `docs/103-pathC-close.md` — this file

kernel.bin: 114864 (unchanged — userspace only).  Sizes:
wmd.bin 14964 (+356).  Each client +30–50 bytes.

---

## Path C status after session 116

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint real apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ⏳ 117+ — keyboard-focus separation from hover, alt-tab cycling,
          multi-window per client, real fonts

The WM is now usable end to end: launch wmd, launch a client,
interact with it via mouse + keyboard, close it with the X.  That
covers the "user expects from a windowing system" baseline.
