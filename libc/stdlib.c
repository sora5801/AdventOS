/*
 * AdventOS libc — stdlib.h implementation.
 *
 * malloc/free is ported verbatim from the legacy libuser heap
 * (free-list, first-fit, split-on-alloc, neighbor-coalesce).
 * The interesting wrinkle: this code is shared (one physical
 * copy of .text) but the heap state must be PER-PROCESS.
 *
 * In real glibc that's solved by .data being COW-mapped per
 * process. We use a simpler trick: g_brk lives at LIBC_HEAP_VAR_VA,
 * a fixed VA inside libc's .data segment. Because each process
 * gets its own physical copy of libc.bin (the kernel allocates
 * fresh pages and memcpy's the image into them at process load
 * time), the .data is already per-process. No COW needed.
 *
 * The kicker: g_brk needs to start as HEAP_START_VA before any
 * malloc runs. We initialize it explicitly (not relying on .bss
 * zero-init) AND verify the magic bytes look right.
 */
#include "libc.h"

#define HEAP_START_VA   0x40200000u
#define M_ALIGN         16
#define M_HDR_SIZE      sizeof(struct mblock)
#define M_MIN_PAYLOAD   16u
#define M_MAGIC         0xCAFEu

struct mblock {
    uint32_t        size;
    uint16_t        free;
    uint16_t        magic;
    struct mblock  *prev;
    struct mblock  *next;
};

/* Inline syscall: SYS_BRK. We can't reuse libuser's wrapper because
 * that lives in the user program — libc has to be self-contained for
 * its own syscall needs. This is the only syscall malloc needs. */
static int sys_brk_internal(int new_brk) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(LIBC_SYS_BRK), "b"(new_brk)
                      : "memory");
    return ret;
}

/* Per-process heap break. Lives in libc's .data so it gets a
 * private physical page per process at libc-load time. */
static uint32_t g_brk = HEAP_START_VA;

static struct mblock *heap_head(void) {
    return (g_brk == HEAP_START_VA) ? NULL : (struct mblock *)HEAP_START_VA;
}

static struct mblock *heap_tail(void) {
    struct mblock *b = heap_head();
    if (!b) return NULL;
    while (b->next) b = b->next;
    return b;
}

static int grow_heap(uint32_t at_least) {
    uint32_t need = (at_least + 4095u) & ~4095u;
    uint32_t want = g_brk + need;
    int got = sys_brk_internal((int)want);
    if (got != (int)want) return -1;

    uint32_t old = g_brk;
    g_brk = (uint32_t)got;

    struct mblock *nb = (struct mblock *)(uintptr_t)old;
    nb->size  = (uint32_t)(g_brk - old - M_HDR_SIZE);
    nb->free  = 1;
    nb->magic = M_MAGIC;
    nb->prev  = NULL;
    nb->next  = NULL;

    if (old == HEAP_START_VA) return 0;

    struct mblock *tail = heap_tail();
    if (!tail) return -1;
    tail->next = nb;
    nb->prev   = tail;

    if (tail->free) {
        tail->size += M_HDR_SIZE + nb->size;
        tail->next  = NULL;
    }
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size_t want = (size + M_ALIGN - 1) & ~(size_t)(M_ALIGN - 1);

    for (;;) {
        for (struct mblock *b = heap_head(); b; b = b->next) {
            if (b->magic != M_MAGIC) return NULL;
            if (!b->free || b->size < want) continue;

            size_t leftover = b->size - want;
            if (leftover >= M_HDR_SIZE + M_MIN_PAYLOAD) {
                struct mblock *n =
                    (struct mblock *)((uintptr_t)b + M_HDR_SIZE + want);
                n->size  = (uint32_t)(leftover - M_HDR_SIZE);
                n->free  = 1;
                n->magic = M_MAGIC;
                n->prev  = b;
                n->next  = b->next;
                if (b->next) b->next->prev = n;
                b->next  = n;
                b->size  = (uint32_t)want;
            }
            b->free = 0;
            return (void *)((uintptr_t)b + M_HDR_SIZE);
        }
        if (grow_heap((uint32_t)(want + M_HDR_SIZE)) != 0) return NULL;
    }
}

void free(void *p) {
    if (!p) return;
    struct mblock *b = (struct mblock *)((uintptr_t)p - M_HDR_SIZE);
    if (b->magic != M_MAGIC || b->free) return;
    b->free = 1;

    if (b->next && b->next->free) {
        struct mblock *n = b->next;
        b->size += M_HDR_SIZE + n->size;
        b->next  = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->free) {
        struct mblock *pp = b->prev;
        pp->size += M_HDR_SIZE + b->size;
        pp->next  = b->next;
        if (b->next) b->next->prev = pp;
        b->magic = 0;
    }
}

void *calloc(size_t nm, size_t sz) {
    size_t total = nm * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    struct mblock *b = (struct mblock *)((uintptr_t)p - M_HDR_SIZE);
    if (b->magic != M_MAGIC) return NULL;
    if (b->size >= n) return p;            /* shrinking — keep block */
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

uint32_t malloc_brk_(void)        { return g_brk; }
uint32_t malloc_total_(void)      { return g_brk - HEAP_START_VA; }
uint32_t malloc_used_(void) {
    uint32_t total = 0;
    for (struct mblock *b = heap_head(); b; b = b->next) {
        if (!b->free) total += b->size + M_HDR_SIZE;
    }
    return total;
}
uint32_t malloc_free_bytes_(void) {
    uint32_t total = 0;
    for (struct mblock *b = heap_head(); b; b = b->next) {
        if (b->free) total += b->size;
    }
    return total;
}

/* atoi: skip whitespace, optional +/-, then decimal digits. No
 * overflow detection — callers handle small ints in this kernel. */
int atoi(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if      (*s == '-') { sign = -1; s++; }
    else if (*s == '+') {             s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

long atol(const char *s) { return (long)atoi(s); }

long strtol(const char *s, char **end, int base) {
    long v = 0; int sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if      (*s == '-') { sign = -1; s++; }
    else if (*s == '+') {             s++; }
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else { base = 10; }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return v * sign;
}

int abs(int x) { return x < 0 ? -x : x; }
