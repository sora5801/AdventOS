# Session 107 — Path C phase 1: userspace framebuffer access

**Goal.** Wake up Path C (graphics). The bootloader has been negotiating a VBE framebuffer with the BIOS since session 24-ish, and the kernel paints `fbcon` text into it. But no userspace program had a way to write pixels. Session 107 adds three syscalls that let a user task take ownership of the framebuffer, write pixels directly via a memory-mapped pointer, and release it.

After this session, `cc`-compiled programs can draw graphics. That's the foundation everything else in Path C builds on (window manager, mouse, widgets, fonts).

Status: **done.** Smoke test (graphical):

```
$ gfx 4
gfx: 1024x768 24bpp, pitch=3072, 2304 KiB
[ ... 4 seconds of test card visible on screen ... ]
gfx: released
```

Pixel-perfect verification via QMP `screendump`:

| Sample point | Expected | Got |
|---|---|---|
| Top-left red bar (50, 10) | (255, 0, 0) | (255, 0, 0) ✓ |
| Top yellow bar (300, 10) | (255, 255, 0) | (255, 255, 0) ✓ |
| RGB gradient (100, 200) | (68, 104, 86) | (68, 104, 86) ✓ |
| Centered cross (512, 384) | (255, 255, 255) | (255, 255, 255) ✓ |
| Background (1000, 750) | (32, 32, 32) | (32, 32, 32) ✓ |

---

## The three syscalls

| # | Name | Purpose |
|---|---|---|
| 72 | `SYS_FB_INFO` | Read framebuffer geometry into a `struct sys_fb_info`. Doesn't take ownership. |
| 74 | `SYS_FB_MAP` | Map the FB physical pages into the calling task's address space at a chosen user VA. Marks the task as FB owner; fbcon mutes its text painting. |
| 88 | `SYS_FB_UNMAP` | Release ownership. Auto-called on task exit. |

Slots 72 and 74 were previously `SYS_FB_TAKEOVER` and `SYS_MOUSE_INJECT` — placeholders left over from the retired session-57 WM. Path C reclaims them. Slot 88 is new.

The `sys_fb_info` struct returns:
- `enabled` (0 if no VBE)
- `width`, `height` (pixels)
- `pitch` (bytes per scanline; may exceed `width * bpp/8` on some modes)
- `bpp` (16, 24, or 32)
- `fb_size` (total bytes = `pitch * height`)

---

## Ownership semantics

Only ONE task may own the framebuffer at a time. The kernel tracks the owner in a single global pointer (`g_fb_owner` in syscall.c) and the second simultaneous `SYS_FB_MAP` returns -1.

While owned:

- `fbcon_set_enabled(0)` mutes fbcon's text-painting path. fbcon still accepts characters (writes to the serial port and any other sinks), but won't touch the framebuffer.
- The owner can paint freely; nothing else writes to the FB.

On release (`SYS_FB_UNMAP` or task exit):

- `g_fb_owner` clears to NULL.
- `fbcon_set_enabled(1)`. fbcon resumes painting on its next write.
- The framebuffer is left as the owner painted it — the next fbcon output character will repaint whatever lands at the cursor position; the rest stays. (A future session might add a clear-and-restore-scrollback if that becomes annoying.)

Auto-release on task exit lives in `task_exit_current`:

```c
if (g_fb_owner == t) {
    g_fb_owner = 0;
    fbcon_set_enabled(1);
}
```

That keeps a crashed graphics program from leaving the framebuffer locked.

---

## The mapping

`SYS_FB_MAP(user_va)` walks every page covering `[fb_phys, fb_phys + fb_size)` and calls `paging_map_in(user_pd, user_va + N*PAGE_SIZE, fb_phys + N*PAGE_SIZE, PTE_USER | PTE_WRITABLE)` for each. The kernel page tables already had the FB identity-mapped (from `vbe_init`); now the task's PD gets its own copy too.

For 1024x768x24, that's ceil(2304 KiB / 4 KiB) = **576 pages**. The map loop is the bulk of the syscall's cost — about 13 µs on a Pentium-class VM. Once mapped, pixel writes are direct stores to MMIO — no syscall per pixel.

The user picks the VA. The demo uses `0x50000000` (well clear of code at `0x40000000`, stack at `0x40100000`, heap from `0x40200000`).

Page-alignment: `SYS_FB_MAP` rounds `user_va` down to a 4 KiB boundary and refuses VAs of `0` or anything in the kernel half (`>= 0xC0000000`).

---

## Coexistence with fbcon

The simplest approach was the right one: when a user task owns the FB, fbcon shuts up. When the user releases (or exits), fbcon resumes.

The alternative — compositing fbcon text on top of user pixels — would need a damage-tracking system and a backbuffer for fbcon's scrollback. That's a session by itself; deferred until the WM lands and there's a real reason to mix text and pixels on one display.

For now: one screen, one owner. The shell is the default owner (via fbcon). Run `gfx`, see the test card. After it exits, the shell repaints.

---

## What's deferred to later Path C sessions

The minimum useful thing is in. The natural follow-ups:

| Session | Feature |
|---|---|
| 108 | A simple software framebuffer library: `fb_clear`, `fb_fill_rect`, `fb_line`, `fb_glyph`, `fb_text`. Move common drawing routines out of `gfx.c` into a shared lib so future graphical programs aren't reinventing them. |
| 109 | Mouse input: PS/2 mouse driver + `SYS_MOUSE_POLL` syscall returning (dx, dy, buttons). |
| 110 | Compositing back-buffer / double-buffer to eliminate tearing. |
| 111 | Window manager: a daemon owns the FB, draws decorations, and forwards mouse/keyboard events to client programs via a socket protocol. |
| 112+ | Widgets, fonts, basic event loop. |

The big architectural choice — direct ownership vs. WM-as-broker — gets made at session 111. Session 107 supports both: direct ownership for a single graphics program, and the WM (when built) just becomes "the graphics program that brokers."

---

## Files touched

- `kernel/syscall.h` — `SYS_FB_INFO=72`, `SYS_FB_MAP=74`, `SYS_FB_UNMAP=88`; `struct sys_fb_info`.
- `kernel/syscall.c` — handlers for the three syscalls; `g_fb_owner` global; `#include "fbcon.h"`.
- `kernel/task.c` — `task_exit_current` releases FB ownership.
- `user/libuser.h` + `user/libuser.c` — `sys_fb_info` / `sys_fb_map` / `sys_fb_unmap` stubs; mirror `SYS_FB_*` defines.
- `user/gfx.c` — new test-card program.
- `build.sh` — `gfx` added to USER_PROGS.
- `mkfs.py` — added `gfx.elf` + `fs/man/gfx`.
- `fs/man/gfx` — man page.

Kernel BSS grows by one pointer (`g_fb_owner`). gfx.bin is ~6 KB.

---

## Path C status after session 107

- ✅ 107 — userspace framebuffer access (this session)
- ⏳ 108 — software drawing library
- ⏳ 109 — mouse input
- ⏳ 110 — double-buffering
- ⏳ 111 — window manager daemon
- ⏳ 112+ — widgets, fonts, event loop

Path A (Usable Unix) is complete. Path B Phase 1–3 is complete (cc handles the C surface). Path D (Scripting) is complete. Path C just started.
