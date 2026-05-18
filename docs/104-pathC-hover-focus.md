# Session 117 — Path C phase 11: hover vs focus separation

**Goal.** Stop conflating "the mouse is over this window" with "this
window receives keyboard input."  Real WMs have separate concepts;
ours does now too.

Status: **done.** New smoke test (`smoke_wmhover.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] IN: green hover border @ y=516 (180/180)
  [OK] OUT: grey hover border @ y=516 (180/180)
  [OK] OUT: no green border (0/180)
  [OK] OUT: wmhello blue band still painted (170/170)
  [OK] OUT: wmd status bar alive (887/924)
```

Plus full regression: all six prior session smoke tests (110–116)
still pass with the new event shape.

What we just verified:
- Move cursor INTO wmhello → wmhello's bottom border turns GREEN
  (the client got `WM_EV_HOVER_ENTER`).
- Move cursor BACK OUT → border turns GREY (the client got
  `WM_EV_HOVER_LEAVE`).
- The window is otherwise undisturbed — `WM_EV_FOCUS` did NOT
  fire from the hover crossing (it only fires on click now).

---

## Two event pairs, two responsibilities

| event              | when it fires                          | typical use |
|--------------------|----------------------------------------|-------------|
| `WM_EV_HOVER_ENTER` | mouse cursor enters the window's content area | tooltip, mouse-tracking widgets, "this is what you're pointing at" feedback |
| `WM_EV_HOVER_LEAVE` | cursor leaves the content area | hide tooltip, stop tracking |
| `WM_EV_FOCUS`      | window becomes the click-focused one (raised on press) | activate keyboard handling, change title-bar styling to "active" |
| `WM_EV_UNFOCUS`    | another window or the desktop gets focus | dim title bar, stop blinking caret, etc. |

Click-focus survives mouse movement.  If you click wmtype, then
move the cursor away to wmpaint without clicking, wmtype stays
focused (still gets the keystrokes); wmpaint just gets a hover
indication.  Press Enter and it still goes to wmtype.

---

## wmd-side implementation

Hover and focus are tracked by independent state variables in the
compositor's main loop:

```c
int prev_hover_idx = -1;  /* g_windows[] index of hovered window */
int prev_focus_id  = 0;   /* client_id of click-focused window  */
```

Hover edges run every tick from the existing hit-test:

```c
if (target != prev_hover_idx) {
    if (prev_hover_idx >= 0 && g_windows[prev_hover_idx].kind == KIND_CLIENT)
        push WM_EV_HOVER_LEAVE to old hover;
    if (target >= 0)
        push WM_EV_HOVER_ENTER to new hover;
    prev_hover_idx = target;
}
```

Focus edges run only when wmd's internal `focused` index changes,
which only happens on a press:

```c
unsigned int new_focus_id =
    (focused >= 0 && g_windows[focused].kind == KIND_CLIENT)
    ? g_windows[focused].client_id : 0u;
if (new_focus_id != prev_focus_id) {
    if (prev_focus_id != 0) push WM_EV_UNFOCUS to old focus;
    if (new_focus_id != 0) push WM_EV_FOCUS to new focus;
    prev_focus_id = new_focus_id;
}
```

Note `prev_focus_id` is a **client_id**, not a slot index — slot
indices shift when `drain_wm_messages` compacts the array on
destroy.  client_ids are stable.

---

## Client adoption

The existing four client apps split as you'd expect once the
distinction lands:

- **wmhello** uses HOVER for the "cursor is here" indicator
  (border colour) and just counts FOCUS edges for the diagnostic
  printf at exit.  This is the visual the smoke test verifies.
- **wmtype** uses FOCUS for the "click to type" status text and
  the blue/grey title bar — exactly right.  It already had
  click-focus semantics; the new model finally makes them
  correct.
- **wmclock** uses FOCUS for its dark-blue vs dark-grey background.
  Same story.
- **wmpaint** uses FOCUS for the toolbar background tint.  Same
  story.

For wmhello specifically, the switch was a four-line change in the
event switch: add HOVER cases for what was the old FOCUS behaviour,
keep FOCUS/UNFOCUS cases that bump the counter but don't change
the visual.

---

## Why this matters

Before the split, opening a wmtype + a wmpaint + just hovering the
mouse between them would yank keyboard focus back and forth.  Type
'r' over wmpaint expecting it to set radius, and you'd get the
character routed to wmpaint (because hover = focus) — but the
expectation in any normal WM is that focus *follows clicks*, not
the mouse.

After the split, the same flow does what users expect: keyboard
goes to wherever you last clicked; the mouse can wander freely.

It also gives clients a clean signal for two different visual
states ("you're pointing at me" vs "I have your keyboard"), which
matches widget toolkits everywhere.

---

## Files touched

- `kernel/syscall.h` — `WM_EV_HOVER_ENTER = 8`, `WM_EV_HOVER_LEAVE = 9`
- `user/libuser.h` — same constants mirrored for clients
- `user/wmd.c` — split hover and focus state; push HOVER on
  hover-change, FOCUS on click-focus change (~15 added lines, plus
  the rename of the old FOCUS handling to HOVER)
- `user/wmhello.c` — react to HOVER for border colour; count FOCUS
  separately for the exit printf
- `smoke_wmhover.py` — new harness, 5 pixel checks across two
  screendumps (cursor IN vs OUT of wmhello)
- `docs/104-pathC-hover-focus.md` — this file

kernel.bin: 114864 (unchanged).  wmd.bin: 14964 → 15764 (+800 B for
the second-event-stream tracking).

---

## Path C status after session 117

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing (old FOCUS = hover)
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint real apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ✅ 117 — hover vs focus separation
- ⏳ 118+ — alt-tab cycling, multi-window per client, real fonts,
          taskbar-style window list

The event ABI is now what a real WM has: HOVER_ENTER, HOVER_LEAVE,
FOCUS, UNFOCUS, MOUSE_MOVE, MOUSE_PRESS, MOUSE_RELEASE, KEY, CLOSE.
Nine event types covering the standard interactions a client app
needs to be responsive without polling.
