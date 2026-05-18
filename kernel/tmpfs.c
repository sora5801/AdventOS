#include "tmpfs.h"
#include "kmalloc.h"
#include "string.h"

struct tmpfile {
    int       in_use;
    int       refs;
    char      name[FS_NAME_MAX + 1];
    uint8_t  *data;
    uint32_t  size;
    uint32_t  cap;
};

static struct tmpfile g_tmp[TMPFS_MAX];

void tmpfs_init(void) {
    for (int i = 0; i < TMPFS_MAX; i++) {
        g_tmp[i].in_use = 0;
        g_tmp[i].data   = NULL;
    }
}

static int valid(int idx) {
    return idx >= 0 && idx < TMPFS_MAX && g_tmp[idx].in_use;
}

static int find_by_name(const char *name) {
    for (int i = 0; i < TMPFS_MAX; i++) {
        if (g_tmp[i].in_use && strcmp(g_tmp[i].name, name) == 0) return i;
    }
    return -1;
}

int tmpfs_open(const char *name) {
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    g_tmp[idx].refs++;
    return idx;
}

int tmpfs_create(const char *name) {
    /* If it already exists, truncate (POSIX `>` semantics). */
    int idx = find_by_name(name);
    if (idx >= 0) {
        g_tmp[idx].size = 0;
        g_tmp[idx].refs++;
        return idx;
    }

    /* Otherwise allocate a fresh slot. */
    for (int i = 0; i < TMPFS_MAX; i++) {
        if (!g_tmp[i].in_use) {
            g_tmp[i].in_use = 1;
            g_tmp[i].refs   = 1;
            g_tmp[i].size   = 0;
            g_tmp[i].cap    = TMPFS_INITIAL_CAP;
            g_tmp[i].data   = (uint8_t *)kmalloc(TMPFS_INITIAL_CAP);
            if (!g_tmp[i].data) {
                g_tmp[i].in_use = 0;
                return -1;
            }
            int j;
            for (j = 0; j < FS_NAME_MAX && name[j]; j++) {
                g_tmp[i].name[j] = name[j];
            }
            g_tmp[i].name[j] = 0;
            return i;
        }
    }
    return -1;
}

int tmpfs_create_append(const char *name) {
    /* Existing file: keep size + data, just bump refs. */
    int idx = find_by_name(name);
    if (idx >= 0) {
        g_tmp[idx].refs++;
        return idx;
    }
    /* Missing: behave exactly like tmpfs_create (fresh empty file). */
    return tmpfs_create(name);
}

void tmpfs_close(int idx) {
    if (!valid(idx)) return;
    if (g_tmp[idx].refs > 0) g_tmp[idx].refs--;
    /* We intentionally KEEP the data after refs hits zero so a later
     * `cat <name>` can still read it. A real OS would have an unlink
     * syscall to free the slot; AdventOS reclaims tmpfs only at reboot. */
}

void tmpfs_inc_ref(int idx) {
    if (valid(idx)) g_tmp[idx].refs++;
}

int tmpfs_read(int idx, uint32_t offset, void *buf, uint32_t n) {
    if (!valid(idx)) return -1;
    struct tmpfile *f = &g_tmp[idx];
    if (offset >= f->size) return 0;
    uint32_t avail = f->size - offset;
    if (n > avail) n = avail;
    memcpy(buf, f->data + offset, n);
    return (int)n;
}

int tmpfs_write(int idx, const void *buf, uint32_t n) {
    if (!valid(idx)) return -1;
    struct tmpfile *f = &g_tmp[idx];

    /* Grow the backing buffer if needed. Doubling avoids quadratic
     * growth for repeated small writes. */
    if (f->size + n > f->cap) {
        uint32_t new_cap = f->cap;
        while (new_cap < f->size + n) new_cap *= 2;
        uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
        if (!new_data) return -1;
        memcpy(new_data, f->data, f->size);
        kfree(f->data);
        f->data = new_data;
        f->cap  = new_cap;
    }

    memcpy(f->data + f->size, buf, n);
    f->size += n;
    return (int)n;
}

uint32_t    tmpfs_size(int idx) { return valid(idx) ? g_tmp[idx].size : 0;     }
const char *tmpfs_name(int idx) { return valid(idx) ? g_tmp[idx].name : NULL;  }
