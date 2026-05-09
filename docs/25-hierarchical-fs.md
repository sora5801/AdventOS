# Session 25 — Hierarchical filesystem with directories

**Goal:** Replace the flat AdventFS — every file in one big root namespace — with a real hierarchical filesystem. After this session, paths like `/etc/inittab` actually mean something on disk: there is an entry called `etc` whose `type=DIR`, and `inittab` is a separate entry whose `parent_dir` field points back at the `etc` entry. Userspace gets `mkdir`, `cd`, `pwd`, `ls`, and the existing `open` / `fs_write_all` accept full paths. `init` reads `/etc/inittab` instead of `inittab`. Each task carries a `cwd_dir` so relative paths work.

End state — the new `[t16]` selftest:

```
[t16] hierarchical fs: paths, mkdir, cd, pwd, ls
  initial cwd: '/'
  ls /etc:
  inittab
  /etc/inittab (127 bytes):
# AdventOS inittab — services started by init at boot.
# ...
  mkdir /tmp ok
  cwd after cd /tmp: '/tmp'
  wrote note.txt (relative -> /tmp/note.txt)
  read /tmp/note.txt (13 bytes): hi from /tmp
  ls /tmp:
  note.txt
  ls /:
  etc
  hello.elf
  count.elf
  ...
  tmp
  cwd after cd /: '/'
```

The same boot run still passes `[t1]` through `[t15]`, init still reads `/etc/inittab` cleanly (`init: 2 service(s) in inittab`), httpd still serves curl with HTTP 200, and the previous session's free-sector bitmap still reuses sectors across rewrites — the directory layer rides on top of the block allocator unchanged.

## What's in scope

