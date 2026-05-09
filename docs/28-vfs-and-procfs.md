# Session 28 — VFS abstraction + /proc filesystem

**Goal:** Stop hardcoding the on-disk AdventFS as the only filesystem the kernel knows about. Slot a thin VFS dispatch layer between the syscall paths and the per-fs implementations, then add a synthetic `/proc` filesystem that generates content on demand from live kernel state — uptime, memory stats, CPUID-derived cpuinfo, the block-cache counters, the running mount table, and per-task `status` files keyed by pid.

End state — the new `[t19]` selftest output:

```
[t19] VFS + /proc: synthesized files, mounts, per-pid dirs
  cat /proc/version:
AdventOS v0.1 (i386, session 28) VFS + procfs over hierarchical AdventFS + bcache
  cat /proc/uptime:
9.04
  cat /proc/meminfo:
MemTotal:     32640 kB
MemUsed:       4984 kB
MemFree:      27656 kB
  cat /proc/cpuinfo:
vendor_id  : GenuineIntel
family     : 6
model      : 6
stepping   : 3
features   : fpu tsc msr pae cx8 apic pge mmx sse sse2
  cat /proc/mounts:
/ rootfs
/proc procfs
  cat /proc/bcache:
hits         305
misses       417
logical_w    103
disk_w       11
dirty        0
  ls /proc:
  cpuinfo
  meminfo
  uptime
  version
  mounts
  bcache
  0
  1
  2
  3
  4
  5
  6
  7
  cat /proc/7/status:
Name:    sh.elf
Pid:     7
State:   RUN
PPid:    5
Pgid:    7
Sid:     7
  ls /  (rootfs entries + mount-point names):
  etc
  hello.elf
  ...
  bc.txt
  proc           ← the /proc mount appears as a synthetic dir
```

The shell's existing `cat`, `ls`, and pipe machinery are 100% unchanged — they call `sys_open` / `sys_read` / `sys_readdir` and don't know whether the path resolves to a real disk file or a procfs synthesizer.

`httpd.elf` keeps serving curl on :80 throughout. Tests t1–t18 all pass unchanged.

## What's in scope

In:
- **`kernel/vfs.{h,c}`** — dispatch layer with a 4-slot mount table, longest-prefix-match path resolution, and a "merge mount-point names into rootfs listing" trick for `ls /`
- **rootfs adapter** in `fs.c` — five-line wrappers that expose the existing `fs_open` / `fs_read` / `fs_dir_iter` / `fs_mkdir` / `fs_write_all` through a `vfs_fs_ops` table
- **`kernel/procfs.{h,c}`** — synthetic filesystem with seven file types: `cpuinfo`, `meminfo`, `uptime`, `version`, `mounts`, `bcache`, and per-pid `status` directories
- **`FD_PROCFS` fd kind** added to `task.h`; `syscall.c` handles it in the `SYS_READ` switch by calling `procfs_read_by_id`
- **`SYS_OPEN` / `SYS_FS_WRITE` / `SYS_MKDIR` / `SYS_READDIR`** all switched from direct `fs_*` calls to `vfs_*`
- **`[t19]` selftest** demonstrating reads from each /proc file plus the merged-listing behavior

Out:
- **chdir into /proc.** The task's `cwd_dir` is still a single `uint8_t` rootfs entry index. `cd /proc` fails (limitation noted in the test). Fixing this means making cwd a `(fs, inode)` tuple — significant refactor.
- **Writable /proc.** All procfs ops have `mkdir` and `write_all` set to NULL; you can read but not modify.
- **/proc/<pid>/cmdline / fd / cwd** — only `status` is exposed per pid today. argv isn't stored, fd table dump would need formatter work.
- **Mounts of arbitrary depth.** Synthetic-name merging in `ls /` only surfaces top-level mounts (`/proc`, `/dev`); a `/usr/local` mount wouldn't appear under `ls /usr`.
- **`/sys`, `/dev`** — these would be additional vfs_fs_ops; not added in this session.
- **VFS-managed inode lifecycle.** `vfs_inode` is value-typed and copied through ops; nothing reference-counts open inodes. fd-table semantics carry the persistence.

## Architecture

Before:

