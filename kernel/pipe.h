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

#define PIPE_MAX     8
#define PIPE_BUF_SZ  4096

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

/* Best-effort write. Returns bytes written. Spins yielding while the
 * ring is full as long as the read end is still open. Returns -1 if
 * the read end has been closed (writer would just be feeding /dev/null). */
int  pipe_write(int idx, const void *buf, int n);

/* For diagnostics. */
int  pipe_read_refs (int idx);
int  pipe_write_refs(int idx);

#endif
