#include "fs.h"
#include "ata.h"
#include "bcache.h"
#include "blkdev.h"
#include "task.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"

/*
 * AdventFS — multi-instance (session 42).
 *
 * Each `struct fs_instance` holds the on-disk superblock and the
 * sector-bitmap for one mounted filesystem. The boot disk's
 * filesystem is the "root" instance, set up by fs_init() and
 * accessed via the singleton fs_open / fs_read / fs_write_all /
 * fs_mkdir API for backward compatibility with the kernel's
 * elf-loader / dyld / shell built-ins. Additional instances —
 * a USB drive mounted at /mnt/usb, in this session — are created
 * via fs_create_instance() and exposed to VFS via
 * fs_make_ops_for(inst).
 *
 * The block I/O dispatch is per-instance:
 *   inst->bdev == NULL  → boot disk; goes through bcache (which
 *                         calls ata_*_sector under the hood)
 *   inst->bdev != NULL  → call the blkdev's read/write directly
 *                         (USB MSC etc.); no caching
 *
 * Per-instance state:
 *   - super        the FS_MAX_FILES file table loaded from disk
 *   - bitmap       which sectors on this disk are allocated
 *   - n_sectors    total sectors visible to this fs (per-disk)
 *   - base_lba     where the superblock starts on the underlying
 *                  device. The boot disk uses FS_DISK_OFFSET_SECTORS
 *                  = 256 (kernel.bin's headroom). The USB drive's
 *                  AdventFS image starts at LBA 0.
 *
 * Path resolution: for paths passed in via VFS (already mount-relative),
 * we always resolve from the instance's root, NOT from any task cwd —
 * cwd_dir lives on the boot fs and doesn't translate to other mounts.
 * The top-level fs_open(path) public API still consults task cwd; that
 * function ALWAYS hits the boot instance.
 */

/* Session 46 bump 1024 → 2048: adding vi.elf pushed fs.img past 1024
 * sectors, breaking new-file allocation against the smaller bitmap. */
#define FS_BITMAP_BYTES_MAX  ((2048 + 7) / 8)   /* room for 2048-sector disks */

struct fs_instance {
    int             in_use;
    int             initialized;
    struct blkdev  *bdev;             /* NULL = boot fs (bcache+ata) */
    uint32_t        base_lba;
    uint32_t        n_sectors;
    struct fs_super super;
    uint8_t         bitmap[FS_BITMAP_BYTES_MAX];
};

#define FS_INSTANCE_MAX  4
static struct fs_instance g_instances[FS_INSTANCE_MAX];
static struct fs_instance *g_root_inst;     /* set by fs_init() */

/* ---- Block I/O dispatch ---------------------------------------- */

static int inst_read_sector(struct fs_instance *inst, uint32_t lba, void *buf) {
    if (inst->bdev) {
        return inst->bdev->read(inst->bdev, lba, 1, buf);
    }
    return bcache_read(lba, buf);
}

static int inst_write_sector(struct fs_instance *inst, uint32_t lba, const void *buf) {
    if (inst->bdev) {
        return inst->bdev->write(inst->bdev, lba, 1, buf);
    }
    return bcache_write(lba, buf);
}

/* ---- Bitmap helpers ------------------------------------------- */

