# Session 124 — Path C phase 18: focus label + right-click menu

**Goal.** Two visible upgrades:
1. The top status bar now tells you which window has keyboard
   focus.
2. Right-clicking a window's title bar opens a small context menu
   with "Raise" and "Close" — both also accessible by other means
   but worth having where the user already is.

Status: **done.** Smoke test (`smoke_wmctxmenu.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] B: ctx menu body @ (458,380) = (32, 40, 48)
  [OK] B: 'Raise' text in menu band (105)
  [OK] B: wmhello title bar visible left of menu (100/100)
  [OK] C: wmhello blue band gone (0/169)
  [OK] C: ctx menu gone @ (458,380) = (10, 24, 40)
  [OK] C: wmd status bar (887/924)
```

Flow verified:
1. boot wmd + wmhello → baseline (A)
2. cursor moves onto wmhello's title bar
3. RIGHT-click → context menu opens at the cursor (B)
4. cursor down to "Close" item
5. LEFT-click → wmhello receives `WM_EV_CLOSE` and exits (C)

All 13 prior smoke tests regress green.

---

## Right-button plumbing

The kernel already tracks all three mouse buttons (`ms.buttons &
0x01 | 0x02 | 0x04` for L/R/M).  wmd previously only watched bit 0.
Session 124 adds:

```c
static int g_prev_right;       /* edge-detect state */
/* ... in tick loop ... */
int right = (ms.buttons & 0x02) ? 1 : 0;
int right_pressed = right && !g_prev_right;
g_prev_right = right;
```

Right-up is unused — the menu is committed by the press, not the
release.

---

## Context menu state

```c
struct ctx_menu_state {
    int          open;
    int          x, y;          /* top-left of popup */
    unsigned int target_id;     /* client_id, not slot index */
};
static struct ctx_menu_state g_ctx_menu;
```

`target_id` is the client_id because slot indices shift across
`drain_wm_messages` compaction.  When the user picks an item, wmd
re-resolves the slot by id:

```c
int target_idx = -1;
for (int i = 0; i < g_window_count; i++) {
    if (g_windows[i].kind == KIND_CLIENT
        && g_windows[i].client_id == g_ctx_menu.target_id) {
        target_idx = i; break;
    }
}
```

If the window died between the right-click and the item-click,
`target_idx` stays -1 and the action is a no-op.

---

## Right-click handler

```c
if (right_pressed) {
    if (g_ctx_menu.open) {
        /* already open — toggle */
        g_ctx_menu.open = 0;
    } else {
        int hit = hit_test(ms.x, ms.y);
        if (hit >= 0 && g_windows[hit].kind == KIND_CLIENT
            && in_titlebar(&g_windows[hit], ms.x, ms.y)) {
            g_ctx_menu.open      = 1;
            g_ctx_menu.target_id = g_windows[hit].client_id;
            g_ctx_menu.x         = ms.x;
            g_ctx_menu.y         = ms.y;
            /* clamp to screen */
            if (g_ctx_menu.x + CTXMENU_W > fb_w)
                g_ctx_menu.x = fb_w - CTXMENU_W;
            if (g_ctx_menu.y + CTXMENU_N_ITEMS * CTXMENU_ITEM_H > fb_h)
                g_ctx_menu.y = fb_h - CTXMENU_N_ITEMS * CTXMENU_ITEM_H;
        }
    }
}
```

The menu ONLY opens on a right-click that lands inside a CLIENT
window's title bar.  Right-clicks on the content area or on
non-client windows (demos, taskbar) are no-ops.  Clamping
guarantees the menu stays inside the FB even when the title bar
was near a screen edge.

---

## Left-click intercept

When the menu is open, any LEFT click closes it:

```c
if (pressed) {
    if (g_ctx_menu.open) {
        int item = ctx_menu_hit(ms.x, ms.y);
        int target_idx = /* lookup by target_id */;
        if (item == 0 && target_idx >= 0) {
            /* Raise */
            g_z_counter++;
            g_windows[target_idx].raised = g_z_counter;
            focused = target_idx;
        } else if (item == 1 && target_idx >= 0) {
            /* Close */
            sys_wm_event_push(g_ctx_menu.target_id, &(struct sys_wm_event){
                .type = WM_EV_CLOSE });
        }
        g_ctx_menu.open = 0;
        goto after_press_hit;
    }
    /* ... existing launcher / taskbar / window hit path ... */
}
```

Goto pattern reused from the launcher (session 119) — same
"intercept eats the click" semantics.

---

## Focus label in the top bar

```c
if (focused >= 0) {
    struct window *fw = &g_windows[focused];
    char fbuf[44];
    int n = 0;
    const char *p = "focus: ";
    while (*p && n < (int)sizeof(fbuf) - 1) fbuf[n++] = *p++;
    for (int i = 0; fw->title[i] && n < ...; i++)
        fbuf[n++] = fw->title[i];
    fbuf[n] = 0;
    gfx_text(&ctx, fb_w / 2 + 80, 5, fbuf, GFX_CYAN, GFX_TRANSPARENT);
}
```

Anchored to `fb_w/2 + 80` so it sits right of the left-side wmd
label without overlapping it on a 1024-wide FB.  Cyan to match
the taskbar focus-button colour.  Empty when no window is
click-focused (i.e. focus == -1).

---

## Files touched

- `user/wmd.c` — focus label in top status bar; `g_prev_right` +
  right-click handler; `g_ctx_menu` state, `paint_ctx_menu`,
  `ctx_menu_hit`; left-click intercept; per-frame paint after the
  launcher
- `smoke_wmctxmenu.py` — new harness, 6 pixel checks (menu body,
  Raise text, wmhello title still visible, post-Close wmhello
  gone, post-Close menu gone, wmd still alive)
- `docs/111-pathC-ctxmenu.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 18548 → 19284 (+736 bytes for the menu paint + handlers).

---

## Path C status after session 124

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing (left button)
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ✅ 117 — hover vs focus separation
- ✅ 118 — taskbar with click-to-focus
- ✅ 119 — Start button + launcher popup
- ✅ 120 — scalable fonts (gfx_text_n)
- ✅ 121 — taskbar clock
- ✅ 122 — multi-window per client (wmpair sample)
- ✅ 123 — --clean mode + launcher catalog update
- ✅ 124 — focus label + right-click context menu
- ⏳ 125+ — window resize, desktop wallpaper, user-prog
          `--gc-sections`, real apps (file manager?)

The compositor's mouse handling now covers both left and right
buttons.  Adding middle-click (e.g. for a "Bring all to front"
gesture) is two more lines of edge-detect — the structural work
is done.
