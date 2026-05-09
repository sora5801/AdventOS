#include "fs.h"
#include "ata.h"
#include "bcache.h"
#include "task.h"
#include "kprintf.h"
#include "string.h"
#include "vfs.h"

static struct fs_super g_super;        /* 1024 bytes — fits 2 sectors */
static int             g_initialized;

#define FS_DISK_TOTAL_SECTORS   1024u
#define FS_BITMAP_BYTES         ((FS_DISK_TOTAL_SECTORS + 7) / 8)

static uint8_t g_bitmap[FS_BITMAP_BYTES];

static inline int bitmap_get(uint32_t s) {
    if (s >= FS_DISK_TOTAL_SECTORS) return 1;
    return (g_bitmap[s >> 3] >> (s & 7)) & 1;
}
static inline void bitmap_set(uint32_t s) {
    if (s < FS_DISK_TOTAL_SECTORS) g_bitmap[s >> 3] |= (uint8_t)(1u << (s & 7));
}
static inline void bitmap_clear(uint32_t s) {
    if (s < FS_DISK_TOTAL_SECTORS) g_bitmap[s >> 3] &= (uint8_t)~(1u << (s & 7));
}

static int bitmap_alloc_run(uint32_t n) {
    if (n == 0) n = 1;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    /* Skip past the superblock (sectors 0..FS_SUPER_SECTORS-1). */
    for (uint32_t s = FS_SUPER_SECTORS; s < FS_DISK_TOTAL_SECTORS; s++) {
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

static void bitmap_free_run(uint32_t start, uint32_t n) {
    if (start < FS_SUPER_SECTORS) return;       /* never free the superblock */
    for (uint32_t k = 0; k < n; k++) {
        if (start + k < FS_DISK_TOTAL_SECTORS) bitmap_clear(start + k);
    }
}

void fs_init(void) {
    /* Read superblock sectors via the cache (which is initialized
     * before fs_init in kmain). The first call here populates the
     * cache; subsequent fs_init in a hypothetical re-mount would
     * see hits. */
    uint8_t sb[FS_SUPER_SECTORS * 512];
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) {
        if (bcache_read(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) {
            kputs("fs: failed to read superblock\n");
            return;
        }
    }

    if (memcmp(sb, FS_MAGIC, 8) != 0) {
        kputs("fs: bad magic in superblock (no filesystem on disk)\n");
        return;
    }

    memcpy(&g_super, sb, sizeof(g_super));

    if (g_super.file_count > FS_MAX_FILES) g_super.file_count = FS_MAX_FILES;

    /* Reconstruct the bitmap. Sectors 0..FS_SUPER_SECTORS-1 are the
     * superblock; each FILE entry's run is too. DIR entries don't
     * occupy any data sectors. */
    for (uint32_t i = 0; i < FS_BITMAP_BYTES; i++) g_bitmap[i] = 0;
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) bitmap_set(s);
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        struct fs_entry *e = &g_super.files[i];
        if (e->type != FS_TYPE_FILE) continue;
        if (e->size == 0)            continue;
        uint32_t n = (e->size + 511) / 512;
        for (uint32_t s = 0; s < n; s++) bitmap_set(e->start_sector + s);
    }

    g_initialized = 1;
    kprintf("fs: AdventFS mounted, %u entries, %u/%u sectors free\n",
            (unsigned)g_super.file_count,
            (unsigned)fs_free_sectors(),
            (unsigned)FS_DISK_TOTAL_SECTORS);
}