static inline int bitmap_get(struct fs_instance *inst, uint32_t s) {
    if (s >= inst->n_sectors) return 1;
    return (inst->bitmap[s >> 3] >> (s & 7)) & 1;
}
static inline void bitmap_set(struct fs_instance *inst, uint32_t s) {
    if (s < inst->n_sectors) inst->bitmap[s >> 3] |= (uint8_t)(1u << (s & 7));
}
static inline void bitmap_clear(struct fs_instance *inst, uint32_t s) {
    if (s < inst->n_sectors) inst->bitmap[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

static int bitmap_alloc_run(struct fs_instance *inst, uint32_t n) {
    if (n == 0) n = 1;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    /* Skip past the superblock (sectors 0..FS_SUPER_SECTORS-1). */
    for (uint32_t s = FS_SUPER_SECTORS; s < inst->n_sectors; s++) {
        if (!bitmap_get(inst, s)) {
            if (run_len == 0) run_start = s;
            run_len++;
            if (run_len >= n) {
                for (uint32_t k = 0; k < n; k++) bitmap_set(inst, run_start + k);
                return (int)run_start;
            }
        } else {
            run_len = 0;
        }
    }
    return -1;
}

static void bitmap_free_run(struct fs_instance *inst, uint32_t start, uint32_t n) {
    if (start < FS_SUPER_SECTORS) return;
    for (uint32_t k = 0; k < n; k++) {
        if (start + k < inst->n_sectors) bitmap_clear(inst, start + k);
    }
}

/* ---- Instance create / init ---------------------------------- */

static struct fs_instance *alloc_instance(void) {
    for (int i = 0; i < FS_INSTANCE_MAX; i++) {
        if (!g_instances[i].in_use) return &g_instances[i];
    }
    return 0;
}

/* Common init: read superblock from the device, populate bitmap. */
static int load_superblock(struct fs_instance *inst) {
    uint8_t sb[FS_SUPER_SECTORS * 512];
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) {
        if (inst_read_sector(inst, inst->base_lba + s, sb + s * 512) != 0) {
            return -1;
        }
    }
    if (memcmp(sb, FS_MAGIC, 8) != 0) {
        return -2;
    }
    memcpy(&inst->super, sb, sizeof(inst->super));
    if (inst->super.file_count > FS_MAX_FILES) inst->super.file_count = FS_MAX_FILES;

    /* Reconstruct bitmap. */
    for (uint32_t i = 0; i < FS_BITMAP_BYTES_MAX; i++) inst->bitmap[i] = 0;
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) bitmap_set(inst, s);
    for (uint32_t i = 0; i < inst->super.file_count; i++) {
        struct fs_entry *e = &inst->super.files[i];
        if (e->type != FS_TYPE_FILE) continue;
        if (e->size == 0)            continue;
        uint32_t n = (e->size + 511) / 512;
        for (uint32_t s = 0; s < n; s++) bitmap_set(inst, e->start_sector + s);
    }
    inst->initialized = 1;
    return 0;
}

struct fs_instance *fs_create_instance(struct blkdev *bdev,
                                       uint32_t base_lba,
                                       uint32_t n_sectors)
{
    struct fs_instance *inst = alloc_instance();
    if (!inst) return 0;
    if (n_sectors > 2048) n_sectors = 2048;     /* bitmap cap */

    inst->in_use      = 1;
    inst->initialized = 0;
    inst->bdev        = bdev;
    inst->base_lba    = base_lba;
    inst->n_sectors   = n_sectors;
    for (uint32_t i = 0; i < FS_BITMAP_BYTES_MAX; i++) inst->bitmap[i] = 0;

    int rc = load_superblock(inst);
    if (rc != 0) {
        inst->in_use = 0;
        kprintf("fs: instance(bdev=%s, base=%u): load_superblock rc=%d\n",
                bdev ? bdev->name : "boot", (unsigned)base_lba, rc);
        return 0;
    }
    kprintf("fs: instance(bdev=%s, base=%u): %u entries, %u sectors visible\n",
            bdev ? bdev->name : "boot", (unsigned)base_lba,
            (unsigned)inst->super.file_count, (unsigned)n_sectors);
    return inst;
}

struct fs_instance *fs_root_instance(void) { return g_root_inst; }

void fs_init(void) {
    /* Boot fs: backed by bcache+ata. base_lba = FS_DISK_OFFSET_SECTORS.
     * Session 46 bump 1024 → 2048 sectors — added vi.elf to mkfs.py
     * pushed the on-disk fs.img past 1024 sectors. */
    g_root_inst = fs_create_instance(/*bdev=*/0,
                                     FS_DISK_OFFSET_SECTORS,
                                     /*n_sectors=*/2048);
}

