#include "procfs.h"
#include "vfs.h"
#include "task.h"
#include "pmm.h"
#include "pit.h"
#include "bcache.h"
#include "string.h"
#include "kprintf.h"
#include "syscall.h"

/*
 * /proc filesystem — synthesizes file content from live kernel state
 * on every read. There are no inodes pre-allocated; each open
 * captures the (kind, pid) pair into vfs_inode.obj_idx and each read
 * regenerates the matching content into a small kernel buffer.
 *
 * obj_idx encoding:
 *   bits 31..16 = kind (PNODE_*)
 *   bits 15..0  = pid for per-pid nodes, 0 otherwise
 *
 * The 16-bit pid limit is fine: TASK_MAX is 16, ids fit easily.
 */

#define PROC_KIND(id)        ((unsigned)(id) >> 16)
#define PROC_PID(id)         ((unsigned)(id) & 0xFFFF)
#define PROC_MAKE(k, p)      ((int)((((unsigned)(k)) << 16) | ((p) & 0xFFFF)))

enum {
    PNODE_NONE        = 0,
    PNODE_ROOT_DIR    = 1,
    PNODE_CPUINFO     = 2,
    PNODE_MEMINFO     = 3,
    PNODE_UPTIME      = 4,
    PNODE_VERSION     = 5,
    PNODE_MOUNTS      = 6,
    PNODE_BCACHE      = 7,
    PNODE_PID_DIR     = 8,
    PNODE_PID_STATUS  = 9,
    PNODE_PID_SANDBOX = 10,    /* session 70 */
    PNODE_PID_LIMITS  = 11,    /* session 71 */
};

/* Static top-level files (visible in `ls /proc`). The pid-numbered
 * subdirectories are appended dynamically by readdir. */
static const struct {
    const char *name;
    int         kind;
} g_static_files[] = {
    { "cpuinfo", PNODE_CPUINFO },
    { "meminfo", PNODE_MEMINFO },
    { "uptime",  PNODE_UPTIME  },
    { "version", PNODE_VERSION },
    { "mounts",  PNODE_MOUNTS  },
    { "bcache",  PNODE_BCACHE  },
    { 0, 0 }
};

/* ---- string-builder helpers --------------------------------------- */

static void sb_str(char *buf, int *o, int cap, const char *s) {
    while (*s && *o < cap - 1) buf[(*o)++] = *s++;
}

static void sb_dec(char *buf, int *o, int cap, uint32_t v) {
    char tmp[12];
    int  ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti > 0 && *o < cap - 1) buf[(*o)++] = tmp[--ti];
}

static void sb_pad_dec(char *buf, int *o, int cap, uint32_t v, int width) {
    char tmp[12];
    int  ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti < width && *o < cap - 1) { buf[(*o)++] = ' '; width--; }
    while (ti > 0 && *o < cap - 1) buf[(*o)++] = tmp[--ti];
}

/* 8-hex-digit, zero-padded — used by the sandbox node for mask dumps. */
static void sb_hex32(char *buf, int *o, int cap, uint32_t v) {
    static const char d[] = "0123456789abcdef";
    for (int i = 7; i >= 0 && *o < cap - 1; i--) {
        buf[(*o)++] = d[(v >> (i * 4)) & 0xF];
    }
}

/* ---- generators ---------------------------------------------------- */

static int gen_cpuinfo(char *buf, int cap) {
    int o = 0;

    /* CPUID leaf 0: vendor in EBX:EDX:ECX. Reading that under -m32
     * is straightforward — the inline asm directly clobbers a/b/c/d. */
    char vendor[13];
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(0));
    *(uint32_t *)(vendor + 0) = b;
    *(uint32_t *)(vendor + 4) = d;
    *(uint32_t *)(vendor + 8) = c;
    vendor[12] = 0;

    sb_str(buf, &o, cap, "vendor_id  : "); sb_str(buf, &o, cap, vendor);
    sb_str(buf, &o, cap, "\n");

    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(1));
    int family    = (int)((a >> 8)  & 0xF);
    int model     = (int)((a >> 4)  & 0xF);
    int stepping  = (int)( a        & 0xF);
    sb_str(buf, &o, cap, "family     : "); sb_dec(buf, &o, cap, family);
    sb_str(buf, &o, cap, "\nmodel      : "); sb_dec(buf, &o, cap, model);
    sb_str(buf, &o, cap, "\nstepping   : "); sb_dec(buf, &o, cap, stepping);
    sb_str(buf, &o, cap, "\nfeatures   :");
    if (d & (1u << 0))  sb_str(buf, &o, cap, " fpu");
    if (d & (1u << 4))  sb_str(buf, &o, cap, " tsc");
    if (d & (1u << 5))  sb_str(buf, &o, cap, " msr");
    if (d & (1u << 6))  sb_str(buf, &o, cap, " pae");
    if (d & (1u << 8))  sb_str(buf, &o, cap, " cx8");
    if (d & (1u << 9))  sb_str(buf, &o, cap, " apic");
    if (d & (1u << 13)) sb_str(buf, &o, cap, " pge");
    if (d & (1u << 23)) sb_str(buf, &o, cap, " mmx");
    if (d & (1u << 25)) sb_str(buf, &o, cap, " sse");
    if (d & (1u << 26)) sb_str(buf, &o, cap, " sse2");
    sb_str(buf, &o, cap, "\n");
    return o;
}

