#ifndef ADVENTOS_FS_H
#define ADVENTOS_FS_H

#include "../include/types.h"

/*
 * AdventFS — hierarchical filesystem (session 25).
 *
 * Disk layout:
 *   LBA 0           Boot sector (MBR)
 *   LBA 1..N        Kernel image
 *   LBA 200..201    Superblock (2 sectors of FS area)
 *   LBA 202+        File data (each file in contiguous sectors)
 *
 * Each entry is 32 bytes:
 *   - 16-byte name (NUL-padded; basename only — no path components)
 *   - 4-byte start_sector
 *   - 4-byte size
 *   - 1-byte type (FREE / FILE / DIR)
 *   - 1-byte parent_dir (entry index, or 0xFF for root entries)
 *   - 6 bytes reserved
 *
 * Directories are entries with type=DIR and size=start_sector=0
 * (no on-disk content of their own). The directory tree is encoded
 * as a parent-pointer forest: walk g_super.files[] and match on
 * parent_dir to enumerate a directory's children.
 *
 * Superblock layout:
 *   Sector 0: 8 magic + 4 file_count + 500 reserved
 *   Sector 1: 16 entries × 32 bytes
 */

/* Bumped from 200 → 256 in session 38: kernel.bin grew past 100 KiB
 * with all the SMP machinery (BKL + g_kvprintf_lock + ac97 + tls
 * etc.), pushing it past sector 200. The FS would land at the wrong
 * offset. 256 sectors = 128 KiB of boot+kernel headroom. */
#define FS_DISK_OFFSET_SECTORS  256u
#define FS_SUPER_SECTORS        5u    /* superblock = 1 header + 4 entry sectors */
#define FS_NAME_MAX             16
#define FS_MAX_FILES            64    /* bumped from 32 in session 29 (network apps) */
#define FS_ENTRY_SIZE           32
#define FS_MAGIC                "ADVENTFS"

#define FS_TYPE_FREE  0
#define FS_TYPE_FILE  1
#define FS_TYPE_DIR   2

#define FS_DIR_ROOT   0xFFu     /* sentinel parent_dir for entries at / */

struct fs_entry {
    char     name[FS_NAME_MAX];   /* basename, NUL-padded */
    uint32_t start_sector;        /* relative to FS_DISK_OFFSET_SECTORS */
    uint32_t size;                /* bytes (files only; 0 for dirs) */
    uint8_t  type;                /* FS_TYPE_* */
    uint8_t  parent_dir;          /* entry idx of parent dir, or FS_DIR_ROOT */
    uint16_t uid;                 /* file owner (session 47). 0 = root.   */
    uint16_t gid;                 /* file group. Stamped on creation.     */
    uint16_t mode;                /* Unix-style rwxrwxrwx in low 9 bits   */
                                  /* (session 48). Used by fs_check_perm. */
} __attribute__((packed));        /* 32 bytes — no more reserved space */

/* Mode bit constants — match POSIX <sys/stat.h> octal values. */
#define FS_MODE_IRUSR  0400u
#define FS_MODE_IWUSR  0200u
#define FS_MODE_IXUSR  0100u
#define FS_MODE_IRGRP  0040u
#define FS_MODE_IWGRP  0020u
#define FS_MODE_IXGRP  0010u
#define FS_MODE_IROTH  0004u
#define FS_MODE_IWOTH  0002u
#define FS_MODE_IXOTH  0001u
#define FS_MODE_RWX_MASK 0777u

/* "want" bits passed to fs_check_perm — same encoding as the
 * per-tier rwx triple (4=R, 2=W, 1=X). */
#define FS_PERM_R  4
#define FS_PERM_W  2
#define FS_PERM_X  1

struct fs_super {
    /* First 512 bytes (sector 0) */
    char     magic[8];
    uint32_t file_count;
    uint8_t  reserved0[500];
    /* Next 512 bytes (sector 1): the entry table */
    struct fs_entry files[FS_MAX_FILES];
} __attribute__((packed));        /* 1024 bytes total */

/* ---- Multi-instance core (session 42) ----------------------------- */