static int fs_write_super_inst(struct fs_instance *inst) {
    uint8_t sb[FS_SUPER_SECTORS * 512];
    for (int i = 0; i < (int)sizeof(sb); i++) sb[i] = 0;
    memcpy(sb, &inst->super, sizeof(inst->super));
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) {
        if (inst_write_sector(inst, inst->base_lba + s, sb + s * 512) != 0) return -1;
    }
    return 0;
}

/* ---- Per-instance entry helpers ------------------------------ */

static uint32_t inst_size(struct fs_instance *inst, int idx) {
    if (!inst || !inst->initialized || idx < 0 ||
        (uint32_t)idx >= inst->super.file_count) return 0;
    return inst->super.files[idx].size;
}

static const char *inst_name(struct fs_instance *inst, int idx) {
    if (!inst || !inst->initialized || idx < 0 ||
        (uint32_t)idx >= inst->super.file_count) return 0;
    return inst->super.files[idx].name;
}

static uint8_t inst_entry_type(struct fs_instance *inst, int idx) {
    if (!inst || !inst->initialized || idx < 0 ||
        (uint32_t)idx >= inst->super.file_count) return FS_TYPE_FREE;
    return inst->super.files[idx].type;
}

/* ---- Path resolution (per-instance) -------------------------- */

static int name_eq_slice(const char *ename, const char *s, int len) {
    if (len > FS_NAME_MAX) return 0;
    for (int j = 0; j < len; j++) if (ename[j] != s[j]) return 0;
    if (len < FS_NAME_MAX && ename[len] != 0) return 0;
    return 1;
}

static int find_in_dir_inst(struct fs_instance *inst, uint8_t parent,
                            const char *name, int len) {
    for (uint32_t i = 0; i < inst->super.file_count; i++) {
        struct fs_entry *e = &inst->super.files[i];
        if (e->type == FS_TYPE_FREE)         continue;
        if (e->parent_dir != parent)         continue;
        if (name_eq_slice(e->name, name, len)) return (int)i;
    }
    return -1;
}

/* Walk a path. `from_root` controls whether a leading non-/ path
 * is resolved from cwd_dir (boot fs only) or always from root
 * (every other instance — VFS-passed paths). */
static int fs_iopen_inst(struct fs_instance *inst, const char *path, int from_root) {
    if (!inst || !inst->initialized || !path) return -1;

    uint8_t parent;
    if (*path == '/') {
        parent = FS_DIR_ROOT;
        path++;
    } else if (from_root) {
        parent = FS_DIR_ROOT;
    } else {
        parent = task_current() ? task_current()->cwd_dir : FS_DIR_ROOT;
    }

    /* "" or "/" alone — caller wants the directory the search is
     * starting from. We don't have an entry index for the root, so
     * return -1 here; the VFS-side rootfs_open handles "" specially. */
    if (!*path) return -1;

    int last = -1;
    while (*path) {
        const char *start = path;
        while (*path && *path != '/') path++;
        int len = (int)(path - start);
        if (len == 0) {
            if (*path) path++;
            continue;
        }
        int found = find_in_dir_inst(inst, parent, start, len);
        if (found < 0) return -1;
        last = found;
        if (*path == '/') {
            if (inst->super.files[found].type != FS_TYPE_DIR) return -1;
            parent = (uint8_t)found;
            path++;
        }
    }
    return last;
}

/* ---- Per-instance read --------------------------------------- */

static int fs_iread_inst(struct fs_instance *inst, int idx,
                         uint32_t offset, void *buf, uint32_t n) {
    if (!inst || !inst->initialized) return -1;
    if (idx < 0 || (uint32_t)idx >= inst->super.file_count) return -1;

    struct fs_entry *e = &inst->super.files[idx];
    if (e->type != FS_TYPE_FILE) return -1;
    if (offset >= e->size) return 0;
    if (offset + n > e->size) n = e->size - offset;

    uint8_t  sec_buf[512];
    uint32_t total = 0;
    uint8_t *out = (uint8_t *)buf;

    while (n > 0) {
        uint32_t abs_off = offset + total;
        uint32_t lba     = inst->base_lba + e->start_sector + (abs_off / 512);
        uint32_t off_in  = abs_off % 512;

        if (inst_read_sector(inst, lba, sec_buf) != 0) return -1;

        uint32_t take = 512 - off_in;
        if (take > n) take = n;
        memcpy(out + total, sec_buf + off_in, take);

        total += take;
        n     -= take;
    }
    return (int)total;
}

