# Session 24 — mmap on a fd, and the page-fault handler grows up

**Goal:** Add `mmap` so a userspace program can map a file into its address space and read it through plain pointer dereferences. The piece that makes it work cleanly is upgrading the page-fault handler from "print diagnostic + halt" to "check if the fault is in an mmap region — if so, allocate a page, read the file slice into it, install the PTE, return; let the CPU retry the instruction." The page fault handler stops being a debug helper and becomes load-bearing infrastructure.

End state — the new `[t15]` selftest:

```
[t15] mmap fd: lazy page-in via #PF handler
  mmap returned VA 0x50000000
  before touch: page is unmapped (next read will fault)
  p[0] = 'H' (0x48)
  bytes 0..15: 48 65 6c 6c 6f 20 66 72 6f 6d 20 61 20 74 65 78
  first line via mmap: Hello from a text file on AdventFS.
```

The `0x48` is `'H'` from the start of `hello.txt`, read directly out of memory at `0x50000000` — a VA the kernel had marked as "this maps fs:hello.txt @ offset 0" but which had no physical page behind it until the user instruction `printf("...", p[0])` tried to load the byte. That load took a page fault, the kernel populated the page, and the load was retried. From the user code's perspective, `p[0]` was just a memory read.

`httpd.elf` keeps serving curl on :80 throughout (`status=200 bytes=317`).

## What's in scope

In:
- `struct task_mmap` per-task region table (8 slots) + `mmap_brk` bump pointer for VA allocation
- VA window `USER_MMAP_START = 0x50000000` to `USER_MMAP_MAX = 0x60000000` (256 MiB)
- `kernel/mmap.{h,c}` — `mmap_register`, `mmap_unregister`, `mmap_handle_fault`
- Page-fault handler in `isr.c` calls `mmap_handle_fault` first; falls through to the existing diagnostic + halt only if the fault wasn't an mmap demand-load
- `SYS_MMAP = 41`, `SYS_MUNMAP = 42` syscalls + libuser `sys_mmap` / `sys_munmap` wrappers
- Lazy page-in: pages are allocated, zero-filled, and `fs_read`-populated on first touch — not at mmap time
- Per-page tail-zero: if the file ends mid-page, the bytes past EOF in that page read as zero
- `[t15]` selftest doing an end-to-end open → mmap → read-via-pointer → unmap cycle
- Lifecycle wiring: task_create initializes empty regions, fork inherits regions verbatim, exec resets

Out:
- `MAP_SHARED` (writes back to disk) — we're effectively `MAP_PRIVATE` only; writes to the page hit a copy and don't propagate
- `MAP_ANONYMOUS` (mmap with no file) — handlers reject `fs_idx < 0` because we haven't built that path
- `mremap` / `mprotect` / `madvise`
- POSIX `prot` / `flags` / `addr` arguments — our `sys_mmap` only takes `(fd, offset, length)`
- Mapping anything that isn't an `FD_FS` fd (no socket / pipe mmap, no anonymous, no /dev/zero)
- Writeback / msync — pages can be modified but the modifications stay in RAM
- Copy-on-write between forked tasks — fork's existing deep-copy duplicates faulted-in pages eagerly; un-faulted regions get inherited as descriptors and re-faulted independently in each task
- SIGBUS on out-of-bounds access (we just zero-fill past EOF and let the read succeed; POSIX would deliver SIGBUS for accessing past the end)
- SIGSEGV from the page fault handler when the address isn't in any region — we still panic on those, deferring "user PF → SIGSEGV" to a future session
- `mmap` with offset that isn't page-aligned (we don't enforce; the kernel just uses what was passed)

## Architecture: lazy page-in via the fault handler

Here's what happens when a user program does `int x = p[0]` where `p` is an mmap'd VA whose page hasn't been touched yet:

```
ring 3:  user instruction        mov al, byte [p]
                                       │
                                       │ CPU walks the user PD
                                       │ PDE present? yes (covers a 4 MiB
                                       │   chunk; was mapped by the inner
                                       │   page-table allocation).
                                       │ PTE present? NO — entry is 0.
                                       │
                                       ▼
                                  Page fault → push iret frame, raise int 14
                                       │
ring 0:  isr_common_stub        ─→ isr_handler(r)
                                       │
                                       │ n == 14:
                                       │ read CR2 (= the user VA)
                                       │ try mmap_handle_fault(r, cr2)
                                       │
                                       │ mmap_handle_fault:
                                       │   walk current task's mmap
                                       │     regions, find the one
                                       │     containing cr2
                                       │   pmm_alloc_page() → phys
                                       │   memset(phys, 0, 4096)
                                       │   compute slice =
                                       │     intersection of (page,
                                       │     region's file-bytes range)
                                       │   if slice nonempty:
                                       │     fs_read(fs_idx, file_off,
                                       │             phys, slice_len)
                                       │   paging_map_in(user_pd, va,
                                       │                 phys,
                                       │                 USER|WRITABLE)
                                       │   invlpg(va)
                                       │   return 0  ← "handled"
                                       │
                                       │ isr_handler returns; isr_common_stub
                                       │ pops the saved frame
                                       ▼
                                  iret → ring 3 at the same EIP that faulted
                                       │
ring 3:  same instruction retries  mov al, byte [p]
                                       │ PTE present now → load succeeds
                                       │ AL = first byte of hello.txt
```

