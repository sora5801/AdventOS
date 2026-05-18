# Session 113 — Path C phase 7: input routing to focused clients

**Goal.** Get mouse events to the client whose window the cursor is
over.  Session 112 stood up the surface plumbing; the compositor
could draw client pixels but the client couldn't tell when the user
clicked them.  Phase 7 closes that loop.

Status: **done.** Smoke test (`smoke_wmevents.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] wmhello blue band @ y=386 (160/160)
  [OK] focus green border @ y=515 (180/180)
  [OK] bg palette advanced (click registered)
  [OK] click marker present (yellow=64 green=0)
  [OK] wmd status bar (887/924)
  [OK] cursor crosshair @ (~449, 461): 22/22
```

What we just verified end-to-end:

1. QMP injects relative mouse motion until the cursor sits inside
   wmhello's surface at (108, 82).
2. wmd's hit-test classifies it as "over a client content area",
   pushes a `WM_EV_FOCUS` into wmhello's queue, then a
   `WM_EV_MOUSE_MOVE` with surface-local `(108, 82)`.
3. wmhello drains the queue, flips its bottom border to GREEN and
   draws a white crosshair at the surface coords it just received.
4. QMP injects a 1s-held button press.  wmd sees the edges (down,
   up) via PS/2 polling and pushes `WM_EV_MOUSE_PRESS` then
   `WM_EV_MOUSE_RELEASE`.
5. wmhello advances its bg palette on press and paints a yellow
   click marker — exactly what the smoke test catches in the
   screendump.

---

## The event ABI

```c
struct sys_wm_event {
    uint32_t  type;         /* WM_EV_*  */
    int32_t   x;            /* surface-local for mouse events */
    int32_t   y;
    uint32_t  button;       /* WM_BUTTON_LEFT / RIGHT / MIDDLE */
    uint32_t  keycode;      /* reserved for session 114 (keyboard) */
};

#define WM_EV_MOUSE_MOVE      1u
#define WM_EV_MOUSE_PRESS     2u
#define WM_EV_MOUSE_RELEASE   3u
#define WM_EV_KEY             4u   /* session 114 */
#define WM_EV_FOCUS           5u
#define WM_EV_UNFOCUS         6u
#define WM_EV_CLOSE           7u   /* WM asks client to destroy */
```

Coordinates are **surface-local** — `(0, 0)` is the top-left of the
shared pixel buffer, not the screen.  wmd does the translation so
clients never need to know where they happen to be placed.

The mouse-event `button` field is the bitmask AT THE TIME of the
event: `WM_BUTTON_LEFT` on a left-button press/release, or the
current mask on a `MOUSE_MOVE`.

---

## Two new syscalls

| #  | name              | who calls         | semantics |
|----|-------------------|-------------------|-----------|
| 94 | SYS_WM_EVENT_PUSH | wmd only          | append one event to a window's queue |
| 95 | SYS_WM_EVENT_POLL | the owning client | drain one event, or return 0 if empty |

Authorization:

- `SYS_WM_EVENT_PUSH` requires `task_current() == g_wm_owner`.
  Anything else gets -1.  This prevents a malicious task from
  forging events at the WM's expense.
- `SYS_WM_EVENT_POLL` requires `w->owner_pid == task_current()->id`.
  A client can only poll its own windows.

Both syscalls take `(window_id, &event)` and return 1/0/-1 in the
familiar shape.

---

## Per-window event ring

Each `wm_window` slot in the kernel gets a tiny ring buffer:

```c
struct wm_window {
    ...
    struct sys_wm_event *events;
    uint32_t             ev_head, ev_tail, ev_size;
};
```

`events` is `NULL` until the first `wm_push_event` call, at which
point we `kmalloc(sizeof(sys_wm_event) * WM_EVENT_QUEUE_DEPTH)` —
24 × 32 = 768 bytes per active window.  Windows that never receive
input cost zero extra heap.

`WM_EVENT_QUEUE_DEPTH = 32`.  On overflow the oldest event gets
dropped, not the newest — better to lose a stale `MOUSE_MOVE` than
to wedge wmd waiting for the client to drain.

Frees on slot destroy, just like the page list:

```c
free_slot_pages(w);
free_slot_events(w);
w->state = WM_SLOT_EMPTY;
```

Total `.bss` impact of session 113: zero (the ring is heap, the
state machine is unchanged).

---

## How wmd decides where events go

Each tick, after polling the mouse and handling drag/raise/focus,
wmd recomputes:

```c
int hover = hit_test(ms.x, ms.y);
int hover_is_client_content =
    (hover >= 0
     && g_windows[hover].kind == KIND_CLIENT
     && ms.y >= g_windows[hover].y + TITLE_H
     && ms.y <  g_windows[hover].y + g_windows[hover].h - 1);
int target = hover_is_client_content ? hover : -1;
```

`target` is the client window the cursor's *content area* is over,
or -1.  Important: cursor over a CLIENT window's TITLE BAR is **not**
a target.  Title-bar clicks belong to the WM (drag).

Hover-change is the FOCUS edge:

```c
if (target != prev_hover_idx) {
    if (prev_hover_idx >= 0 ...) push UNFOCUS to old;
    if (target >= 0)              push FOCUS    to new;
    prev_hover_idx = target;
}
```

Mouse motion / press / release pushes happen only while `target >=
0`.  Surface-local coords:

```c
int sx = ms.x - (w->x + 1);            /* 1-px frame */
int sy = ms.y - (w->y + TITLE_H);
/* clipped to [0, surface_w - 1] x [0, surface_h - 1] */
```

`MOUSE_MOVE` is rate-limited to "only when ms.{x,y} actually
changed" so a stationary cursor doesn't flood the queue.

---

## libwm client API

Two new lines for client code:

```c
struct wm_event ev;
while (wm_poll_event(&w, &ev)) handle(ev);
```

`wm_event` is a re-export of `sys_wm_event` so client code never
imports `libuser.h` directly.  The implementation is one line —
forwards to `sys_wm_event_poll(w->id, ...)`.

---

## The wmhello demo

Session 113's wmhello now reacts to every event type:

| event           | reaction |
|-----------------|----------|
| FOCUS / UNFOCUS | bottom border flips green ↔ grey |
| MOUSE_MOVE      | crosshair drawn at `(mx, my)` |
| MOUSE_PRESS     | bg palette advances; yellow marker at `(x, y)` |
| MOUSE_RELEASE   | green marker at `(x, y)` |

`bg_idx` cycles through a 5-entry palette so the smoke test can tell
from a single screendump that *a* press was processed (the bg
differs from palette[0] = `0x101030`).

---

## A QEMU quirk: button event timing

The smoke test holds the press for 1.0s before releasing.  Shorter
holds (0.5s) reliably drop the down-edge from the kernel's PS/2
view.  Root cause:

- QMP `input-send-event` with `{"type": "btn"}` triggers QEMU's
  PS/2 emulation to queue a packet.
- The kernel's PS/2 driver polls via `keyboard_poll_once` (called
  from every `SYS_MOUSE_POLL`) — every 16 ms or so given wmd's
  frame loop.