/* ---- Per-instance dir iter ----------------------------------- */

static int fs_idir_iter_inst(struct fs_instance *inst, int dir_idx, int *iter) {
    if (!inst || !inst->initialized || !iter) return -1;
    uint8_t parent = (dir_idx < 0) ? FS_DIR_ROOT : (uint8_t)dir_idx;

    for (uint32_t i = (uint32_t)*iter; i < inst->super.file_count; i++) {
        struct fs_entry *e = &inst->super.files[i];
        if (e->type == FS_TYPE_FREE)        continue;
        if (e->parent_dir != parent)        continue;
        *iter = (int)i + 1;
        return (int)i;
    }
    *iter = (int)inst->super.file_count;
    return -1;
}

/* ---- Per-instance mkdir / write -------------------------------- */

static int alloc_slot_inst(struct fs_instance *inst) {
    if (inst->super.file_count >= FS_MAX_FILES) return -1;
    int idx = (int)inst->super.file_count;
    inst->super.file_count++;
    struct fs_entry *e = &inst->super.files[idx];
    for (int i = 0; i < FS_NAME_MAX; i++) e->name[i] = 0;
    e->start_sector = 0;
    e->size         = 0;
    e->type         = FS_TYPE_FREE;
    e->parent_dir   = FS_DIR_ROOT;
    for (int i = 0; i < (int)sizeof(e->reserved); i++) e->reserved[i] = 0;
    return idx;
}

static int split_path_inst(struct fs_instance *inst, const char *path,
                           const char **base_out, int *base_len_out, int from_root) {
    const char *last_slash = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    int parent;
    const char *base;

    if (!last_slash) {
        if (from_root || inst != g_root_inst) {
            parent = (int)FS_DIR_ROOT;
        } else {
            parent = task_current() ? (int)task_current()->cwd_dir : (int)FS_DIR_ROOT;
        }
        base = path;
    } else if (last_slash == path) {
        parent = (int)FS_DIR_ROOT;
        base = path + 1;
    } else {
        char buf[128];
        int n = (int)(last_slash - path);
        if (n >= (int)sizeof(buf)) return -1;
        for (int i = 0; i < n; i++) buf[i] = path[i];
        buf[n] = 0;
        parent = fs_iopen_inst(inst, buf, from_root);
        if (parent < 0) return -1;
        if (inst->super.files[parent].type != FS_TYPE_DIR) return -1;
        base = last_slash + 1;
    }

    int base_len = 0;
    while (base[base_len]) base_len++;
    if (base_len == 0)        return -1;
    if (base_len > FS_NAME_MAX) return -1;

    *base_out = base;
    *base_len_out = base_len;
    return parent;
}

static int fs_imkdir_inst(struct fs_instance *inst, const char *path, int from_root) {
    if (!inst || !inst->initialized || !path) return -1;

    const char *base;
    int base_len;
    int parent = split_path_inst(inst, path, &base, &base_len, from_root);
    if (parent < 0) return -1;

    if (find_in_dir_inst(inst, (uint8_t)parent, base, base_len) >= 0) return -1;

    int idx = alloc_slot_inst(inst);
    if (idx < 0) return -1;

    struct fs_entry *e = &inst->super.files[idx];
    for (int i = 0; i < base_len; i++) e->name[i] = base[i];
    e->type       = FS_TYPE_DIR;
    e->parent_dir = (uint8_t)parent;

    if (fs_write_super_inst(inst) != 0) return -1;
    return idx;
}