That's the whole thing. The user code didn't make any syscalls in the read path — the only "I/O" was the page fault transition, which is invisible from C-level. From a debugging perspective the user program is just doing pointer arithmetic; from a kernel perspective, the file is being demand-loaded one page at a time on touch.

## struct task_mmap

```c
struct task_mmap {
    int      in_use;
    uint32_t va_start;       /* page-aligned */
    uint32_t va_end;
    int      fs_idx;         /* AdventFS index */
    uint32_t file_offset;
    uint32_t file_length;    /* bytes from file (may be < va_end-va_start) */
};

struct task {
    ...
    struct task_mmap mmaps[8];
    uint32_t         mmap_brk;
};
```

8 regions per task is plenty for the demo. `mmap_brk` bump-allocates VAs from `USER_MMAP_START`; munmap doesn't reclaim VAs back into the pool — same simplification as `sys_brk` makes for the heap.

The `va_end - va_start` is rounded up to a page; `file_length` is the actual byte count from the file. If a user mmaps a 13-byte file, `va_end - va_start == 4096` (one page) but `file_length == 13`. Bytes 13..4095 within the page read as zero, post-fault.

## The fault handler

```c
int mmap_handle_fault(struct registers *r, uint32_t cr2) {
    struct task *t = task_current();
    if (!t || !t->is_user) return -1;

    struct task_mmap *m = find_region(t, cr2);
    if (!m) return -1;

    /* PTE-already-present + protection-violation = caller bug, not
     * our problem (we map RW so this shouldn't happen). */
    if (r->err_code & 1) return -1;

    uint32_t va_page = cr2 & ~(PAGE_4K - 1u);

    void *page = pmm_alloc_page();
    if (!page) return -1;
    for (int i = 0; i < (int)PAGE_4K / 4; i++) ((uint32_t *)page)[i] = 0;

    /* Fill the file slice. The file part of the region is
     * [m->va_start, m->va_start + m->file_length); the page is at
     * va_page; intersection is what we fs_read. */
    uint32_t page_off = va_page - m->va_start;
    if (page_off < m->file_length) {
        uint32_t take = m->file_length - page_off;
        if (take > PAGE_4K) take = PAGE_4K;
        fs_read(m->fs_idx, m->file_offset + page_off, page, take);
    }

    if (paging_map_in((uint32_t *)t->cr3, va_page, (uintptr_t)page,
                      PTE_USER | PTE_WRITABLE) != 0) {
        pmm_free_page(page);
        return -1;
    }
    __asm__ volatile ("invlpg (%0)" :: "r"(va_page) : "memory");
    return 0;
}
```

A couple of details worth calling out:

**Zero-fill before fs_read.** The page initially has whatever was sitting in PMM-allocated memory before us. Real OSes always zero-fill pages handed to a new owner because not doing so leaks kernel data (ASLR canaries, kernel pointers, etc.) into userspace. Even within a single OS, leaking a previous user's data is a bug. We zero the full 4096 bytes; the fs_read overlays the file slice on top.

