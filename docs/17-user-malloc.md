# Session 17 — User-mode malloc and SYS_BRK

**Goal:** Give every user task its own growable heap. Mechanically: add `SYS_BRK` (a Linux-style `brk(2)` that maps fresh user pages on demand between the current break and the requested target) and port the kernel's existing free-list allocator into libuser as `malloc()` / `free()`.

End state — the `[t7]` selftest at boot:

```
[t7] malloc / free / brk
  initial brk: 0x40200000  (heap empty)
  malloc 32/64/128:
    a=0x40200010  b=0x40200040  c=0x40200090
    brk now 0x40201000  (grew by 4096 bytes)
    *a=0xa1a1a1a1  *b=0xb2b2b2b2  *c=0xc3c3c3c3  (read back ok)
  free(b) + malloc(64) -> b2=0x40200040  reused=yes
  after free-all: used=0  free=4080  brk=0x40201000
  malloc(8192) -> 0x40200010  brk=0x40204000  (grew 16384 bytes total)
```

That's a fresh shell process: heap starts empty, three small mallocs trigger one page of growth, the addresses reflect 16-byte headers + 16-byte-aligned payloads, freeing the middle block lets a same-sized realloc reuse the exact slot, and a big enough request grows the heap by another 12 KiB. `httpd.elf` keeps serving curl on :80 throughout.

## What's in scope

In:
- `SYS_BRK = 27` syscall (Linux-flavored): `brk(0)` queries current break; `brk(new)` grows or shrinks
- Per-task `heap_brk` field on `struct task`; init at task_create, inherit on fork, reset on exec
- Kernel growth: maps fresh pages from `pmm_alloc_page()` into the user PD via `paging_map_in()` between the current break (page-rounded up) and the requested break (page-rounded up)
- Kernel shrink: not implemented (we have no `paging_unmap_in`); break can decrement, pages stay mapped until exec or process exit
- libuser allocator port of `kernel/kmalloc.c`: same 16-byte-aligned headered blocks, first-fit, split-on-alloc, neighbor-coalesce on free
- libuser growth strategy: malloc walks the free list; on no-fit, calls `sys_brk(brk + page-rounded-up(needed))`, drops a fresh free block on the new pages, retries
- Diagnostics: `malloc_total`, `malloc_used`, `malloc_free_bytes`, `malloc_brk` for tests
- New typedef `uint16_t` in libuser.h (the allocator header packs `free` and `magic` into 16-bit fields to keep the header at 16 bytes)

Out:
- `sbrk(delta)` — additive variant; trivially derivable, not added
- `mmap` / anonymous mappings beyond the heap window
- `realloc` — could be added in a few lines (alloc new, memcpy, free old) but the demo doesn't need it
- `calloc` — same comment; just `malloc + memset`
- Multiple heap arenas / size-class / slab allocators
- Heap unmap on shrink — needs `paging_unmap_in` plumbing, deferred
- Real `mremap`-style heap relocation
- Per-task heap-stats syscalls — exposed only via libuser-internal accessors today
- Thread safety on the user side (single-threaded user tasks; the lock can come back if we add user threads)

## Architecture: each process gets a brk-managed VA window

Every user PD has the same layout for the heap:

```
high VA ──┬─────────────────────────┐
          │                         │
          │   USER_HEAP_MAX         │  0x40600000
          │      (4 MiB cap)        │
          │   ───────────────────   │  ← unmapped here
          │                         │
          │   heap pages            │  mapped on demand by SYS_BRK
          │   (start small, grow)   │
          │                         │
          │   USER_HEAP_START       │  0x40200000
          │                         │
          │   ───────────────────   │  ← unmapped between stack and heap
          │                         │
          │   user stack (1 page)   │  0x40100000
          │   user code/data        │  0x40000000
low  VA ──┴─────────────────────────┘
```

The kernel tracks a single `heap_brk` per task. It starts equal to `USER_HEAP_START` (zero pages mapped). The user-side malloc calls `sys_brk(target)` to extend; the kernel maps `(target - heap_brk)` worth of pages, page-rounded up, and updates `heap_brk = target`. From the malloc's POV the heap is `[USER_HEAP_START, heap_brk)`.

