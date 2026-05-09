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

#define FS_DISK_OFFSET_SECTORS  200u
#define FS_SUPER_SECTORS        3u    /* superblock = 1 header + 2 entry sectors */
#define FS_NAME_MAX             16
#define FS_MAX_FILES            32    /* bumped from 16 in session 26 (coreutils) */
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
    uint8_t  reserved[6];
} __attribute__((packed));        /* 32 bytes */

struct fs_super {
    /* First 512 bytes (sector 0) */
    char     magic[8];
    uint32_t file_count;
    uint8_t  reserved0[500];
    /* Next 512 bytes (sector 1): the entry table */
    struct fs_entry files[FS_MAX_FILES];
} __attribute__((packed));        /* 1024 bytes total */

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
struct vfs_fs_ops;
struct vfs_fs_ops *fs_rootfs_ops(void);

#endif