**fs_read directly into the freshly-allocated page.** `pmm_alloc_page` returns a physical address that's identity-mapped under the kernel's master PD (which is what we're operating under from inside the fault handler — kernel CR3 is restored on entry to ring 0 via the TSS). So writing through the physical address as if it's a kernel virtual address works.

**`paging_map_in` then `invlpg`.** `paging_map_in` only invlpg's when targeting the kernel master PD (because for any other PD the assumption is "the caller is building a user PD that's not yet active"). The fault handler is the exception: we're modifying the *active* user PD. So we invlpg ourselves.

**`r->err_code & 1` is the protection-violation bit.** Set means the PTE was present but the access wasn't allowed (e.g., write to a read-only page). We don't use that today (everything is mapped USER|WRITABLE), so its presence indicates something we're not modeling correctly — bail to the panic path.

## What if the fault isn't ours?

The `if (mmap_handle_fault(r, cr2) == 0) return;` short-circuits the existing handler. Anything `mmap_handle_fault` returns -1 for falls through to:

```c
kprintf("\n[!] CPU EXCEPTION %u: %s ...\n", ...);
if (n == 14) { /* CR2 dump, error-bit decode */ }
kputs("System halted.\n");
for (;;) cli; hlt;
```

That's the same diagnostic we've had since session 3. The kernel still halts on a real fault — null pointer dereference in user code, stack overflow into a guard page, kernel pointer arithmetic gone wrong. The mmap handler just claims the cases that ARE legitimate.

The right next step is "user-mode PF that mmap_handle_fault didn't claim → SIGSEGV via the signal layer from session 16." That's a 5-line addition documented in pitfalls, deferred so this session stays focused.

## SYS_MMAP and SYS_MUNMAP

```c
case SYS_MMAP: {
    int      fd  = (int)a;
    uint32_t off = b;
    uint32_t len = c;
    struct task *t = task_current();
    if (fd < 0 || fd >= TASK_MAX_FDS)        { ret = 0; break; }
    if (t->fds[fd].kind != FD_FS)            { ret = 0; break; }
    void *va = mmap_register((uint32_t)t->fds[fd].obj_idx, off, len);
    ret = (int32_t)(uintptr_t)va;
}

case SYS_MUNMAP: {
    ret = mmap_unregister((uint32_t)a, (uint32_t)b);
}
```

`SYS_MMAP` is opinionated: `fd` must be `FD_FS`; we extract its `obj_idx` (the disk-FS file index) and pass to `mmap_register`. No support yet for `FD_TMPFS`, `FD_PIPE_*`, `FD_SOCK`, or anonymous mmap — adding those is one case per kind in the fault handler's slice path.

`SYS_MUNMAP` walks each page in the range, calls `paging_translate` to find the physical page (returns 0 if never faulted in), unmaps and frees if present.

Returns are kept simple: `mmap` returns the user VA cast to int (zero on failure), `munmap` returns 0/-1.

## What [t15] verifies

```
mmap returned VA 0x50000000
before touch: page is unmapped (next read will fault)
p[0] = 'H' (0x48)
bytes 0..15: 48 65 6c 6c 6f 20 66 72 6f 6d 20 61 20 74 65 78
first line via mmap: Hello from a text file on AdventFS.
```

- The VA is `USER_MMAP_START` (0x50000000) — first mmap in the shell process.
- The "unmapped" line is informational; we can't actually probe whether it's mapped without faulting.
- `p[0] = 'H'` is the byte at file offset 0 of `hello.txt`. Reading it triggers the page fault; the handler runs; control returns; the load completes.
- `48 65 6c 6c 6f 20 66 72 6f 6d 20 61 20 74 65 78` is "Hello from a tex" in ASCII hex.
- The full first line is printed by writing characters one at a time via `sys_write(1, &p[i], 1)` — each `p[i]` is a memory load against the now-faulted-in page.

Reading 274 bytes (the file's size) from one page (`0x50000000` to `0x50001000`) takes one fault. Subsequent reads on the same page hit the now-resident page directly.

If the file were 8000 bytes, two pages would back the mapping; touching `p[0]` would fault in the first page, touching `p[5000]` later would fault in the second. Each page is independent.

## Files added / modified

| File | Change |
|---|---|
| `kernel/mmap.{h,c}` | New. Region register/unregister + `mmap_handle_fault` |
| `kernel/task.{h,c}` | `task_mmap` struct + per-task array + `mmap_brk`; init/fork/exec lifecycle |
| `kernel/isr.c` | Page-fault handler tries mmap first; falls through to diagnostic+halt |
| `kernel/syscall.{h,c}` | `SYS_MMAP = 41`, `SYS_MUNMAP = 42` |
| `user/libuser.{h,c}` | `sys_mmap` / `sys_munmap` wrappers |
| `user/sh.c` | `[t15]` selftest |

## Design decisions

**Lazy page-in, not eager.** The whole point. mmap'ing a 100 MiB file with eager page-in would mean 100 MiB of `fs_read` and 100 MiB of physical pages allocated up front. Lazy means you pay for what you touch — and for sequential code that touches all of it, the cost is the same plus a fault-overhead per page (cheap). For random-access or rarely-touched code, lazy is dramatically cheaper.

**Effective MAP_PRIVATE only.** Writes to a page after it's been faulted in stay in that page; they don't propagate to disk. POSIX MAP_PRIVATE does the same for file-backed mappings; MAP_SHARED would write back. Our model is private + zero-init-tail-on-fault, which matches what most code wants for read-only file mapping.

**No file-write-back.** Implementing it means tracking dirty pages (a per-page bit) and flushing on `msync` or munmap. Real OSes do this; we punt.

**Per-task region table is small (8 slots).** Real kernels use a balanced tree of VMAs (Linux's `vm_area_struct`). At our scale, a flat array fits in struct task without dynamic allocation. Walking 8 entries on every page fault is microseconds.

**`mmap_brk` bump allocator, no VA reuse.** Same compromise the heap (session 17) makes — VAs leak when munmap fires, but the address space is huge (256 MiB window). Reclaiming VAs would mean either tracking holes (linked list) or compacting (rare in real OSes).

**VAs are page-aligned by construction.** `mmap_register` rounds up `length` to a page and bumps `mmap_brk` by that amount. The handler assumes page-aligned regions; partial-page first or last pages would need extra zero-bookkeeping.

**File-side I/O happens in fault context.** `fs_read` is called from inside `mmap_handle_fault` which runs on the user task's kernel stack with IF=0 (CPU clears IF on entry to int 14). `fs_read` does ATA PIO with polling — works fine in this context. A real OS might async-load and let the task block on the IO; we don't have async I/O.

**Fork inherits the region descriptors.** When `task_fork` runs, the deep-PD copy from session 14 duplicates the *faulted-in* pages. The mmap descriptor table is copied verbatim, so future faults in either parent or child re-resolve through their own copies of the metadata. Pages NOT yet faulted in either side will be faulted-and-populated independently (each side gets its own fresh physical page).

**Exec resets the regions.** All slots cleared, `mmap_brk` reset to start. The new ELF starts with an empty mmap window.

**Zero-fill EVERY freshly-allocated page.** Even pages that will be 100% file-backed get zeroed before fs_read. Cheap insurance against information leaks if a future fs_read returns short or fails.

**`fs_read` failure does not fail the fault handler.** We only check `pmm_alloc_page` and `paging_map_in` for failure; if `fs_read` returns short or -1, the page just has zeros where the file bytes would have been. That's POSIX-compliant for "mmap past end of file" (zeros) and for fs errors it's at worst data corruption rather than a crash. In a more defensive impl we'd surface a SIGBUS.

**One mmap call = one region.** No overlap detection, no merging. Two `sys_mmap` calls to the same fd return two distinct VAs.

## Pitfalls

1. **Unhandled user-mode page faults still panic.** `mmap_handle_fault` returns -1 → diagnostic + halt. A null pointer dereference in a user program takes the kernel down. The right fix is to check `r->cs & 3 == 3` and deliver SIGSEGV via `signal_send`; deferred to keep this session focused.
2. **fs_read failure during fault is silent.** Documented above. If the file is unreadable mid-mmap, the user sees zeroed pages instead of an error.
3. **No munmap on task exit.** When a task dies, `paging_destroy_user_pd` frees all the user pages including the mmap-faulted ones. The mmap region table is part of the task struct and goes away with it. But if there's a kernel bug where `mmap_handle_fault` somehow leaks pages (it doesn't today), the leak would persist beyond exit.
4. **`mmap_brk` doesn't recycle VAs.** Eight munmaps + eight mmaps would consume 16 VA slots' worth of bump space. With 256 MiB available it'd take a long time to exhaust.
5. **Regions don't merge or split.** mapping the same file twice creates two separate regions; munmap of a partial range fails (we require an exact match on (va_start, length)).
6. **Fork's eager copy of faulted-in pages is wasteful.** Real OSes use copy-on-write — both parent and child start by sharing the physical page, marked read-only; the first writer page-faults and gets a private copy. We deep-copy all faulted-in pages eagerly, doubling memory immediately.
7. **No page replacement.** If pmm_alloc_page returns NULL (all pages used), the fault returns -1 → panic. A real OS would page out the LRU mapped page to disk and free its frame.
8. **fs_read in fault context blocks the whole CPU.** ATA PIO is synchronous polling. A user touching an mmap'd page on a slow disk would freeze everything for the read time. With real disks this matters; with QEMU's instant disk, not so much.
9. **`SYS_MMAP` returns int.** With our 256 MiB window starting at 0x50000000, returned VAs are at most 0x60000000 = 1.6 billion = fits in unsigned 32-bit but is "negative" if read as signed. The cast through `(int32_t)(uintptr_t)va` and back to `(void *)(uint32_t)ret` works as long as everyone agrees to read it as unsigned.
10. **No `prot` argument**. We always map RW. A read-only mmap should be RO-mapped so writes to a file-backed mapping fail loudly. We map writable, the writes succeed (into the private page), the user might think they updated the file.

## What might come next

User-mode page faults that aren't mmap-claimed should deliver SIGSEGV instead of panicking the kernel — that's a 5-line addition using session 16's signal_send. Then anonymous mmap (no fd, region is pure RAM with zero-fill on fault). Then proper MAP_SHARED + writeback so an editor can mmap a file and have its changes persist. Then COW on fork so duplicated address spaces don't double memory immediately. Then page replacement for OOM situations. Each of those is its own session.
