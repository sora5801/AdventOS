# Session 122 — Path C phase 16: multi-window per client

**Goal.** Let one client process open more than one WM window.
Session 112's protocol could already handle multiple windows from
different processes (one per task), but a single task that called
`SYS_WM_CREATE` twice would have both calls return the same
client-side VA `0x60000000`, clobbering the first window's
mapping.

Status: **done.** Smoke test (`smoke_wmpair.py`, 6/6 pass):

```
=== pixel checks ===
  [OK] pair-A blue band @ y=386 (146/160)
  [OK] pair-A teal bg @ (371,408) = (16, 48, 64)
  [OK] pair-B magenta band @ y=426 (147/160)
  [OK] pair-B magenta-bg @ (581,448) = (48, 16, 48)
  [OK] pair-A taskbar button @ (138,753) = (48, 56, 72)
  [OK] pair-B taskbar button @ (282,753) = (48, 56, 72)
```

Verifies that `wmpair` (a single client process) successfully
opens TWO windows with distinct backgrounds (teal vs magenta),
both visible at the same time, both with their own taskbar
buttons.

---

## The fix

One new field in `struct task`:

```c
/* Bump allocator for WM-surface VAs.  Each SYS_WM_CREATE call
 * advances this; starts at 0 (sentinel = "first call"). */
uint32_t  next_wm_va;
```

And one tweak inside `wm_create_window`:

```c
/* Old: client_va = WM_SURFACE_VA_BASE; (every call) */
if (client->next_wm_va == 0)
    client->next_wm_va = WM_SURFACE_VA_BASE;
uint32_t client_va = client->next_wm_va;
/* ... allocate + map ... */
client->next_wm_va = client_va
                   + WM_MAX_PAGES_PER_WIN * PAGE_SIZE;   /* +1 MiB */
```

The bump advances by **`WM_MAX_PAGES_PER_WIN * PAGE_SIZE = 1 MiB`**
per allocation, regardless of how big the actual surface is.  This
matches the WM-side per-slot VA reservation, so the two address
spaces are arithmetically congruent.

The bump only advances on a successful commit (the increment is
*after* the `rollback:` label), so a failed allocation can be
retried at the same VA without growing a leak.

---

## What about fork?

A WM-client task that calls `sys_fork()` clones its `task` struct,
including `next_wm_va`.  The child inherits the bump pointer, so
its own subsequent `SYS_WM_CREATE` allocates at the *next* free
slot from the parent's perspective.  Since the child has its own
PD (cloned at fork), the parent's actual windows aren't visible
to the child — but the VA region they occupied IS reserved, so the
child won't pick a conflicting VA.

This is conservative.  A cleaner future fix: only inherit
`next_wm_va` if the parent's windows actually got cloned (which
today they DO, page by page, through `paging_clone_user_pd` — but
the kernel WM table only tracks the parent's pid as owner).
Session 122 lives with the conservative semantics; no client
currently does fork-without-exec across a WM-window boundary.

---

## The wmpair demo

`wmpair` is a deliberately minimal proof: open two windows from
one process, paint distinctive colours in each, repeat.

```c
wm_open(&w1, "pair-A", 200, 100);    /* gets VA 0x60000000 */
wm_open(&w2, "pair-B", 200, 100);    /* gets VA 0x60100000 */
/* ... per-frame paint loop ... */
```

The single-process design also tests the kernel's
`wm_pop_message` correctly handling two op=1 events in one drain.

Closing EITHER window via the WM's red X delivers a single
`WM_EV_CLOSE` to that window; wmpair sets a `closed` flag and
both surfaces tear down on the next iteration.

---

## Files touched

- `kernel/task.h` — `uint32_t next_wm_va` field
- `kernel/wm.c` — use & advance the bump in `wm_create_window`
- `user/wmpair.c` — new ~110-line sample client
- `build.sh` — `wmpair` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmpair.elf` + man page packed
- `fs/man/wmpair` — new
- `smoke_wmpair.py` — new harness, 6 pixel checks
- `docs/109-pathC-multiwindow.md` — this file

kernel.bin: 114864 (unchanged — pure userspace-facing change).
wmpair.bin: new, 10832 bytes.

---

## Path C status after session 122

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
- ✅ 120 — scalable fonts (gfx_text_n)
- ✅ 121 — taskbar clock
- ✅ 122 — multi-window per client
- ⏳ 123+ — `--gc-sections` for user-prog links, desktop wallpaper,
          context menus, hide the demo windows

The protocol now supports the full "real WM" usage pattern: one
process can manage several windows, each gets independent input
events, the user closes them one by one or via process exit.  A
file-manager-style app or a multi-document editor is now
implementable with the same APIs.
