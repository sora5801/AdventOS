# Session 143 — Path C phase 36: toast notifications

**Goal.** Apps post short status text ("saved /tmp/foo (12 B)",
"copied 48 B") and the WM pops up a fading toast in the
bottom-right.  Decoupled from per-window event queues — any task
can push, wmd is the only consumer.

Status: **done.**  Smoke `smoke_wmtoast.py` (4/4):

```
=== checks ===
  [OK] toast bg present (6108 dark-slate px)
  [OK] toast border (552 blue px)
  [OK] toast text rendered (388 white px)
  [OK] wmd status bar alive (877/924)
```

Boot wmd + wmedit, click into wmedit, type "hi", hit Ctrl-S.
Save triggers `wm_notify("saved /tmp/toast (2 B)")`; the
screendump catches the toast in its full-opacity phase, painted
in a 240×36 box anchored at the bottom-right.

---

## What a toast looks like

```
+----------------------------------+
|                                  |
|  saved /tmp/toast (2 B)          |   ← white text on dark slate
|                                  |   + blue border + 2-px shadow
+----------------------------------+
                              240 x 36, anchored 12 px from the
                              bottom-right, 12 px above the taskbar
```

- bg:     `0x202830` (matches taskbar / display panels)
- border: `0x4080E0` (focused-window blue)
- text:   `0xE0F0FF` (slightly-tinted white)
- shadow: `0x050508` (right + bottom 2-px strip)

Lifetime: **180 frames** (~3 s at 60 fps).  Last **30 frames**
(~0.5 s) are a linear fade — each colour lerps toward the
wallpaper-dark `0x0A0A14`, so by retirement the toast has
melted into the background.  No alpha channel; just per-pixel
RGB lerp.

Up to **4 stacked** toasts at once; if a 5th arrives, the
oldest gets evicted.

---

## Plumbing

### Kernel (kernel/wm.c)

Tiny ring buffer:

```c
#define WM_NOTIFY_MAX        8
#define WM_NOTIFY_TEXT_MAX  64
struct wm_notify_slot { char text[WM_NOTIFY_TEXT_MAX]; int len; };
static struct wm_notify_slot g_notify_ring[WM_NOTIFY_MAX];
static int g_notify_head, g_notify_tail;

int wm_notify_push(const char *text, int len);
int wm_notify_pop (char *buf, int cap);
```

Same shape as the Alt+Tab counter from session 135, just with
text payloads.  Push from any task, pop only from wmd (no
explicit ownership check — wmd is the natural single consumer
because nothing else holds the FB).

### Syscalls (kernel/syscall.{c,h})

Two new slots:

```c
#define SYS_WM_NOTIFY       105   /* (ebx=text, ecx=len)  → 0/-1 */
#define SYS_WM_POLL_NOTIFY  106   /* (ebx=buf, ecx=cap)   → bytes or 0 */
```

Cases delegate straight to `wm_notify_push` / `wm_notify_pop`
with a hard cap of 256 bytes on the push side (the ring itself
truncates at 63).

### libuser + libwm

`user/libuser.c` gets the two `int $0x80` thin wrappers
(`sys_wm_notify`, `sys_wm_poll_notify`).  `libwm/libwm.c` adds:

```c
int wm_notify(const char *text) {
    int n = 0;
    while (text[n] && n < 256) n++;
    return sys_wm_notify(text, n);
}
```

So apps just call `wm_notify("string")` — measure-and-push in
one line.

### wmd (user/wmd.c)

Toast slot array:

```c
#define TOAST_MAX            4
#define TOAST_LIFE_FRAMES  180
#define TOAST_FADE_FRAMES   30
struct toast_slot {
    int          in_use;
    char         text[64];
    int          len;
    unsigned int spawn_frame;
};
static struct toast_slot g_toasts[TOAST_MAX];
```

Each frame:

```c
drain_toasts(tick);             /* pull up to TOAST_MAX strings */
/* ... compositor + taskbar + launcher + ctx menu paint ... */
paint_toasts(&ctx, tick);       /* draw stacked bottom-right */
```

`drain_toasts` evicts the oldest if all slots are in use.
`paint_toasts` walks slots, computes `age = tick - spawn_frame`,
retires if `age >= LIFE_FRAMES`, and fades during the last
`FADE_FRAMES` by lerping bg/border/text toward the
wallpaper-dark colour.

### wmedit hooks

`Ctrl-S save` and `Ctrl-C copy` both build a small status
string with `dec()` and call `wm_notify(...)`:

```c
if (k == 0x13) {              /* Ctrl-S */
    int rc = save_file();
    /* "saved <path> (N B)" or "save failed: <path>" */
    char tn[80]; int p = 0;
    /* ...sprintf-substitute via dec()... */
    wm_notify(tn);
}
```

---

## What stays out of scope

- **Click to dismiss.**  Toasts auto-expire; no click handler.
  Easy add (hit-test the stack in the mouse-press path) but not
  needed for the smoke.
- **Persistent notification center.**  Toasts vanish forever;
  no log to scroll back through.
- **Icons / per-app colours.**  Plain text only.  Eventually:
  pass an `app_id` argument to `wm_notify` and look up an icon
  glyph in a small palette.
- **True alpha blend.**  Fade is RGB-lerp-toward-dark, not
  proper compositing.  Works because the wallpaper is dark; on
  a lighter wallpaper the fade would read as "darkens then
  vanishes" instead of "fades to transparent."
- **Cross-window stacking with other tasks.**  All toasts share
  one stack regardless of which task posted them.  Fine for
  now; we have one user.
- **Notification on snap / resize / Alt-Tab.**  Could surface
  any WM action; only save/copy do for now.

---

## Files touched

- `kernel/wm.c`, `kernel/wm.h` — `wm_notify_push` / `_pop`
  + ring buffer
- `kernel/syscall.h`, `kernel/syscall.c` — 2 new syscall slots
- `user/libuser.c`, `user/libuser.h` — `sys_wm_notify` /
  `sys_wm_poll_notify` thin wrappers
- `libwm/libwm.c`, `libwm/libwm.h` — `wm_notify(text)`
  convenience
- `user/wmd.c` — toast state, drain, lerp helper, paint, hooked
  into the main loop
- `user/wmedit.c` — `wm_notify` on Ctrl-S save and Ctrl-C copy
- `smoke_wmtoast.py` — new harness, 4 checks
- `docs/129-pathC-notifications.md` — this file

Sizes:
- kernel.bin: 147632 → 151728 (+4 KB; notify ring + 2 syscall
  cases + new SYS_WM_NOTIFY const)
- wmd.bin:    16760 → 18136 (+1.4 KB; toast state + paint + lerp)
- wmedit.bin: 15480 → 16012 (+0.5 KB; two wm_notify call sites
  with dec-formatting)

---

## Path C status after session 143

- ✅ 107..142 — see prior docs
- ✅ 143 — toast notifications

The desktop now gives the user visible confirmation of save /
copy actions instead of silently doing them.  Any app can opt
in by linking libwm and calling `wm_notify(...)`.
