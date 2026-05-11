#include "pty.h"
#include "task.h"
#include "string.h"
#include "kprintf.h"

struct pty {
    int               in_use;
    volatile int      master_refs;
    volatile int      slave_refs;

    /* Two SPSC rings — master and slave each drain "their" ring and
     * fill the other's. Same volatile + yield pattern as pipe.c. */
    volatile uint32_t m_to_s_head;
    volatile uint32_t m_to_s_tail;
    uint8_t           m_to_s[PTY_BUF_SZ];

    volatile uint32_t s_to_m_head;
    volatile uint32_t s_to_m_tail;
    uint8_t           s_to_m[PTY_BUF_SZ];
};

static struct pty g_ptys[PTY_MAX];

void pty_init(void) {
    for (int i = 0; i < PTY_MAX; i++) g_ptys[i].in_use = 0;
}

int pty_new(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].in_use) {
            memset(&g_ptys[i], 0, sizeof(g_ptys[i]));
            g_ptys[i].in_use      = 1;
            g_ptys[i].master_refs = 1;
            g_ptys[i].slave_refs  = 1;
            return i;
        }
    }
    return -1;
}

static int valid(int idx) {
    return idx >= 0 && idx < PTY_MAX && g_ptys[idx].in_use;
}

void pty_inc_master(int idx) { if (valid(idx)) g_ptys[idx].master_refs++; }
void pty_inc_slave (int idx) { if (valid(idx)) g_ptys[idx].slave_refs++;  }

static void try_free(struct pty *p) {
    if (p->master_refs == 0 && p->slave_refs == 0) p->in_use = 0;
}

void pty_close_master(int idx) {
    if (!valid(idx)) return;
    if (g_ptys[idx].master_refs > 0) g_ptys[idx].master_refs--;
    try_free(&g_ptys[idx]);
}

void pty_close_slave(int idx) {
    if (!valid(idx)) return;
    if (g_ptys[idx].slave_refs > 0) g_ptys[idx].slave_refs--;
    try_free(&g_ptys[idx]);
}

int pty_master_refs(int idx) { return valid(idx) ? g_ptys[idx].master_refs : 0; }
int pty_slave_refs (int idx) { return valid(idx) ? g_ptys[idx].slave_refs  : 0; }

/* ---- public read / write (inlined per direction, mirroring pipe.c) -- */

int pty_master_read(int idx, void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    /* Block until at least one byte is in s_to_m OR the slave side
     * has closed (then ring is drained → 0 = EOF). */
    while (p->s_to_m_head == p->s_to_m_tail && p->slave_refs > 0) {
        task_yield();
    }
    int copied = 0;
    char *out = (char *)buf;
    while (copied < n && p->s_to_m_tail != p->s_to_m_head) {
        out[copied++] = (char)p->s_to_m[p->s_to_m_tail];
        p->s_to_m_tail = (p->s_to_m_tail + 1) % PTY_BUF_SZ;
    }
    return copied;
}

int pty_slave_read(int idx, void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    while (p->m_to_s_head == p->m_to_s_tail && p->master_refs > 0) {
        task_yield();
    }
    int copied = 0;
    char *out = (char *)buf;
    while (copied < n && p->m_to_s_tail != p->m_to_s_head) {
        out[copied++] = (char)p->m_to_s[p->m_to_s_tail];
        p->m_to_s_tail = (p->m_to_s_tail + 1) % PTY_BUF_SZ;
    }
    return copied;
}

int pty_master_write(int idx, const void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    int copied = 0;
    const char *in = (const char *)buf;
    while (copied < n) {
        if (p->slave_refs == 0) return copied > 0 ? copied : -1;
        uint32_t next = (p->m_to_s_head + 1) % PTY_BUF_SZ;
        if (next == p->m_to_s_tail) { task_yield(); continue; }
        p->m_to_s[p->m_to_s_head] = (uint8_t)in[copied++];
        p->m_to_s_head = next;
    }
    return copied;
}

int pty_slave_write(int idx, const void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    int copied = 0;
    const char *in = (const char *)buf;
    while (copied < n) {
        if (p->master_refs == 0) return copied > 0 ? copied : -1;
        uint32_t next = (p->s_to_m_head + 1) % PTY_BUF_SZ;
        if (next == p->s_to_m_tail) { task_yield(); continue; }
        p->s_to_m[p->s_to_m_head] = (uint8_t)in[copied++];
        p->s_to_m_head = next;
    }
    return copied;
}
