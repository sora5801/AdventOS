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

/* Same translation but against an arbitrary user PD instead of the
 * currently-loaded CR3. Used by the ptrace PEEK/POKE path (session 57)
 * to read/write a tracee's user memory while running on the tracer's
 * CR3. Returns 0 if unmapped. */
uint32_t  paging_user_va_to_pa(uint32_t *pd, uint32_t virt);

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

/* Deep-copy `parent` into a fresh user PD. Kernel PDEs (0..7) are shared
 * by reference (same page-table pointers as paging_create_user_pd would
 * install). Each user PDE is rebuilt: a new PT is allocated, then for
 * every present PTE in the parent's PT a new physical page is allocated
 * and the parent page's contents are memcpy'd into it.
 *
 * Returns the child PD's virtual address (== physical, identity-mapped),
 * or NULL on out-of-memory. On failure all partial allocations are freed.
 *
 * Session 75: if `data_pages_out` is non-NULL, the function fills it with
 * the count of user DATA pages allocated (excluding the child PD itself
 * and PT pages). The fork path uses this to set the child's cur_rss_pages
 * to a number that reflects what was actually allocated, rather than
 * blindly copying the parent's counter and assuming they match. */
uint32_t *paging_clone_user_pd(uint32_t *parent, uint32_t *data_pages_out);

#endif