Cap is 4 MiB. A real OS would scale with `RLIMIT_DATA` / `RLIMIT_AS`; we hardcode.

## SYS_BRK

```c
case SYS_BRK: {
    uint32_t want = a;
    struct task *t = task_current();

    if (want == 0)                                   { ret = t->heap_brk; break; }
    if (want < USER_HEAP_START || want > USER_HEAP_MAX) {
        ret = t->heap_brk;
        break;
    }

    uint32_t cur_pg = (t->heap_brk + 0xFFF) & ~0xFFF;
    uint32_t new_pg = (want        + 0xFFF) & ~0xFFF;

    if (new_pg > cur_pg) {
        /* Grow: map fresh pages from cur_pg to new_pg. Roll back
         * (well — surrender the partially-mapped range; see deep
         * dive notes) if any allocation fails. */
        uint32_t mapped_to = cur_pg;
        int ok = 1;
        for (uint32_t va = cur_pg; va < new_pg; va += 4096) {
            void *page = pmm_alloc_page();
            if (!page) { ok = 0; break; }
            for (int i = 0; i < 4096/4; i++) ((uint32_t *)page)[i] = 0;
            if (paging_map_in((uint32_t *)t->cr3, va, (uintptr_t)page,
                              PTE_USER | PTE_WRITABLE) != 0) {
                pmm_free_page(page);
                ok = 0; break;
            }
            mapped_to = va + 4096;
        }
        if (!ok) { ret = t->heap_brk; break; }
    }

    t->heap_brk = want;
    ret = t->heap_brk;
}
```

Three semantic decisions worth calling out:

1. **`brk(0)` is the query form.** It's not POSIX but it's Linux's old behavior and keeps the API single-syscall. The user-side allocator uses it for `malloc_brk()` reporting.

2. **The break is byte-granular even though pages are 4 KiB.** A user calling `brk(HEAP_START + 100)` causes the kernel to map one full page (covering [HEAP_START, HEAP_START+4096)) and set `heap_brk = HEAP_START + 100`. The 96 trailing bytes inside the mapped page are harmless — the user-side allocator won't touch addresses beyond `g_brk`. This matches Linux's brk semantics.

3. **No shrink yet.** The page-table direction is missing — `paging.h` has `paging_map_in` but no `paging_unmap_in`, and adding one is a half-session of plumbing that this milestone doesn't need. So `brk(smaller)` decrements `heap_brk` but leaves the pages mapped. They get reclaimed wholesale when `paging_destroy_user_pd` runs at exec / exit.

The page-fill loop zeroes the new pages before mapping them. Without that, a user heap could observe bytes from whichever process previously owned that physical page — the same security bug Linux's slab allocator's POISON catches in debug builds. We're freestanding; we just zero.

## libuser malloc — same allocator, growable foundation

The kernel's `kmalloc.c` is small: 16-byte-header free-list with prev/next threaded in physical address order, first-fit, split when there's leftover, coalesce both neighbors on free. The port to libuser is largely mechanical:

| Kernel kmalloc | libuser malloc | Why |
|---|---|---|
| `static spinlock_t g_lock` | (removed) | User tasks are single-threaded |
| `static struct kmblock *g_head` | (replaced — see below) | Avoid .bss-resolves-at-zero issue |
| `kmalloc_init(start, end)` | implicit; first malloc grows | Heap starts empty, grows on demand |
| Static `[start, end)` range | Dynamic — extends via `sys_brk` | Caller doesn't know how big the heap will get |
| Returns `NULL` on no-fit | Calls `grow_heap` first, then NULL | Same allocator, with auto-growth retry |

**The `g_head` problem.** Session 15 found a sharp edge: `user.ld` discards `.bss`, so a zero-initialized file-scope static gets linked at address 0. The kernel allocator's `static struct kmblock *g_head;` would land in .bss → resolve at NULL → first dereference page-faults.

