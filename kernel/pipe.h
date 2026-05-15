#ifndef ADVENTOS_PIPE_H
#define ADVENTOS_PIPE_H

#include "../include/types.h"

/*
 * In-kernel pipes. Each pipe is a fixed-size SPSC ring buffer with
 * separate per-end refcounts:
 *
 *   read_refs  - how many fds reference the read end
 *   write_refs - how many fds reference the write end
 *
 * fork() bumps the per-end count for each fd in the parent's table
 * pointing at the pipe; close() decrements; when both reach zero the
 * pipe is freed back into the table for reuse.
 *
 * Semantics chosen to match POSIX as closely as our scheduler allows:
 *   - pipe_read blocks until bytes arrive OR write_refs == 0 (EOF)
 *   - pipe_write blocks until space OR read_refs == 0 (EPIPE-ish)
 *
 * Both blocking modes use task_yield in a loop. Same approach as
 * sock_read/accept; same trade-off (no proper wait queues yet).
 */

/* Session 74 bump from 8 -> 24: agentd's background-job machinery
 * holds 2 pipes (stdout + stderr) per running job, and JOB_MAX is 8
 * — that alone burns 16 pipes. The remaining 8 stay reserved for
 * normal shell pipelines + selftest harnesses. Cost is ~25 KiB of
 * kernel BSS (24 * sizeof(struct pipe), each ~1048 bytes after the
 * PIPE_BUF_SZ trim below).
 *
 * NOTE: PIPE_BUF_SZ stayed at 4096 in earlier sessions, but bumping
 * PIPE_MAX to 24 with the 4 KiB ring would push kernel .bss past
 * 0x9FC00 — the BIOS's Extended BIOS Data Area. The kernel's
 * `rep stosb` zero-bss in entry.S would then clobber the EBDA mid-
 * boot, causing the kernel to hang during VBE / fbcon_init. The
 * trim to 1 KiB keeps .bss in the safe 0x300b0..0x9DB18 window.
 * If PIPE_MAX ever drops back to <= 12 the ring can grow again. */
#define PIPE_MAX     24
#define PIPE_BUF_SZ  1024

void pipe_init(void);

/* Allocate a fresh pipe with read_refs=1, write_refs=1. Returns pipe
 * idx (caller installs into two fds) or -1 if the table is full. */
int  pipe_new(void);

/* Increase per-end refcount. Used by fork() / dup2(). */
void pipe_inc_read (int idx);
void pipe_inc_write(int idx);

/* Drop a reference to one end. When the corresponding refcount hits
 * zero the other end will see EOF / EPIPE on its next operation. */
void pipe_close_read (int idx);
void pipe_close_write(int idx);

/* Blocking read. Returns bytes copied, 0 on EOF (write end closed
 * AND ring drained), -1 on bad idx. */
int  pipe_read (int idx, void *buf, int n);

/* Session 74 — peek for the non-blocking syscall path. Returns:
 *    1  bytes are available OR the writer has closed (so a follow-up
 *       pipe_read returns >0 or 0=EOF without blocking)
 *    0  ring empty and writer still open — pipe_read would yield-loop
 *   -1  bad idx
 * The syscall layer uses this to honor FD_FL_NONBLOCK without
 * peeking into struct pipe internals. */
int  pipe_read_avail(int idx);

/* Best-effort write. Returns bytes written. Spins yielding while the
 * ring is full as long as the read end is still open. Returns -1 if
 * the read end has been closed (writer would just be feeding /dev/null). */
int  pipe_write(int idx, const void *buf, int n);

/* For diagnostics. */
int  pipe_read_refs (int idx);
int  pipe_write_refs(int idx);

#endif
