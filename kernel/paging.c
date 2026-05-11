#include "paging.h"
#include "pmm.h"
#include "string.h"

/* x86 32-bit, 4 KiB pages, no PSE/PAE. Layout:
 *
 *   linear:   |  31..22 (PD index) | 21..12 (PT index) | 11..0 (offset) |
 *   PDE:      | 31..12 PT phys     | flags                                |
 *   PTE:      | 31..12 page phys   | flags                                |
 */

#define PD_INDEX(a)  (((a) >> 22) & 0x3FF)
#define PT_INDEX(a)  (((a) >> 12) & 0x3FF)
#define PAGE_MASK    (~0xFFFu)

static uint32_t *g_pd;
static int       g_paging_on;

static inline void load_cr3(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

static inline uint32_t read_cr0(void) {
    uint32_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}

static inline void invlpg(uintptr_t a) {
    __asm__ volatile ("invlpg (%0)" :: "r"(a) : "memory");
}

/* Internal map operating on a specific page directory. Works whether
 * paging is on or not — kernel mappings are identity-equivalent and
 * every PD shares them via copied PDEs. The PDE inherits PTE_USER from
 * `flags` since both the PDE and the PTE must allow user access. */
static int do_map_pd(uint32_t *pd, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    uint32_t pd_i = PD_INDEX(virt);
    uint32_t pt_i = PT_INDEX(virt);

    if (!(pd[pd_i] & PTE_PRESENT)) {
        uint32_t *pt = (uint32_t *)pmm_alloc_page();
        if (!pt) return -1;
        memset(pt, 0, PAGE_SIZE);
        pd[pd_i] = ((uint32_t)(uintptr_t)pt & (uint32_t)PAGE_MASK)
                   | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else if (flags & PTE_USER) {
        pd[pd_i] |= PTE_USER;
    }

    uint32_t *pt = (uint32_t *)(uintptr_t)(pd[pd_i] & (uint32_t)PAGE_MASK);
    pt[pt_i] = ((uint32_t)phys & (uint32_t)PAGE_MASK)
               | (flags & 0xFFFu) | PTE_PRESENT;
    return 0;
}

static int do_map(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    return do_map_pd(g_pd, virt, phys, flags);
}

void paging_init(void) {
    g_pd = (uint32_t *)pmm_alloc_page();
    if (!g_pd) return;
    memset(g_pd, 0, PAGE_SIZE);

    /* Identity-map all physical RAM, leaving page 0 unmapped so a null
     * dereference page-faults loudly. Round up to the configured RAM
     * size (m=32 MiB by default) rather than just the PMM-tracked
     * free pages — BIOS-reserved regions like the ACPI tables, MP
     * tables, and EBDA live above the free-pool top but still need
     * to be readable. Cap at 1 GiB. */
    uint32_t total_pages = pmm_total_pages();
    if (total_pages > (0x40000000u >> PMM_PAGE_SHIFT)) {
        total_pages = 0x40000000u >> PMM_PAGE_SHIFT;        /* 1 GiB */
    }
    /* Round up to the next 4 MiB (PD entry size) so we cover any
     * reserved tail-end pages the firmware planted. */
    uintptr_t end = (uintptr_t)total_pages << PMM_PAGE_SHIFT;
    end = (end + 0x3FFFFFu) & ~0x3FFFFFu;

    for (uintptr_t a = PAGE_SIZE; a < end; a += PAGE_SIZE) {
        if (do_map(a, a, PTE_WRITABLE) != 0) return;
    }

    load_cr3((uint32_t)(uintptr_t)g_pd);
    write_cr0(read_cr0() | 0x80000000u);
    g_paging_on = 1;
}

int paging_map(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    int r = do_map(virt, phys, flags);
    if (r == 0 && g_paging_on) invlpg(virt);
    return r;
}

int paging_unmap(uintptr_t virt) {
    uint32_t pd_i = PD_INDEX(virt);
    uint32_t pt_i = PT_INDEX(virt);

    if (!(g_pd[pd_i] & PTE_PRESENT)) return -1;
    uint32_t *pt = (uint32_t *)(uintptr_t)(g_pd[pd_i] & (uint32_t)PAGE_MASK);
    pt[pt_i] = 0;
    if (g_paging_on) invlpg(virt);
    return 0;
}

uintptr_t paging_translate(uintptr_t virt) {
    uint32_t pd_i = PD_INDEX(virt);
    uint32_t pt_i = PT_INDEX(virt);

    if (!(g_pd[pd_i] & PTE_PRESENT)) return 0;
    uint32_t *pt = (uint32_t *)(uintptr_t)(g_pd[pd_i] & (uint32_t)PAGE_MASK);
    if (!(pt[pt_i] & PTE_PRESENT)) return 0;
    return (uintptr_t)((pt[pt_i] & (uint32_t)PAGE_MASK) | (virt & 0xFFF));
}

/* Cross-PD walk — translate `virt` in `pd`'s address space to physical.
 * Used by the ptrace PEEK / POKE path (session 57) to read another
 * task's user memory without doing a CR3 swap. Kernel pages are
 * identity-mapped in low RAM, so dereferencing the PT pointers
 * directly is safe.
 *
 * Returns 0 if the target page isn't mapped, matching paging_translate. */
uint32_t paging_user_va_to_pa(uint32_t *pd, uint32_t virt) {
    if (!pd) return 0;
    uint32_t pd_i = PD_INDEX(virt);
    uint32_t pt_i = PT_INDEX(virt);
    if (!(pd[pd_i] & PTE_PRESENT)) return 0;
    uint32_t *pt = (uint32_t *)(uintptr_t)(pd[pd_i] & (uint32_t)PAGE_MASK);
    if (!(pt[pt_i] & PTE_PRESENT)) return 0;
    return (pt[pt_i] & (uint32_t)PAGE_MASK) | (virt & 0xFFFu);
}

int       paging_is_enabled(void) { return g_paging_on; }
uintptr_t paging_pd_addr(void)    { return (uintptr_t)g_pd; }

/* Create a fresh PD for a user process. Kernel mappings are shared by
 * copying the kernel PDEs (and thus pointing at the same kernel page
 * tables) — any kernel page-table edit is visible everywhere. User
 * pages get their own PDEs added later via paging_map_in. */
uint32_t *paging_create_user_pd(void) {
    uint32_t *pd = (uint32_t *)pmm_alloc_page();
    if (!pd) return NULL;
    memset(pd, 0, PAGE_SIZE);

    /* Kernel identity-mapped region currently fits in PDEs 0..7 (32 MiB). */
    for (int i = 0; i < 8; i++) {
        pd[i] = g_pd[i];
    }
    /* Mirror any "global kernel" PDEs above the user-VA range. The
     * LAPIC at 0xFEE00000 lives in PDE 1019 (= 0xFEE00000 / 4 MiB);
     * mmap regions, IO APIC, and similar high-half MMIO would go
     * here too. Sharing by reference is safe — these are kernel-
     * only mappings and the user PD never modifies them. */
    for (int i = 256; i < 1024; i++) {
        if (g_pd[i] & PTE_PRESENT) {
            pd[i] = g_pd[i];
        }
    }
    return pd;
}

int paging_map_in(uint32_t *pd, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    int r = do_map_pd(pd, virt, phys, flags);
    if (r == 0 && g_paging_on && (uintptr_t)pd == (uintptr_t)g_pd) invlpg(virt);
    return r;
}

uint32_t *paging_clone_user_pd(uint32_t *parent) {
    if (!parent) return NULL;

    uint32_t *child = (uint32_t *)pmm_alloc_page();
    if (!child) return NULL;
    memset(child, 0, PAGE_SIZE);

    /* Mirror kernel PDEs by reference — same page tables, same mappings.
     * Kernel code/data/stacks live here and must stay reachable after the
     * eventual CR3 swap. */
    for (int i = 0; i < 8; i++) {
        child[i] = parent[i];
    }

    for (uint32_t i = 8; i < 1024; i++) {
        if (!(parent[i] & PTE_PRESENT)) continue;

        uint32_t *parent_pt = (uint32_t *)(uintptr_t)(parent[i] & (uint32_t)PAGE_MASK);
        uint32_t *child_pt  = (uint32_t *)pmm_alloc_page();
        if (!child_pt) {
            paging_destroy_user_pd(child);
            return NULL;
        }
        memset(child_pt, 0, PAGE_SIZE);

        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t pte = parent_pt[j];
            if (!(pte & PTE_PRESENT)) continue;

            void *src_page = (void *)(uintptr_t)(pte & (uint32_t)PAGE_MASK);
            void *dst_page = pmm_alloc_page();
            if (!dst_page) {
                /* Free what we already allocated in this PT. The outer
                 * paging_destroy_user_pd will reach this child_pt via
                 * the (already-set) child PDE — install it first. */
                child[i] = ((uint32_t)(uintptr_t)child_pt & (uint32_t)PAGE_MASK)
                           | (parent[i] & 0xFFFu);
                paging_destroy_user_pd(child);
                return NULL;
            }

            /* Both pages are identity-mapped under the kernel master PD,
             * so we can copy via their physical addresses directly. */
            memcpy(dst_page, src_page, PAGE_SIZE);

            child_pt[j] = ((uint32_t)(uintptr_t)dst_page & (uint32_t)PAGE_MASK)
                          | (pte & 0xFFFu);
        }

        /* Install the child PT into the child PDE with the parent's
         * exact PDE flags (PRESENT/USER/WRITABLE/etc.). */
        child[i] = ((uint32_t)(uintptr_t)child_pt & (uint32_t)PAGE_MASK)
                   | (parent[i] & 0xFFFu);
    }

    return child;
}

void paging_destroy_user_pd(uint32_t *pd) {
    if (!pd) return;
    /* Skip PDEs 0..7 — those reference page tables shared with the
     * kernel master PD. ALSO skip any high PDE whose value matches
     * the kernel master PD's PDE — that's a "mirror by reference"
     * we set up for kernel-only mappings (LAPIC, future IO APIC).
     * Their underlying PTs are owned by the kernel and freeing them
     * would corrupt cross-PD shared state. */
    for (uint32_t i = 8; i < 1024; i++) {
        if (!(pd[i] & PTE_PRESENT))    continue;
        if (i >= 256 && pd[i] == g_pd[i]) {
            /* mirrored kernel PDE — skip free */
            pd[i] = 0;
            continue;
        }
        uint32_t *pt = (uint32_t *)(uintptr_t)(pd[i] & (uint32_t)PAGE_MASK);
        for (uint32_t j = 0; j < 1024; j++) {
            if (pt[j] & PTE_PRESENT) {
                pmm_free_page((void *)(uintptr_t)(pt[j] & (uint32_t)PAGE_MASK));
            }
        }
        pmm_free_page(pt);
        pd[i] = 0;
    }
    pmm_free_page(pd);
}

uint32_t paging_pd_used(void) {
    uint32_t n = 0;
    for (int i = 0; i < 1024; i++) if (g_pd[i] & PTE_PRESENT) n++;
    return n;
}