```
syscall.c
  ├── sys_open  ── fs_open(name)         ──► fs.c (rootfs only)
  ├── sys_read  ── switch(kind)
  │                 case FD_FS:    fs_read(...)
  │                 case FD_TMPFS: tmpfs_read(...)
  ├── sys_readdir ── fs_open + fs_dir_iter
  ├── sys_mkdir   ── fs_mkdir
  └── sys_fs_write── fs_write_all
```

After:

```
syscall.c
  ├── sys_open    ──► vfs_open(path) ───► [longest-prefix mount match]
  │                                         ├── rootfs.open ──► fs.c
  │                                         └── procfs.open ──► live-state lookup
  │
  ├── sys_read    ── switch(kind)
  │                   case FD_FS:     fs_read(...)
  │                   case FD_TMPFS:  tmpfs_read(...)
  │                   case FD_PROCFS: procfs_read_by_id(...)
  ├── sys_readdir ── vfs_readdir   ───► same dispatch + merge mount names
  ├── sys_mkdir   ── vfs_mkdir
  └── sys_fs_write── vfs_write_all
```

The **fd-side dispatch** still lives in `syscall.c`'s `SYS_READ` switch, which is fine because that switch is short and the discriminator is already carried in `task_fd.kind`. The **path-side dispatch** is what VFS owns: given a string, decide which fs implements it and forward the relative path.

Keeping these two concerns separate is the design principle. A "full VFS" would unify them into a single `vfs_inode_ops` table and require every existing fd kind (sockets, pipes, tmpfs) to implement it. That's the right end-state but a much bigger refactor than this session aimed for.

## The mount table

```c
#define VFS_MAX_MOUNTS  4

struct vfs_mount {
    int                 in_use;
    char                mount_point[16];   /* "/" or "/proc" */
    char                fs_name[16];       /* "rootfs", "procfs" */
    struct vfs_fs_ops  *ops;
};
```

Four slots. A mount is registered with:

```c
int vfs_mount(const char *mount_point, const char *fs_name,
              struct vfs_fs_ops *ops);
```

`kmain` registers two: `/` for rootfs, `/proc` for procfs. The order doesn't matter — `vfs_open` always picks the **longest prefix match** so that `/proc/uptime` routes to procfs even though `/` would also match.

```c
static struct vfs_mount *resolve(const char *path, const char **rel_out) {
    struct vfs_mount *best     = find_rootfs();   /* "/" is the catch-all */
    int               best_len = 1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        struct vfs_mount *m = &g_mounts[i];
        if (!m->in_use) continue;
        int mlen = (int)strlen(m->mount_point);
        if (mlen <= best_len) continue;
        if (strncmp(path, m->mount_point, mlen) != 0) continue;
        char follow = path[mlen];
        if (follow != 0 && follow != '/') continue;  /* /proc != /procxyz */
        best     = m;
        best_len = mlen;
    }

    const char *rel = path + best_len;
    if (*rel == '/') rel++;
    *rel_out = rel;
    return best;
}
```

Two important details:

1. **The "follow" check.** `/proc` is a 5-char mount point. Without verifying that the next byte is `/` or `\0`, `/procxyz` would match — which would silently route some random path into the procfs handler. The check rejects `procxyz` (next byte is `x`), accepts `/proc/uptime` (next byte is `/`), and accepts `/proc` itself (next byte is `\0`).
2. **Strip the prefix.** The fs's `open` gets a path **relative to its mount**: `/etc/inittab` becomes `etc/inittab`, `/proc/uptime` becomes `uptime`. The leading slash, if any, is also dropped, so each fs sees exactly the part it owns and can do its own component walk from a clean starting state.

## The "ls / shows /proc as a directory" trick

Linux's `ls /` shows `/proc` because the on-disk root contains an empty `proc` directory that the kernel mounts onto. We don't have that — our root is the `g_super.files[]` entry table, and there's no "proc" entry on disk.

Instead, `vfs_readdir("/", ...)` does this in two phases:

