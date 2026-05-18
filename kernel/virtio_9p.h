/*
 * virtio-9p — paravirtualized host filesystem passthrough. Speaks
 * the 9P2000.L protocol over the virtio transport, exposing files
 * from the QEMU host to AdventOS via a VFS mount at /mnt/9p.
 *
 * QEMU CLI:
 *   -fsdev local,id=hostfs,path=/some/host/dir,security_model=none \
 *   -device virtio-9p-pci,fsdev=hostfs,mount_tag=hostshare,disable-modern=on
 *
 * From inside AdventOS:
 *   $ ls /mnt/9p
 *   $ cat /mnt/9p/readme.txt
 *
 * Limitations vs a real 9P client:
 *  - read-only-ish: we wire up read / readdir / getattr but defer
 *    write/create to a follow-up session
 *  - stateless: each VFS op WALKs from root, OPENs, READs, then
 *    CLUNKs. Slower than caching fids but matches the VFS shape
 *    (no close() op in struct vfs_fs_ops).
 *  - single mount: one virtio-9p device, mounted at one mount point.
 */
#ifndef ADVENTOS_VIRTIO_9P_H
#define ADVENTOS_VIRTIO_9P_H

#include "../include/types.h"

void virtio_9p_init(void);
int  virtio_9p_available(void);

/* Mount the 9P share at `mount_point` via VFS. Returns 0 on success,
 * -1 if no device or already mounted. */
int  virtio_9p_mount(const char *mount_point);

/* Read `n` bytes at `offset` from the file identified by `inode_slot`
 * (the obj_idx that 9p stuffed into a struct task_fd at SYS_OPEN time).
 * Returns bytes read, 0 on EOF, -1 on error. Called from SYS_READ
 * for FD_9P file descriptors. */
int  virtio_9p_fd_read(int inode_slot, uint32_t offset,
                       void *buf, uint32_t n);

/* Release the inode-cache slot allocated by v9p_vfs_open(). Called
 * from SYS_CLOSE's release_fd path. */
void virtio_9p_fd_close(int inode_slot);

/* Remove `rel_path` (relative to the 9p mount point) from the host
 * share. is_dir = 0 for files (Tunlinkat), 1 for empty directories
 * (Tunlinkat with AT_REMOVEDIR). Returns 0 / -1. Called from SYS_UNLINK
 * and SYS_RMDIR when the path lives under the 9p mount. */
int  virtio_9p_unlink_path(const char *rel_path, int is_dir);

#endif
