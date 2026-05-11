#ifndef ADVENTOS_PTY_H
#define ADVENTOS_PTY_H

#include "../include/types.h"

/*
 * Pseudo-terminal pairs (session 52).
 *
 * A pty is a bidirectional byte pipe with a "master" and a "slave"
 * end. Reads on one side return bytes written on the other:
 *
 *   master --write-> [m_to_s ring] --read-> slave   (shell's stdin)
 *   master <-read--- [s_to_m ring] <-write- slave   (shell's stdout)
 *
 * Use case: sshd allocates a pty, forks a shell with the slave end as
 * stdin/stdout/stderr, then shuttles bytes between its SSH channel
 * and the master end. The shell sees a TTY-shaped fd; sshd doesn't
 * need to know anything about line discipline.
 *
 * Refcount model mirrors pipes: each end has its own count so dup2
 * / fork can hand out additional fds and close() correctly tears
 * them down. When either side's count drops to zero, the OTHER
 * side's read returns 0 (EOF) and its write returns -1.
 *
 * No line discipline at this layer — bytes pass through verbatim.
 * The slave-side process supplies whatever cooked/raw mode logic
 * it wants in userspace (sh.elf's read_line_interactive does it).
 *
 * Concurrency: two-direction shuttle is what unlocks the persistent
 * shell — the parent process can fork two helpers, one for each
 * direction, and they never need to share state because each only
 * reads from one end and writes to the other.
 */

#define PTY_MAX     4
#define PTY_BUF_SZ  4096

void pty_init(void);

/* Allocate a fresh pty with master_refs=1, slave_refs=1. Returns the
 * pty idx or -1 if the table is full. Caller installs two fds. */
int  pty_new(void);

/* Per-end refcount adjustments — used by fork() / dup2(). */
void pty_inc_master(int idx);
void pty_inc_slave (int idx);
void pty_close_master(int idx);
void pty_close_slave (int idx);
int  pty_master_refs(int idx);
int  pty_slave_refs (int idx);

/* Read from a side's INPUT ring. master_read drains s_to_m;
 * slave_read drains m_to_s. Blocks (task_yield) until at least one
 * byte is available OR the other side has closed AND the ring is
 * drained (returns 0 = EOF). -1 = bad idx. */
int  pty_master_read(int idx, void *buf, int n);
int  pty_slave_read (int idx, void *buf, int n);

/* Write to the OTHER side's input ring. master_write puts bytes into
 * m_to_s (so slave_read picks them up); slave_write puts into s_to_m.
 * Blocks while the destination ring is full; returns -1 if the other
 * side has closed (writes would land in /dev/null). */
int  pty_master_write(int idx, const void *buf, int n);
int  pty_slave_write (int idx, const void *buf, int n);

#endif
