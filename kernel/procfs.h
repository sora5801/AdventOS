#ifndef ADVENTOS_PROCFS_H
#define ADVENTOS_PROCFS_H

#include "vfs.h"

/*
 * /proc filesystem (session 28). Mounted at /proc by kmain. Files
 * are synthesized on demand: there's no on-disk presence and no
 * persistent storage. Each open captures (kind, pid) into the
 * vfs_inode's obj_idx field; each read regenerates the requested
 * content into a small kernel buffer and returns the requested
 * slice.
 *
 * Files exposed:
 *   /proc/cpuinfo         CPU vendor (CPUID) + features summary
 *   /proc/meminfo         total / used / free pages from PMM
 *   /proc/uptime          seconds since boot from pit_seconds()
 *   /proc/version         kernel version string
 *   /proc/mounts          one line per registered VFS mount
 *   /proc/bcache          block-cache hit/miss/dirty stats
 *   /proc/<pid>/status    name, state, parent_pid, sid, pgid
 */

struct vfs_fs_ops *procfs_ops(void);

/* Direct read entry — called by syscall.c when an fd has kind ==
 * FD_PROCFS. Decodes the obj_idx, regenerates the content, returns
 * the slice. Returns bytes read, 0 at EOF, -1 on error. */
int procfs_read_by_id(int id, uint32_t offset, void *buf, uint32_t n);

#endif