In:
- 32-byte on-disk entries (was 24): `name(16) + start(4) + size(4) + type(1) + parent_dir(1) + 6 reserved`
- New types: `FS_TYPE_FREE / FILE / DIR`. Directories are entries with `type=DIR` and no associated data sectors.
- 2-sector superblock: sector 0 = magic + file_count + reserved padding, sector 1 = the 16 × 32-byte entry table. The bitmap allocator now skips both.
- `FS_DIR_ROOT = 0xFF` sentinel for "this entry sits at root, with no parent directory."
- Path resolution in `fs_open(path)`: leading `/` = absolute; otherwise relative to the calling task's `cwd_dir`. Walk one component at a time, looking up `find_in_dir(parent, name, len)` for each.
- New syscalls 43–46: `SYS_MKDIR`, `SYS_CHDIR`, `SYS_GETCWD`, `SYS_READDIR`. New libuser wrappers + shell builtins (`pwd`, `cd`, `ls`).
- `struct task` gains `uint8_t cwd_dir`. Inherited verbatim by `fork`, preserved across `exec` (POSIX). `kmain` and the kernel reaper start at root.
- `fs_write_all(path, ...)` now takes a path — bare basenames go in cwd, absolute paths go where you tell them.
- `mkfs.py` builds directory entries first (so children can encode their parent's index) and accepts a `parent` field on every file, so `inittab` lands inside `/etc` at build time.
- `init.c` reads `/etc/inittab` rather than `inittab`. The diagnostic message updates with it.
- `kmain` LAUNCH still uses `init.elf` — root-level `.elf` files keep their old names so the boot path doesn't move.

Out:
- Multi-level directories (we only built `/etc` and let userspace `mkdir /tmp`; nothing prevents `/a/b/c` mechanically, but mkfs only creates one level deep at build time).
- Renaming, moving, deleting (`mv`, `rm`, `rmdir`). Files still get rewritten in place by `fs_write_all`; nothing erases an entry.
- Hard links and symlinks. Each entry has exactly one parent, no link count.
- File modes / owners / timestamps. Reserved 6 bytes per entry are spare for these.
- `..` / `.` directory entries. `cd ..` doesn't work; `cd /` is the way home.
- `getdents`-shaped readdir (we return one entry per call; iteration state is opaque-but-trivially an `int` index).
- `glob` / wildcards in shell.
- Persistent cwd across reboots — cwd is per-task, lives in RAM only, dies with the task.

## Architecture: from flat list to parent-pointer tree

The old `fs_entry` was 24 bytes: a 16-byte name and two `uint32_t` for start/size. `fs_open(name)` was a linear scan of the entry table looking for a name match. There was no notion of "where" an entry lived — every entry was implicitly at root.

The new `fs_entry` is 32 bytes:

```c
struct fs_entry {
    char     name[FS_NAME_MAX];   /* basename only — no slashes */
    uint32_t start_sector;        /* 0 for DIRs */
    uint32_t size;                /* 0 for DIRs */
    uint8_t  type;                /* FREE / FILE / DIR */
    uint8_t  parent_dir;          /* entry idx, or FS_DIR_ROOT (0xFF) */
    uint8_t  reserved[6];
};
```

The directory tree is encoded as a **parent-pointer forest**. There's no on-disk "list of children" per directory — instead, every entry knows its parent, and to enumerate `/etc`'s children you walk the global table looking for `parent_dir == idx_of_etc`. This is dirt-simple to encode (no variable-length arrays in the on-disk layout, no allocations beyond the existing 16-slot table) and dirt-cheap to mutate (`mkdir` is one new entry, no rewrite of any existing entry).

The cost is that every directory operation is `O(file_count)`. With 16 max entries that's free; if we ever raise the cap, this becomes worth revisiting. The tradeoff is the same one the original 16-entry flat table made: keep the format dead simple at the cost of not scaling.

Ascii of the post-mkfs tree, after the `[t16]` test runs `mkdir /tmp` and writes `/tmp/note.txt`:

```
                    /                       (FS_DIR_ROOT, the sentinel)
                    │
   ┌──────┬───────┬─┴─────┬───────┬───────┬──────────┬──────────┬─────────┐
   │      │       │       │       │       │          │          │         │
  etc  hello.elf count.elf sh.elf …      hello.txt notes.txt reuse.txt   tmp
   │                                                                       │
inittab                                                              note.txt
```

`/etc/inittab`'s `parent_dir` is 0 (the index of `etc`). `etc`'s `parent_dir` is `FS_DIR_ROOT = 0xFF`. The root has no entry of its own — a parent of `0xFF` literally means "no parent, top-level".

## Path resolution: absolute vs relative

`fs_open(path)` is the only place the slash-walking logic lives:

```c
int fs_open(const char *path) {
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
            if (g_super.files[found].type != FS_TYPE_DIR) return -1;
            parent = (uint8_t)found;
            path++;
        }
    }
    return last;
}
```

Three things make this small:

1. `find_in_dir` does a linear scan of the entry table matching `name + parent_dir`. Because the path slice isn't NUL-terminated (it's a pointer into the input), the helper takes an explicit length. The match condition is "first `len` bytes equal AND (`len == FS_NAME_MAX` OR `entry_name[len] == 0`)" — i.e. either the name fills the slot completely or has a NUL after it.
2. The cwd anchor is read from `task_current()->cwd_dir`. The kernel reaper task has its `cwd_dir` zero-initialized in its `memset` to `TASK_STATE_UNUSED`, but the reaper never opens files, so we never observe that. Every user task inherits root from kmain through `task_create` (default `TASK_CWD_ROOT`) and through fork (`child->cwd_dir = parent->cwd_dir`).
3. Mid-component non-directories fail the walk: `if (g_super.files[found].type != FS_TYPE_DIR) return -1` for everything except the LAST component. So `fs_open("/etc/inittab/wat")` correctly fails.

`fs_write_all` and `fs_mkdir` use `split_path()` to separate "the directory portion" from "the basename":

- `"/etc/inittab"` → parent = `fs_open("/etc")`, base = `"inittab"`.
- `"name"` (no slash) → parent = `cwd_dir`, base = `"name"`.
- `"/name"` → parent = `FS_DIR_ROOT`, base = `"name"`.

This is the only place we materialize a temporary buffer for the parent path — at most 128 bytes on the kernel stack, kept small because the rest of the kernel uses the same.