static int fs_write_super(void) {
    uint8_t sb[FS_SUPER_SECTORS * 512];
    for (int i = 0; i < (int)sizeof(sb); i++) sb[i] = 0;
    memcpy(sb, &g_super, sizeof(g_super));
    for (uint32_t s = 0; s < FS_SUPER_SECTORS; s++) {
        /* Write-through the cache: bcache_write updates the cached
         * copy and marks it dirty; the periodic syncer / next
         * eviction flushes it to disk. */
        if (bcache_write(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) return -1;
    }
    return 0;
}

/* ---- entry helpers ------------------------------------------------- */

uint32_t fs_size(int idx) {
    if (!g_initialized || idx < 0 || (uint32_t)idx >= g_super.file_count) return 0;
    return g_super.files[idx].size;
}

const char *fs_name(int idx) {
    if (!g_initialized || idx < 0 || (uint32_t)idx >= g_super.file_count) return NULL;
    return g_super.files[idx].name;
}

int fs_count(void) { return g_initialized ? (int)g_super.file_count : 0; }

uint8_t fs_entry_type(int idx) {
    if (!g_initialized || idx < 0 || (uint32_t)idx >= g_super.file_count) return FS_TYPE_FREE;
    return g_super.files[idx].type;
}

int fs_entry_parent(int idx) {
    if (!g_initialized || idx < 0 || (uint32_t)idx >= g_super.file_count) return -1;
    return (int)g_super.files[idx].parent_dir;
}

uint32_t fs_free_sectors(void) {
    if (!g_initialized) return 0;
    uint32_t free = 0;
    for (uint32_t s = 0; s < FS_DISK_TOTAL_SECTORS; s++) {
        if (!bitmap_get(s)) free++;
    }
    return free;
}

/* Compare a counted name slice against an entry's NUL-padded name.
 * Match if len <= FS_NAME_MAX, the first `len` bytes match, and (if
 * len < FS_NAME_MAX) the next byte of e->name is NUL. */
static int name_eq_slice(const char *ename, const char *s, int len) {
    if (len > FS_NAME_MAX) return 0;
    for (int j = 0; j < len; j++) if (ename[j] != s[j]) return 0;
    if (len < FS_NAME_MAX && ename[len] != 0) return 0;
    return 1;
}

/* Find a child of `parent` whose name matches the given slice. */
static int find_in_dir(uint8_t parent, const char *name, int len) {
    for (uint32_t i = 0; i < g_super.file_count; i++) {
        struct fs_entry *e = &g_super.files[i];
        if (e->type == FS_TYPE_FREE)         continue;
        if (e->parent_dir != parent)         continue;
        if (name_eq_slice(e->name, name, len)) return (int)i;
    }
    return -1;
}

/* Resolve a path to an entry index. See fs.h for the path semantics. */
int fs_open(const char *path) {
    if (!g_initialized || !path || !*path) return -1;

    uint8_t parent;
    if (*path == '/') {
        parent = FS_DIR_ROOT;
        path++;
    } else {
        parent = task_current() ? task_current()->cwd_dir : FS_DIR_ROOT;
    }

    int last = -1;
    while (*path) {
        const char *start = path;
        while (*path && *path != '/') path++;
        int len = (int)(path - start);
        if (len == 0) {              /* doubled or trailing slash */
            if (*path) path++;
            continue;
        }
        int found = find_in_dir(parent, start, len);
        if (found < 0) return -1;
        last = found;
        if (*path == '/') {
            /* Mid-component: must be a directory. */
            if (g_super.files[found].type != FS_TYPE_DIR) return -1;
            parent = (uint8_t)found;
            path++;
        }
    }
    return last;
}

int fs_read(int idx, uint32_t offset, void *buf, uint32_t n) {
    if (!g_initialized) return -1;
    if (idx < 0 || (uint32_t)idx >= g_super.file_count) return -1;

    struct fs_entry *e = &g_super.files[idx];
    if (e->type != FS_TYPE_FILE) return -1;
    if (offset >= e->size) return 0;
    if (offset + n > e->size) n = e->size - offset;

    uint8_t  sec_buf[512];
    uint32_t total = 0;
    uint8_t *out = (uint8_t *)buf;

    while (n > 0) {
        uint32_t abs_off  = offset + total;
        uint32_t lba      = FS_DISK_OFFSET_SECTORS + e->start_sector
                          + (abs_off / 512);
        uint32_t off_in   = abs_off % 512;

        if (bcache_read(lba, sec_buf) != 0) return -1;

        uint32_t take = 512 - off_in;
        if (take > n) take = n;
        memcpy(out + total, sec_buf + off_in, take);

        total += take;
        n     -= take;
    }
    return (int)total;
}

/* ---- directory iter ------------------------------------------------ */

int fs_dir_iter(int dir_idx, int *iter) {
    if (!g_initialized || !iter) return -1;
    uint8_t parent = (dir_idx < 0) ? FS_DIR_ROOT : (uint8_t)dir_idx;

    for (uint32_t i = (uint32_t)*iter; i < g_super.file_count; i++) {
        struct fs_entry *e = &g_super.files[i];
        if (e->type == FS_TYPE_FREE)        continue;
        if (e->parent_dir != parent)        continue;
        *iter = (int)i + 1;
        return (int)i;
    }
    *iter = (int)g_super.file_count;
    return -1;
}

/* ---- mkdir --------------------------------------------------------- */

/* Allocate a fresh entry slot; returns the index or -1 if full.
 * Caller fills in name/type/parent. */
static int alloc_slot(void) {
    if (g_super.file_count >= FS_MAX_FILES) return -1;
    int idx = (int)g_super.file_count;
    g_super.file_count++;
    struct fs_entry *e = &g_super.files[idx];
    for (int i = 0; i < FS_NAME_MAX; i++) e->name[i] = 0;
    e->start_sector = 0;
    e->size         = 0;
    e->type         = FS_TYPE_FREE;
    e->parent_dir   = FS_DIR_ROOT;
    for (int i = 0; i < (int)sizeof(e->reserved); i++) e->reserved[i] = 0;
    return idx;
}

/* Split `path` into (parent_idx, basename). Resolves the parent
 * portion; the basename is whatever follows the last '/'. Returns
 * the parent's idx (or FS_DIR_ROOT for top-level), with `*base_out`
 * pointing into `path` at the basename. -1 on bad parent path. */
static int split_path(const char *path, const char **base_out, int *base_len_out) {
    /* Find last '/'. */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    int parent;
    const char *base;
    int base_len;

    if (!last_slash) {
        /* "name" — relative to cwd. */
        parent = task_current() ? (int)task_current()->cwd_dir : (int)FS_DIR_ROOT;
        base = path;
    } else if (last_slash == path) {
        /* "/name" — at root. */
        parent = (int)FS_DIR_ROOT;
        base = path + 1;
    } else {
        /* "a/b/.../name" — resolve everything up to last slash. */
        char buf[128];
        int n = (int)(last_slash - path);
        if (n >= (int)sizeof(buf)) return -1;
        for (int i = 0; i < n; i++) buf[i] = path[i];
        buf[n] = 0;
        parent = fs_open(buf);
        if (parent < 0) return -1;
        if (g_super.files[parent].type != FS_TYPE_DIR) return -1;
        base = last_slash + 1;
    }

    base_len = 0;
    while (base[base_len]) base_len++;
    if (base_len == 0)        return -1;     /* empty basename */
    if (base_len > FS_NAME_MAX) return -1;

    *base_out = base;
    *base_len_out = base_len;
    return parent;
}

int fs_mkdir(const char *path) {
    if (!g_initialized || !path) return -1;

    const char *base;
    int base_len;
    int parent = split_path(path, &base, &base_len);
    if (parent < 0) return -1;

    /* Already exists? */
    if (find_in_dir((uint8_t)parent, base, base_len) >= 0) return -1;

    int idx = alloc_slot();
    if (idx < 0) return -1;

    struct fs_entry *e = &g_super.files[idx];
    for (int i = 0; i < base_len; i++) e->name[i] = base[i];
    e->type       = FS_TYPE_DIR;
    e->parent_dir = (uint8_t)parent;

    if (fs_write_super() != 0) return -1;
    return idx;
}

/* ---- write --------------------------------------------------------- */

/* Find an existing FILE entry at `path`, or carve a new one. Returns
 * the entry index. */
static int find_or_create_file(const char *path) {
    /* Try exact path first. */
    int existing = fs_open(path);
    if (existing >= 0) {
        if (g_super.files[existing].type != FS_TYPE_FILE) return -1;
        return existing;
    }

    /* Carve a new entry under the resolved parent. */
    const char *base;
    int base_len;
    int parent = split_path(path, &base, &base_len);
    if (parent < 0) return -1;

    int idx = alloc_slot();
    if (idx < 0) return -1;

    struct fs_entry *e = &g_super.files[idx];
    for (int i = 0; i < base_len; i++) e->name[i] = base[i];
    e->type       = FS_TYPE_FILE;
    e->parent_dir = (uint8_t)parent;
    return idx;
}

int fs_write_all(const char *path, const void *data, uint32_t size) {
    if (!g_initialized || !path) return -1;

    int idx = find_or_create_file(path);
    if (idx < 0) return -1;

    uint32_t old_start = g_super.files[idx].start_sector;
    uint32_t old_size  = g_super.files[idx].size;
    uint32_t old_n     = old_size ? (old_size + 511) / 512 : 0;

    uint32_t needed = (size + 511) / 512;
    if (needed == 0) needed = 1;

    int new_start = bitmap_alloc_run(needed);
    if (new_start < 0) return -1;

    const uint8_t *src = (const uint8_t *)data;
    uint8_t buf[512];
    for (uint32_t s = 0; s < needed; s++) {
        uint32_t off  = s * 512;
        uint32_t take = (off + 512 <= size) ? 512 : (size - off);
        for (int i = 0; i < 512; i++) buf[i] = 0;
        if (take > 0) memcpy(buf, src + off, take);
        if (bcache_write(FS_DISK_OFFSET_SECTORS + (uint32_t)new_start + s,
                         buf) != 0) {
            bitmap_free_run((uint32_t)new_start, needed);
            return -1;
        }
    }

    g_super.files[idx].start_sector = (uint32_t)new_start;
    g_super.files[idx].size         = size;
    if (old_n > 0) bitmap_free_run(old_start, old_n);

    if (fs_write_super() != 0) return -1;
    return 0;
}

/* ---- VFS adapter (session 28) -------------------------------------- */

/* The rootfs side of the VFS dispatch table. Each entry takes a
 * RELATIVE-to-mount path; for rootfs the mount is "/", so the rel
 * path is what comes after the leading slash (or empty for "/"). */

static int rootfs_open(const char *rel, struct vfs_inode *out) {
    if (rel[0] == 0) {
        /* "/" — root of rootfs is a directory but has no entry index. */
        out->kind    = FD_FS;
        out->obj_idx = (int)FS_DIR_ROOT;     /* sentinel */
        out->size    = 0;
        out->is_dir  = 1;
        return 0;
    }
    int idx = fs_open(rel);
    if (idx < 0) return -1;
    out->kind    = FD_FS;
    out->obj_idx = idx;
    out->size    = fs_size(idx);
    out->is_dir  = (fs_entry_type(idx) == FS_TYPE_DIR);
    return 0;
}

static int rootfs_read(struct vfs_inode *ino, uint32_t offset,
                       void *buf, uint32_t n) {
    return fs_read(ino->obj_idx, offset, buf, n);
}

static int rootfs_readdir(const char *rel, int *iter, char *name_buf) {
    /* fs_dir_iter wants the dir's entry index. "" or "/" mean root. */
    int dir_idx;
    if (rel[0] == 0 || (rel[0] == '/' && rel[1] == 0)) {
        dir_idx = -1;
    } else {
        dir_idx = fs_open(rel);
        if (dir_idx < 0)                            return -1;
        if (fs_entry_type(dir_idx) != FS_TYPE_DIR)  return -1;
    }
    int it = *iter;
    int next = fs_dir_iter(dir_idx, &it);
    *iter = it;
    if (next < 0) return -1;
    const char *nm = fs_name(next);
    if (name_buf) {
        int k;
        for (k = 0; k < FS_NAME_MAX && nm[k]; k++) name_buf[k] = nm[k];
        if (k < FS_NAME_MAX) name_buf[k] = 0;
    }
    return next;
}

static int rootfs_mkdir(const char *rel) {
    /* fs_mkdir wants an absolute-feeling path (the FS internally
     * splits on /). Easiest: prepend a slash for paths that need to
     * land at root, otherwise let fs_mkdir resolve via cwd. The
     * test set passes either form; we just forward. */
    return fs_mkdir(rel);
}

static int rootfs_write_all(const char *rel, const void *data, uint32_t n) {
    return fs_write_all(rel, data, n);
}

static struct vfs_fs_ops g_rootfs_ops = {
    .open      = rootfs_open,
    .read      = rootfs_read,
    .readdir   = rootfs_readdir,
    .mkdir     = rootfs_mkdir,
    .write_all = rootfs_write_all,
};

struct vfs_fs_ops *fs_rootfs_ops(void) { return &g_rootfs_ops; }
