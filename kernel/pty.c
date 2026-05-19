#include "pty.h"
#include "task.h"
#include "string.h"
#include "kprintf.h"
#include "signal.h"

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

    /* Session 56: line discipline. fg_pgrp tracks which process group
     * receives signals (Ctrl-C etc.); mode bits decide whether to
     * intercept. Set by tcsetpgrp(slave_fd, ...) and tty_set_mode. */
    volatile int      fg_pgrp;
    volatile int      mode;
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
            /* Default mode: ISIG on (Ctrl-C generates signal),
             * matching how a real Unix opens a new pty. */
            g_ptys[i].mode        = PTY_MODE_ISIG;
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
    struct pty *p = &g_ptys[idx];
    if (p->master_refs > 0) p->master_refs--;
    /* Session 158 — POSIX-ish carrier loss: the last master close
     * delivers SIGHUP to whichever process group has the slave as
     * its controlling terminal.  Mirrors what Linux does when an
     * xterm closes: the inner shell receives SIGHUP and the default
     * action terminates it, so the slave isn't left spinning on
     * read-returns-0.  fg_pgrp is set by tcsetpgrp(slave_fd, ...) —
     * the inner sh.elf calls it at startup, so by the time wmterm
     * closes, fg_pgrp is the sh's own pgid (or a foreground command
     * that sh ran with tcsetpgrp). */
    if (p->master_refs == 0 && p->fg_pgrp > 0) {
        signal_send_pgrp((uint32_t)p->fg_pgrp, SIGHUP);
    }
    try_free(p);
}

void pty_close_slave(int idx) {
    if (!valid(idx)) return;
    if (g_ptys[idx].slave_refs > 0) g_ptys[idx].slave_refs--;
    try_free(&g_ptys[idx]);
}

int pty_master_refs(int idx) { return valid(idx) ? g_ptys[idx].master_refs : 0; }
int pty_slave_refs (int idx) { return valid(idx) ? g_ptys[idx].slave_refs  : 0; }

/* ---- public read / write (inlined per direction, mirroring pipe.c) -- */

/* "Is there a deliverable signal pending right now?" — used by the
 * blocking reads below to behave like an EINTR-style interruptible
 * syscall. Without this, a TX helper task blocked in master_read can't
 * be killed (e.g. for an SSH rekey: parent needs to tear down the
 * stale TX before re-deriving keys, otherwise the TX's COW copy of
 * the connection state keeps it encrypting with the old key after the
 * peer has switched). Returns -1 to the caller, which sshd treats as
 * "shutdown the helper". */
static int signal_interrupted(void) {
    struct task *t = task_current();
    if (!t) return 0;
    return (t->sig_pending & ~t->sig_mask) != 0;
}

/* Session 157 — non-blocking peek helpers.  Used by sys_read on
 * an FD_PTY_M / FD_PTY_S that has FD_FL_NONBLOCK set so the WM
 * client (wmterm) doesn't block its main loop on a quiet shell. */
int pty_master_read_avail(int idx) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    return (p->s_to_m_head != p->s_to_m_tail) ? 1 : 0;
}

int pty_slave_read_avail(int idx) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    return (p->m_to_s_head != p->m_to_s_tail) ? 1 : 0;
}

int pty_master_read(int idx, void *buf, int n) {
    if (!valid(idx)) return -1;
    struct pty *p = &g_ptys[idx];
    /* Block until at least one byte is in s_to_m OR the slave side
     * has closed (then ring is drained → 0 = EOF) OR a signal is
     * pending (interruptible syscall semantics). */
    while (p->s_to_m_head == p->s_to_m_tail && p->slave_refs > 0) {
        if (signal_interrupted()) return -1;
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
        if (signal_interrupted()) return -1;
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
        uint8_t b = (uint8_t)in[copied];

        /* Line discipline: when ISIG is enabled (default), three
         * control characters become signals to the foreground pgrp
         * instead of being passed through to the slave. The byte is
         * "consumed" from the input — counted toward the return as
         * if written, but not actually pushed to the ring. */
        if (p->mode & PTY_MODE_ISIG) {
            int sig = 0;
            if      (b == 0x03) sig = SIGINT;    /* Ctrl-C */
            else if (b == 0x1C) sig = SIGQUIT;   /* Ctrl-\ */
            else if (b == 0x1A) sig = SIGTSTP;   /* Ctrl-Z */
            if (sig && p->fg_pgrp > 0) {
                signal_send_pgrp((uint32_t)p->fg_pgrp, sig);
                copied++;
                continue;
            }
        }

        uint32_t next = (p->m_to_s_head + 1) % PTY_BUF_SZ;
        if (next == p->m_to_s_tail) { task_yield(); continue; }
        p->m_to_s[p->m_to_s_head] = b;
        p->m_to_s_head = next;
        copied++;
    }
    return copied;
}

void pty_set_fg_pgrp(int idx, int pgid) {
    if (!valid(idx)) return;
    g_ptys[idx].fg_pgrp = pgid;
}

int pty_get_fg_pgrp(int idx) {
    return valid(idx) ? g_ptys[idx].fg_pgrp : 0;
}

void pty_set_mode(int idx, int mode) {
    if (!valid(idx)) return;
    g_ptys[idx].mode = mode;
}

int pty_get_mode(int idx) {
    return valid(idx) ? g_ptys[idx].mode : 0;
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