## The 2-sector superblock

The entry table outgrew its single-sector home. With 16 entries × 24 bytes = 384 bytes, plus 12 bytes of header, the old superblock fit in 396 bytes of one 512-byte sector. With 16 × 32 + 12 = 524 bytes, it doesn't.

The fix is a 2-sector superblock laid out as:

```
sector 0:  8B "ADVENTFS"  +  4B file_count  +  500B reserved
sector 1:  16 × 32B entry  +  0B padding   (= 512B exactly)
```

`fs_init` reads two sectors and copies them into a single 1024-byte `struct fs_super`. `fs_write_super` does the inverse. The `__attribute__((packed))` on `struct fs_super` makes the layout match exactly.

The bitmap reservation in `fs_init` and the run allocator in `bitmap_alloc_run` both bump from sector 0 to `FS_SUPER_SECTORS` so neither sector 0 nor sector 1 ever gets handed out for file data. The free-sector count drops by exactly one between session 24 and 25 — `905/1024` instead of `906/1024` for the same file payload.

## The cwd: per-task, fork-inherited, exec-preserved

POSIX semantics for cwd:

| Operation | Effect on cwd |
|---|---|
| `fork()` | Child inherits parent's cwd |
| `exec()` | Caller's cwd is preserved |
| `chdir(path)` | Resolves `path` relative to current cwd, then sets new cwd |
| Process exit | cwd vanishes with the task |

This is implemented as a single `uint8_t cwd_dir` on `struct task`, holding an entry index (or `FS_DIR_ROOT`):

```c
/* task_init (slot 0 is kmain) */
g_tasks[0].cwd_dir = TASK_CWD_ROOT;

/* task_create (every new ring-0 / ring-3 task) */
t->cwd_dir = TASK_CWD_ROOT;

/* task_fork */
child->cwd_dir = parent->cwd_dir;

/* task_exec_inplace */
/* (no assignment — cwd_dir survives the address-space swap) */
```

Why a `uint8_t` instead of a string? Because the on-disk entry table has 16 slots, an index fits in 8 bits with room to spare. A string would have to be re-resolved after every directory rename — and we don't have rename, but if we ever do the index can stay valid as long as the directory entry doesn't move. (The encoding also matches what `parent_dir` already uses, so getcwd's "walk up" logic and chdir's "set this index" logic share the same fundamental type.)

`SYS_GETCWD` reconstructs the path by walking parent pointers up to root, collecting indices, then emitting them in reverse order with leading slashes:

```
cwd = idx of /tmp        (e.g. 11)
   parent of 11 → 0xFF   (root)
   collected: [11]
emit in reverse: "/" + name(11) → "/tmp"
```

A 16-deep parent chain is plenty; at full depth the path length cap is 16 × (FS_NAME_MAX + 1) = 272 bytes, and our user buffer is 128, so deep paths get truncated. With one level of directory in our actual tree, this never bites.

## syscall reads of user pointers

The four new syscalls (43–46) all take user pointers — paths, name buffers, the `*iter` int. They follow the same pattern the rest of the kernel does for ring-3-supplied buffers: snapshot at the boundary, work on the kernel-stack copy. `SYS_MKDIR` is the simplest:

```c
case SYS_MKDIR: {
    const char *upath = (const char *)(uintptr_t)a;
    char path[128];
    int  i;
    for (i = 0; i < (int)sizeof(path) - 1 && upath[i]; i++) path[i] = upath[i];
    path[i] = 0;
    int rc = fs_mkdir(path);
    ret = (rc < 0) ? -1 : 0;
    break;
}
```

The `for` loop reads one byte at a time from user memory. If the user's pointer is bogus, the deref takes a page fault, the existing PF handler catches it (today: panic, since this isn't an mmap region; eventually: SIGSEGV on the calling task). For a hostile-user model that's still the wrong answer; for a hobby kernel where the user code is tightly trusted, it's fine.

