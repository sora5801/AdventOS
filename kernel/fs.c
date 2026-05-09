#include "fs.h"
#include "ata.h"
#include "kprintf.h"
#include "string.h"

static struct fs_super g_super;
static int             g_initialized;

/* The disk we live on. We don't have a partition table — the boot
 * sector + kernel image live before the FS area, so the disk's total
 * sector count minus FS_DISK_OFFSET_SECTORS is what we have to spend.
 * Hardcode a generous cap; QEMU images we boot from are well over this. */
#define FS_DISK_TOTAL_SECTORS   1024u    /* 512 KiB of FS area */
#define FS_BITMAP_BYTES         ((FS_DISK_TOTAL_SECTORS + 7) / 8)

/* Free-sector bitmap (session 23). 1 bit per sector relative to
 * FS_DISK_OFFSET_SECTORS; bit set => allocated. Sector 0 (the
 * superblock) is permanently allocated. The bitmap is in-RAM only —
 * recomputed at mount by walking the superblock — so we never have
 * to keep an on-disk copy in sync. */
static uint8_t g_bitmap[FS_BITMAP_BYTES];

static inline int bitmap_get(uint32_t s) {
    if (s >= FS_DISK_TOTAL_SECTORS) return 1;     /* off-disk = used */
    return (g_bitmap[s >> 3] >> (s & 7)) & 1;
}
static inline void bitmap_set(uint32_t s) {
    if (s < FS_DISK_TOTAL_SECTORS) g_bitmap[s >> 3] |= (uint8_t)(1u << (s & 7));
}
static inline void bitmap_clear(uint32_t s) {
    if (s < FS_DISK_TOTAL_SECTORS) g_bitmap[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

/* First-fit allocator. Find the lowest run of `n` contiguous free
 * sectors and mark them used. Returns the start sector or -1 on
 * out-of-disk. Sector 0 is skipped (superblock). */
static int bitmap_alloc_run(uint32_t n) {
    if (n == 0) n = 1;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    for (uint32_t s = 1; s < FS_DISK_TOTAL_SECTORS; s++) {
        if (!bitmap_get(s)) {
            if (run_len == 0) run_start = s;
            run_len++;
            if (run_len >= n) {
                for (uint32_t k = 0; k < n; k++) bitmap_set(run_start + k);
                return (int)run_start;
            }
        } else {
            run_len = 0;
        }
    }
    return -1;
}

/* Mark the run [start, start+n) as free. */
static void bitmap_free_run(uint32_t start, uint32_t n) {
    if (start == 0) return;       /* never free the superblock */
    for (uint32_t k = 0; k < n; k++) {
        if (start + k < FS_DISK_TOTAL_SECTORS) bitmap_clear(start + k);
    }
}

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

    /* Reconstruct the bitmap by walking files. Sector 0 is permanently
     * allocated (superblock); each file's contiguous run is too. */
    for (uint32_t i = 0; i < FS_BITMAP_BYTES; i++) g_bitmap[i] = 0;
    bitmap_set(0);
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        struct fs_entry *e = &g_super.files[i];
        if (e->size == 0) continue;
        uint32_t n = (e->size + 511) / 512;
        for (uint32_t s = 0; s < n; s++) {
            bitmap_set(e->start_sector + s);
        }
    }

    g_initialized = 1;
    kprintf("fs: AdventFS mounted, %u files, %u/%u sectors free\n",
            (unsigned)g_super.file_count,
            (unsigned)fs_free_sectors(),
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
    uint32_t free = 0;
    for (uint32_t s = 0; s < FS_DISK_TOTAL_SECTORS; s++) {
        if (!bitmap_get(s)) free++;
    }
    return free;
}

/* Persist g_super back to LBA FS_DISK_OFFSET_SECTORS. The struct is
 * packed and sized to fit a 512-byte sector by construction. */
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

    /* Snapshot the existing range so we can free it AFTER the new
     * write commits. allocate-first keeps the file readable through
     * the old entry if the new write fails halfway. */
    uint32_t old_start = g_super.files[idx].start_sector;
    uint32_t old_size  = g_super.files[idx].size;
    uint32_t old_n     = old_size ? (old_size + 511) / 512 : 0;

    uint32_t needed = (size + 511) / 512;
    if (needed == 0) needed = 1;            /* always allocate at least one sector */

    int new_start = bitmap_alloc_run(needed);
    if (new_start < 0) {
        return -1;                          /* out of contiguous space */
    }

    /* Write the data. Last sector is zero-padded to 512 bytes. If a
     * sector write fails, roll back the bitmap and leave the entry
     * pointing at its prior contents. */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t buf[512];
    for (uint32_t s = 0; s < needed; s++) {
        uint32_t off  = s * 512;
        uint32_t take = (off + 512 <= size) ? 512 : (size - off);

        for (int i = 0; i < 512; i++) buf[i] = 0;
        if (take > 0) memcpy(buf, src + off, take);

        if (ata_write_sector(FS_DISK_OFFSET_SECTORS + (uint32_t)new_start + s,
                             buf) != 0) {
            bitmap_free_run((uint32_t)new_start, needed);
            return -1;
        }
    }

    /* Commit: point the entry at the new range, free the old range
     * (if there was one), then push the superblock out. */
    g_super.files[idx].start_sector = (uint32_t)new_start;
    g_super.files[idx].size         = size;

    if (old_n > 0) bitmap_free_run(old_start, old_n);

    if (fs_write_super() != 0) return -1;
    return 0;
}
