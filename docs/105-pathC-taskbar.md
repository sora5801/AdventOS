# Session 118 — Path C phase 12: taskbar

**Goal.** Surface the open windows somewhere clickable.  Sessions
116–117 made focus cooperative and explicit; phase 12 adds the
last piece of standard WM chrome: a taskbar at the bottom of the
screen that shows every client window and brings it forward on
click.

Status: **done.** Smoke test (`smoke_wmtaskbar.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] BEFORE: taskbar strip bg @ y=755 (700/700)
  [OK] BEFORE: button bg @ (74,753) = (48, 56, 72)
  [OK] BEFORE: title text on button (155)
  [OK] AFTER: button bg @ (74,753) = (48, 224, 224)
  [OK] AFTER: wmhello blue band still painted (169/169)
```

What the test verifies:
- The taskbar strip (dark blue 0x182030) spans the bottom 28 px
  of the framebuffer.
- wmhello's button is visible inside the strip with its title
  text in white.
- Click on the button → wmhello becomes click-focused → the
  button highlights in `wmhello.frame_color` (CYAN).
- Compositor still alive afterward.

All 7 prior smoke tests (110–117) regress green: the taskbar
doesn't intrude on the regions they sample.

---

## Layout

```
┌───────────────────────────────────────────────────────────────┐
│ wmd - AdventOS Path C session 111                             │  ← 18 px status bar
├───────────────────────────────────────────────────────────────┤
│                                                                │
│   (demo + client windows live here)                            │
│                                                                │
│                                                                │
├───────────────────────────────────────────────────────────────┤
│ ▮ wmhello                                                     │  ← 28 px taskbar
└───────────────────────────────────────────────────────────────┘
```

Constants:

```c
#define TASKBAR_H        28
#define TASKBAR_BTN_W    140
#define TASKBAR_BTN_PAD  4
```

The bar is dark blue `0x182030u` to distinguish it from the
desktop background `0x0A1828u`; a 1-px GFX_GREY line marks the
top edge.  Each client button is 140×20 px with 4 px of horizontal
padding between buttons.  At 1024-px width that's 7 visible
buttons before overflow — same cap as `WM_MAX_WINDOWS`.

---

## Painting

A new `paint_taskbar(ctx, focused_idx)` runs once per frame after
all the windows.  It walks `g_windows[]` in registration order
(not z order — taskbar buttons stay in their place even when
windows raise above each other) and emits a button for each
`KIND_CLIENT` entry:

```c
unsigned int fill = is_focused ? w->frame_color : 0x303848u;
gfx_fill_rect(ctx, bx, by, TASKBAR_BTN_W, bh, fill);
gfx_rect(ctx, bx, by, TASKBAR_BTN_W, bh,
         is_focused ? GFX_WHITE : GFX_GREY);
gfx_text(ctx, bx + 6, by + 5, w->title, GFX_WHITE, GFX_TRANSPARENT);
```

- Unfocused button: dark slate `0x303848` background, grey border.
- Focused button: window's own `frame_color` (CYAN for current
  clients), white border.  Match between button highlight and
  window title-bar colour makes it obvious which button is which
  window.

The 16-character title that wmd already truncated at registration
time fits the 140-px button width without further clipping.

---

## Hit handling

Done in the press handler, before the regular window hit-test:

```c
int tb_hit = taskbar_hit((int)ctx.width, (int)ctx.height,
                         ms.x, ms.y);
if (tb_hit >= 0) {
    g_z_counter++;
    g_windows[tb_hit].raised = g_z_counter;
    focused = tb_hit;
    goto after_press_hit;
}
```

`taskbar_hit` returns the `g_windows[]` index of the clicked
button, or -1 if the click lands in the strip but not on a
button (e.g. between buttons or past the last one).  Crucially,
a hit BYPASSES the close-button intercept and the drag logic —
no need to worry about either when you're clicking the taskbar.

After the goto, the session-117 focus-edge code still fires
(`prev_focus_id != new_focus_id`), so the focused client receives
a clean `WM_EV_FOCUS` event.

---

## Things deliberately NOT done

- **Right-click context menu** (close, minimize, etc.).  Adds
  more event-routing complexity than session 118 wanted to absorb.
- **System-tray icons.**  No.
- **Window minimize / "iconified" state.**  Would need a new
  visibility flag in `struct window` plus paint-skip; out of
  scope.
- **Clock / system info on the right side.**  wmclock the
  *application* exists; wmd doesn't replicate that work in chrome.
- **Drag the bar.**  Bar is fixed at the bottom.

The taskbar's job is "list the windows and let me click one."
Anything beyond that gets its own session.

---

## Files touched

- `user/wmd.c` — `TASKBAR_*` constants, `taskbar_hit`,
  `paint_taskbar`, press-handler taskbar intercept,
  per-frame call after windows are painted (~75 lines)
- `smoke_wmtaskbar.py` — new harness, 5 pixel checks with
  before/after screendumps
- `docs/105-pathC-taskbar.md` — this file

kernel.bin: 114864 (unchanged — userspace only).
wmd.bin: 15764 → 16436 (+672 bytes for the taskbar painter +
hit-test).

---

## Path C status after session 118

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
- ✅ 117 — hover vs focus separation
- ✅ 118 — taskbar with click-to-focus
- ⏳ 119+ — alt-tab cycling (needs modifier-key support in
          sys_kbd_poll), multi-window per client, real
          (larger) fonts, app launcher

The user-facing WM surface is now complete enough that someone
could pick up the system and use it without a manual:

- Click the X to close a window.
- Click a window's title bar to drag it.
- Click anywhere else in a window to focus + raise it.
- Click an empty taskbar button to focus + raise a hidden window.
- Type — keystrokes go to whatever you last clicked.
- The mouse cursor visibly tracks position with a colour code
  for "I'm pressed but not on a window" (yellow) vs "I'm
  dragging a window" (red) vs idle (white).
