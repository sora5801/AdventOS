# Session 119 — Path C phase 13: in-WM app launcher

**Goal.** Make the WM self-sufficient: a user with only a graphical
session (and not a shell on the side) should be able to start any
known WM-client app.

Status: **done.** Smoke test (`smoke_wmlauncher.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] A: Start button green @ y=754 (32/48)
  [OK] A: no client taskbar buttons (88/88)
  [OK] B: launcher body @ x=150 (67/70)
  [OK] B: launcher item text (155)
  [OK] C: wmhello blue band painted (169/169)
  [OK] C: wmhello taskbar button @ (138,753) = (48, 56, 72)
```

End-to-end sequence verified:

1. **A** — boot wmd alone (no other args, no clients).  Taskbar
   shows just the green Start button on the left; the rest of the
   strip is empty.
2. Click Start → popup opens above the button with one item per
   launchable app.
3. **B** — popup body visible (`0x202830`); item text rendered in
   white.
4. Click "wmhello" item → wmd `fork()`s, the child `exec()`s
   `/wmhello.elf`.
5. wmhello calls `SYS_WM_CREATE`, drain_wm_messages picks it up,
   wmd composites it.
6. **C** — wmhello's blue title band visible in its newly-painted
   window AND its button appears in the taskbar.

The user never touched the shell — every interaction went through
the framebuffer + mouse cursor.

---

## Layout

```
   ┌──────────────────┐
   │ wmhello          │  ← LAUNCH_W = 160
   │ wmtype           │     N_LAUNCH_ITEMS = 4 today
   │ wmclock          │     LAUNCH_ITEM_H = 22
   │ wmpaint          │
   ├────────┬─────────┴──────────────────────────────────────────┐
   │ Start  │ (window buttons)                                   │
   └────────┴────────────────────────────────────────────────────┘
       ↑
    START_BTN_W = 64 reserved on the left of the taskbar
```

The popup is anchored to the Start button's top-left and rendered
last in the frame (after taskbar) so its drop shadow falls on the
taskbar strip itself, not on a window.

---

## State

Just two new bits:

```c
struct launch_entry { const char *label, *path; };
static const struct launch_entry g_launch_items[] = {
    { "wmhello", "/wmhello.elf" },
    { "wmtype",  "/wmtype.elf"  },
    { "wmclock", "/wmclock.elf" },
    { "wmpaint", "/wmpaint.elf" },
};
static int g_launcher_open;
```

The catalog is hard-coded — there's no "discovery" pass over the
filesystem.  Adding an app means listing it here and in the
`WMCLIENT_PROGS` build variable.  Keeps wmd small and removes any
ambiguity about what's "launchable from the WM".

---

## Hit-test order in the press handler

```c
if (g_launcher_open) {
    /* eat the click — close popup, launch if item hit */
}
if (start_button_hit(...)) g_launcher_open = 1;
if (taskbar_hit(...))     /* raise + focus that window */;
hit_test(...);             /* normal window hit */
```

Important: when the popup is open, **any** click anywhere closes
it.  Either it lands on an item (launch + close) or it doesn't
(just close).  This matches dock/menu UX everywhere.

---

## Fork + exec from a graphical task

```c
int pid = sys_fork();
if (pid == 0) {
    const char *argv[2] = { g_launch_items[li].path, 0 };
    sys_exec(g_launch_items[li].path, argv);
    sys_exit(127);
}
```

The child inherits:
- a copy of wmd's page directory, so the FB at `0x50000000`
  and the WM-surface region at `0x60000000+` are mapped — but
  `paging_clone_user_pd` deep-copies the *content* of the FB and
  every WM-shared page, so those mappings reference fresh pages,
  not wmd's originals.  The new task is **NOT** the FB owner
  (`g_fb_owner` is a single global pointer to wmd).
- the open fd table; that includes the shared stdin/stdout to
  the controlling TTY, so the child's `printf` lands on the
  same serial console as wmd's (visible in our smoke test
  trace).

The child immediately `exec`s, which calls `paging_destroy_user_pd`
on the cloned PD and rebuilds it for the new ELF.  Net result:
the child is exactly the same as if launched from the shell — its
own clean page table, no leftover state from wmd.

If `exec` fails (bad path, etc.), the child falls through to
`sys_exit(127)`.  wmd's parent never reaps the child explicitly;
init does the parent-of-orphans dance, and a future
`SYS_WAIT_NB` poll in wmd's main loop is the obvious follow-up
(today the zombie sticks around until wmd exits).

---

## Why not a dedicated `wmlaunch` *client* app?

Considered.  Two reasons against:

1. **Modal popup behaviour.**  The popup should appear / disappear
   on a single click without focus dancing.  Making it a separate
   client would mean it needs its own WM window, then the WM has
   to decide it's a "popup" not a regular window, etc.  Doable
   but more plumbing than the in-WM render.
2. **It IS the WM's job.**  Real OSes embed the dock / Start
   menu in the compositor for the same reason: latency.  The
   in-WM render hits the FB the same frame the user clicked
   Start.

A future `wmlaunch` standalone client makes sense if we ever want
right-click context menus on the desktop or alternative launchers
— those are pluggable.  The built-in Start menu stays for the
"the system always has this" minimum.

---

## Files touched

- `user/wmd.c` — START_BTN_W / LAUNCH_* constants, catalog,
  `g_launcher_open` state, `start_button_hit` + `launcher_hit`,
  `paint_launcher`, press-handler intercepts (launcher click +
  Start click), per-frame `paint_launcher` after `paint_taskbar`
- `smoke_wmlauncher.py` — new harness; three screendumps + 6
  pixel checks across the click-open-launch flow
- `docs/106-pathC-launcher.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 16436 → 17556 (+1120 bytes — popup paint + intercept).

---

## Path C status after session 119

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ✅ 117 — hover vs focus separation
- ✅ 118 — taskbar with click-to-focus
- ✅ 119 — Start button + launcher popup
- ⏳ 120+ — alt-tab cycling, multi-window per client, real fonts,
          desktop wallpapers, system-tray clock in the taskbar

The WM is now self-bootstrapping: `wmd` alone with no other
processes spawned can produce the full user experience — open
apps, switch between them, close them — all through the
framebuffer and the mouse.  That's the milestone Path C was
heading toward since session 107.