/* AdventFS is now multi-instance: the boot disk's filesystem is one
 * instance, the USB drive mounted at /mnt/usb is another. Each
 * instance owns its own superblock + sector bitmap and is bound to
 * a backing block device.
 *
 * The public single-instance API below (`fs_open`, `fs_read`, …)
 * still operates on the BOOT instance — those calls are how the
 * kernel's elf loader / dyld / shell built-ins find files on the
 * boot disk. Anything that wants a non-boot mount goes through VFS.
 */
struct fs_instance;
struct blkdev;
struct vfs_fs_ops;

/* Create a fresh fs instance backed by `bdev`. The superblock is
 * read from `base_lba` on that device; `n_sectors` caps the bitmap
 * (everything past it is marked permanently allocated). Returns
 * NULL on bad-magic / read-failure / out-of-table.
 *
 * Pass bdev=NULL to use the global bcache + ATA path (the boot fs
 * does this internally; outside callers should always pass a real
 * blkdev). */
struct fs_instance *fs_create_instance(struct blkdev *bdev,
                                       uint32_t base_lba,
                                       uint32_t n_sectors);

/* The root fs instance — set up by fs_init(). NULL before fs_init. */
struct fs_instance *fs_root_instance(void);

/* Build a vfs_fs_ops table whose ops are bound to `inst` via the
 * fs_data field. The returned pointer aliases statically-allocated
 * storage inside fs.c, one per instance. */
struct vfs_fs_ops *fs_make_ops_for(struct fs_instance *inst);

/* Per-instance read used by SYS_READ. The fd table stores the
 * fs_instance pointer (or NULL = boot fs) alongside the entry idx;
 * this dispatches to the right superblock. */
int         fs_read_at(void *fs_data, int idx, uint32_t offset,
                       void *buf, uint32_t n);

/* ---- Boot-instance singleton API (rest of the kernel uses this) -- */

void        fs_init(void);

/* Path-aware open. `path` may be:
 *     "/abs/path"   resolve from root
 *     "rel/path"    resolve from the calling task's cwd
 *     "name"        bare basename, in cwd (no directory walk)
 *
 * Returns the matching entry index or -1 if any component is missing
 * or a non-final component isn't a directory. */
int         fs_open(const char *path);

int         fs_read(int idx, uint32_t offset, void *buf, uint32_t n);
uint32_t    fs_size(int idx);
const char *fs_name(int idx);
int         fs_count(void);
uint8_t     fs_entry_type(int idx);
int         fs_entry_parent(int idx);
int         fs_entry_uid(int idx);
int         fs_entry_gid(int idx);
int         fs_entry_mode(int idx);    /* low 9 bits = rwxrwxrwx (session 48) */

/* Permission check: does the calling task (uid/gid) have all of
 * `want` (R | W | X bits) on entry `idx`? Returns 1 = allowed,
 * 0 = denied, -1 = bad index. Root (uid 0) always allowed. */
int         fs_check_perm(int idx, int want);

/* Modify mode / owner on an existing entry. Both syscalls write
 * the superblock through immediately. Caller must already have
 * checked privilege (root for chown; root-or-owner for chmod). */
int         fs_chmod_idx(int idx, uint16_t mode);
int         fs_chown_idx(int idx, uint16_t uid, uint16_t gid);

/* Whole-file write. Path semantics same as fs_open. Creates the file
 * if absent. Allocates a fresh contiguous sector run from the bitmap;
 * old sectors are returned to the free pool on success. */
int         fs_write_all(const char *path, const void *data, uint32_t size);

/* Create a directory at `path`. Parent must already exist. Returns
 * the new entry's index or -1. */
int         fs_mkdir(const char *path);

/* Iterate the children of a directory. `iter` starts at 0; each call
 * returns the next child index and advances `iter`, or -1 when done.
 * `dir_idx` is FS_DIR_ROOT for /, or an index returned by fs_open. */
int         fs_dir_iter(int dir_idx, int *iter);

uint32_t    fs_free_sectors(void);

/* The rootfs adapter exposed to the VFS layer (session 28). Pass to
 * vfs_mount to register the on-disk AdventFS as the system root. */
struct vfs_fs_ops *fs_rootfs_ops(void);

#endif
