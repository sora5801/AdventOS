#ifndef ADVENTOS_TMPFS_H
#define ADVENTOS_TMPFS_H

#include "../include/types.h"
#include "fs.h"     /* for FS_NAME_MAX */

/*
 * Tiny in-memory writable filesystem. Lives alongside the read-only
 * AdventFS — sys_open looks up the on-disk fs first, then tmpfs;
 * sys_open_w creates (or truncates) a tmpfs entry and returns it as
 * a writable fd.
 *
 * Each tmpfile holds:
 *   - a fixed-cap buffer (kmalloc'd, grows with kmalloc/kfree as needed)
 *   - an refs counter that fork/dup2 bump and close drops
 *   - a name so future sys_open / `cat foo` can find it
 *
 * The data sticks around even when refs == 0, so "echo hi > foo;
 * cat foo" works cleanly. A more realistic system would have an
 * "unlink" syscall to reclaim space; we don't bother.
 */

#define TMPFS_MAX        16
#define TMPFS_INITIAL_CAP  256

void tmpfs_init(void);

/* Look up an existing tmpfile by name; if found, bumps refs and
 * returns idx. If not found, returns -1. Used by sys_open after the
 * read-only fs lookup misses. */
int  tmpfs_open(const char *name);

/* Create (or truncate) a tmpfile. Bumps refs; returns idx. -1 on
 * out-of-table. Used by sys_open_w (the `>` redirect target). */
int  tmpfs_create(const char *name);

/* Drop one fd reference. The data stays — only when "unlinked" (we
 * don't have unlink yet) AND refs==0 would the buffer get freed. */
void tmpfs_close(int idx);

/* Bump refcount. Used by fork() copying the fd table and by dup2(). */
void tmpfs_inc_ref(int idx);

int  tmpfs_read (int idx, uint32_t offset, void *buf, uint32_t n);

/* Append `n` bytes to the tmpfile, growing the backing buffer as
 * needed. Returns bytes written or -1 on OOM / bad idx. */
int  tmpfs_write(int idx, const void *buf, uint32_t n);

uint32_t    tmpfs_size(int idx);
const char *tmpfs_name(int idx);

#endif
