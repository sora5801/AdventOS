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
     * dereference page-faults loudly. Cap at 1 GiB to bound how many
     * page tables we burn on huge-RAM systems. */
    uint32_t total_pages = pmm_total_pages();
    if (total_pages > (0x40000000u >> PMM_PAGE_SHIFT)) {
        total_pages = 0x40000000u >> PMM_PAGE_SHIFT;        /* 1 GiB */
    }
    uintptr_t end = (uintptr_t)total_pages << PMM_PAGE_SHIFT;

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
    return pd;
}

int paging_map_in(uint32_t *pd, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    int r = do_map_pd(pd, virt, phys, flags);
    if (r == 0 && g_paging_on && (uintptr_t)pd == (uintptr_t)g_pd) invlpg(virt);
    return r;
}

void paging_destroy_user_pd(uint32_t *pd) {
    if (!pd) return;
    /* Skip PDEs 0..7 — those reference page tables that are shared
     * with the kernel master PD. Free everything from PDE 8 upward. */
    for (uint32_t i = 8; i < 1024; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
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
