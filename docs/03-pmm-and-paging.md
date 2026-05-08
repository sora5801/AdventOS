# Session 3 — Physical Memory Manager + 4 KB paging

**Goal:** Page-granular allocation backed by a real bitmap, then turn on identity-mapped paging.

## Why a bitmap

For ~32 MB of RAM the bitmap is tiny: 8192 pages = 1024 bytes. We could've gone fancier (buddy, slab, zoned) but the workload doesn't demand it. The bitmap is the path of least resistance and the API surface is correct enough that swapping in something more sophisticated later wouldn't ripple.

The bitmap covers the **full 4 GiB 32-bit address space** statically: `g_bitmap[1<<20 / 8] = g_bitmap[131072]`. That's 128 KiB sitting in BSS forever. We waste the storage but never need to deal with "the PMM bitmap itself needs allocation" chicken-and-egg.

## Init flow

```c
void pmm_init(void) {
    // 1. Mark every page in range as USED.
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));

    // 2. Trim total_pages to highest USABLE address.
    uint64_t max_end = 0;
    for each E820 entry where type == USABLE:
        max_end = max(max_end, entry->base + entry->length);
    g_total_pages = max_end / PAGE_SIZE;

    // 3. ONLY NOW initialize used count.
    g_used_pages = g_total_pages;

    // 4. Mark each USABLE entry as free, decrementing used count.
    for each E820 entry where type == USABLE:
        mark_range_free(entry->base, entry->base + entry->length);

    // 5. Re-mark known-used regions (these were just freed in step 4):
    mark_range_used(0, PMM_PAGE_SIZE);                // null catcher
    mark_range_used(0x8000, 0x9000);                  // E820 buffer
    mark_range_used(&kernel_start, &bss_end);         // kernel image
}
```

Steps 4 and 5 overlap intentionally — the kernel image and the E820 buffer both live inside USABLE regions per E820, so the simple "free all usable, then re-mark what we know is reserved" pattern is the simplest correct one.

## The two trim bugs

**First bug.** Initial code counted `max_end` across **all** E820 entries, not just usable. The QEMU dump has a reserved region at 0xFFFC0000 (BIOS flash window). That made `g_total_pages` come out as 1,048,576 (4 GiB / 4 KiB), and `meminfo` reported "total: 4194304 KB" with most of it "used". Looked like 32 MB of RAM had become 4 GB of leaks.

