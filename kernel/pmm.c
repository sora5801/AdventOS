#include "pmm.h"
#include "memmap.h"
#include "spinlock.h"
#include "string.h"

/* Cover the entire 32-bit address space at 4 KiB granularity. */
#define PMM_MAX_PAGES    (1u << 20)             /* 1 Mi pages = 4 GiB   */
#define PMM_BITMAP_BYTES (PMM_MAX_PAGES / 8)    /* 128 KiB              */

static uint8_t   g_bitmap[PMM_BITMAP_BYTES];
static uint32_t  g_total_pages;
static uint32_t  g_used_pages;
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

extern uint8_t kernel_start;   /* from linker_kernel.ld */
extern uint8_t bss_end;

static inline int  bit_get  (uint32_t p) { return (g_bitmap[p >> 3] >> (p & 7)) & 1; }
static inline void bit_set  (uint32_t p) { g_bitmap[p >> 3] |=  (uint8_t)(1u << (p & 7)); }
static inline void bit_clear(uint32_t p) { g_bitmap[p >> 3] &= (uint8_t)~(1u << (p & 7)); }

static void mark_range_used(uintptr_t start, uintptr_t end) {
    uint32_t p_start = (uint32_t)(start >> PMM_PAGE_SHIFT);
    uint32_t p_end   = (uint32_t)((end + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT);
    if (p_end > g_total_pages) p_end = g_total_pages;
    for (uint32_t p = p_start; p < p_end; p++) {
        if (!bit_get(p)) { bit_set(p); g_used_pages++; }
    }
}

/* Round inward — never mark partial pages free. */
static void mark_range_free(uintptr_t start, uintptr_t end) {
    uint32_t p_start = (uint32_t)((start + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT);
    uint32_t p_end   = (uint32_t)(end >> PMM_PAGE_SHIFT);
    if (p_end > g_total_pages) p_end = g_total_pages;
    for (uint32_t p = p_start; p < p_end; p++) {
        if (bit_get(p)) { bit_clear(p); g_used_pages--; }
    }
}

void pmm_init(void) {
    /* Bitmap starts all-ones; we'll free in-range pages below. */
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));

    /* Find the highest address that's actually USABLE per E820, and trim
     * total_pages to that. Reserved high-memory MMIO regions (e.g. the
     * BIOS flash window at 0xFFFC0000) shouldn't inflate "total". */
    uint64_t max_end = 0;
    uint32_t n = memmap_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_TYPE_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > max_end) max_end = end;
    }
    if (max_end > 0x100000000ULL) max_end = 0x100000000ULL;
    uint32_t actual = (uint32_t)((max_end + PMM_PAGE_SIZE - 1) >> PMM_PAGE_SHIFT);
    if (actual > PMM_MAX_PAGES) actual = PMM_MAX_PAGES;
    g_total_pages = actual;

    /* Now that total_pages is trimmed, "used" must reflect pages in
     * range only — they all start as used. */
    g_used_pages = g_total_pages;

    /* Free all USABLE regions. */
    for (uint32_t i = 0; i < n; i++) {
        const struct e820_entry *e = memmap_entry(i);
        if (e->type != E820_TYPE_USABLE) continue;
        uint64_t s = e->base;
        uint64_t f = e->base + e->length;
        if (s >= 0x100000000ULL) continue;
        if (f >  0x100000000ULL) f = 0x100000000ULL;
        mark_range_free((uintptr_t)s, (uintptr_t)f);
    }

    /* Now lock down regions we know we're using. */
    /*  - page 0:           catch null pointer dereferences after paging  */
    /*  - 0x7C00-0x7E00:    boot sector image (still useful as scratch?)  */
    /*  - 0x8000-0x9000:    E820 buffer (memmap.c reads it forever)       */
    /*  - kernel image:     .text + .rdata + .data + .bss (incl. bitmap)  */
    mark_range_used(0, PMM_PAGE_SIZE);
    mark_range_used(0x8000, 0x9000);
    mark_range_used((uintptr_t)&kernel_start, (uintptr_t)&bss_end);
}

void *pmm_alloc_page(void) {
    spin_lock(&g_pmm_lock);
    uint32_t total_bytes = (g_total_pages + 7) >> 3;
    for (uint32_t byte = 0; byte < total_bytes; byte++) {
        if (g_bitmap[byte] == 0xFF) continue;
        for (uint32_t bit = 0; bit < 8; bit++) {
            uint32_t page = byte * 8 + bit;
            if (page == 0)             continue;     /* null catcher */
            if (page >= g_total_pages) { spin_unlock(&g_pmm_lock); return NULL; }
            if (!(g_bitmap[byte] & (1u << bit))) {
                g_bitmap[byte] |= (uint8_t)(1u << bit);
                g_used_pages++;
                spin_unlock(&g_pmm_lock);
                return (void *)(uintptr_t)(page << PMM_PAGE_SHIFT);
            }
        }
    }
    spin_unlock(&g_pmm_lock);
    return NULL;
}

void *pmm_alloc_contiguous(uint32_t n) {
    if (n == 0) return NULL;
    if (n == 1) return pmm_alloc_page();

    spin_lock(&g_pmm_lock);
    uint32_t start = 1;
    while (start + n <= g_total_pages) {
        uint32_t k = 0;
        while (k < n && !bit_get(start + k)) k++;
        if (k == n) {
            for (uint32_t i = 0; i < n; i++) bit_set(start + i);
            g_used_pages += n;
            spin_unlock(&g_pmm_lock);
            return (void *)(uintptr_t)(start << PMM_PAGE_SHIFT);
        }
        start += k + 1;
    }
    spin_unlock(&g_pmm_lock);
    return NULL;
}

void pmm_free_page(void *page) {
    uintptr_t addr = (uintptr_t)page;
    if (addr & (PMM_PAGE_SIZE - 1)) return;
    uint32_t p = (uint32_t)(addr >> PMM_PAGE_SHIFT);
    spin_lock(&g_pmm_lock);
    if (p < g_total_pages && bit_get(p)) {
        bit_clear(p);
        g_used_pages--;
    }
    spin_unlock(&g_pmm_lock);
}

void pmm_free_contiguous(void *page, uint32_t n) {
    uintptr_t addr = (uintptr_t)page;
    if (addr & (PMM_PAGE_SIZE - 1)) return;
    uint32_t start = (uint32_t)(addr >> PMM_PAGE_SHIFT);
    spin_lock(&g_pmm_lock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = start + i;
        if (p >= g_total_pages) break;
        if (bit_get(p)) { bit_clear(p); g_used_pages--; }
    }
    spin_unlock(&g_pmm_lock);
}

uint32_t pmm_total_pages(void) { return g_total_pages; }
uint32_t pmm_used_pages (void) { return g_used_pages;  }
uint32_t pmm_free_pages (void) { return g_total_pages - g_used_pages; }
