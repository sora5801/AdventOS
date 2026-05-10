#ifndef ADVENTOS_VFS_H
#define ADVENTOS_VFS_H

#include "../include/types.h"

/*
 * VFS — virtual filesystem dispatch layer (session 28).
 *
 * Sits at the syscall layer between user open/read/readdir/mkdir
 * and the per-fs implementations. Mounts a small table of
 * filesystems at distinct mount points; vfs_open(path) finds the
 * longest mount-prefix match and forwards the (relative) path to
 * that fs's ops table.
 *
 *   /              rootfs (the on-disk AdventFS in fs.c)
 *   /proc          procfs (synthetic, generated on-the-fly)
 *
 * The fd table on `struct task` keeps its existing FD_FS / FD_TMPFS
 * / FD_PIPE_R / etc. discriminator. VFS adds one more kind —
 * FD_PROCFS — for fds whose backing fs is the procfs synthesizer.
 * At read time, syscall.c switches on the kind and routes to the
 * appropriate per-fs reader. The path-resolution dispatch lives
 * here; the fd-side dispatch lives in syscall.c. Keeping the two
 * separate means existing fd kinds (TMPFS, sockets, pipes) don't
 * need to know about VFS at all.
 */

#define VFS_MAX_MOUNTS  4
#define VFS_MOUNT_NAMELEN  16

/*
 * Result of vfs_open(): everything syscall.c needs to populate an
 * fd entry plus a quick "is this a directory" check.
 *   kind     a task_fd kind constant — FD_FS for rootfs, FD_PROCFS for proc
 *   obj_idx  fs-specific opaque handle. For rootfs it's the entry index
 *            into g_super.files; for procfs it's a (kind<<16 | pid)
 *            packed identifier.
 *   size     advisory; not all fs's track it accurately
 *   is_dir   1 if the resolved inode names a directory, else 0
 */
struct vfs_inode {
    int      kind;
    int      obj_idx;
    uint32_t size;
    int      is_dir;
    void    *fs_data;     /* the fs instance that owns this inode;
                           * NULL for single-instance fs's like procfs.
                           * Copied through to task_fd by SYS_OPEN so
                           * SYS_READ knows which instance to dispatch
                           * back into. */
};

/*
 * Per-fs operation table. All paths passed to ops are RELATIVE to
 * the mount point — no leading slash, possibly empty (means "the
 * root of this fs"). VFS strips the mount prefix before dispatch.
 *
 * The `fs_data` field lets multi-instance filesystems (e.g. AdventFS
 * mounted twice — once on the boot ATA disk at /, once on a USB drive
 * at /mnt/usb) carry per-mount context through to their op funcs
 * without colliding on global state. Single-instance fs's (procfs)
 * just leave it NULL and ignore the argument.
 */
struct vfs_fs_ops {
    void *fs_data;
    int (*open)     (void *fs_data, const char *rel_path, struct vfs_inode *out);
    int (*read)     (void *fs_data, struct vfs_inode *ino, uint32_t offset,
                     void *buf, uint32_t n);
    int (*readdir)  (void *fs_data, const char *rel_dir, int *iter, char *name_buf);
    int (*mkdir)    (void *fs_data, const char *rel_path);
    int (*write_all)(void *fs_data, const char *rel_path, const void *data, uint32_t n);
};

void vfs_init (void);

/* Register a filesystem at `mount_point` with display `fs_name`.
 * Returns 0 on success, -1 if the table is full or the mount point
 * is already taken. The mount point is stored verbatim — pass "/"
 * for the root fs and "/proc" (etc.) for additional mounts. The
 * fs_name appears in /proc/mounts and is otherwise informational. */
int  vfs_mount(const char *mount_point, const char *fs_name,
               struct vfs_fs_ops *ops);

/* Path-level operations. All paths are absolute or relative;
 * relative paths always resolve against the rootfs (mount "/"). */
int  vfs_open    (const char *path, struct vfs_inode *out);
int  vfs_read    (struct vfs_inode *ino, uint32_t offset,
                  void *buf, uint32_t n);
int  vfs_readdir (const char *path, int *iter, char *name_buf);
int  vfs_mkdir   (const char *path);
int  vfs_write_all(const char *path, const void *data, uint32_t n);

/* Diagnostics — used by /proc/mounts. Walks live mounts and emits
 * "<mount_point> <fs_name>\n" lines into buf, returning bytes
 * written (capped to cap). */
int  vfs_describe_mounts(char *buf, int cap);

#endif
