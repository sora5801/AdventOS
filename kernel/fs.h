#ifndef ADVENTOS_FS_H
#define ADVENTOS_FS_H

#include "../include/types.h"

/*
 * AdventFS — read-only flat filesystem.
 *
 * Disk layout:
 *   LBA 0           Boot sector (MBR)
 *   LBA 1..N        Kernel image (loaded by bootloader)
 *   LBA 200         Superblock (sector 0 of FS area)
 *   LBA 201+        File data (each file in contiguous sectors)
 *
 * Superblock fits in one 512-byte sector:
 *   - 8-byte magic "ADVENTFS"
 *   - uint32 file_count
 *   - 16 fs_entry slots (each: 16-byte name + uint32 start_sector + uint32 size)
 *   - Total: 8 + 4 + 16*24 = 396 bytes (rest of sector is zero padding)
 *
 * `start_sector` in each entry is RELATIVE to FS_DISK_OFFSET_SECTORS,
 * so adding 200 gives the absolute LBA on disk.
 */

#define FS_DISK_OFFSET_SECTORS  200
#define FS_NAME_MAX             16
#define FS_MAX_FILES            16
#define FS_MAGIC                "ADVENTFS"

struct fs_entry {
    char     name[FS_NAME_MAX];   /* NUL-padded                              */
    uint32_t start_sector;        /* relative to FS_DISK_OFFSET_SECTORS      */
    uint32_t size;                /* bytes                                   */
} __attribute__((packed));

struct fs_super {
    char            magic[8];
    uint32_t        file_count;
    struct fs_entry files[FS_MAX_FILES];
} __attribute__((packed));

void        fs_init(void);
int         fs_open(const char *name);                                  /* idx or -1   */
int         fs_read(int idx, uint32_t offset, void *buf, uint32_t n);   /* bytes or -1 */
uint32_t    fs_size(int idx);
const char *fs_name(int idx);
int         fs_count(void);

/* Write `size` bytes from `data` as the entire contents of `name`,
 * creating the file if it doesn't exist. Replaces any prior contents.
 * Allocates a fresh contiguous sector run from the disk's free area
 * (no in-place rewrite — old sectors leak; cheap and simple), then
 * persists the updated superblock back to disk so the file survives
 * a reboot. Returns 0 on success, -1 on error (no slot, write fail). */
int         fs_write_all(const char *name, const void *data, uint32_t size);

/* Free disk space remaining (in sectors past the high-water mark).
 * Used by the shell's selftest for sanity reporting. */
uint32_t    fs_free_sectors(void);

#endif