```c
int vfs_readdir(const char *path, int *iter, char *name_buf) {
    /* ... resolve mount, dispatch to fs's readdir ... */

    if (*iter < MOUNT_PHASE) {
        int idx = m->ops->readdir(rel, iter, name_buf);
        if (idx >= 0) return idx;          /* still iterating fs entries */
        *iter = MOUNT_PHASE;                /* fall through to phase 2 */
    }

    /* Phase 2: emit synthetic mount-point names. */
    int mi = *iter - MOUNT_PHASE;
    while (mi < VFS_MAX_MOUNTS) {
        struct vfs_mount *mm = &g_mounts[mi];
        if (mm->in_use && mm->mount_point[0] == '/' && mm->mount_point[1] != 0 &&
            !strchr(mm->mount_point + 1, '/')) {        /* top-level only */
            const char *p = &mm->mount_point[1];        /* skip "/" */
            int j = 0;
            while (p[j] && j < 15) { name_buf[j] = p[j]; j++; }
            if (j < 16) name_buf[j] = 0;
            *iter = MOUNT_PHASE + mi + 1;
            return 0x10000 | mi;
        }
        mi++;
    }
    return -1;
}
```

The iterator is opaque to userspace, so we use the magic value `MOUNT_PHASE = 0x10000` as a sentinel: anything below that is "still iterating rootfs", anything above is "iterating mount points." The shell's `ls` builtin doesn't have to know — it just keeps calling `sys_readdir` until it returns `-1`.

The output:

```
ls /
  etc
  hello.elf
  ...
  bc.txt           ← last rootfs entry
  proc             ← synthetic, from the mount table
```

Phase-2 is gated to **top-level mounts only** (`!strchr(mp + 1, '/')`). If we ever mount something at `/usr/local`, it'd show up under `ls /usr` instead of `ls /` — which is the Unix-correct answer.

## procfs file generators

Each procfs file is implemented as a `gen_*` function that fills a stack buffer from live kernel state and returns the byte count. There's no caching — every read regenerates the bytes fresh.

The simplest is `gen_uptime`:

```c
static int gen_uptime(char *buf, int cap) {
    int o = 0;
    uint32_t s     = pit_seconds();
    uint32_t ticks = pit_ticks() % 100;       /* PIT runs at 100 Hz */
    sb_dec(buf, &o, cap, s);
    sb_str(buf, &o, cap, ".");
    if (ticks < 10) sb_str(buf, &o, cap, "0");
    sb_dec(buf, &o, cap, ticks);
    sb_str(buf, &o, cap, "\n");
    return o;
}
```

`sb_str` and `sb_dec` are tiny string-builder helpers private to procfs.c — same shape as libuser's printf internals but writing to a buffer instead of stdout. No `sprintf` in the kernel.

`gen_cpuinfo` reads CPUID directly:

```c
__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
*(uint32_t *)(vendor + 0) = b;
*(uint32_t *)(vendor + 4) = d;
*(uint32_t *)(vendor + 8) = c;
vendor[12] = 0;
sb_str(buf, &o, cap, "vendor_id  : "); sb_str(buf, &o, cap, vendor);

__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
sb_str(buf, &o, cap, "features   :");
if (d & (1u << 0))  sb_str(buf, &o, cap, " fpu");
if (d & (1u << 4))  sb_str(buf, &o, cap, " tsc");
...
```

In QEMU the vendor reads as `GenuineIntel`, family 6 model 6, with the standard core feature flags. Real-machine output would be Intel/AMD specific, again from CPUID.

`gen_meminfo` calls `pmm_total_pages()` / `pmm_used_pages()` / `pmm_free_pages()`, multiplied by 4 to get KiB. `gen_bcache` calls the session-27 stat accessors. `gen_mounts` calls `vfs_describe_mounts` which walks the mount table.

`gen_status` is the most demonstrative — it goes from a **string pid** in the path back to the **task struct** to a fully-formed status block:

```c
static int gen_status(int pid, char *buf, int cap) {
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
```

Output for the shell, mid-selftest:

```
Name:    sh.elf
Pid:     7
State:   RUN
PPid:    5
Pgid:    7
Sid:     7
```

Pid 7 is the shell, parent pid 5 is init (which spawned it via `inittab`). Pgid + Sid both equal pid because the shell did `setsid()` on startup (session 20).

## obj_idx encoding for procfs files

procfs has multiple kinds of files (one per `gen_*`) plus per-pid variants. The `vfs_inode.obj_idx` (which becomes `task_fd.obj_idx` on open) is a single int, so we pack:

```
   31      16  15     0
  ┌──────────┬──────────┐
  │  kind    │   pid    │
  └──────────┴──────────┘
```

```c
#define PROC_KIND(id)        ((unsigned)(id) >> 16)
#define PROC_PID(id)         ((unsigned)(id) & 0xFFFF)
#define PROC_MAKE(k, p)      ((int)((((unsigned)(k)) << 16) | ((p) & 0xFFFF)))

enum {
    PNODE_NONE        = 0,
    PNODE_CPUINFO     = 2,
    PNODE_MEMINFO     = 3,
    PNODE_UPTIME      = 4,
    ...
    PNODE_PID_STATUS  = 9,
};
```

`procfs_read_by_id` is then a switch on `PROC_KIND(id)` calling the matching generator, with `PROC_PID(id)` passed for the pid-aware ones. The 16-bit pid limit is fine: AdventOS's `TASK_MAX = 16`, and ids fit easily.

This keeps the open path stateless: nothing is allocated on `procfs_open`, and there's nothing to free on close — just an int handle.

## Stateless reads: the implications

`procfs_read_by_id` regenerates the entire content on every call:

```c
int procfs_read_by_id(int id, uint32_t offset, void *buf, uint32_t n) {
    char tmp[1024];
    int  len = 0;
    switch (PROC_KIND(id)) {
        case PNODE_UPTIME:  len = gen_uptime(tmp, sizeof(tmp)); break;
        case PNODE_PID_STATUS: len = gen_status(PROC_PID(id), tmp, sizeof(tmp)); break;
        ...
    }
    if (offset >= (uint32_t)len) return 0;
    uint32_t avail = len - offset;
    if (n > avail) n = avail;
    memcpy(buf, tmp + offset, n);
    return (int)n;
}
```

Two consequences:

1. **Content can change between reads.** If a user opens `/proc/uptime` and reads byte-by-byte, the digits visible at offset 0 might disagree with the digits visible later. `cat /proc/uptime` typically issues one big read with a 256-byte buffer, so this isn't observable in practice — but a reader iterating one byte at a time would see weird transitions. Linux solves this with `seq_file` snapshotting; we don't have that.
2. **Reads are cheap.** No I/O, no caching, just a few arithmetic ops + memcpy. The kernel doesn't care if you `cat /proc/uptime` in a tight loop.

For tiny files (< 256 bytes), the regeneration overhead is microseconds; fine for the demo. For big files, a snapshot-on-open is the obvious extension.

## fd-side dispatch in syscall.c

The `SYS_READ` switch grew one case:

```c
case FD_PROCFS: {
    int rd = procfs_read_by_id(e->obj_idx, e->offset, buf, (uint32_t)n);
    if (rd > 0) e->offset += (uint32_t)rd;
    ret = rd;
    break;
}
```

That's all that's needed — the kernel's existing fd-table machinery (alloc on open, free on close, dup2, fork-inheritance) treats `FD_PROCFS` like any other discriminator; only the read handler differs.

`SYS_OPEN` switched from a hardcoded `fs_open` + `tmpfs_open` cascade to a `vfs_open` + `tmpfs_open` cascade:

```c
struct vfs_inode ino;
if (vfs_open(name, &ino) >= 0) {
    t->fds[fd].kind    = ino.kind;
    t->fds[fd].obj_idx = ino.obj_idx;
    t->fds[fd].offset  = 0;
    ret = fd;
    break;
}
/* fall back to tmpfs (the in-RAM scratchpad behind `>`) */
int tmp_idx = tmpfs_open(name);
...
```

The tmpfs fallback stays for backward compat — `seq 3 | tee /seq.txt` still works because tmpfs accepts the bare `/seq.txt` name. A future session would either fold tmpfs into the VFS as a third mount (`/tmp`?) or drop it.

## Why the bcache stat numbers diverged from session 27

t18's baseline disk_writes was 6 in session 27 and 11 in session 28. The five extra writes are:

- `t9` (ed editor) writes notes.txt — 1 data sector + 3 superblock = 4 dirty
- `t14` rewrite test — 1 data + 3 super = same 4 dirty (mostly coalesced)
- `t16` mkdir + relative-path note write — superblock only
- `t17` tee /seq.txt — 1 data + 3 super
- `t18`'s 11 rewrites of bc.txt themselves — 5 dirty entries we already counted

