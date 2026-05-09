#include "vfs.h"
#include "kprintf.h"
#include "string.h"
#include "task.h"

/*
 * Path-dispatch only. The VFS doesn't allocate inodes on the heap;
 * vfs_inode is value-typed and copied through fs ops on demand. fs
 * implementations stuff their internal handle into obj_idx and read
 * it back when sys_read calls per-fs read paths.
 */

struct vfs_mount {
    int                 in_use;
    char                mount_point[16];   /* e.g. "/", "/proc" */
    char                fs_name[16];       /* "rootfs", "procfs" */
    struct vfs_fs_ops  *ops;
};

#define MOUNT_PHASE  0x10000     /* iter sentinel: switch to mount-name phase */

static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        g_mounts[i].in_use = 0;
    }
}

int vfs_mount(const char *mount_point, const char *fs_name,
              struct vfs_fs_ops *ops) {
    if (!mount_point || !fs_name || !ops) return -1;

    /* Reject duplicate mount points. */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use &&
            strcmp(g_mounts[i].mount_point, mount_point) == 0) return -1;
    }
    /* Find a free slot. */
    int idx = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return -1;

    struct vfs_mount *m = &g_mounts[idx];
    m->in_use = 1;
    strncpy(m->mount_point, mount_point, sizeof(m->mount_point) - 1);
    m->mount_point[sizeof(m->mount_point) - 1] = 0;
    strncpy(m->fs_name, fs_name, sizeof(m->fs_name) - 1);
    m->fs_name[sizeof(m->fs_name) - 1] = 0;
    m->ops = ops;

    kprintf("vfs: mounted '%s' at %s\n", fs_name, mount_point);
    return 0;
}

/* Find the rootfs (mount at "/"). Returns NULL if it isn't mounted
 * yet — early kernel boot before vfs_init or before the rootfs
 * mount is registered. Callers that hit a NULL must fail gracefully. */
static struct vfs_mount *find_rootfs(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use &&
            g_mounts[i].mount_point[0] == '/' &&
            g_mounts[i].mount_point[1] == 0) return &g_mounts[i];
    }
    return 0;
}

/* Find the longest mount-point prefix of `path`. Always-absolute
 * path expected. Returns the matching mount + sets *rel_out to
 * the relative-to-mount path. The rootfs ("/") is the catch-all
 * if nothing more specific matches. */
static struct vfs_mount *resolve(const char *path, const char **rel_out) {
    if (!path || path[0] != '/') return 0;        /* caller filters relative */

    struct vfs_mount *best = find_rootfs();
    int               best_len = 1;               /* "/" */

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_mount *m = &g_mounts[i];
        if (!m->in_use) continue;
        int mlen = (int)strlen(m->mount_point);
        if (mlen <= best_len) continue;            /* not longer than current best */
        if (strncmp(path, m->mount_point, mlen) != 0) continue;
        char follow = path[mlen];
        if (follow != 0 && follow != '/') continue; /* /proc shouldn't match /procxyz */
        best     = m;
        best_len = mlen;
    }

    if (!best) return 0;
    /* Strip prefix. After "/proc" → "" or "/foo"; after "/" → "etc/inittab" etc.
     * We always advance past the slash that follows the prefix when
     * present so the relative path doesn't have a leading "/". */
    const char *rel = path + best_len;
    if (*rel == '/') rel++;
    *rel_out = rel;
    return best;
}

int vfs_open(const char *path, struct vfs_inode *out) {
    if (!path || !out) return -1;

    /* Relative path → always rootfs (no concept of cwd-on-procfs yet). */
    if (path[0] != '/') {
        struct vfs_mount *r = find_rootfs();
        if (!r) return -1;
        return r->ops->open(path, out);
    }

    /* "/" by itself is the rootfs root. */
    if (path[0] == '/' && path[1] == 0) {
        struct vfs_mount *r = find_rootfs();
        if (!r) return -1;
        return r->ops->open("", out);
    }

    const char *rel;
    struct vfs_mount *m = resolve(path, &rel);
    if (!m) return -1;
    return m->ops->open(rel, out);
}