static int gen_meminfo(char *buf, int cap) {
    int o = 0;
    uint32_t total_kb = pmm_total_pages() * 4;
    uint32_t used_kb  = pmm_used_pages()  * 4;
    uint32_t free_kb  = pmm_free_pages()  * 4;
    sb_str(buf, &o, cap, "MemTotal:  "); sb_pad_dec(buf, &o, cap, total_kb, 8);
    sb_str(buf, &o, cap, " kB\nMemUsed:   "); sb_pad_dec(buf, &o, cap, used_kb, 8);
    sb_str(buf, &o, cap, " kB\nMemFree:   "); sb_pad_dec(buf, &o, cap, free_kb, 8);
    sb_str(buf, &o, cap, " kB\n");
    return o;
}

static int gen_uptime(char *buf, int cap) {
    int o = 0;
    uint32_t s    = pit_seconds();
    uint32_t ticks = pit_ticks() % 100;        /* PIT runs at 100 Hz */
    sb_dec(buf, &o, cap, s);
    sb_str(buf, &o, cap, ".");
    /* zero-pad the centisecond fraction to 2 digits */
    if (ticks < 10) sb_str(buf, &o, cap, "0");
    sb_dec(buf, &o, cap, ticks);
    sb_str(buf, &o, cap, "\n");
    return o;
}

static int gen_version(char *buf, int cap) {
    int o = 0;
    sb_str(buf, &o, cap,
           "AdventOS v0.1 (i386, session 28) "
           "VFS + procfs over hierarchical AdventFS + bcache\n");
    return o;
}

static int gen_mounts(char *buf, int cap) {
    return vfs_describe_mounts(buf, cap);
}

static int gen_bcache(char *buf, int cap) {
    int o = 0;
    sb_str(buf, &o, cap, "hits         "); sb_dec(buf, &o, cap, bcache_hits());
    sb_str(buf, &o, cap, "\nmisses       "); sb_dec(buf, &o, cap, bcache_misses());
    sb_str(buf, &o, cap, "\nlogical_w    "); sb_dec(buf, &o, cap, bcache_logical_writes());
    sb_str(buf, &o, cap, "\ndisk_w       "); sb_dec(buf, &o, cap, bcache_disk_writes());
    sb_str(buf, &o, cap, "\ndirty        "); sb_dec(buf, &o, cap, bcache_dirty());
    sb_str(buf, &o, cap, "\n");
    return o;
}

static int gen_status(int pid, char *buf, int cap) {
    /* Find the task by id. We walk task_at() over all slots so that
     * tasks not at slot==id (which is most of them) are still found. */
    struct task *t = 0;
    for (uint32_t i = 0; i < 16; i++) {
        struct task *tt = task_at(i);
        if (tt && (int)tt->id == pid) { t = tt; break; }
    }
    if (!t) return 0;

    int o = 0;
    sb_str(buf, &o, cap, "Name:    "); sb_str(buf, &o, cap, t->name);
    sb_str(buf, &o, cap, "\nPid:     "); sb_dec(buf, &o, cap, t->id);
    sb_str(buf, &o, cap, "\nState:   "); sb_str(buf, &o, cap, task_state_name(t->state));
    sb_str(buf, &o, cap, "\nPPid:    "); sb_dec(buf, &o, cap, t->parent_id);
    sb_str(buf, &o, cap, "\nPgid:    "); sb_dec(buf, &o, cap, t->pgid);
    sb_str(buf, &o, cap, "\nSid:     "); sb_dec(buf, &o, cap, t->sid);
    sb_str(buf, &o, cap, "\n");
    return o;
}

