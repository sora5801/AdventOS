# Session 125 — Path C phase 19: gc-sections attempt + smoke fix

**Goal.** Apply the same `-ffunction-sections` + `--gc-sections`
trick that shaved 21 KiB off `kernel.bin` (session 112) to user
programs.  Expected savings: ~6–8 KiB per WM client that links
libgfx + libuser.

Status: **not shipped.** Two latent issues surfaced during the
experiment that need their own fixes before the optimization can
land.  The session shipped two byproducts:
- a regression-test bug fix (smoke_wmlauncher targeting the wrong
  popup item after session 123's catalog growth) that had been
  silently failing,
- a documentation comment in `build.sh` recording the two issues
  for future work.

---

## What the experiment looked like

Adding to `USER_CFLAGS`:

```sh
-ffunction-sections
```

…and to every user-program link line:

```sh
"$LD" -m i386pe -T user/user.ld --gc-sections -o ...
```

…plus a `KEEP(*(.startup))` on the user.ld `_start` section and a
`LONG(0)` anchor at the top of `.data` so output `.data` stays
PROGBITS (without it, programs whose only globals are zero-init
BSS — like `init` — got NOBITS `.data`, and the kernel ELF loader
faulted on first BSS write).

Per-binary results:

| program  | before | after | delta |
|----------|--------|-------|-------|
| wmd      | 19284  | 14912 | −23 % |
| wmhello  | 10832  | 2416  | −78 % |
| wmtype   | 12316  | 4172  | −66 % |
| wmclock  | 11228  | 4812  | −57 % |
| wmpaint  | 11528  | 4488  | −61 % |
| wmpair   | 10832  | 4400  | −59 % |

About 35 KiB saved across the 6 WM-related binaries.  Most of that
is libgfx + libuser code that the linker correctly identified as
unused per-binary (e.g. wmhello doesn't draw text so the font
table is dead weight).

---

## Issue 1 — LIBC_TABLE indirection

`libuser.c`:

```c
void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ((int (*)(const char *, va_list))LIBC_TABLE[LIBC_FN_VPRINTF])(fmt, args);
    va_end(args);
}
```

`LIBC_TABLE` is `(void * const *)(LIBC_BASE + 0x10)` — a fixed-VA
function-pointer table populated by `libc.bin` at boot.  The
linker can't see the call through that pointer, so when it does
GC reachability analysis from `_start` and `main`, lots of
libuser entry points (`putchar`, `sys_write`, `sys_write_str`,
etc.) get dropped.

The PE/COFF linker is unfortunately lenient about undefined
symbols — it silently resolves them to `0` instead of erroring.
The resulting binary crashes the first time `printf` jumps to
its dropped helper.

Symptom that exposed this: after gc-sections, `wmhello` could
register a WM window (its taskbar button appeared) but never
printed its expected startup `printf` — and never painted its
surface either (suggesting the crash happened just after
`wm_open` returned).

Fix (future): mark every libuser exported symbol with
`__attribute__((used))`, or have the build pass `-u <sym>` to ld
for each unconditional entry, or add `EXTERN(...)` declarations to
`user.ld`.  Need to enumerate the keep-set first.

---

## Issue 2 — FB pages on exec

When `wmd` (which holds the FB at VA `0x50000000`) forks for the
Start-menu launcher, the child inherits the FB mapping in its
cloned page directory.  The child then `exec`s the chosen
program, which calls `paging_destroy_user_pd` on the inherited
PD before building the new program's address space.

`paging_destroy_user_pd` walks every PT and calls `pmm_free_page`
on every mapped page — including the FB pages.  Those are MMIO
addresses pointing into VRAM, not RAM the PMM bitmap owns.
`pmm_free_page` happily marks them "free" in the bitmap, which
corrupts the allocator: the next allocation may hand out a slice
of VRAM as if it were RAM.

This bug is **pre-existing** (sessions 107+), but stays latent
because the post-exec wmhello binary was big enough (~10 KiB)
that the subsequent allocator behaviour didn't visibly affect
correctness in the visible smoke window.  After gc-sections
shrank wmhello to ~2.4 KiB, the allocator state at the time
wmhello calls `wm_create_window` differs enough that the bug
becomes deterministically visible: wmhello's surface VAs end
up pointing into VRAM, so its paints write to the wrong physical
addresses and wmd composites blank pages.

Fix (future): one of —
1. In `sys_exec`, walk the old PD BEFORE `paging_destroy_user_pd`
   and unmap any FB region (`v->fb_phys..v->fb_phys+fb_size`)
   without freeing.
2. In `paging_destroy_user_pd`, range-check each freed page
   against the FB region and skip the `pmm_free_page` for those.
3. Add a per-process "is FB owner" flag that gates the FB
   mapping clone in `paging_clone_user_pd` — so the child
   never gets the FB mapping in the first place.

(3) is the cleanest and most consistent with `g_fb_owner`'s
single-owner model.

---

## What did ship: smoke_wmlauncher fix

While debugging the gc-sections experiment I noticed
`smoke_wmlauncher.py` had been silently failing since session 123.
Session 123 added `wmpair` to the launcher catalog, growing it
from 4 items to 5.  That shifted the popup's top y from 648 to
626 (the popup's `LAUNCH_ITEM_H = 22` per item).

The smoke test was hard-coded to target item 0 at y=648..669
(centre y=659).  With the new layout, that y range is item 1
(wmtype), not item 0 (wmhello).  The launcher was correctly
spawning whatever was clicked — just not wmhello.

The fix is a single line in the smoke test, retargeting the
cursor to item 0's new centre y=636.

This bug had been silently FAIL'ing the test from session 123
onward.  Sessions 123 and 124 ran regressions but used
`grep "FAIL" || echo "(0 FAIL)"` which sometimes printed
`(0 FAIL)` due to QEMU process-collision flakes that produced no
output at all.  Lesson for the harness: detect "(0 OK)" too, not
just "(0 FAIL)".

---

## Files touched

- `build.sh` — documentation comment explaining why
  -ffunction-sections is NOT enabled for user programs (mirrors
  the kernel's enable comment from session 112)
- `smoke_wmlauncher.py` — cursor dy bumped from −10 to −13 so the
  click lands on item 0 of the 5-item popup (was hitting item 1
  since session 123)
- `docs/112-pathC-gc-attempt.md` — this file (postmortem)

kernel.bin and every binary unchanged.

---

## Path C status after session 125

- ✅ 107..124 — see prior session docs
- ⏭️ 125 — gc-sections experiment parked behind two latent fixes;
          smoke_wmlauncher fix shipped
- ⏳ 126 — file manager client (wmfiles) or wallpaper or window
          resize — the next "real feature" session

Net for the user: the WM is unchanged; the test suite is more
honest about what it's actually verifying.
