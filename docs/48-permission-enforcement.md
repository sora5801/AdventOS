# Session 48 — Permission enforcement

**Goal:** close the gap session 47 explicitly left open. Multi-user creds and stored ownership were informational only — every uid could read, write, and exec every file. This session adds:

- A `mode` field (Unix `rwxrwxrwx`) on every on-disk `fs_entry`.
- `SYS_CHMOD` / `SYS_CHOWN` / `SYS_FS_MODE` syscalls.
- A `fs_check_perm` helper that the syscall layer enforces at the three I/O entry points: `SYS_OPEN` for read, `SYS_FS_WRITE` for write, `task_exec_inplace` for execute.

End state — new `[t31]` selftest:

```
[t31] permission enforcement: chmod, chown, open(R), write(W), exec(X)
  perm.txt mode = 0644 (expect 0644)
  PASS  default mode 0644 on new file
  sh.elf mode = 0755 (expect 0755)
  PASS  mkfs ELF mode 0755
  PASS  chmod 0600 as root
  PASS  chmod readback = 0600
  PASS  non-owner chmod denied
  PASS  non-root chown denied
  PASS  non-root open(0600 root-file) denied
  PASS  root chown to 1000 OK
  PASS  owner open(0600 own-file) allowed
  PASS  non-owner write(0644 root-file) denied
  PASS  exec of non-executable file fails
  PASS  (positive exec case covered by t30 login flow)
```

Every multi-user pivot landed cleanly:

- New files default to `0644` (rw-r--r--). `mkfs.py` stamps ELFs as `0755` and data files (`/etc/inittab`, `/etc/passwd`, `hello.txt`) as `0644`. Directories get `0755`.
- `chmod 0600 perm.txt` from root → readback shows `0600`.
- `chmod 0666 perm.txt` from uid 1000 → denied (non-owner). ✓
- `chown root_file 1000:1000` from uid 1000 → denied (non-root). ✓
- `open("root_owned_0600_file")` from uid 1000 → denied. ✓
- After `chown perm.txt 1000:1000` as root, uid 1000 can open the same file. ✓
- `write("0644 root-file")` from uid 1000 → denied. ✓
- `exec("perm.txt")` (mode 0644, no x bits) → returns failure rather than running the data file as code. ✓

cryptotest 27/27, real-world Cloudflare HTTPS GET, usbtest, `[t30]` login flow, `[t9b]` vi — all still pass.

## What's in scope

In:

- **`kernel/fs.h`** — `struct fs_entry` grows a `uint16_t mode` field carved out of the last 2 reserved bytes. The entry is still exactly 32 bytes — on-disk wire format unchanged. New constants: `FS_MODE_IRUSR/IWUSR/IXUSR/...`, `FS_PERM_R/W/X` for the helper, `FS_MODE_RWX_MASK = 0777`.
- **`kernel/fs.c`** — `find_or_create_file_inst` stamps `mode = 0644` on newly-created entries. New helpers:
  - `fs_entry_mode(idx)` — read the mode of an entry.
  - `fs_check_perm(idx, want)` — returns 1 if the calling task may access entry `idx` with `want` (any combination of `FS_PERM_R/W/X`). Root bypasses; otherwise picks user / group / other tier based on uid/gid match.
  - `fs_chmod_idx(idx, mode)` and `fs_chown_idx(idx, uid, gid)` — primitive mutators that write the superblock through immediately.
- **`kernel/syscall.{c,h}`** — three new syscalls + enforcement in three existing ones:
  - `SYS_FS_MODE(path)` → mode bits or -1.
  - `SYS_CHMOD(path, mode)` — owner-or-root check.
  - `SYS_CHOWN(path, uid, gid)` — root-only.
  - `SYS_OPEN` — after vfs resolves a disk entry, calls `fs_check_perm(idx, FS_PERM_R)`; -1 if denied.
  - `SYS_FS_WRITE` — if file exists, requires write perm; new files allowed (no directory-write enforcement today).
  - `task_exec_inplace` — after fs_open, calls `fs_check_perm(idx, FS_PERM_X)`; -3 if denied.
- **`user/libuser.{c,h}`** — `sys_fs_mode`, `sys_chmod`, `sys_chown` wrappers + the matching syscall numbers.
- **`libc/stdio.c`** — `printf("%o", ...)` octal formatter (so `[t31]` and any future tooling can pretty-print mode bits readably).
- **`mkfs.py`** — `encode_entry` takes a `mode` argument; `build_image` defaults dirs to `0o755`, ELFs to `0o755`, raw blobs (libc.bin) to `0o755`, data files to `0o644`. Printed verbose output shows the mode in octal next to size.
- **`user/sh.c`** — `[t31]` selftest section covering twelve assertion lines.

Out (deferred):

