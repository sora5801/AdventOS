# Session 126 — Path C phase 19: VBE FB pages stay out of PMM

**Goal.** Stop the kernel from ever returning VBE framebuffer
MMIO addresses to the PMM bitmap.  Session 125's gc-sections
experiment surfaced this as a latent bug: when a process holds
the FB (mapped via `SYS_FB_MAP` at VA `0x50000000`), various
points in the kernel can hand its physical MMIO addresses to
`pmm_free_page` as if they were RAM.

Status: **done.** All 14 prior smoke tests regress green.

---

## Two paths to the bug

Both involve `(uint32_t *)pd` walks that don't distinguish RAM
from MMIO:

### 1. `paging_destroy_user_pd`

Called when a task exits, or when `sys_exec` retires the old PD
before loading a new one.

```c
for (uint32_t j = 0; j < 1024; j++) {
    if (pt[j] & PTE_PRESENT) {
        pmm_free_page((void *)(uintptr_t)(pt[j] & PAGE_MASK));
    }
}
```

If `pt[j]` covers a page in the FB region, `pmm_free_page` marks
its physical address "free" in the bitmap.  The PMM later hands
that range out as RAM — VRAM aliasing RAM is exactly the kind of
"weird hardware-shaped corruption" that makes for nasty
afternoons.

### 2. `paging_clone_user_pd`

Called on `sys_fork`.  Deep-copies parent pages page by page:

```c
void *src_page = (void *)(uintptr_t)(pte & PAGE_MASK);
void *dst_page = pmm_alloc_page();
memcpy(dst_page, src_page, PAGE_SIZE);
```

For an FB page, `src_page` is MMIO.  The memcpy reads the live
screen pixels into a fresh RAM page; the child's PD then has the
VA pointing at a *snapshot* of the screen, not the FB.  Writes
from the child go to RAM, not to VRAM — which is the *correct*
semantic outcome (single-owner FB!), but the path achieving it is
backwards: it allocates a page to hold a snapshot we don't want,
then lets the destroy step deal with it.

---

## The fix

Both functions now read the VBE FB extent once and check each PTE
before its `pmm_free_page` or `memcpy`.

```c
uint32_t fb_lo = 0, fb_hi = 0;
const struct vbe_state *v = vbe_state();
if (v && v->enabled) {
    fb_lo = v->fb_phys & PAGE_MASK;
    fb_hi = (v->fb_phys + v->fb_size + 0xFFFu) & PAGE_MASK;
}
```

In `paging_destroy_user_pd`, the per-PTE inner loop becomes:

```c
uint32_t phys = pt[j] & PAGE_MASK;
if (fb_lo && phys >= fb_lo && phys < fb_hi) {
    /* MMIO FB page — unmap but DON'T free. */
    continue;
}
pmm_free_page((void *)(uintptr_t)phys);
```

In `paging_clone_user_pd`, we just skip cloning FB-region PTEs
entirely:

```c
uint32_t parent_phys = pte & PAGE_MASK;
if (fb_lo && parent_phys >= fb_lo && parent_phys < fb_hi) {
    continue;
}
```

The child's PD doesn't get the FB mapping at all.  That matches
`g_fb_owner`'s single-owner model: only the binding task can
write to the FB; children must explicitly take it (via
`SYS_FB_MAP`) if they want it.

---

## What stays the same

- The original FB-owner task still has the FB mapped at
  `0x50000000`.  When it eventually exits, `paging_destroy_user_pd`
  walks its PD, hits the FB PTEs, skips the `pmm_free_page` — the
  PT itself still gets freed (it's a normal RAM page), the PD page
  itself still gets freed, the FB pages just aren't returned to
  the allocator.
- `task_exit_current` still calls `wm_release_if_bound` and
  `fbcon_set_enabled(1)`, so the next task that wants the FB can
  grab it.
- A future `SYS_FB_MAP` from a different task re-maps the same
  physical FB pages — the kernel never lost track of them; only
  the PMM bitmap-keeping was at risk, and that path now skips
  these addresses.

---

## Why session 125 noticed it

Before this session, the bug was latent: the PMM's free-page
choice depends on bitmap scan order.  Pre–session-125,
`wmhello.bin` was ~10 KiB, so the post-launcher allocation
pattern reached for RAM pages in a region the corrupted bitmap
hadn't yet exposed.  Session 125's `--gc-sections` experiment
shrank `wmhello.bin` to 2.4 KiB; the allocator state at the time
`wmhello` did `wm_create_window` then sat on the corrupted slot,
and the new client surface's pixel pages aliased VRAM.  Writes
from `wmhello` went to the screen at undefined offsets; `wmd`
read the same pages back via its mapping and composited zeros.

Session 125 reverted the gc-sections change; this session ships
the underlying fix so the gc-sections optimization (and any other
allocator-pressure-sensitive change) can land cleanly later.

---

## Files touched

- `kernel/paging.c` — `#include "vbe.h"`; FB-extent computation +
  range check in `paging_destroy_user_pd` and
  `paging_clone_user_pd`
- `docs/113-pathC-fb-mmio-guard.md` — this file

kernel.bin: 114864 (unchanged — the new code is small and the
linker's gc-sections eats any difference from the comment-only
revert).  No userspace changes.

---

## Path C status after session 126

- ✅ 107..124 — see prior session docs
- ⏭️ 125 — gc-sections attempt parked; smoke fix shipped
- ✅ 126 — VBE FB MMIO no longer enters the PMM bitmap
- ⏳ 127+ — LIBC_TABLE-aware gc-sections (second blocker from
          session 125), or a real-feature session (file manager,
          wallpaper, window resize)
