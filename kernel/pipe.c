#include "pipe.h"
#include "task.h"
#include "string.h"
#include "kprintf.h"

struct pipe {
    int               in_use;
    volatile int      read_refs;
    volatile int      write_refs;

    /* SPSC ring. The producer-side is whoever has FD_PIPE_W; consumer
     * is FD_PIPE_R. Both indices are 32-bit aligned, so loads/stores
     * are atomic on x86 and we don't need a lock. The volatile is
     * load-bearing — the read/write loops yield in a busy-poll until
     * the other side advances head/tail, and the compiler must not
     * hoist the head/tail check out of the loop. */
    volatile uint32_t head;
    volatile uint32_t tail;
    uint8_t           buf[PIPE_BUF_SZ];
};

static struct pipe g_pipes[PIPE_MAX];

void pipe_init(void) {
    for (int i = 0; i < PIPE_MAX; i++) g_pipes[i].in_use = 0;
}

int pipe_new(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!g_pipes[i].in_use) {
            memset(&g_pipes[i], 0, sizeof(g_pipes[i]));
            g_pipes[i].in_use     = 1;
            g_pipes[i].read_refs  = 1;
            g_pipes[i].write_refs = 1;
            return i;
        }
    }
    return -1;
}

static int valid(int idx) {
    return idx >= 0 && idx < PIPE_MAX && g_pipes[idx].in_use;
}

void pipe_inc_read (int idx) { if (valid(idx)) g_pipes[idx].read_refs++;  }
void pipe_inc_write(int idx) { if (valid(idx)) g_pipes[idx].write_refs++; }

static void try_free(struct pipe *p) {
    if (p->read_refs == 0 && p->write_refs == 0) p->in_use = 0;
}

void pipe_close_read(int idx) {
    if (!valid(idx)) return;
    if (g_pipes[idx].read_refs > 0) g_pipes[idx].read_refs--;
    try_free(&g_pipes[idx]);
}

void pipe_close_write(int idx) {
    if (!valid(idx)) return;
    if (g_pipes[idx].write_refs > 0) g_pipes[idx].write_refs--;
    try_free(&g_pipes[idx]);
}

int pipe_read_refs (int idx) { return valid(idx) ? g_pipes[idx].read_refs  : 0; }
int pipe_write_refs(int idx) { return valid(idx) ? g_pipes[idx].write_refs : 0; }

/* Session 74 — fast peek used by the FD_FL_NONBLOCK path of SYS_READ.
 * Mirrors pipe_read's first-line gate exactly: data or EOF -> ok to
 * call pipe_read; empty + writer open -> tell caller to skip. */
int pipe_read_avail(int idx) {
    if (!valid(idx)) return -1;
    struct pipe *p = &g_pipes[idx];
    if (p->head != p->tail) return 1;       /* bytes queued */
    if (p->write_refs == 0)  return 1;      /* drained EOF */
    return 0;                               /* would block */
}

int pipe_read(int idx, void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pipe *p = &g_pipes[idx];

    /* Block until at least one byte is available, OR the writer
     * closed AND the ring is drained (true EOF). */
    while (p->head == p->tail && p->write_refs > 0) {
        task_yield();
    }

    int copied = 0;
    char *out = (char *)buf;
    while (copied < n && p->tail != p->head) {
        out[copied++] = (char)p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SZ;
    }
    return copied;     /* 0 -> EOF (writer closed and drained) */
}

int pipe_write(int idx, const void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pipe *p = &g_pipes[idx];

    int copied = 0;
    const char *in = (const char *)buf;
    while (copied < n) {
        /* If reader gave up, stop pretending we wrote anything we didn't. */
        if (p->read_refs == 0) {
            return copied > 0 ? copied : -1;
        }

        uint32_t next = (p->head + 1) % PIPE_BUF_SZ;
        if (next == p->tail) {
            /* Ring full; let the reader run. */
            task_yield();
            continue;
        }
        p->buf[p->head] = (uint8_t)in[copied++];
        p->head = next;
    }
    return copied;
}
