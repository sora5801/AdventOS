#ifndef ADVENTOS_PMM_H
#define ADVENTOS_PMM_H

#include "../include/types.h"

#define PMM_PAGE_SIZE  4096
#define PMM_PAGE_SHIFT 12

/*
 * Physical Memory Manager.
 *
 * The PMM owns ALL physical RAM at page granularity (4 KiB). It marks
 * pages as used or free in a bitmap whose static size covers the full
 * 4 GiB 32-bit address space (128 KiB of BSS). Pages above the actual
 * top of RAM remain marked as used so they're never handed out.
 *
 * Initialization order:
 *   1) memmap_init()   — must be done first
 *   2) pmm_init()      — fills the bitmap from E820
 *   3) kmalloc_init()  — pulls a contiguous chunk from PMM
 *   4) paging_init()   — pulls page-table pages from PMM
 *
 * Allocations are returned as physical addresses. While paging is off,
 * physical == virtual. After paging_init runs the kernel runs with an
 * identity map, so the same pointer values are still valid.
 */

void      pmm_init(void);

void     *pmm_alloc_page(void);
void     *pmm_alloc_contiguous(uint32_t n_pages);
void      pmm_free_page(void *page);
void      pmm_free_contiguous(void *page, uint32_t n_pages);

uint32_t  pmm_total_pages(void);
uint32_t  pmm_used_pages(void);
uint32_t  pmm_free_pages(void);

#endif