/* /proc/<pid>/sandbox — session 70.
 *
 * Layout:
 *   Active:    0|1
 *   Denials:   <total>
 *   Mask:      <hex0> <hex1> <hex2> <hex3>
 *   Recent:                          (only present if Denials > 0)
 *     tick=<lo16> sc=<NN> SYS_NAME
 *     ...                            (up to SANDBOX_RECENT_N entries,
 *                                     oldest first)
 *
 * Stable plain-text format intended both for human eyes and for
 * /proc-aware agents that just grep for "Denials:" or split lines
 * starting with "  tick=". A future /proc/<pid>/sandbox.json could
 * emit the same data as JSON for richer tooling. */
static int gen_sandbox(int pid, char *buf, int cap) {
    struct task *t = 0;
    for (uint32_t i = 0; i < 16; i++) {
        struct task *tt = task_at(i);
        if (tt && (int)tt->id == pid) { t = tt; break; }
    }
    if (!t) return 0;

    int o = 0;
    sb_str(buf, &o, cap, "Active:    ");
    sb_dec(buf, &o, cap, (uint32_t)(t->sandbox_active ? 1 : 0));
    sb_str(buf, &o, cap, "\nDenials:   ");
    sb_dec(buf, &o, cap, t->sandbox_denials);
    sb_str(buf, &o, cap, "\nMask:      ");
    for (int i = 0; i < 4; i++) {
        if (i) sb_str(buf, &o, cap, " ");
        sb_hex32(buf, &o, cap, t->sandbox_mask[i]);
    }
    sb_str(buf, &o, cap, "\n");

    if (t->sandbox_denials > 0) {
        sb_str(buf, &o, cap, "Recent:\n");

        /* Walk the ring from oldest to newest. The head points at
         * the NEXT slot to overwrite, so (head + i) % N visits in
         * chronological order. Slot value 0 = never written (the
         * memset(t, 0, ...) at task alloc time). */
        for (int i = 0; i < SANDBOX_RECENT_N; i++) {
            int     idx   = (t->sandbox_recent_head + i) % SANDBOX_RECENT_N;
            uint32_t e    = t->sandbox_recent[idx];
            if (e == 0) continue;
            uint16_t tick = (uint16_t)(e >> 16);
            uint16_t sc   = (uint16_t)(e & 0xFFFFu);
            sb_str(buf, &o, cap, "  tick=");
            sb_dec(buf, &o, cap, tick);
            sb_str(buf, &o, cap, " sc=");
            sb_dec(buf, &o, cap, sc);
            sb_str(buf, &o, cap, " ");
            sb_str(buf, &o, cap, syscall_name(sc));
            sb_str(buf, &o, cap, "\n");
        }
    }
    return o;
}

/* /proc/<pid>/limits — session 71.
 *
 * Layout:
 *   RssPages:   <cur> / <cap>      ("/-" suffix if no cap)
 *   CpuTicks:   <cur> / <cap>
 *   Fds:        <live> / <cap>
 *   WallTicks:  <now> / <deadline>
 *
 * "0" in a cap field is printed as "-" so unset caps don't look
 * like a hard 0. Same pattern across all four fields. */
static int gen_limits(int pid, char *buf, int cap_buf) {
    struct task *t = 0;
    for (uint32_t i = 0; i < 16; i++) {
        struct task *tt = task_at(i);
        if (tt && (int)tt->id == pid) { t = tt; break; }
    }
    if (!t) return 0;

    /* Count live fds for the "current" half of the Fds: line. */
    int live_fds = 0;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (t->fds[i].kind != FD_FREE) live_fds++;
    }

    int o = 0;
    sb_str(buf, &o, cap_buf, "RssPages:   ");
    sb_dec(buf, &o, cap_buf, t->cur_rss_pages);
    sb_str(buf, &o, cap_buf, " / ");
    if (t->max_rss_pages) sb_dec(buf, &o, cap_buf, t->max_rss_pages);
    else                  sb_str(buf, &o, cap_buf, "-");

    sb_str(buf, &o, cap_buf, "\nCpuTicks:   ");
    sb_dec(buf, &o, cap_buf, t->cur_cpu_ticks);
    sb_str(buf, &o, cap_buf, " / ");
    if (t->max_cpu_ticks) sb_dec(buf, &o, cap_buf, t->max_cpu_ticks);
    else                  sb_str(buf, &o, cap_buf, "-");

    sb_str(buf, &o, cap_buf, "\nFds:        ");
    sb_dec(buf, &o, cap_buf, (uint32_t)live_fds);
    sb_str(buf, &o, cap_buf, " / ");
    if (t->max_fds)       sb_dec(buf, &o, cap_buf, t->max_fds);
    else                  sb_str(buf, &o, cap_buf, "-");

    sb_str(buf, &o, cap_buf, "\nWallTicks:  ");
    sb_dec(buf, &o, cap_buf, pit_ticks());
    sb_str(buf, &o, cap_buf, " / ");
    if (t->wall_deadline_ticks) sb_dec(buf, &o, cap_buf, t->wall_deadline_ticks);
    else                        sb_str(buf, &o, cap_buf, "-");
    sb_str(buf, &o, cap_buf, "\n");
    return o;
}