- When two button-state-flips happen within ~600 ms of each
  other, QEMU coalesces them in a way that loses the down-edge
  from the polled byte stream.  Real hardware doesn't do this;
  real users don't press at 2 Hz; the smoke test paces with 1.0s.

---

## Files touched

### Kernel

- `kernel/syscall.h` — `SYS_WM_EVENT_PUSH/POLL` (94/95), `struct
  sys_wm_event`, `WM_EV_*` and `WM_BUTTON_*` constants
- `kernel/wm.h` — `wm_push_event` / `wm_poll_event` declarations,
  `WM_EVENT_QUEUE_DEPTH = 32`
- `kernel/wm.c` — per-slot ring buffer (lazy-allocated), push/pop
  with overflow-drop-oldest, free-on-destroy
- `kernel/syscall.c` — 2 dispatch cases

### Userspace

- `user/libuser.h`, `user/libuser.c` — `SYS_WM_EVENT_PUSH/POLL`
  constants, `struct sys_wm_event`, 2 inline-asm wrappers
- `libwm/libwm.h`, `libwm/libwm.c` — `struct wm_event`,
  `wm_poll_event(struct wm_window *, struct wm_event *)`
- `user/wmd.c` — hover-detection in main loop; FOCUS/UNFOCUS on
  hover-change; MOUSE_MOVE/PRESS/RELEASE pushes when over a client
- `user/wmhello.c` — drain events each frame; bg palette + focus
  border + click marker + cursor crosshair driven by the event
  stream

### Tests + docs

- `smoke_wmevents.py` — new headless harness (QMP screendump + 6
  pixel checks: blue band, focus border, bg palette advanced,
  click marker, wmd status bar, cursor crosshair)
- `docs/100-pathC-input.md` — this file

kernel.bin: 114864 → 114864 (gc-sections eats the new code's churn).
`user/_obj/wmd.bin`: 13456 → 14448 (+992 bytes for the event-routing
logic).

---

## Path C status after session 113

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — input routing to focused client
- ⏳ 114 — keyboard events (the WM_EV_KEY type is already reserved
          in the ABI; wmd needs to drain `sys_kbd_poll` and forward)
- ⏳ 115+ — real apps: a clock, a drawing program, a text editor
          that uses the new event API end-to-end

Session 114 wires up keyboard so a focused window can actually
type things.  Session 115 onwards builds the first real apps that
exist solely as WM clients.
