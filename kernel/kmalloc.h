#ifndef ADVENTOS_KMALLOC_H
#define ADVENTOS_KMALLOC_H

#include "../include/types.h"

/*
 * Free-list allocator with neighbor coalescing.
 *
 *   kmalloc_init(start, end)  — give it a contiguous heap region
 *   kmalloc(size)             — first-fit; splits if there's slack
 *   kzalloc(size)             — kmalloc + memset(0)
 *   kfree(p)                  — marks the block free and merges
 *                               with adjacent free neighbors
 *
 * Internals: every block carries a 16-byte header { size, free,
 * magic, prev, next }. `prev`/`next` thread blocks in *physical*
 * order so kfree can coalesce in O(1). Allocation is O(N) because
 * we walk that same list looking for the first free block big
 * enough; for our heap sizes this is fine.
 *
 * Thread safety: every public op takes a global spinlock.
 */

#define KMALLOC_ALIGN 16

void       kmalloc_init(uintptr_t heap_start, uintptr_t heap_end);
void      *kmalloc(size_t size);
void      *kzalloc(size_t size);
void       kfree(void *p);

uintptr_t  kmalloc_heap_start(void);
uintptr_t  kmalloc_heap_end(void);
size_t     kmalloc_used(void);
size_t     kmalloc_free(void);
size_t     kmalloc_total(void);
void       kmalloc_block_counts(uint32_t *used_blocks, uint32_t *free_blocks);
size_t     kmalloc_largest_free(void);

#endif