/* ---- VFS ops ------------------------------------------------------- */

/* Parse a leading non-negative decimal integer. Returns -1 if no
 * digits, else the consumed-byte count and writes the value into
 * *out. Accepts a max of 5 digits since pids are uint16. */
static int parse_pid(const char *s, int *out) {
    int n = 0, v = 0;
    while (s[n] >= '0' && s[n] <= '9' && n < 5) {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    if (n == 0) return -1;
    *out = v;
    return n;
}

/* Verify that pid is currently a live task. Returns 1 if so.
 * Used to gate /proc/<pid> opens and to filter readdir of /proc. */
static int pid_is_live(int pid) {
    for (uint32_t i = 0; i < 16; i++) {
        struct task *t = task_at(i);
        if (t && (int)t->id == pid) return 1;
    }
    return 0;
}

static int procfs_open(void *fs_data, const char *rel, struct vfs_inode *out) {
    (void)fs_data;
    if (!rel || !out) return -1;

    /* "" or "/" → /proc itself, a directory. */
    if (rel[0] == 0 || (rel[0] == '/' && rel[1] == 0)) {
        out->kind    = FD_PROCFS;
        out->obj_idx = PROC_MAKE(PNODE_ROOT_DIR, 0);
        out->size    = 0;
        out->is_dir  = 1;
        return 0;
    }

    /* Static top-level files. */
    for (int i = 0; g_static_files[i].name; i++) {
        if (strcmp(rel, g_static_files[i].name) == 0) {
            out->kind    = FD_PROCFS;
            out->obj_idx = PROC_MAKE(g_static_files[i].kind, 0);
            out->size    = 1024;       /* advisory; real size set at read */
            out->is_dir  = 0;
            return 0;
        }
    }

    /* Per-pid: "<pid>" or "<pid>/status". */
    int pid;
    int n = parse_pid(rel, &pid);
    if (n < 0)               return -1;
    if (!pid_is_live(pid))   return -1;

    if (rel[n] == 0) {
        /* "<pid>" — the task's directory. */
        out->kind    = FD_PROCFS;
        out->obj_idx = PROC_MAKE(PNODE_PID_DIR, pid);
        out->size    = 0;
        out->is_dir  = 1;
        return 0;
    }

    if (rel[n] == '/' && strcmp(rel + n + 1, "status") == 0) {
        out->kind    = FD_PROCFS;
        out->obj_idx = PROC_MAKE(PNODE_PID_STATUS, pid);
        out->size    = 256;
        out->is_dir  = 0;
        return 0;
    }
    if (rel[n] == '/' && strcmp(rel + n + 1, "sandbox") == 0) {
        out->kind    = FD_PROCFS;
        out->obj_idx = PROC_MAKE(PNODE_PID_SANDBOX, pid);
        out->size    = 1024;
        out->is_dir  = 0;
        return 0;
    }
    if (rel[n] == '/' && strcmp(rel + n + 1, "limits") == 0) {
        out->kind    = FD_PROCFS;
        out->obj_idx = PROC_MAKE(PNODE_PID_LIMITS, pid);
        out->size    = 256;
        out->is_dir  = 0;
        return 0;
    }
    return -1;
}

int procfs_read_by_id(int id, uint32_t offset, void *buf, uint32_t n) {
    int kind = (int)PROC_KIND(id);
    int pid  = (int)PROC_PID(id);

    char tmp[1024];
    int  len = 0;

    switch (kind) {
        case PNODE_CPUINFO:    len = gen_cpuinfo(tmp, sizeof(tmp));      break;
        case PNODE_MEMINFO:    len = gen_meminfo(tmp, sizeof(tmp));      break;
        case PNODE_UPTIME:     len = gen_uptime (tmp, sizeof(tmp));      break;
        case PNODE_VERSION:    len = gen_version(tmp, sizeof(tmp));      break;
        case PNODE_MOUNTS:     len = gen_mounts (tmp, sizeof(tmp));      break;
        case PNODE_BCACHE:     len = gen_bcache (tmp, sizeof(tmp));      break;
        case PNODE_PID_STATUS:  len = gen_status (pid, tmp, sizeof(tmp));  break;
        case PNODE_PID_SANDBOX: len = gen_sandbox(pid, tmp, sizeof(tmp));  break;
        case PNODE_PID_LIMITS:  len = gen_limits (pid, tmp, sizeof(tmp));  break;
        default: return 0;
    }

    if (offset >= (uint32_t)len) return 0;
    uint32_t avail = (uint32_t)len - offset;
    if (n > avail) n = avail;
    memcpy(buf, tmp + offset, n);
    return (int)n;
}

/* For symmetry with the rootfs adapter — the actual fd-side read
 * happens in syscall.c, which calls procfs_read_by_id. This entry
 * just satisfies the vfs_fs_ops slot. */
static int procfs_read_via_inode(void *fs_data, struct vfs_inode *ino,
                                 uint32_t off, void *buf, uint32_t n) {
    (void)fs_data;
    return procfs_read_by_id(ino->obj_idx, off, buf, n);
}

/* Render `pid` as decimal into `name_buf` (16 bytes). */
static void emit_pid_name(int pid, char *name_buf) {
    char tmp[6]; int ti = 0;
    int v = pid;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (ti > 0 && j < 15) name_buf[j++] = tmp[--ti];
    name_buf[j] = 0;
}

static int procfs_readdir(void *fs_data, const char *rel, int *iter, char *name) {
    (void)fs_data;
    if (!iter || !name) return -1;

    /* Listing /proc itself: emit static files then live pids. */
    if (rel[0] == 0 || (rel[0] == '/' && rel[1] == 0)) {
        int n_static = 0;
        while (g_static_files[n_static].name) n_static++;

        if (*iter < n_static) {
            int i = *iter;
            const char *fn = g_static_files[i].name;
            int j = 0;
            while (fn[j] && j < 15) { name[j] = fn[j]; j++; }
            if (j < 16) name[j] = 0;
            *iter = i + 1;
            return PROC_MAKE(g_static_files[i].kind, 0);
        }

        /* Per-pid directories. iter encodes (n_static + slot_index). */
        int slot = *iter - n_static;
        while (slot < 16) {
            struct task *t = task_at(slot);
            if (t && t->state != TASK_STATE_UNUSED &&
                     t->state != TASK_STATE_DEAD) {
                emit_pid_name((int)t->id, name);
                *iter = n_static + slot + 1;
                return PROC_MAKE(PNODE_PID_DIR, (int)t->id);
            }
            slot++;
        }
        return -1;
    }

    /* Listing /proc/<pid>: status + sandbox. */
    int pid;
    int n = parse_pid(rel, &pid);
    if (n < 0 || rel[n] != 0)  return -1;
    if (!pid_is_live(pid))     return -1;

    static const struct {
        const char *name;
        int         kind;
    } pid_files[] = {
        { "status",  PNODE_PID_STATUS  },
        { "sandbox", PNODE_PID_SANDBOX },
        { "limits",  PNODE_PID_LIMITS  },
    };
    int n_pid_files = (int)(sizeof(pid_files) / sizeof(pid_files[0]));

    if (*iter < n_pid_files) {
        int i = *iter;
        const char *fn = pid_files[i].name;
        int j = 0;
        while (fn[j] && j < 15) { name[j] = fn[j]; j++; }
        if (j < 16) name[j] = 0;
        *iter = i + 1;
        return PROC_MAKE(pid_files[i].kind, pid);
    }
    return -1;
}

/* /proc is read-only. The mkdir / write_all slots are NULL so vfs
 * returns -1 on any attempt to mutate. */
static struct vfs_fs_ops g_procfs_ops = {
    .open      = procfs_open,
    .read      = procfs_read_via_inode,
    .readdir   = procfs_readdir,
    .mkdir     = 0,
    .write_all = 0,
};

struct vfs_fs_ops *procfs_ops(void) { return &g_procfs_ops; }