The fix is structural: there's no `g_head` at all. The list head is implicit — the heap is allocated as a contiguous VA range starting at `HEAP_START_VA`, so the first block, if any, is *always* at exactly `HEAP_START_VA`. The single bit of mutable state we need is `g_brk`, which we initialize to `HEAP_START_VA` (a non-zero literal — guaranteed `.data`):

```c
#define HEAP_START_VA   0x40200000u
static uint32_t g_brk = HEAP_START_VA;        /* explicit init → .data */

static struct mblock *heap_head(void) {
    return (g_brk == HEAP_START_VA) ? NULL                     /* empty */
                                    : (struct mblock *)HEAP_START_VA;
}
```

`g_brk == HEAP_START_VA` is the universal "heap empty" predicate. Once the first `sys_brk` grows the heap, `g_brk` increases and `heap_head()` returns the first block.

Tail-of-list lookup walks from the head — no `g_tail` either:

```c
static struct mblock *heap_tail(void) {
    struct mblock *b = heap_head();
    if (!b) return NULL;
    while (b->next) b = b->next;
    return b;
}
```

For a heap of N blocks this is O(N), fine at our scale. A real allocator would track tail explicitly, but we don't.

**The grow path** is the new-vs-kernel logic:

```c
static int grow_heap(uint32_t at_least) {
    uint32_t need = (at_least + 4095) & ~4095;
    uint32_t want = g_brk + need;
    int got = sys_brk((int)want);
    if (got != (int)want) return -1;          /* OOM or out-of-range */

    uint32_t old = g_brk;
    g_brk = (uint32_t)got;

    /* Lay down a fresh free block on the new pages. */
    struct mblock *nb = (struct mblock *)old;
    nb->size  = (g_brk - old) - M_HDR_SIZE;
    nb->free  = 1;
    nb->magic = M_MAGIC;
    nb->prev  = NULL;
    nb->next  = NULL;

    if (old == HEAP_START_VA) return 0;       /* first growth: nb is head */

    struct mblock *tail = heap_tail();
    tail->next = nb;
    nb->prev   = tail;

    /* Coalesce with old tail if it was free. */
    if (tail->free) {
        tail->size += M_HDR_SIZE + nb->size;
        tail->next  = NULL;
    }
    return 0;
}
```

**malloc** is then almost-identical to the kernel's `kmalloc`, with the no-fit path retrying after a grow:

```c
void *malloc(size_t size) {
    if (size == 0) return NULL;
    size_t want = (size + M_ALIGN - 1) & ~(M_ALIGN - 1);

    for (;;) {
        for (struct mblock *b = heap_head(); b; b = b->next) {
            if (b->magic != M_MAGIC)         return NULL;
            if (!b->free || b->size < want)  continue;

            /* Split if leftover would be a usable block. */
            if (b->size - want >= M_HDR_SIZE + M_MIN_PAYLOAD) {
                struct mblock *n =
                    (struct mblock *)((uintptr_t)b + M_HDR_SIZE + want);
                n->size  = b->size - want - M_HDR_SIZE;
                n->free  = 1;
                n->magic = M_MAGIC;
                n->prev  = b;
                n->next  = b->next;
                if (b->next) b->next->prev = n;
                b->next  = n;
                b->size  = want;
            }
            b->free = 0;
            return (void *)((uintptr_t)b + M_HDR_SIZE);
        }

        if (grow_heap(want + M_HDR_SIZE) != 0) return NULL;
        /* fall through and re-walk the list — the new free block
         * (or the coalesced tail) will satisfy us this iteration. */
    }
}
```

**free** is a verbatim port: mark free, coalesce next, coalesce prev. The kernel's spinlock disappears.

## What the test demonstrates

Reading `[t7]`'s output line by line:

```
initial brk: 0x40200000  (heap empty)
```

`sys_brk(0)` returns `USER_HEAP_START`. No pages mapped yet. The shell hasn't touched its heap.

```
malloc 32/64/128:
  a=0x40200010  b=0x40200040  c=0x40200090
  brk now 0x40201000  (grew by 4096 bytes)
```