- **Directory write permission** — required for "this user can create files in `/home/foo` but not in `/etc`". Today, new-file creation is unconditional; only overwrites of *existing* files check perms. Doing this right needs a `parent->mode & W` check at `find_or_create_file_inst`, which is a four-line addition I left for later.
- **setuid / setgid bits + saved-uid** — would let an unprivileged user run a setuid-root program (like Unix `passwd`). The exec path would need to switch the task's uid based on the file's setuid bit, and `SYS_SETUID` would need a saved-uid distinction. Out of scope.
- **The sticky bit** for `/tmp`-style directories.
- **Per-file ACLs**, capabilities, namespaces, etc.
- **`stat -l`** — there's no standalone `ls -l` / `stat` user program. `sys_fs_owner` + `sys_fs_mode` are wired up; building the CLI is a separate small project.
- **Mode-passing on creation** — POSIX `open(path, O_CREAT, mode)` lets the caller specify the initial mode + apply umask. Our `SYS_FS_WRITE` only takes a path; new files always get `0644`. Adding a third arg to the syscall + a userspace umask is straightforward but didn't make the cut.

## Architecture

```
                  USER PROGRAM
                  ┌──────────────────────────┐
                  │  sys_open / sys_fs_write │
                  │  / sys_chmod / sys_chown │
                  │  / sys_fs_mode           │
                  └──────────┬───────────────┘
                             │ int 0x80
                             ▼
                  KERNEL — syscall.c
                  ┌──────────────────────────┐
                  │  SYS_OPEN:                                       │
                  │    vfs_open(path) → ino                          │
                  │    if (ino.kind == FD_FS):                       │
                  │      fs_check_perm(ino.obj_idx, FS_PERM_R) — deny│
                  │                                                  │
                  │  SYS_FS_WRITE:                                   │
                  │    if (file exists)                              │
                  │      fs_check_perm(idx, FS_PERM_W) — deny        │
                  │    else allow (no dir-W check today)             │
                  │                                                  │
                  │  SYS_CHMOD:                                      │
                  │    require cur.uid==0 OR cur.uid==file.uid       │
                  │                                                  │
                  │  SYS_CHOWN:                                      │
                  │    require cur.uid==0                            │
                  │                                                  │
                  │  task_exec_inplace:                              │
                  │    fs_check_perm(idx, FS_PERM_X) — deny          │
                  └──────────┬───────────────────────────────────────┘
                             │
                             ▼
                  fs_check_perm(idx, want):
                  ┌──────────────────────────────────────┐
                  │   if (task.uid == 0) return ALLOW;   │
                  │   if (task.uid == file.uid)          │
                  │     bits = (mode >> 6) & 7  /* u */  │
                  │   else if (task.gid == file.gid)     │
                  │     bits = (mode >> 3) & 7  /* g */  │
                  │   else                               │
                  │     bits = (mode >> 0) & 7  /* o */  │
                  │   return (bits & want) == want;      │
                  └──────────────────────────────────────┘
```

## Mode layout — POSIX-shaped

Mode is stored as a `uint16_t`; only the low 9 bits matter today.

```
  bit 8: S_IRUSR  (owner read)   0400
  bit 7: S_IWUSR  (owner write)  0200
  bit 6: S_IXUSR  (owner exec)   0100
  bit 5: S_IRGRP  (group read)   0040
  bit 4: S_IWGRP  (group write)  0020
  bit 3: S_IXGRP  (group exec)   0010
  bit 2: S_IROTH  (other read)   0004
  bit 1: S_IWOTH  (other write)  0002
  bit 0: S_IXOTH  (other exec)   0001
```

Standard octal:

```
  0644 = rw-r--r--   (default for data files)
  0755 = rwxr-xr-x   (default for ELFs)
  0700 = rwx------   (private executable)
  0600 = rw-------   (private data)
```

