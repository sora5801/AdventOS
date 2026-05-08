#include "fs.h"
#include "ata.h"
#include "kprintf.h"
#include "string.h"

static struct fs_super g_super;
static int             g_initialized;

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

    g_initialized = 1;
    kprintf("fs: AdventFS mounted, %u files\n", (unsigned)g_super.file_count);
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