Fix: filter by `type == E820_TYPE_USABLE` when computing `max_end`. The 0xFFFC0000 region is real (it's mapped MMIO for flash) but it's not RAM we should track in the PMM.

**Second bug, surfaced by fixing the first.** After trimming `g_total_pages` from 1,048,576 down to 8160, the displayed values turned negative-looking:

```
Physical Memory Manager:
  total pages : 8160
  used pages  : 1041594            ← way more than total
  free pages  : 4293933862         ← unsigned wraparound
```

`g_used_pages` had been initialized to `PMM_MAX_PAGES` (1 Mi) and decremented once per freed page. After the trim, the bitmap bits above `g_total_pages` were still set (correctly — they don't represent real memory) and were still counted in `g_used_pages` (incorrectly).

Fix: set `g_used_pages = g_total_pages` *after* the trim. The bits beyond `g_total_pages` keep their value 1, but we no longer count them.

```
Physical Memory Manager:
  total pages : 8160  (32640 KB)
  used pages  : 1178  (4712 KB)
  free pages  : 6982  (27928 KB)
```

That checks out: kernel image + heap reservation + page tables + the unreported 0xA0000–0xF0000 video/ROM gap that's not in any USABLE entry, ≈ 1178 pages.

## Allocation

Two flavors:

```c
void *pmm_alloc_page(void);                  // single 4 KiB page
void *pmm_alloc_contiguous(uint32_t n);      // N consecutive
void  pmm_free_page(void *page);
void  pmm_free_contiguous(void *page, uint32_t n);
```

Single-page alloc walks the bitmap a byte at a time and skips full ones (`0xFF`), then bit-scans the partials. Contiguous alloc is naive O(N): scan from page 1, for each candidate start check the next N bits, skip past any obstruction we find. Page 0 stays reserved as the null catcher.

For our heap reservation (4 MiB = 1024 pages), this completes in microseconds at boot. The `kmtest` shell command later in session 6 stresses fragmentation in the *kmalloc* free-list, not the PMM bitmap.

## Paging structure

Standard 32-bit, no PSE, no PAE:

```
linear address:  | 31..22 PD index | 21..12 PT index | 11..0 offset |
PDE/PTE:         | 31..12 phys frame | 11..0 flags |
```

Flags we actually use:

```c
#define PTE_PRESENT     0x001
#define PTE_WRITABLE    0x002
#define PTE_USER        0x004        // matters in session 5
```

The page directory itself is a 4 KiB page allocated by the PMM. Each page table is also 4 KiB. For 32 MB of identity-mapped RAM at 4 KiB granularity we need 8 page tables (8 × 4 MiB = 32 MiB). All allocated lazily.

## The skip-page-0 trick

```c
for (uintptr_t a = PAGE_SIZE; a < end; a += PAGE_SIZE) {
    do_map(a, a, PTE_WRITABLE);
}
```

Loop starts at `PAGE_SIZE`, not 0. Page 0 stays unmapped. Any null pointer dereference now triggers a page fault instead of silently reading garbage at physical 0.

QEMU verification: `pagefault` shell command reads from 0x40000000 (above our 32 MiB cap):

```
[!] CPU EXCEPTION 14: Page fault (err=0x0) at 8:12b24  eflags=0x10216
    fault addr (CR2) = 0x40000000
    cause = page not present, read, supervisor mode
```

The same handler also catches null derefs (`*(int*)0`).

## Page fault handler decode

```c
if (n == 14) {
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    uint32_t e = r->err_code;
    kprintf("    fault addr (CR2) = 0x%08x\n", cr2);
    kprintf("    cause = %s, %s, %s%s%s\n",
            (e & 1)  ? "protection violation" : "page not present",
            (e & 2)  ? "write" : "read",
            (e & 4)  ? "user mode" : "supervisor mode",
            (e & 8)  ? ", reserved-bit set" : "",
            (e & 16) ? ", instruction fetch" : "");
}
```

CR2 holds the linear address that caused the fault. The error code on the stack has the cause bits. Decoding both into english is what makes this useful when something goes wrong in user mode three sessions later.

## kmalloc rewired

`kmalloc_init` now takes its bounds from the PMM:

```c
const uint32_t HEAP_PAGES = 1024;        // 4 MiB
void *heap = pmm_alloc_contiguous(HEAP_PAGES);
kmalloc_init((uintptr_t)heap, (uintptr_t)heap + HEAP_PAGES * PMM_PAGE_SIZE);
```

PMM marks those 1024 pages as used. `paginfo` later shows them in PD entries 0..1 of the master PD. After paging is on, kmalloc allocations come out of identity-mapped pages — no special handling needed.

## Why cap identity map at 1 GiB

```c
uint32_t total_pages = pmm_total_pages();
if (total_pages > (0x40000000u >> PMM_PAGE_SHIFT)) {
    total_pages = 0x40000000u >> PMM_PAGE_SHIFT;       // 1 GiB cap
}
```

If a future build ran on 4 GiB of RAM, we'd burn 4 MiB on page tables (1024 PTs × 4 KiB) just for the identity map. 1 GiB cap is a sanity stop. Anything beyond would have to be mapped on demand.

## Files added

| File | Role |
|---|---|
| `kernel/pmm.{c,h}` | Bitmap allocator, `pmm_alloc_page` / `pmm_alloc_contiguous` / `pmm_free_*` / stats |
| `kernel/paging.{c,h}` | Page directory + lazy PT allocation, `paging_init` / `paging_map` / `paging_translate` |
| `kernel/isr.c` | Page-fault handler extended to decode CR2 + error code |

`kmalloc.c`/`shell.c` updated; `kernel.c` boot sequence adds PMM init then paging init between memmap_init and the rest.

## Design decisions

**Static bitmap covering 4 GiB.** Wastes 128 KB of BSS forever. Dynamic sizing would save it but require a chicken-and-egg solution (where do you allocate the bitmap from before the bitmap exists?). Static is simpler.

**Identity map all of physical RAM.** Means kernel pointers = physical addresses, no copy_from_user yet, no separate kernel virtual address space. Will need to revisit if we ever do >1 GiB of RAM or higher-half kernel.

**Skip page 0.** Costs us 4 KiB of unmappable physical RAM at the bottom but pays back every time a null deref turns into a clean page fault instead of "kernel reads zeros from 0x0 and continues".

**`paging_translate` for debugging.** Walks the PD/PT tree to convert virtual → physical. Used by `paginfo` to print `translate(0xb8000) = 0xb8000` as a smoke test that the identity map is real.

## Deferred

- Per-process page directories (session 5).
- USER bit propagation (session 5).
- `paging_destroy_user_pd` (session 7).
- Higher-half kernel mapping (never).
- Page eviction / swapping (never).

## Pitfalls

1. **`g_used_pages` accounting must be set up after `g_total_pages` is finalized.** Otherwise it'll count bitmap bits past the trimmed total and the math goes negative under `uint32_t`.
2. **Filter E820 by type when finding "top of memory".** Reserved high regions (BIOS flash window, ACPI tables) aren't real RAM and shouldn't inflate the count.
3. **The PMM bitmap itself sits inside the kernel image** (BSS). Step 5 of `pmm_init` (mark `&kernel_start..&bss_end` as used) covers the bitmap automatically.
4. **CR3 holds a physical address, not a virtual one.** With identity mapping the distinction doesn't matter, but it will once we have user PDs (session 5).
