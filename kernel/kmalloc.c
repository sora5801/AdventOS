#include "kmalloc.h"
#include "spinlock.h"
#include "string.h"

/*
 * Block header. Sits immediately before the payload of every block,
 * free or used. Total: 16 bytes (matches KMALLOC_ALIGN).
 *
 *   size   — payload bytes (not including this header)
 *   free   — 1 if free
 *   magic  — sanity tag for double-free / corruption detection
 *   prev   — previous block in physical address order
 *   next   — next block in physical address order
 */
struct kmblock {
    uint32_t        size;
    uint16_t        free;
    uint16_t        magic;
    struct kmblock *prev;
    struct kmblock *next;
};

#define KM_MAGIC        0xCAFEu
#define KM_HDR_SIZE     ((size_t)sizeof(struct kmblock))
#define KM_MIN_PAYLOAD  16u   /* don't split if remainder would be smaller */

static spinlock_t      g_lock = SPINLOCK_INIT;
static struct kmblock *g_head;
static uintptr_t       g_heap_start;
static uintptr_t       g_heap_end;

static inline size_t round_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static struct kmblock *block_from_payload(void *p) {
    return (struct kmblock *)((uintptr_t)p - KM_HDR_SIZE);
}

static void *payload_of(struct kmblock *b) {
    return (void *)((uintptr_t)b + KM_HDR_SIZE);
}

void kmalloc_init(uintptr_t start, uintptr_t end) {
    uintptr_t s = (start + KMALLOC_ALIGN - 1) & ~(uintptr_t)(KMALLOC_ALIGN - 1);
    uintptr_t e = end & ~(uintptr_t)(KMALLOC_ALIGN - 1);
    if (e < s + KM_HDR_SIZE + KM_MIN_PAYLOAD) {
        g_head = NULL;
        g_heap_start = s;
        g_heap_end   = s;
        return;
    }

    g_head = (struct kmblock *)s;
    g_head->size  = (uint32_t)(e - s - KM_HDR_SIZE);
    g_head->free  = 1;
    g_head->magic = KM_MAGIC;
    g_head->prev  = NULL;
    g_head->next  = NULL;

    g_heap_start  = s;
    g_heap_end    = e;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size_t want = round_up(size, KMALLOC_ALIGN);

    spin_lock(&g_lock);

    for (struct kmblock *b = g_head; b != NULL; b = b->next) {
        if (b->magic != KM_MAGIC) {
            /* Heap corrupted — bail rather than crash unpredictably. */
            spin_unlock(&g_lock);
            return NULL;
        }
        if (!b->free || b->size < want) continue;

        size_t leftover = b->size - want;
        if (leftover >= KM_HDR_SIZE + KM_MIN_PAYLOAD) {
            /* Split: carve off the front, leave a smaller free block after. */
            struct kmblock *n =
                (struct kmblock *)((uintptr_t)b + KM_HDR_SIZE + want);
            n->size  = (uint32_t)(leftover - KM_HDR_SIZE);
            n->free  = 1;
            n->magic = KM_MAGIC;
            n->prev  = b;
            n->next  = b->next;
            if (b->next) b->next->prev = n;
            b->next  = n;
            b->size  = (uint32_t)want;
        }
        b->free = 0;

        spin_unlock(&g_lock);
        return payload_of(b);
    }

    spin_unlock(&g_lock);
    return NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;

    spin_lock(&g_lock);

    struct kmblock *b = block_from_payload(p);
    if (b->magic != KM_MAGIC || b->free) {
        /* Double-free or corruption — be quiet, do nothing. */
        spin_unlock(&g_lock);
        return;
    }
    b->free = 1;

    /* Coalesce with the next block if it's free. */
    if (b->next && b->next->free) {
        struct kmblock *n = b->next;
        b->size += KM_HDR_SIZE + n->size;
        b->next  = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }

    /* Coalesce with the previous block if it's free. */
    if (b->prev && b->prev->free) {
        struct kmblock *pp = b->prev;
        pp->size += KM_HDR_SIZE + b->size;
        pp->next  = b->next;
        if (b->next) b->next->prev = pp;
        b->magic = 0;
    }

    spin_unlock(&g_lock);
}

uintptr_t kmalloc_heap_start(void) { return g_heap_start; }
uintptr_t kmalloc_heap_end(void)   { return g_heap_end;   }

size_t kmalloc_total(void) {
    return (size_t)(g_heap_end - g_heap_start);
}

size_t kmalloc_used(void) {
    size_t total = 0;
    spin_lock(&g_lock);
    for (struct kmblock *b = g_head; b; b = b->next) {
        if (!b->free) total += b->size + KM_HDR_SIZE;
    }
    spin_unlock(&g_lock);
    return total;
}

size_t kmalloc_free(void) {
    size_t total = 0;
    spin_lock(&g_lock);
    for (struct kmblock *b = g_head; b; b = b->next) {
        if (b->free) total += b->size;
    }
    spin_unlock(&g_lock);
    return total;
}

size_t kmalloc_largest_free(void) {
    size_t best = 0;
    spin_lock(&g_lock);
    for (struct kmblock *b = g_head; b; b = b->next) {
        if (b->free && b->size > best) best = b->size;
    }
    spin_unlock(&g_lock);
    return best;
}

void kmalloc_block_counts(uint32_t *used_blocks, uint32_t *free_blocks) {
    uint32_t u = 0, f = 0;
    spin_lock(&g_lock);
    for (struct kmblock *b = g_head; b; b = b->next) {
        if (b->free) f++; else u++;
    }
    spin_unlock(&g_lock);
    if (used_blocks) *used_blocks = u;
    if (free_blocks) *free_blocks = f;
}
