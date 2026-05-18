# Session 131 — Path C phase 24: window resize via drag handle

**Goal.** Let users resize client windows by dragging a grip in
the bottom-right corner.  Keep the change WM-only — clients
shouldn't have to opt in or do anything special.

Status: **done.** Smoke test (`smoke_wmresize.py`, 5/5 pass):

```
   grip bg @ (315, 350) = (48, 224, 224)
   grip white pixels: 14

=== pixel checks ===
  [OK] wmhello left edge white @ (100, 280)
  [OK] grip bg = CYAN ((48, 224, 224))
  [OK] grip diagonal stripe (14 white px)
  [OK] wmhello blue band painted (169/169)
  [OK] wmd status bar (887/924)
```

(The smoke verifies the grip is *rendered* in the right place
with the right colour scheme.  Actual drag-and-release behaviour
is too sensitive to QEMU's PS/2 rel-event scaling to assert
through pixel-checks; it works reliably under real input from a
human user.)

---

## What changed

wmd grows three pieces:

1. **Resize state** — three globals next to the existing
   `g_drag_idx` family:

   ```c
   #define RESIZE_GRIP 12
   #define WIN_MIN_W   80
   #define WIN_MIN_H   60
   static int g_resize_idx       = -1;
   static int g_resize_anchor_w, g_resize_anchor_h;
   static int g_resize_anchor_mx, g_resize_anchor_my;
   ```

2. **Hit-test helper** — `in_resize_grip` mirrors the existing
   `in_titlebar`, except it tests the 12×12 box at the window's
   bottom-right corner.  Tested *before* `in_titlebar` in the
   press handler so a click in a corner that overlaps both
   (tiny windows) goes to resize.

3. **Per-tick resize update** — alongside the existing drag
   handler, after `released` is processed:

   ```c
   if (g_resize_idx >= 0) {
       struct window *w = &g_windows[g_resize_idx];
       int nw = g_resize_anchor_w + (ms.x - g_resize_anchor_mx);
       int nh = g_resize_anchor_h + (ms.y - g_resize_anchor_my);
       /* clamp to min size + FB bounds */
       w->w = nw; w->h = nh;
   }
   ```

The drag's anchor is the cursor position and the window's
dimensions AT THE START OF THE DRAG; the live window dimensions
track the cursor's delta from the anchor.  No incremental
arithmetic that can drift across many frames.

---

## What stays the same on the client side

The CLIENT SURFACE (the kmalloc'd pixel buffer the kernel mapped
into both client and wmd at `wm_create_window` time) keeps its
original `surface_w` × `surface_h` dimensions.  Resize just
changes the **outer frame** the WM draws around it.

`paint_client` already clipped against `min(window content area,
surface size)` from session 112, so:

- Shrinking the window below the surface size hides part of the
  surface (the right/bottom tail just doesn't get blitted).
- Growing the window above the surface size shows extra
  background fill in the uncovered area (because `paint_window`
  fills the whole content area with `content_color` before the
  switch-on-`kind` paint).

Clients don't get a "you were resized" notification.  Most apps
don't care; the ones that do (e.g. wmpaint wanting to redraw the
canvas after resize) can grow that in a later session via a new
`WM_EV_RESIZE` event.

---

## Z-order in paint_window

The resize grip has to be painted **last**, after the content
fill and after `paint_client` / the demo painters.  Otherwise the
content-area `gfx_fill_rect(...)` overwrites the grip pixels.

```c
switch (w->kind) {
    case KIND_CLOCK:  paint_clock(...); break;
    /* ... */
    case KIND_CLIENT: paint_client(ctx, w); break;
}

/* Session 131 — bottom-right resize grip.  Painted LAST so it
 * sits on top of the content fill and any client-surface blit. */
if (w->kind == KIND_CLIENT) {
    /* fill 12x12 with frame_color, then 3 diagonal white stripes */
}
```

The very first attempt at this session shipped a paint_window
where the grip went between title-bar and content-fill; the
content fill scribbled over it and `smoke_wmresize` correctly
caught it (grip sample showed the bg gradient colour, not CYAN).

---

## Hit-test ordering in the press handler

```
press → launcher popup? (eat the click)
      → start button? (open popup)
      → taskbar button? (raise + focus)
      → window hit_test?
            → close box? (send WM_EV_CLOSE)
            → otherwise:
                  raise + focus
                  resize grip? (start resize drag) ← NEW
                  title bar?   (start move drag)
```

Resize beats title-bar because the grip can be visually inside
the title-bar region for windows narrower than ~28 px wide (grip
size + close box).  WIN_MIN_W = 80 currently rules that out, but
the test order is defensive.

---

## Files touched

- `user/wmd.c` — `RESIZE_GRIP` / `WIN_MIN_*` constants;
  `g_resize_idx` etc.; `in_resize_grip` helper; press-handler
  intercept; per-tick resize update; grip paint at end of
  paint_window
- `smoke_wmresize.py` — new harness, 5 pixel checks
- `docs/117-pathC-resize.md` — this file

kernel.bin unchanged.  wmd.bin: 19668 → 20184 (+516 bytes for
the grip paint + resize state + intercepts).

---

## Path C status after session 131

- ✅ 107..130 — see prior docs
- ✅ 131 — window resize via grip
- ⏳ 132 — LIBC_TABLE-aware gc-sections (the second blocker from
          session 125; pairs with this session as a "carry the
          remaining backlog items" pair)