The first malloc(32) walks the empty list, finds nothing, calls `grow_heap(32 + 16) = grow_heap(48)`. That rounds up to 4096 and asks for `brk = 0x40201000`. Kernel maps one page; `g_brk` becomes 0x40201000. A fresh free block is laid down at 0x40200000 with header 16 bytes + payload 4080 bytes = 4096 total.

malloc walks the list, finds the new block, splits: 32 bytes payload from offset 16, header for the leftover at offset 16 + 32 = 48. So:
- a's header at 0x40200000, a's payload at 0x40200010
- leftover header at 0x40200030, leftover payload at 0x40200040

malloc(64): leftover block has 4080 - 32 - 16 = 4032 bytes, big enough. Same split:
- b's header at 0x40200030, b's payload at 0x40200040
- leftover header at 0x40200080, leftover payload at 0x40200090

malloc(128): same dance. c's payload at 0x40200090.

```
*a=0xa1a1a1a1  *b=0xb2b2b2b2  *c=0xc3c3c3c3  (read back ok)
```

Confirms the heap pages are real, writable, and persist across yields/syscalls.

```
free(b) + malloc(64) -> b2=0x40200040  reused=yes
```

Free b. No coalescing happens (its neighbors a and the after-c-block are both used or absent). The free list now has b's slot in it. malloc(64) first-fit finds it: same address as b. Reuse confirmed.

```
after free-all: used=0  free=4080  brk=0x40201000
```

free(a), free(b2), free(c) coalesces everything back into a single 4080-byte free block. Total managed = 4096, used = 0, free = 4080 (= 4096 - 16 header). brk unchanged.

```
malloc(8192) -> 0x40200010  brk=0x40204000  (grew 16384 bytes total)
```

8192 bytes doesn't fit in 4080. `grow_heap(8192 + 16)` rounds up to 12288, asks for `brk = 0x40201000 + 12288 = 0x40204000`. Kernel maps 3 more pages. New free block at 0x40201000 with payload 12288 - 16 = 12272. Coalesce with the existing tail (free, 4080 bytes): merged block at 0x40200000 with payload 4080 + 16 + 12272 = 16368. malloc walks, finds 16368 ≥ 8192, splits, returns 0x40200010 (same as the original `a` from earlier — no surprise, it's the same VA).

## Files modified / added

| File | Change |
|---|---|
| `kernel/task.{h,c}` | `heap_brk` field; init at task_create, inherit on fork, reset on exec |
| `kernel/syscall.{h,c}` | `SYS_BRK = 27`; per-page map loop |
| `user/libuser.{h,c}` | `sys_brk` wrapper, `malloc` / `free`, diagnostics; `uint16_t` typedef |
| `user/sh.c` | `[t7]` selftest |

## Design decisions

**Linux-style brk, not sbrk.** sbrk(delta) is conceptually nicer for users (returns the previous break, ignoring how it had to grow) but ABI-uglier on the kernel side (signed delta vs unsigned target). The libuser side could trivially layer sbrk on top of brk — we just don't need it for the demo.

**Heap window 4 MiB max.** With 32 MiB total RAM and several user processes coexisting, a per-process cap is essential. 4 MiB is enough for any of our demos, far above what a real "small Unix" process needs, and leaves room for fork to deep-copy the heap without immediately starving the system.

**Grow chunk = exactly the requested rounded-up amount.** No "grow by 16 pages at a time" heuristic — every grow is the minimum needed. Costs an extra syscall per malloc when the user is allocating in tiny increments, but keeps memory usage predictable. A growth heuristic could go either way; this is the simplest.

**No shrink.** `brk(smaller)` decrements `heap_brk` without unmapping. Pages stay mapped. They're reclaimed at exec (full PD destroy) or exit. The cost: a user task that allocates 4 MiB once and frees it all keeps the 4 MiB resident. For shorter-lived tasks, this is fine; for long-running daemons, it's a memory leak on the order of the high-water mark. Adding `paging_unmap_in` is a half-day; documented as next-session work.