int vfs_read(struct vfs_inode *ino, uint32_t offset,
             void *buf, uint32_t n) {
    /* The fd entry has kind==ino->kind; syscall.c uses that kind to
     * decide which read path to call. This vfs_read is provided for
     * symmetry / future-proofing — currently no syscall calls it. */
    (void)ino; (void)offset; (void)buf; (void)n;
    return -1;
}

int vfs_readdir(const char *path, int *iter, char *name_buf) {
    if (!path || !iter || !name_buf) return -1;

    /* Relative paths use rootfs. */
    if (path[0] != '/') {
        struct vfs_mount *r = find_rootfs();
        if (!r || !r->ops->readdir) return -1;
        return r->ops->readdir(path, iter, name_buf);
    }

    const char *rel;
    struct vfs_mount *m = resolve(path, &rel);
    if (!m || !m->ops->readdir) return -1;

    /* For everything other than "/", just delegate to the fs. */
    int is_root_listing =
        (path[0] == '/' && (path[1] == 0 || (path[1] == '/' && path[2] == 0)));
    if (!is_root_listing) {
        return m->ops->readdir(rel, iter, name_buf);
    }

    /* Root listing: first emit rootfs entries, then synthetic
     * mount-point names so that `ls /` shows /proc alongside the
     * regular files. The transition is signaled by *iter crossing
     * MOUNT_PHASE. */
    if (*iter < MOUNT_PHASE) {
        int idx = m->ops->readdir(rel, iter, name_buf);
        if (idx >= 0) return idx;
        /* Fell through end-of-rootfs: switch to mount phase. */
        *iter = MOUNT_PHASE;
    }

    int mi = *iter - MOUNT_PHASE;
    while (mi < VFS_MAX_MOUNTS) {
        struct vfs_mount *mm = &g_mounts[mi];
        if (mm->in_use &&
            mm->mount_point[0] == '/' && mm->mount_point[1] != 0 &&
            /* skip multi-segment mounts; we only surface top-level ones */
            !strchr(mm->mount_point + 1, '/')) {
            const char *p = &mm->mount_point[1];   /* skip leading "/" */
            int j = 0;
            while (p[j] && j < 15) { name_buf[j] = p[j]; j++; }
            if (j < 16) name_buf[j] = 0;
            *iter = MOUNT_PHASE + mi + 1;
            return 0x10000 | mi;                    /* synthetic idx */
        }
        mi++;
    }
    *iter = MOUNT_PHASE + VFS_MAX_MOUNTS;
    return -1;
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;

    if (path[0] != '/') {
        struct vfs_mount *r = find_rootfs();
        if (!r || !r->ops->mkdir) return -1;
        return r->ops->mkdir(path);
    }

    const char *rel;
    struct vfs_mount *m = resolve(path, &rel);
    if (!m || !m->ops->mkdir) return -1;
    return m->ops->mkdir(rel);
}

int vfs_write_all(const char *path, const void *data, uint32_t n) {
    if (!path) return -1;

    if (path[0] != '/') {
        struct vfs_mount *r = find_rootfs();
        if (!r || !r->ops->write_all) return -1;
        return r->ops->write_all(path, data, n);
    }

    const char *rel;
    struct vfs_mount *m = resolve(path, &rel);
    if (!m || !m->ops->write_all) return -1;
    return m->ops->write_all(rel, data, n);
}

int vfs_describe_mounts(char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    int o = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_mount *m = &g_mounts[i];
        if (!m->in_use) continue;
        const char *fields[] = { m->mount_point, " ", m->fs_name, "\n" };
        for (int f = 0; f < 4; f++) {
            const char *s = fields[f];
            while (*s && o < cap - 1) buf[o++] = *s++;
        }
    }
    if (o < cap) buf[o] = 0;
    return o;
}
