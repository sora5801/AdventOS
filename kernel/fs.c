#include "fs.h"
#include "ata.h"
#include "kprintf.h"
#include "string.h"

static struct fs_super g_super;
static int             g_initialized;

/* Next free sector RELATIVE to FS_DISK_OFFSET_SECTORS. Sector 0 of
 * the FS area holds the superblock; sectors 1..N-1 hold file data;
 * sectors N..end are free. fs_init computes this by walking the
 * file table; fs_write_all bumps it on every save. We don't (yet)
 * have a free-sector bitmap, so a saved-then-saved-again file leaks
 * its previous on-disk range — fine for a demo OS, painful for a
 * production one. */
static uint32_t        g_high_water;

/* The disk we live on. We don't have a partition table — the boot
 * sector + kernel image live before the FS area, so the disk's total
 * sector count minus FS_DISK_OFFSET_SECTORS is what we have to spend.
 * Hardcode a generous cap; QEMU images we boot from are well over this. */
#define FS_DISK_TOTAL_SECTORS   1024u    /* 512 KiB of FS area */

void fs_init(void) {
    /* Read superblock from sector 0 of FS area. */
    uint8_t sec[512];
    if (ata_read_sector(FS_DISK_OFFSET_SECTORS, sec) != 0) {
        kputs("fs: failed to read superblock\n");
        return;
    }

    if (memcmp(sec, FS_MAGIC, 8) != 0) {
        kputs("fs: bad magic in superblock (no filesystem on disk)\n");
        return;
    }

    /* The on-disk struct is bit-identical to our C struct (packed). */
    memcpy(&g_super, sec, sizeof(g_super));

    if (g_super.file_count > FS_MAX_FILES) {
        g_super.file_count = FS_MAX_FILES;
    }

    /* Compute the high-water mark by walking files. Sector 0 is the
     * superblock; the smallest file_run end sector +1 is where free
     * space starts. */
    g_high_water = 1;
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        struct fs_entry *e = &g_super.files[i];
        uint32_t end = e->start_sector + (e->size + 511) / 512;
        if (end > g_high_water) g_high_water = end;
    }

    g_initialized = 1;
    kprintf("fs: AdventFS mounted, %u files (free sec %u..%u)\n",
            (unsigned)g_super.file_count,
            (unsigned)g_high_water,
            (unsigned)FS_DISK_TOTAL_SECTORS);
}

int fs_open(const char *name) {
    if (!g_initialized) return -1;
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        if (strncmp(g_super.files[i].name, name, FS_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int fs_read(int idx, uint32_t offset, void *buf, uint32_t n) {
    if (!g_initialized) return -1;
    if (idx < 0 || (uint32_t)idx >= g_super.file_count) return -1;

    struct fs_entry *e = &g_super.files[idx];
    if (offset >= e->size) return 0;
    if (offset + n > e->size) n = e->size - offset;

    /* Read sector by sector. The file's first byte is at:
     *     LBA = FS_DISK_OFFSET_SECTORS + e->start_sector
     *           (offset 0 within that sector). */
    uint8_t  sec_buf[512];
    uint32_t total = 0;
    uint8_t *out = (uint8_t *)buf;

    while (n > 0) {
        uint32_t abs_off  = offset + total;
        uint32_t lba      = FS_DISK_OFFSET_SECTORS + e->start_sector
                          + (abs_off / 512);
        uint32_t off_in   = abs_off % 512;

        if (ata_read_sector(lba, sec_buf) != 0) return -1;

        uint32_t take = 512 - off_in;
        if (take > n) take = n;
        memcpy(out + total, sec_buf + off_in, take);

        total += take;
        n     -= take;
    }
    return (int)total;
}

uint32_t fs_size(int idx) {
    if (!g_initialized) return 0;
    if (idx < 0 || (uint32_t)idx >= g_super.file_count) return 0;
    return g_super.files[idx].size;
}

const char *fs_name(int idx) {
    if (!g_initialized) return NULL;
    if (idx < 0 || (uint32_t)idx >= g_super.file_count) return NULL;
    return g_super.files[idx].name;
}

int fs_count(void) {
    if (!g_initialized) return 0;
    return (int)g_super.file_count;
}

uint32_t fs_free_sectors(void) {
    if (!g_initialized) return 0;
    if (g_high_water >= FS_DISK_TOTAL_SECTORS) return 0;
    return FS_DISK_TOTAL_SECTORS - g_high_water;
}

/* Persist g_super back to LBA FS_DISK_OFFSET_SECTORS. The struct is
 * packed and sized to fit a 512-byte sector by construction (8 magic
 * + 4 file_count + 16 entries × 24 bytes = 396 bytes). */
static int fs_write_super(void) {
    uint8_t sec[512];
    for (int i = 0; i < 512; i++) sec[i] = 0;
    memcpy(sec, &g_super, sizeof(g_super));
    return ata_write_sector(FS_DISK_OFFSET_SECTORS, sec);
}

/* Find an existing entry by name, or allocate a new slot. Returns
 * the entry index or -1 if the table is full. */
static int find_or_create_entry(const char *name) {
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        if (strncmp(g_super.files[i].name, name, FS_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    if (g_super.file_count >= FS_MAX_FILES) return -1;
    int idx = (int)g_super.file_count;
    g_super.file_count++;

    struct fs_entry *e = &g_super.files[idx];
    /* Zero the name buffer first so trailing bytes are clean if the
     * new name is shorter than a previous slot occupant. */
    for (int i = 0; i < FS_NAME_MAX; i++) e->name[i] = 0;
    int j;
    for (j = 0; j < FS_NAME_MAX && name[j]; j++) e->name[j] = name[j];
    e->start_sector = 0;
    e->size         = 0;
    return idx;
}

int fs_write_all(const char *name, const void *data, uint32_t size) {
    if (!g_initialized) return -1;
    if (!name)          return -1;

    int idx = find_or_create_entry(name);
    if (idx < 0) return -1;

    uint32_t needed = (size + 511) / 512;
    if (needed == 0) needed = 1;            /* always allocate at least one sector */

    if (g_high_water + needed > FS_DISK_TOTAL_SECTORS) {
        return -1;                          /* out of disk */
    }

    uint32_t new_start = g_high_water;
    g_high_water += needed;

    /* Write the data. Last sector is zero-padded to 512 bytes. */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t buf[512];
    for (uint32_t s = 0; s < needed; s++) {
        uint32_t off  = s * 512;
        uint32_t take = (off + 512 <= size) ? 512 : (size - off);

        for (int i = 0; i < 512; i++) buf[i] = 0;
        if (take > 0) memcpy(buf, src + off, take);

        if (ata_write_sector(FS_DISK_OFFSET_SECTORS + new_start + s, buf) != 0) {
            return -1;
        }
    }

    /* Update the file table entry, then push the superblock out. */
    g_super.files[idx].start_sector = new_start;
    g_super.files[idx].size         = size;

    if (fs_write_super() != 0) return -1;
    return 0;
}