**Allocator state lives in `g_brk` only — no `g_head` or `g_tail`.** Driven entirely by `user.ld`'s `.bss` discard policy: a NULL-initialized pointer would resolve to address 0. Making the head implicit (= `(mblock *)HEAP_START_VA`) avoids needing any pointer to live in `.data`. The cost is one comparison per `heap_head()` call.

**Header is 16 bytes — packed with `uint16_t free` + `uint16_t magic`.** Keeps payloads naturally 16-byte-aligned. SSE-style loads on the heap aren't a concern in our compiler config (`-mno-sse`), but 16-byte alignment is canonical and matches both `kmalloc` and most C-library mallocs.

**Magic value catches double-free.** `0xCAFE` in the header; `free()` checks before doing anything. A double-free silently returns rather than crashing or corrupting the list. Same as the kernel allocator.

**Heap pages are deep-copied on fork.** No special handling needed — `paging_clone_user_pd` from session 14 already deep-copies every present user-PD entry, so the child gets its own physical pages with byte-identical content at the same VAs. After fork both processes can `malloc`/`free` independently. (COW would be the obvious win here; deferred.)

**Exec resets the heap.** `task_exec_inplace` sets `heap_brk = USER_HEAP_START`. The old PD is destroyed, the new one starts fresh. The new ELF's `g_brk` (in libuser, .data) is reloaded with `HEAP_START_VA` — both kernel-side and user-side state agree the heap is empty.

## Pitfalls

1. **The `.bss` issue from session 15 strikes again.** Any file-scope static that needs to be reliably non-NULL on first read MUST have an explicit non-zero initializer to land in `.data`. We work around it by making the allocator stateless except for `g_brk` (initialized to `HEAP_START_VA`) and computing the list head from that.
2. **Page-zero before mapping into user PD.** Without it, the heap exposes whichever process previously held those physical pages. Cheap and load-bearing.
3. **Roll-back on partial map failure leaves pages mapped.** The kernel's grow loop allocates pages one at a time; if pmm_alloc_page returns NULL halfway through, the already-mapped pages stay mapped (no `paging_unmap_in` to roll them back). The user sees `sys_brk` return the unchanged `heap_brk`, so it won't access the partially-mapped range, but the physical pages are leaked until the PD is destroyed. Acceptable failure mode for a demo OS; a real one would unmap.
4. **`brk(want)` rounds the kernel's mapping up to whole pages but `heap_brk` is byte-granular.** If a user calls `brk(HEAP_START + 100)` and then `brk(HEAP_START)`, we don't unmap the page (no shrink) but do leave a "ghost" valid mapping at the byte-after-brk addresses. Reading or writing those bytes won't fault but is undefined behavior — same as Linux. Don't.
5. **The list walk for `heap_tail` is O(N) per growth.** A bigger heap with many small allocations could see grow_heap take a perceptible chunk of time. A `g_tail` pointer would fix it; we don't have one because the .bss issue, see #1. We could put a "tail offset from HEAP_START" `uint32_t` in `.data` next to `g_brk` if it ever mattered.
6. **No locking.** Single-threaded user tasks today. The moment we add user threads, all the obvious races (two threads splitting the same free block, two threads coalescing across each other) become live.
7. **`malloc(0)` returns NULL.** POSIX permits this OR returning a unique-but-non-dereferenceable pointer. We pick NULL.
8. **Block magic catches some corruption but not all.** Wild writes that happen to leave the magic intact aren't caught. Double-frees within the same magic-intact block ARE caught (the `free` flag is checked). A real allocator might add canary bytes between blocks.

## What might come next

`paging_unmap_in` so brk(smaller) actually frees pages. Then real `realloc` (with in-place expansion when the next-block-is-free fast path applies). Then COW heap on fork — most real workloads fork rarely-modified address spaces, and COW would turn a typical fork from 16 KiB of memcpy into one PDE flip. After that, `mmap(MAP_ANONYMOUS)` for allocations bigger than a heap chunk wants to handle, and the rest of the unix-flavored memory management story falls into place.
