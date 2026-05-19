# Session 147 — Path C phase 40: workspaces / virtual desktops

**Goal.** Four virtual desktops.  Each window lives on exactly one
workspace; only the current workspace's windows are painted or
clickable.  Switch via Alt+1..4 keyboard shortcut or by clicking
one of four numbered buttons in the top status bar.

Status: **done.**  Smoke `smoke_workspaces.py` (6/6 pass):

```
=== checks ===
  [OK] WS1 selected at boot (ws0-cyan 18)
  [OK] wmedit visible on WS1 (546 px)
  [OK] WS2 selected after click (ws1-cyan 17)
  [OK] wmedit HIDDEN on WS2 (0 px)
  [OK] wmedit reappears on WS1 return (546 px)
  [OK] wmd status bar alive (842/924)
```

Boot wmd + wmedit on workspace 1 (initial).  Click the workspace-2
button: wmedit disappears (it's still on WS1, kept in memory, just
not painted).  Click workspace-1 button: wmedit reappears with the
same content + position it had before.

---

## Plumbing

### Kernel — workspace request channel

Mirrors the session-135 Alt+Tab pattern:

```c
/* kernel/wm.c */
static volatile int g_workspace_pending = -1;

void wm_post_workspace(int n) {
    if (n >= 0 && n < 4) g_workspace_pending = n;
}

int wm_poll_workspace(struct task *caller) {
    if (g_wm_owner != caller) return -1;
    int n = g_workspace_pending;
    g_workspace_pending = -1;
    return n;
}
```

One-slot — only the most-recent request matters (rapid Alt+1
Alt+3 collapses to the second).

Exposed via `SYS_WM_POLL_WORKSPACE = 107`; wmd polls once per
frame.

### USB-HID — Alt+1..4 detection

In `kernel/usb_hid.c:emit_for_usage`, after the existing
Alt+Tab intercept:

```c
if (alt && usage >= 0x1E && usage <= 0x21) {
    extern void wm_post_workspace(int n);
    int ws = (int)usage - 0x1E;     /* HID '1'..'4' -> 0..3 */
    wm_post_workspace(ws);
    return;     /* don't fall through to ASCII inject */
}
```

Bypasses the keyboard ring so shells don't see "1" / "2" /
"3" / "4" appearing while the user switches workspaces.

### wmd — workspace state + gating

```c
#define NUM_WORKSPACES 4
static int g_current_workspace = 0;

struct window {
    /* ...existing fields... */
    int workspace;   /* 0..3 */
};
```

Each frame, drain the kernel workspace channel:

```c
int ws = sys_wm_poll_workspace();
if (ws >= 0 && ws < NUM_WORKSPACES && ws != g_current_workspace) {
    g_current_workspace = ws;
    focused = -1;   /* prev focus is now on a hidden workspace */
}
```

Paint loop + `hit_test` skip windows on other workspaces:

```c
if (g_windows[idx].workspace != g_current_workspace) continue;
```

New client windows (`drain_wm_messages` op=1) get
`workspace = g_current_workspace` at registration time — the
window opens on whichever workspace the user is currently
viewing.

### Top status bar — four numbered buttons

```
┌──────────────────────────────────────────────┐
│ wmd  [1][2][3][4]              focus: wmedit │   ← y=0..17, 18 px tall
└──────────────────────────────────────────────┘
```

Four 22×14 buttons at `x = 44 + ws * 24`, `y = 2`.  Current
workspace is filled cyan with a black digit; others slate
(0x303848) with white.  Clicks at `(ms.x, ms.y)` inside any
button switch via the same path as Alt+N (set
`g_current_workspace`, drop focus).

---

## Why focus resets on switch

If the focused window is on the workspace you're leaving, the
keyboard-key router would keep sending WM_EV_KEY to it — making
the user think their keystrokes are "lost" (they're going to a
hidden window).  Setting `focused = -1` after a switch means
the user must click on a visible window to refocus, which
matches every other WM's behaviour.

---

## What stays out of scope

- **Move-window-to-workspace.**  Once a window opens on a
  workspace, it stays there.  No "send window to workspace 2"
  context-menu item yet.  Future polish.
- **Workspace overview.**  No "show me thumbnails of all 4
  workspaces" mode (Expose / Mission Control).  The four
  number buttons are the only visualisation.
- **Per-workspace wallpapers.**  Wallpaper is shared across
  all workspaces; only window visibility changes.
- **Persistence.**  Workspace state vanishes when wmd exits.
- **More than 4 workspaces.**  Hard-coded to `NUM_WORKSPACES = 4`
  to match the keyboard shortcut (Alt+1..4) and the top-bar
  button space.  Could grow if needed.

---

## Files touched

- `kernel/wm.c`, `kernel/wm.h` — `g_workspace_pending` slot +
  `wm_post_workspace` / `wm_poll_workspace`
- `kernel/syscall.h`, `kernel/syscall.c` — `SYS_WM_POLL_WORKSPACE = 107`
- `user/libuser.c`, `user/libuser.h` — `sys_wm_poll_workspace`
- `kernel/usb_hid.c` — Alt+1..4 intercept routes to
  `wm_post_workspace`
- `user/wmd.c`:
  - `workspace` field on every window
  - `g_current_workspace` global
  - drain in main loop + focus reset
  - paint loop + `hit_test` skip-other-workspace
  - top-bar workspace buttons (paint + click handler)
- `smoke_workspaces.py` — new harness, 6 pixel checks
- `docs/133-pathC-workspaces.md` — this file

Sizes:
- kernel.bin: 159920 (workspace ring is tiny; --gc-sections
  absorbed the diff)
- wmd.bin:    18620 → 19168 (+548 B for workspace state +
  buttons + paint gating)

---

## Path C status after session 147

- ✅ 107..146 — see prior docs
- ✅ 147 — workspaces (Alt+1..4 + top-bar buttons)
- ⚠️  wmterm input + close — still deferred

Four virtual desktops give serious headroom for the user's
window collection.  Pair with snap-to-edge (session 138) and
edge-resize (session 146) for full keyboard-and-mouse window
management.