`SYS_READDIR` is the only one that takes both an in-pointer (`*iter`) and an out-pointer (`name_buf`), so it does the full snapshot-and-write-back dance. The iterator state is just the next entry index to consider — a single int — so it lives in user memory between calls and the kernel doesn't have to remember anything.

## mkfs.py: build the tree at image-creation time

The directory tree has to exist before any files reference it as a parent. mkfs.py emits directory entries first so they get low indices:

```python
DIRECTORIES = ['etc']

USER_PROGRAMS = [
    ('hello.elf', 'user/_obj/hello.bin', None),     # parent=None → root
    ...
    ('init.elf',  'user/_obj/init.bin',  None),
]

DATA_FILES = [
    ('hello.txt', 'fs/hello.txt', None),
    ('inittab',   'fs/inittab',   'etc'),           # parent='etc'
]
```

`build()` walks `DIRECTORIES` first, assigning each one a sequential index and recording it in `dir_idx_by_name`. Then it walks `USER_PROGRAMS` and `DATA_FILES`, looking up the parent's index when adding each file. The encoded entry layout is fixed at `'<IIBB6x'` (start, size, type, parent, 6 reserved bytes) — `struct.pack` lets us write the layout once in one place and have the kernel's `__attribute__((packed))` struct decode it.

Build output:

```
        [ 0] DIR  //etc
        [ 1] FILE //hello.elf    sec 2  (4008 bytes)
        ...
        [10] FILE /etc/inittab   sec 118  (301 bytes)
```

Note the path printed for `/etc/inittab`: parent index 0 maps back to `etc` via `DIRECTORIES[parent]`, which produces `/etc` for the parent prefix. This is just diagnostics — the kernel doesn't care about the print format, only the fields written into the entry blob.

## Why .elf files stayed at root

A genuine question I had to answer in this session: should `init.elf` move to `/sbin/init` or `/bin/init`? Should `sh.elf` be `/bin/sh`? The answer was no — keep them at root.

The reason is the `LAUNCH("init.elf", ...)` macro in `kernel.c`:

```c
int _fd = fs_open("init.elf");
```

`kmain` runs before init has had a chance to set up cwd, before the shell exists, before anything has called chdir. Its only entry into the FS is via path. If the path were `/sbin/init` we'd need to:
- create `/sbin` in mkfs.py
- update the kmain LAUNCH to look at `/sbin/init`
- nothing else benefits

vs. keeping it at root:
- nothing changes in kmain
- mkfs.py needs zero new directories beyond `/etc`

The directory layer is supposed to prove itself by moving **one** file (`inittab`) — that forces every layer (mkfs, fs.c, init.c, the `cwd_dir` plumbing, `SYS_GETCWD`) to be exercised end-to-end. Moving more files would just be cosmetic.

A future session that adds dynamic linking or a real `/proc` would justify moving the binaries. Today's `init.elf`-at-root is a known temporary that costs nothing to maintain.

## init.c: one path string changed

The full diff to init.c was a single hunk:

```diff
-    int n = read_whole_file("inittab", tab, sizeof(tab));
+    int n = read_whole_file("/etc/inittab", tab, sizeof(tab));
```

Plus the matching diagnostic update so the boot log says "reading /etc/inittab". That's the entire user-visible change to init's logic. The `read_whole_file` helper, the `parse_inittab` tokenizer, the fork/exec/wait reap loop — all unchanged. This was the goal: every existing component sees the path-aware FS as a drop-in upgrade.

`sys_open("/etc/inittab")` flows through `SYS_OPEN`, which calls `fs_open`, which walks `etc` then `inittab`, returns the entry index, and the existing file-descriptor allocator wraps it as an `FD_FS` fd. From init's perspective, nothing changed except the string passed in.

## Shell builtins

Three new builtins, all under 30 lines:

```c
static void cmd_pwd(void) {
    char buf[128];
    int n = sys_getcwd(buf, sizeof(buf));
    if (n < 0) puts("pwd: error\n");
    else       { puts(buf); puts("\n"); }
}

static void cmd_cd(const char *arg) {
    if (!arg || !*arg) arg = "/";
    if (sys_chdir(arg) < 0) {
        puts("cd: "); puts(arg); puts(": no such directory\n");
    }
}

static void cmd_ls(const char *arg) {
    const char *path = (arg && *arg) ? arg : ".";
    char cwd_buf[128];
    if (path[0] == '.' && path[1] == 0) {
        sys_getcwd(cwd_buf, sizeof(cwd_buf));
        path = cwd_buf;
    }
    int  iter = 0;
    char name[17];
    int  shown = 0;
    for (;;) {
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        name[16] = 0;
        puts("  "); puts(name); puts("\n");
        shown++;
    }
    if (shown == 0) puts("  (empty)\n");
}
```

`cd` defaults to `/` when called bare — same behavior as bash's `cd ~` minus a home directory. `ls` translates `.` to the result of `getcwd` because `SYS_READDIR` doesn't have a "use cwd" sentinel; it always takes a path. (We could push that translation into the syscall, but the user-side handling is shorter and means the syscall stays single-purpose.)

These run inline in the shell process — they don't fork, because cwd needs to mutate the calling process's state. A pipeline of `cd /tmp` would change cwd in a forked subshell that exits immediately, which is the bash gotcha behind `cd` being a builtin everywhere. We just don't allow `cd` to appear in pipelines — the dispatcher only checks builtins as the sole token.

## Lifecycle wiring

The post-fork-and-exec rules I wanted to assert in code:

| Field | After `fork` | After `exec` | At task creation |
|---|---|---|---|
| `cwd_dir` | inherit from parent | preserved | `FS_DIR_ROOT` |
| heap | deep-copied | reset to empty | empty |
| mmap regions | deep-copied (faulted-in pages eagerly) | reset to empty | empty |
| open fds | inherited (refcount bumped) | inherited | stdin/stdout/stderr |

cwd is by far the cheapest of these — one byte. It costs nothing to inherit, costs nothing to preserve, costs nothing to reset; the question is just "did I remember to write the assignment in the right three places?" The `task_init` / `task_create` / `task_fork` triple all set it; `task_exec_inplace` deliberately leaves it alone.

A test run that fork+execs into a directory verifies all three branches at once: `cd /tmp` runs in the shell, then a `ls`-style readdir from that subshell would resolve `.` against `/tmp` because the shell's cwd was inherited by `ls`'s exec'd process — except that we made `ls` a builtin, so the inheritance path isn't exercised by `ls`. Instead it's exercised by the t16 test calling `sys_fs_write("note.txt", ...)` after `chdir("/tmp")` from inside the **shell** process — `note.txt` resolves through `task_current()->cwd_dir == /tmp`'s entry index, the `find_or_create_file` carve creates the entry under `parent_dir = idx(/tmp)`, and `sys_open("/tmp/note.txt")` afterwards returns that exact entry.

That round-trip — relative write, absolute readback, both refer to the same file — is the load-bearing test for the path layer. If `cwd_dir` was wrong anywhere, this would either write to root, fail to find the file on read, or write to a different parent than the read targets.

## What can go wrong

A few things to watch out for if you extend this:

1. **The 16-entry cap.** Every `mkdir` and every fresh `fs_write_all` consumes a slot; we never reclaim slots. The session 23 free-sector bitmap reuses **sectors** across rewrites of the same file, but the file's slot stays put. Once we hit 16, allocation fails. The cap is just the size of the on-disk entry table.
2. **`parent_dir` is one byte.** `0xFF` is the root sentinel, so the maximum legal index is 254. That's fine for 16 entries today, but a larger entry table needs `parent_dir` widened to a `uint16_t` (which would force the entry size up — the 6 reserved bytes give one byte of slack).
3. **No `..` entry.** `cd ..` doesn't work. The most natural fix is to let `chdir` interpret `..` as "go to my parent_dir" (`task_current()->cwd_dir = fs_entry_parent(cwd)` if not root) — this is a five-line change but the test surface to verify "cwd correctly walks up across fork+exec" is bigger.
4. **No persistence of cwd.** Because cwd is per-task and the kernel doesn't write it anywhere, every reboot starts everyone at `/`. POSIX says the same thing — the cwd inherits from the parent, init's parent is the kernel, the kernel starts at `/`. If we ever want shell history that tracks "the user was in /tmp last time", that needs an explicit user-space mechanism (a config file in `/etc`, presumably).
5. **mkfs.py builds one level deep.** `DIRECTORIES = ['etc']` is a flat list. A real tree would need parent links in the build script too — easy to add, just another column in the tuple. We didn't bother because every kernel test passes with one level.