static int find_or_create_file_inst(struct fs_instance *inst, const char *path, int from_root) {
    int existing = fs_iopen_inst(inst, path, from_root);
    if (existing >= 0) {
        if (inst->super.files[existing].type != FS_TYPE_FILE) return -1;
        return existing;
    }

    const char *base;
    int base_len;
    int parent = split_path_inst(inst, path, &base, &base_len, from_root);
    if (parent < 0) return -1;

    int idx = alloc_slot_inst(inst);
    if (idx < 0) return -1;

    struct fs_entry *e = &inst->super.files[idx];
    for (int i = 0; i < base_len; i++) e->name[i] = base[i];
    e->type       = FS_TYPE_FILE;
    e->parent_dir = (uint8_t)parent;
    return idx;
}

static int fs_iwrite_all_inst(struct fs_instance *inst, const char *path,
                              const void *data, uint32_t size, int from_root) {
    if (!inst || !inst->initialized || !path) return -1;

    int idx = find_or_create_file_inst(inst, path, from_root);
    if (idx < 0) return -1;

    uint32_t old_start = inst->super.files[idx].start_sector;
    uint32_t old_size  = inst->super.files[idx].size;
    uint32_t old_n     = old_size ? (old_size + 511) / 512 : 0;

    uint32_t needed = (size + 511) / 512;
    if (needed == 0) needed = 1;

    int new_start = bitmap_alloc_run(inst, needed);
    if (new_start < 0) return -1;

    const uint8_t *src = (const uint8_t *)data;
    uint8_t buf[512];
    for (uint32_t s = 0; s < needed; s++) {
        uint32_t off  = s * 512;
        uint32_t take = (off + 512 <= size) ? 512 : (size - off);
        for (int i = 0; i < 512; i++) buf[i] = 0;
        if (take > 0) memcpy(buf, src + off, take);
        if (inst_write_sector(inst, inst->base_lba + (uint32_t)new_start + s, buf) != 0) {
            bitmap_free_run(inst, (uint32_t)new_start, needed);
            return -1;
        }
    }

    inst->super.files[idx].start_sector = (uint32_t)new_start;
    inst->super.files[idx].size         = size;
    if (old_n > 0) bitmap_free_run(inst, old_start, old_n);

    if (fs_write_super_inst(inst) != 0) return -1;
    return 0;
}

/* ---- Boot-instance singleton wrappers ------------------------- */

uint32_t fs_size(int idx)       { return inst_size(g_root_inst, idx); }
const char *fs_name(int idx)    { return inst_name(g_root_inst, idx); }
int fs_count(void)              { return g_root_inst && g_root_inst->initialized
                                       ? (int)g_root_inst->super.file_count : 0; }
uint8_t fs_entry_type(int idx)  { return inst_entry_type(g_root_inst, idx); }
int fs_entry_parent(int idx) {
    if (!g_root_inst || !g_root_inst->initialized || idx < 0 ||
        (uint32_t)idx >= g_root_inst->super.file_count) return -1;
    return (int)g_root_inst->super.files[idx].parent_dir;
}

uint32_t fs_free_sectors(void) {
    if (!g_root_inst || !g_root_inst->initialized) return 0;
    uint32_t free = 0;
    for (uint32_t s = 0; s < g_root_inst->n_sectors; s++) {
        if (!bitmap_get(g_root_inst, s)) free++;
    }
    return free;
}

int fs_open(const char *path) {
    /* Backward-compatible: cwd-relative for top-level callers. */
    return fs_iopen_inst(g_root_inst, path, /*from_root=*/0);
}

int fs_read(int idx, uint32_t offset, void *buf, uint32_t n) {
    return fs_iread_inst(g_root_inst, idx, offset, buf, n);
}

int fs_read_at(void *fs_data, int idx, uint32_t offset, void *buf, uint32_t n) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    return fs_iread_inst(inst, idx, offset, buf, n);
}

int fs_dir_iter(int dir_idx, int *iter) {
    return fs_idir_iter_inst(g_root_inst, dir_idx, iter);
}