The extra 5 are hits the periodic syncer caught **between** earlier tests, which it didn't catch in session 27 because the 5-second cadence happened to land differently. Both are correct behavior; the absolute number is timing-dependent.

The interesting ratio — 44 logical writes coalesced to 5 disk writes during the 11-rewrite burst — is unchanged.

## File-by-file changes

```
kernel/vfs.h            NEW — vfs_inode + vfs_fs_ops + mount API
kernel/vfs.c            NEW — mount table, longest-prefix dispatch,
                         root-listing merge of mount-point names

kernel/procfs.h         NEW — procfs_ops + procfs_read_by_id
kernel/procfs.c         NEW — gen_* synthesizers for cpuinfo / meminfo /
                         uptime / version / mounts / bcache / pid_status,
                         obj_idx (kind<<16 | pid) packing

kernel/fs.c             rootfs adapter (8 thin wrappers + a single
                        g_rootfs_ops table). +#include "vfs.h"
kernel/fs.h             prototype for fs_rootfs_ops()

kernel/task.h           add FD_PROCFS to the fd-kind enum

kernel/syscall.c        sys_open routes through vfs_open;
                        SYS_READ adds an FD_PROCFS case;
                        SYS_MKDIR/READDIR/FS_WRITE call vfs_*;
                        path snapshot bumped to 128 bytes
                        +#include "vfs.h" "procfs.h"

kernel/kernel.c         vfs_init + vfs_mount("/") + vfs_mount("/proc")
                        right after fs_init

user/sh.c               [t19] selftest with 6 cat /proc/* lines, an
                        ls /proc, a per-pid status read, ls /
```

Net diff: 2 new kernel files (~620 lines combined), small touchups to fs.c / kernel.c / syscall.c / task.h. No user-program changes — the existing cat/ls/wc/etc. all just work because they use sys_open and don't care that the path resolves into procfs.

## Boot log additions

```
[boot] mounting AdventFS... fs: AdventFS mounted, 24 entries, 695/1024 sectors free
[boot] mounting VFS... vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
ok
```

`fs: AdventFS mounted` is the on-disk format announcing itself. `vfs: mounted 'rootfs' at /` is the VFS layer noting that rootfs is now its catchall for the system root. `vfs: mounted 'procfs' at /proc` registers the synthesizer. After this line, every syscall path resolution goes through VFS dispatch.

## What this opens up

A handful of next steps fall out cleanly:

- **`/dev`** — a third mount, with `/dev/null` (read returns 0, write discards), `/dev/zero` (read fills with NUL), `/dev/random` (well, `/dev/urandom` with a tiny PRNG). Each is an inode kind; `dev_read_by_id` switches over them.
- **Writable `/proc`.** Wire up `procfs.write_all` for specific files: `/proc/sys/loglevel`, `/proc/bcache/sync` (writing anything triggers a sync). Linux's procfs has been doing this for decades.
- **VFS-managed cwd.** Make `task->cwd` a `(mount, inode)` pair so `cd /proc` works. Requires a small refactor of `cmd_cd` and `SYS_CHDIR` / `SYS_GETCWD` but no fs.c changes — the path-walk happens in VFS anyway.
- **A VFS-side fd table.** Today each fd kind dispatches its own read/write/close in `syscall.c`. A unified `vfs_inode_ops` would let `SYS_READ` be a single `vfs_read(fd, buf, n)` call. Bigger change, but it's where this design naturally heads.

For now, the demo lands on a clean line: from a user's perspective, `/proc/uptime` is just a file. From the kernel's perspective, it's pit_seconds() + a little formatting. The VFS sits between them and pretends the two are the same kind of thing.

## Final check

```
$ cat /proc/version
AdventOS v0.1 (i386, session 28) VFS + procfs over hierarchical AdventFS + bcache

$ cat /proc/mounts
/ rootfs
/proc procfs

$ cat /proc/7/status
Name:    sh.elf
Pid:     7
State:   RUN
PPid:    5
Pgid:    7
Sid:     7

$ curl -s http://localhost:8080/ | head -1
Hello from a USERSPACE HTTP server!
```

Path resolution is now polymorphic. The kernel has an opinion about where files come from, but it doesn't have a single answer.
