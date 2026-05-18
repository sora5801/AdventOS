# Session 135 — Path C phase 28: Alt-Tab cycling

**Goal.** Add the standard "cycle to next window" keyboard
shortcut.  Alt+Tab is the convention; the blocker was the kernel
keyboard ABI not surfacing modifier-key state.

Status: **done.** Smoke test (`smoke_wmalttab.py`, 4/4 pass):

```
=== pixel checks ===
  [OK] A: button 0 unfocused = (48, 56, 72)
  [OK] A: button 1 unfocused = (48, 56, 72)
  [OK] B: Alt+Tab focused one window (b0_cyan=True, b1_cyan=False)
  [OK] B: wmd status bar (875/924)
```

Two `wmhello` instances open with no focus.  Send Alt+Tab via
QMP; wmd's taskbar button 0 highlights in CYAN (its frame_color)
to indicate it's now the focused window.

---

## Design: separate channel for the chord

The kernel's main keyboard ring is single-consumer: whoever
polls first drains the byte.  When the shell is at its prompt
it sits in `sys_read_line`, draining everything; if Alt+Tab
went through that ring, the shell would eat it.  Even with the
shell parked, a race remains.

Session 135 sidesteps that by giving Alt+Tab its own dedicated
channel.  USB-HID's `emit_for_usage` recognises Alt+Tab early
and calls a new kernel helper that bypasses the keyboard ring
entirely:

```c
if (alt && usage == 0x2B /* Tab */) {
    extern void wm_post_alttab(void);
    wm_post_alttab();
    return;        /* don't fall through to '\t' */
}
```

`wm_post_alttab()` is in `kernel/wm.c` — bumps a global
counter capped at 8.  A new syscall `SYS_WM_POLL_ALTTAB`
(100) returns 1 + decrements if the counter is non-zero, 0
otherwise.  Only the bound WM task (`g_wm_owner`) gets the
counter; everyone else sees 0.

wmd polls once per frame:

```c
while (sys_wm_poll_alttab() > 0) {
    /* cycle `focused` to next CLIENT window, wrap */
}
```

Net effect: Alt+Tab arrives at wmd deterministically, regardless
of what the shell or any other userspace process is doing.

---

## Cycle behaviour

```c
int start = focused;             /* may be -1 */
int next  = -1;
for (int step = 1; step <= g_window_count; step++) {
    int idx = ((start + step) % g_window_count
               + g_window_count) % g_window_count;
    if (g_windows[idx].kind == KIND_CLIENT) {
        next = idx;
        break;
    }
}
if (next >= 0) {
    g_windows[next].minimized = 0;     /* unhide as a bonus */
    g_z_counter++;
    g_windows[next].raised = g_z_counter;
    focused = next;
}
```

Walk forward from the current `focused` in registration order,
wrapping at `g_window_count`.  Skip non-CLIENT slots (demo
windows when wmd runs without `--clean`).  First CLIENT
encountered wins; raise + un-minimize + focus.

Pressing Alt+Tab repeatedly cycles through all client windows
in registration order.  When focus reaches the last client and
Alt+Tab fires again, it wraps to the first.

---

## What happens for the user

- Click into wmtype, type text — keystrokes go to wmtype.
- Press Alt+Tab — focus jumps to the next client (say wmpaint).
  Keystrokes now go to wmpaint.
- Press Alt+Tab again — wraps to the first if only two; else
  goes to the third.

The cycling doesn't ring-buffer the most-recent-used order yet
(real WMs do).  Future polish: track an MRU list so Alt+Tab
goes "most recent" not "next in registration."

Minimized windows un-minimize on Alt+Tab, which matches how
desktops universally handle it.

---

## Files touched

### Kernel
- `kernel/usb_hid.c` — modifier-aware Alt+Tab intercept;
  HID_MOD_LALT / RALT defines added; the sentinel-byte routing
  from a prior bench is replaced with a call to `wm_post_alttab`
- `kernel/wm.h` — `wm_post_alttab` / `wm_poll_alttab` declarations
- `kernel/wm.c` — `g_alttab_pending` counter + the two functions;
  capped at 8 so a stuck key doesn't overflow
- `kernel/syscall.h` — `SYS_WM_POLL_ALTTAB = 100`
- `kernel/syscall.c` — dispatch case forwarding to `wm_poll_alttab`

### Userspace
- `user/libuser.h`, `user/libuser.c` — `SYS_WM_POLL_ALTTAB`
  constant + `sys_wm_poll_alttab` inline-asm wrapper
- `user/wmd.c` — per-frame poll + cycle loop; un-minimizes
  the target window in addition to raising + focusing

### Tests + docs
- `smoke_wmalttab.py` — new harness, 4 pixel checks
- `docs/121-pathC-alttab.md` — this file

kernel.bin: 135344 → 135344 (unchanged; the new code fits in
the slack).  wmd.bin: 16456 → 16440 (-16 B; the syscall wrapper
+ cycle loop replace a slightly larger keyboard-byte intercept
that's no longer needed).

---

## Why USB-HID and not the PS/2 path

PS/2 keyboard on this QEMU build is unreliable (session 68
documented it); USB-HID is the actual functional path.  The
USB-HID layer already had the modifier byte from the HID
boot-protocol 8-byte report, so the change is one if-test:
`(mods & ALT) && usage == 0x2B → wm_post_alttab()`.

If a future kernel needs PS/2 Alt+Tab, the same intercept can
be added in `kernel/keyboard.c::process_scancode` — track Alt
key down/up via scancodes 0x38/0xB8, then on Tab scancode
(0x0F) call `wm_post_alttab`.  Not done in this session
because no current setup actually drives the PS/2 path.

---

## Path C status after session 135

- ✅ 107..134 — see prior docs
- ✅ 135 — Alt+Tab cycling through client windows

The user-facing keyboard surface is now complete enough for
real multi-window work: type into the focused window, press
Alt+Tab to switch, type again.  Same gesture as every other
desktop OS.