int fs_mkdir(const char *path) {
    return fs_imkdir_inst(g_root_inst, path, /*from_root=*/0);
}

int fs_write_all(const char *path, const void *data, uint32_t size) {
    return fs_iwrite_all_inst(g_root_inst, path, data, size, /*from_root=*/0);
}

/* ---- VFS adapter (per-instance, takes fs_data via vfs ops) ---- */

static int rootfs_open(void *fs_data, const char *rel, struct vfs_inode *out) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    if (rel[0] == 0) {
        out->kind    = FD_FS;
        out->obj_idx = (int)FS_DIR_ROOT;
        out->size    = 0;
        out->is_dir  = 1;
        out->fs_data = inst;
        return 0;
    }
    /* Always root-relative for VFS-passed paths. */
    int idx = fs_iopen_inst(inst, rel, /*from_root=*/1);
    if (idx < 0) return -1;
    out->kind    = FD_FS;
    out->obj_idx = idx;
    out->size    = inst_size(inst, idx);
    out->is_dir  = (inst_entry_type(inst, idx) == FS_TYPE_DIR);
    out->fs_data = inst;
    return 0;
}

static int rootfs_read(void *fs_data, struct vfs_inode *ino, uint32_t offset,
                       void *buf, uint32_t n) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    return fs_iread_inst(inst, ino->obj_idx, offset, buf, n);
}

static int rootfs_readdir(void *fs_data, const char *rel, int *iter, char *name_buf) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    int dir_idx;
    if (rel[0] == 0 || (rel[0] == '/' && rel[1] == 0)) {
        dir_idx = -1;
    } else {
        dir_idx = fs_iopen_inst(inst, rel, /*from_root=*/1);
        if (dir_idx < 0)                                      return -1;
        if (inst_entry_type(inst, dir_idx) != FS_TYPE_DIR)    return -1;
    }
    int it = *iter;
    int next = fs_idir_iter_inst(inst, dir_idx, &it);
    *iter = it;
    if (next < 0) return -1;
    const char *nm = inst_name(inst, next);
    if (name_buf) {
        int k;
        for (k = 0; k < FS_NAME_MAX && nm[k]; k++) name_buf[k] = nm[k];
        if (k < FS_NAME_MAX) name_buf[k] = 0;
    }
    return next;
}

static int rootfs_mkdir(void *fs_data, const char *rel) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    return fs_imkdir_inst(inst, rel, /*from_root=*/1);
}

static int rootfs_write_all(void *fs_data, const char *rel,
                            const void *data, uint32_t n) {
    struct fs_instance *inst = (struct fs_instance *)fs_data;
    if (!inst) inst = g_root_inst;
    return fs_iwrite_all_inst(inst, rel, data, n, /*from_root=*/1);
}

/* One ops table per instance — the per-instance .fs_data binds the
 * ops to the right superblock. The boot fs gets g_root_ops which
 * has fs_data = NULL → resolved to g_root_inst at dispatch. */
static struct vfs_fs_ops g_root_ops = {
    .fs_data   = 0,
    .open      = rootfs_open,
    .read      = rootfs_read,
    .readdir   = rootfs_readdir,
    .mkdir     = rootfs_mkdir,
    .write_all = rootfs_write_all,
};

static struct vfs_fs_ops g_inst_ops[FS_INSTANCE_MAX];

struct vfs_fs_ops *fs_rootfs_ops(void) { return &g_root_ops; }

struct vfs_fs_ops *fs_make_ops_for(struct fs_instance *inst) {
    if (!inst) return 0;
    int idx = (int)(inst - &g_instances[0]);
    if (idx < 0 || idx >= FS_INSTANCE_MAX) return 0;
    g_inst_ops[idx] = (struct vfs_fs_ops){
        .fs_data   = inst,
        .open      = rootfs_open,
        .read      = rootfs_read,
        .readdir   = rootfs_readdir,
        .mkdir     = rootfs_mkdir,
        .write_all = rootfs_write_all,
    };
    return &g_inst_ops[idx];
}
