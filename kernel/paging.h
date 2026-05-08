#ifndef ADVENTOS_PAGING_H
#define ADVENTOS_PAGING_H

#include "../include/types.h"

#define PAGE_SIZE       4096

#define PTE_PRESENT     0x001u
#define PTE_WRITABLE    0x002u
#define PTE_USER        0x004u
#define PTE_PWT         0x008u
#define PTE_PCD         0x010u
#define PTE_ACCESSED    0x020u
#define PTE_DIRTY       0x040u
#define PTE_PSE         0x080u
#define PTE_GLOBAL      0x100u

/* Build a page directory and identity-map all of physical RAM at 4 KiB
 * granularity, then enable CR0.PG. Page 0 is intentionally left unmapped
 * to catch null-pointer dereferences. */
void      paging_init(void);

/* Returns 0 on success, -1 on out-of-memory (no PT could be allocated). */
int       paging_map(uintptr_t virt, uintptr_t phys, uint32_t flags);
int       paging_unmap(uintptr_t virt);

/* Translate a virtual address to physical, or 0 if unmapped. */
uintptr_t paging_translate(uintptr_t virt);

int       paging_is_enabled(void);
uintptr_t paging_pd_addr(void);
uint32_t  paging_pd_used(void);

/* Per-PD operations for building user address spaces. */
uint32_t *paging_create_user_pd(void);
int       paging_map_in(uint32_t *pd, uintptr_t virt, uintptr_t phys, uint32_t flags);

/* Tear down a user PD: walks PDEs 8..1023 (kernel uses 0..7 — those are
 * shared, never freed here), frees every user page mapped under each
 * present PT, frees the PT pages, then frees the PD page itself. */
void      paging_destroy_user_pd(uint32_t *pd);

#endif