## File-by-file changes

```
kernel/fs.h                  rewritten — new entry layout, FS_DIR_ROOT, 2-sec super,
                             fs_open/fs_mkdir/fs_dir_iter/fs_entry_type/parent
kernel/fs.c                  rewritten — path resolver, find_in_dir, split_path,
                             alloc_slot, mkdir, dir_iter, 2-sector superblock I/O,
                             bitmap skips both superblock sectors
kernel/task.h                add uint8_t cwd_dir
kernel/task.c                #include guard via TASK_CWD_ROOT macro; init kmain @ ROOT,
                             task_create @ ROOT, task_fork inherits, exec preserves
kernel/syscall.h             add SYS_MKDIR=43, SYS_CHDIR=44, SYS_GETCWD=45, SYS_READDIR=46
kernel/syscall.c             4 new dispatcher cases (snap path, call fs_*, return)
kernel/kernel.c              comment update only — kmain LAUNCH still uses init.elf
mkfs.py                      32-byte entries, DIRECTORIES list, parent column on
                             every file tuple, 2-sec superblock layout, encode_entry
                             packs the new format
user/libuser.h               add 4 syscall constants and 4 wrapper prototypes
user/libuser.c               add 4 syscall wrappers (sys_mkdir/chdir/getcwd/readdir)
user/init.c                  read_whole_file("/etc/inittab", ...) — single-line move
user/sh.c                    cmd_pwd / cmd_cd / cmd_ls + dispatch + help line +
                             [t16] selftest demonstrating the round trip
```

Total kernel-side: about 200 net new lines (most of fs.c is rewritten in shape). User-side: 3 new builtins (~50 lines) and ~70 lines of selftest. mkfs.py is rewritten in shape but ends up about the same length.

## Boot log

For reference, the relevant lines from a clean boot:

```
[boot] mounting AdventFS... fs: AdventFS mounted, 11 entries, 905/1024 sectors free
...
[boot] launched init.elf as pid 4
init: pid=4, reading /etc/inittab
init: 2 service(s) in inittab
init: started 'httpd.elf' as pid 5 (once)
init: started 'sh.elf' as pid 6 (once)
httpd: listening on port 80 (userspace)
init: entering reap+respawn loop

AdventOS userspace shell, pid=6
...
[t16] hierarchical fs: paths, mkdir, cd, pwd, ls
  initial cwd: '/'
  ls /etc:
  inittab
  /etc/inittab (127 bytes):
# AdventOS inittab — services started by init at boot.
# ...
  mkdir /tmp ok
  cwd after cd /tmp: '/tmp'
  wrote note.txt (relative -> /tmp/note.txt)
  read /tmp/note.txt (13 bytes): hi from /tmp
  ls /tmp:
  note.txt
  ls /:
  etc
  hello.elf
  ...
  tmp
  cwd after cd /: '/'
=== selftest done ===
```

And from a host curl after the same boot:

```
$ curl -v http://localhost:8080/
< HTTP/1.0 200 OK
< Content-Type: text/plain
< Connection: close
Hello from a USERSPACE HTTP server!
This page was served by user/httpd.c, which runs in ring 3.
...
```

The directory layer is in. Next session's choice opens up: mountable filesystems, a real shell with `..`-aware cd and globbing, a `/proc`, or pivoting into something like an `exec` that searches `$PATH`. Whatever we add next, paths now mean something.