The remaining 3 mode bits (setuid 04000 / setgid 02000 / sticky 01000) are reserved in our `uint16_t` but not interpreted. Adding them later means handling the setuid-on-exec case (task uid changes to the file's uid) — explicitly deferred.

## fs_check_perm: three-tier lookup

```c
int fs_check_perm(int idx, int want) {
    if (!g_root_inst || !g_root_inst->initialized || idx < 0 ||
        (uint32_t)idx >= g_root_inst->super.file_count) return -1;
    struct task *t = task_current();
    /* Root (or kernel context with no current task) bypasses. */
    if (!t || t->uid == 0) return 1;

    struct fs_entry *e = &g_root_inst->super.files[idx];
    int shift;
    if (t->uid == e->uid)      shift = 6;    /* user tier */
    else if (t->gid == e->gid) shift = 3;    /* group tier */
    else                        shift = 0;    /* other tier */
    int bits = (e->mode >> shift) & 0x7;
    return ((bits & want) == want) ? 1 : 0;
}
```

Three tiers, exactly like Unix `access(2)`:

1. Caller's uid matches file's owner → use the owner triple.
2. Otherwise, caller's gid matches the file's group → use the group triple.
3. Otherwise → use the other triple.

The shift `6 / 3 / 0` is the rwx-bit base for each tier. We mask `& 0x7` to pull a 3-bit value and require all requested bits (`(bits & want) == want`) — so asking for `R | W` against `r--` denies.

Root bypasses everything — matches the conventional "superuser sees all" model. Kernel contexts (where `task_current()` returns NULL, e.g., the boot-time `fs_init` reading the superblock) also bypass.

## chmod / chown rules

```c
case SYS_CHMOD: {
    /* Only the file's owner or root may chmod. */
    ...
    if (cur->uid != 0 && cur->uid != (uint16_t)owner) {
        ret = -1; break;
    }
    ret = fs_chmod_idx(idx, (uint16_t)mode);
    break;
}

case SYS_CHOWN: {
    /* Only root may chown. */
    ...
    if (cur->uid != 0) { ret = -1; break; }
    ret = fs_chown_idx(idx, (uint16_t)new_uid, (uint16_t)new_gid);
    break;
}
```

Same as Unix: the file's owner can `chmod` their own files (changing their access for *other* users), but only the superuser can `chown` (because giving a file away should require explicit superuser intent — otherwise a non-root user could circumvent quota or accountability).

Both syscalls write the on-disk superblock through immediately via `fs_chmod_idx` / `fs_chown_idx`, so changes survive reboot.

## Enforcement points

Three syscalls enforce permissions:

1. **`SYS_OPEN`** — at the syscall site, after `vfs_open` resolves a path to an inode. If the resolved `kind == FD_FS` (a disk-backed file, not procfs / tmpfs), `fs_check_perm(idx, FS_PERM_R)` runs. Denied → `ret = -1` and no fd is allocated.

2. **`SYS_FS_WRITE`** — at the syscall site. If the named file exists, `fs_check_perm(idx, FS_PERM_W)` runs. Denied → `-1`. New-file creation skips this; there's no enforceable "parent directory write" concept on our flat-ish FS yet.

3. **`task_exec_inplace`** (called by `SYS_EXEC`) — after `fs_open(path)` succeeds, `fs_check_perm(fd, FS_PERM_X)` runs. Denied → return `-3`. The check happens before ELF loading, so a denied exec leaves the caller's address space untouched.

Reads via `sys_read` after a successful `sys_open` don't re-check — once you have an fd you're trusted to read. That matches POSIX (`open` is the access-check moment).

## Why dirs have mode 0755 but no enforcement on directory writes

`mkfs.py` stamps directories with `0o755`. The mode field is honored: `fs_check_perm` against a directory would return the right answer if we ever consulted it. **We don't currently consult it** — directory entries appear in our `find_or_create_file_inst` only as the parent reference for a new file, and we don't check the parent's `W` bit.

This means a non-root user *can* create new files anywhere on the FS (subject to the `/etc/inittab`-style file-already-exists check). They just can't overwrite root-owned files, can't change other people's permissions, and can't execute non-executable bytes. That's the "informational ownership" of session 47 plus enforcement against *existing* files — the full POSIX picture also needs directory-W to prevent unauthorized file creation.

Adding it is a four-line change at `find_or_create_file_inst`: look up the parent dir's entry, call `fs_check_perm(parent, FS_PERM_W)`, abort if denied. I left it out to keep this session focused on the three explicit syscall sites the user mentioned.

## Test results

12 assertions in `[t31]`, all PASS. Notable ones:

- **Default modes** — both runtime-created files (0644) and mkfs-shipped ELFs (0755) report the expected mode. Verifies the on-disk format change and the `find_or_create_file_inst` stamp both work.
- **chmod by owner / non-owner** — root chmod's perm.txt to 0600, the readback matches; uid 1000 tries to chmod the same file and gets -1.
- **chown gated to root** — uid 1000's `chown` returns -1; root's `chown perm.txt 1000:1000` succeeds; the file's owner is now 1000.
- **Open enforcement** — uid 1000 can't open a 0600 root-owned file; after chown to 1000, the same uid 1000 *can* open it (now it's the owner tier reading the file with bit `0400` set in mode `0600`).
- **Write enforcement** — uid 1000 can't write a 0644 root-owned file (other tier has no W).
- **Exec enforcement** — uid 1000 trying to `sys_exec("perm.txt")` (which has mode 0644 — no X bits anywhere) gets a failure return from exec. The kernel returns -3 from `task_exec_inplace` for "no execute permission"; the SYS_EXEC handler translates this to -1 for the caller.

Plus the positive exec case: `[t30]` already exercises `uid 1000 exec("sh.elf")` and succeeds because `sh.elf` has mode 0755 (other tier has X). No regression.

cryptotest 27/27, real-world Cloudflare HTTPS, usbtest, vi `[t9b]`, full login flow `[t30]` — all unaffected.

## What's next

- **Directory write enforcement** — gate file creation on parent `W`. Four-line patch as described above.
- **setuid / setgid bits + saved-uid** — would let us add a real `passwd` user-space command (setuid root, edits `/etc/passwd`) and tighten the security model.
- **POSIX `open(path, O_CREAT, mode)` + `umask`** — letting users specify modes on creation rather than hardcoded 0644.
- **`ls -l` / `stat`** user programs that display the new metadata.
- **In-kernel `chown` race window** — `fs_chmod_idx` and `fs_chown_idx` write the superblock through but aren't locked against concurrent FS operations. Single-threaded enough for the demo; a real multi-CPU system needs an `fs_lock` around mutator ops.
- **POSIX `access(2)`** syscall so a non-syscall-issuing program can ask "could I open this?" without actually opening.
