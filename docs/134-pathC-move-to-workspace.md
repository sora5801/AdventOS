# Session 148 — Path C phase 41: move window to workspace

**Goal.** Session 147 added 4 workspaces but didn't let the user
*re-assign* a window after creation.  If you opened wmedit on the
wrong workspace you had to close it and reopen — annoying enough
to make workspaces less useful than they should be.

This session adds 4 "Move to WS N" entries to the right-click
title-bar context menu (which itself dates from session 124).
Right-click a title → click "Move to WS 3" → window jumps to
workspace 3.

Status: **done.**  Smoke `smoke_movetows.py` (5/5):

```
=== checks ===
  [OK] baseline wmedit on WS0 (546 px)
  [OK] context menu opens on right-click (6660 slate px)
  [OK] wmedit hidden after move (WS0 view: 0)
  [OK] wmedit visible after WS3 switch (546 px)
  [OK] wmd status bar alive (842/924)
```

---

## Context menu layout

The session-124 menu had two items; with this session it gets four
more — one per workspace.

```
+--------------+
| Raise        |   item 0
| Close        |   item 1
+--------------+
| Move to WS 1 |   item 2 — workspace 0
| Move to WS 2 |   item 3 — workspace 1
| Move to WS 3 |   item 4 — workspace 2
| Move to WS 4 |   item 5 — workspace 3
+--------------+
   128 x 108
```

`CTXMENU_W` widened from 100 → 128 to fit "Move to WS 4".  Total
height = 6 × 18 = 108 px.

The current workspace's "Move to WS N" entry is **dimmed**
(0x707880 grey) so the user knows clicking it is a no-op:

```c
if (i >= 2 && i < 2 + NUM_WORKSPACES
    && (i - 2) == g_current_workspace) {
    fg = 0x707880u;     /* dimmed */
}
```

Selecting it still does nothing — the handler sets
`window.workspace = dst` which is just the existing value.

---

## Handler

In the existing context-menu press handler:

```c
} else if (item >= 2 && item < 2 + NUM_WORKSPACES
           && target_idx >= 0) {
    int dst = item - 2;
    g_windows[target_idx].workspace = dst;
    if (dst != g_current_workspace && focused == target_idx) {
        focused = -1;
    }
}
g_ctx_menu.open = 0;
```

The `focused` drop matches session 147's workspace-switch
behaviour — if the focused window is moving off the current
workspace, kill focus so subsequent keystrokes don't get routed
to an invisible window.

The window's `workspace` field changes immediately.  On the next
paint, the workspace-filter check (added in session 147) skips
the moved window from the current view; switching to its new
workspace via Alt+N or a top-bar click reveals it.

---

## What stays out of scope

- **Drag between workspaces.**  No "drag window onto a workspace
  button to send it there."  Pure menu-driven for now.
- **Move all-from-workspace.**  No "send all windows from WS3
  to WS1" bulk operation.
- **Per-workspace launcher.**  Apps you open from the Start
  menu still land on the current workspace; no way to
  pre-target.
- **Keyboard shortcut for move.**  No "Ctrl+Alt+Shift+N to move
  focused window to WS N."  Would need a new kernel channel
  for the modifier combination.

---

## Files touched

- `user/wmd.c`:
  - `CTXMENU_W` widened 100 → 128
  - `CTXMENU_N_ITEMS` grew 2 → `2 + NUM_WORKSPACES` (= 6)
  - `g_ctx_labels[]` gains "Move to WS 1..4" rows
  - `paint_ctx_menu` dims the current-workspace row
  - Press handler dispatches `item >= 2` to set
    `window.workspace = dst`
- `smoke_movetows.py` — new harness, 5 pixel checks
- `docs/134-pathC-move-to-workspace.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged net)
- wmd.bin: 19168 → 19424 (+256 B for the 4 new menu rows +
  handler dispatch + dim-current logic)

---

## Path C status after session 148

- ✅ 107..147 — see prior docs
- ✅ 148 — move window to workspace via context menu
- ⚠️  wmterm input + close — still deferred

Workspaces are now fully usable: open on the wrong one →
right-click → Move to WS N.
