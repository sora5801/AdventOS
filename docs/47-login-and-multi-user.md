# Session 47 — Login + multi-user

**Goal:** turn AdventOS from "everything runs as root" into a real multi-user system. Process credentials (uid, gid) on every task, inherited across fork and surviving exec. A `login` program that authenticates against `/etc/passwd` using salted SHA-256 hashes and drops privileges before exec'ing the shell. File ownership stamped into each on-disk entry so a user's `:w` from vi (or any `sys_fs_write`) is recorded as theirs.

End state — `[t30]` selftest:

```
[t30] multi-user: /etc/passwd, login, setuid, file ownership
  /etc/passwd (191 bytes):
root:ABCDef01$2fbe4871ffd5853c...:0:0:/:sh.elf
guest:GH23ij45$e2ea47ad2d7afa79...:1000:1000:/:sh.elf
  selftest uid=0 gid=0 (expect 0/0)
  id as root:
  uid=0 gid=0 pid=33 pgid=8
  id after sys_setuid(1000):
  uid=1000 gid=1000 pid=34 pgid=8
  ownertest.txt owner: uid=0 gid=0 (expect 0/0)
  guesttest.txt owner: uid=1000 gid=1000 (expect 1000/1000)

AdventOS login: guest
password: guest
Welcome, guest (uid=1000 gid=1000)

AdventOS userspace shell, pid=36
advent$ id.elf
uid=1000 gid=1000 pid=37 pgid=37
advent$ exit
bye
  login -> sh -> id exited code=0
```

Six things working in sequence:

1. `/etc/passwd` is a real file on the boot AdventFS, parsed verbatim by `login.elf`.
2. `selftest` runs as uid 0 because init spawned it (kernel-spawned tasks default to root).
3. Fork+exec preserves creds across both — the spawned child of `id.elf` reports the same uid as its parent.
4. `sys_setuid(1000)` from a root task drops privileges; the resulting `id` reports the new uid.
5. File ownership: `sys_fs_write` stamps the current task's uid+gid onto the new on-disk entry. A root-owned write produces a uid=0 entry; a uid=1000 write produces a uid=1000 entry.
6. End-to-end login: keystrokes "guest\nguest\nid.elf\nexit\n" injected via `tty_inject`, `login.elf` authenticates against the stored salt+SHA-256 hash, drops to uid 1000, execs `sh.elf`, the shell prompts, runs `id`, sees uid 1000, exits cleanly.

## What's in scope

In:

- **`kernel/task.{c,h}`** — `struct task` grows two fields: `uint16_t uid, gid`. `task_fork` copies both from parent to child. `task_exec_inplace` leaves them untouched (POSIX: exec preserves creds).
- **`kernel/syscall.{c,h}`** — five new syscalls:
  - `SYS_GETUID`, `SYS_GETGID` return the current task's creds.
  - `SYS_SETUID`, `SYS_SETGID` set them, but only root may set a different uid; non-root callers may "set" to their current uid (no-op) and fail any other target.
  - `SYS_FS_OWNER(path)` returns `(uid << 16) | gid` of the named file, or -1 if missing. Used by `ls`-style tooling.
- **`kernel/fs.{c,h}`** — `struct fs_entry` grows `uint16_t uid, gid` carved out of what used to be 6 reserved bytes (still 32 bytes total — wire format unchanged). `find_or_create_file_inst` stamps the current task's creds onto every newly-created entry. `fs_entry_uid` / `fs_entry_gid` getters expose them.
- **`mkfs.py`** — `encode_entry` takes `uid` / `gid` params (default 0, root-owned). A new `gen_passwd_file()` function generates `/etc/passwd` at build time with salted SHA-256 hashes for the demo users (root/root and guest/guest). Written in binary mode (`'wb'`) so Windows hosts don't bake `\r\n` into the file and trip the parser.
- **`fs/passwd`** — generated each build, two-line `name:salt$hash:uid:gid:home:shell` records.
- **`user/login.c`** — reads `/etc/passwd`, prompts (echo-on for username, echo-off for password using `tty_set_mode(TTY_ICANON)`), computes SHA-256 of `salt || password` via libcrypto, compares lowercase-hex to the stored hash, and on success `sys_setgid` + `sys_setuid` + `sys_exec` of the user's shell.
- **`user/id.c`** — prints `uid=N gid=N pid=N pgid=N`. The Unix `id` minus the group-list and the supplemental groups.
- **`user/libuser.{c,h}`** — wrappers for the five new syscalls.
- **`user/sh.c`** — new `[t30]` selftest section exercising the full flow.
- **`build.sh`** — `login` joins TLS_PROGS (it links against libcrypto for SHA-256); `id` joins USER_PROGS.

Out:

- **Per-file permission bits.** Read/write/execute bits would need rwx-mask bytes alongside uid/gid; we don't have them yet. Every file is currently world-readable + world-writable. The "ownership" is informational, not enforced.
- **`chown` / `chmod`.** Would be trivial to add given the uid/gid storage already exists; nothing in this session needs them.
- **Group lists** (supplementary groups). One gid per task; no `setgroups`.
- **`/etc/shadow` separation.** The hash sits in `/etc/passwd` itself rather than a separate root-readable file. World-readable, but the hash is salted so a stolen passwd file isn't an instant rainbow-table.
- **Saved-uid / effective-uid distinction.** Real Unix has ruid/euid/suid for setuid binaries. We have one uid only; `sys_setuid` is one-way for non-root tasks.
- **Real boot-time login**. `inittab` still runs `once sh.elf selftest` (so all the existing `[t*]` tests still drive). Replacing that with `once login.elf` gives you a real boot-time login prompt, but breaks automated testing. Documented; not done.
- **Password change** (`passwd` command). Users can edit `/etc/passwd` with `vi` if they're root.

## Architecture

```
       BUILD TIME (mkfs.py)
       ┌─────────────────────────────────────────────────┐
       │  USERS = [(root, root, 0, 0, /, sh.elf),        │
       │           (guest, guest, 1000, 1000, /, sh.elf)]│
       │                                                 │
       │  for each user, sha256(salt || password):       │
       │    name:salt$lowercase_hex:uid:gid:home:shell   │
       │  → fs/passwd (binary mode — no CRLF)            │
       │  → fs.img → os.img                              │
       └─────────────────────────────────────────────────┘

       BOOT TIME — init runs sh.elf selftest, [t30] tests:
                                  │
       ┌──────────────────────────┴───────────────────────┐
       │  ./id.elf                 ./login.elf (forked)   │
       │  uid=0 gid=0 (root)       reads /etc/passwd      │
       │                           prompt:  "AdventOS     │
       │                            login: "  (TTY_ICANON │
       │                                       + ECHO)   │
       │                           prompt: "password: "  │
       │                                       (TTY_ICANON│
       │                                       only)     │
       │                                                  │
       │  fork + setuid(1000)      sha256(salt||pw) ==    │
       │  ./id.elf                  stored hash?          │
       │  uid=1000 gid=1000              │                │
       │                                 ▼                │
       │                           sys_setgid(gid)        │
       │                           sys_setuid(uid)        │
       │                           sys_exec(shell)        │
       │                                 │                │
       │                                 ▼                │
       │                           sh.elf prompts +       │
       │                           runs as guest          │
       └──────────────────────────────────────────────────┘

       FILE I/O — every sys_fs_write stamps creator's creds:
       ┌──────────────────────────────────────────────────┐
       │  fs.c::find_or_create_file_inst:                 │
       │    struct task *cur = task_current();            │
       │    e->uid = cur ? cur->uid : 0;                  │
       │    e->gid = cur ? cur->gid : 0;                  │
       │                                                  │
       │  sys_fs_owner("foo.txt") → (uid << 16) | gid     │
       └──────────────────────────────────────────────────┘
```

## /etc/passwd format

```
name:salt$sha256hex:uid:gid:home:shell
```

Six colon-separated fields per line (one user per line):

1. **name** — login name. Used both for the prompt match and as the human-readable identifier.
2. **salt$hash** — 8-character ASCII salt + `$` + 64 lowercase hex chars of `SHA-256(salt || password)`. The salt is per-user (mkfs.py uses different fixed salts for each demo user, but real production would generate per-user random salts).
3. **uid** — decimal integer. 0 = root.
4. **gid** — decimal integer.
5. **home** — home directory path. We parse it but don't yet `chdir` to it on login.
6. **shell** — program name to exec on successful login. `sh.elf` for both demo users.

Generated at build time by `mkfs.py`:

```python
USERS = [
    ('root',  'root',  0,    0,    '/', 'sh.elf'),
    ('guest', 'guest', 1000, 1000, '/', 'sh.elf'),
]

def gen_passwd_file():
    import hashlib
    lines = []
    for i, (name, password, uid, gid, home, shell) in enumerate(USERS):
        salt = USER_SALTS[i % len(USER_SALTS)]
        h = hashlib.sha256((salt + password).encode('ascii')).hexdigest()
        lines.append(f'{name}:{salt}${h}:{uid}:{gid}:{home}:{shell}')
    return '\n'.join(lines) + '\n'
```

## The 31-byte fs_entry stays 32 bytes

`struct fs_entry` was already exactly 32 bytes — the on-disk wire format — with 6 reserved bytes at the tail. We carved two of them for uid + two for gid, leaving 2 reserved. **The on-disk format size doesn't change**, so existing images keep loading and the bitmap-sized `n_sectors` cap doesn't drift:

```c
struct fs_entry {                  /* still exactly 32 bytes */
    char     name[FS_NAME_MAX];    /* 16 */
    uint32_t start_sector;         /* 4 */
    uint32_t size;                 /* 4 */
    uint8_t  type;                 /* 1 */
    uint8_t  parent_dir;           /* 1 */
    uint16_t uid;                  /* 2  ← session 47 */
    uint16_t gid;                  /* 2  ← session 47 */
    uint8_t  reserved[2];          /* 2 */
} __attribute__((packed));
```

mkfs.py's `encode_entry` matches:
```python
struct.pack('<IIBBHH2x', start, size, type_, parent, uid, gid)
```

Files baked into the boot image default to root-owned (uid=0, gid=0); runtime-created files get their creator's creds stamped on. Old images built before session 47 still load — the now-uid/gid fields are zero in the on-disk bytes, which (since 0 means root) is the most-permissive interpretation.

## SETUID rules

The kernel-side rules are deliberately minimal:

```c
case SYS_SETUID: {
    struct task *t = task_current();
    uint32_t target = a;
    if (target > 0xFFFFu) { ret = -1; break; }
    if (t->uid == 0) {
        /* root: may set any uid */
        t->uid = (uint16_t)target;
        ret = 0;
    } else if ((uint16_t)target == t->uid) {
        /* No-op for non-root setting current uid */
        ret = 0;
    } else {
        ret = -1;
    }
    break;
}
```

The full POSIX `setuid` has ruid/euid/suid plumbing that handles setuid-on-exec binaries; we don't have setuid-bit binaries, so the three-uid distinction reduces to one. Root drops privileges to a chosen uid (one-way for that task) and that's it. Non-root tasks can call `setuid(getuid())` as a no-op (matches POSIX behavior for "no actual change").

## Password verification

login.c's verify path is straightforward:

```c
static int verify_password(struct user_entry *u, const char *password) {
    struct sha256 s;
    int sl = 0; while (u->salt[sl])  sl++;
    int pl = 0; while (password[pl]) pl++;
    sha256_init(&s);
    sha256_update(&s, u->salt, sl);
    sha256_update(&s, password, pl);
    uint8_t digest[32];
    sha256_final(&s, digest);
    char got[68];
    hex_lower(digest, 32, got);
    return my_strcmp(got, u->hash) == 0;
}
```

The salt is concatenated BEFORE the password (not after) — that ordering matters because mkfs.py uses the same: `hashlib.sha256((salt + password).encode('ascii'))`. Switch one to the other-ordering and authentication fails consistently.

The hash is compared via a simple `strcmp`, NOT a constant-time compare. For a real authentication system this is a timing-attack vector — an attacker who can measure microsecond-level response times can extract the hash byte-by-byte. For an in-OS demo on QEMU it doesn't matter; documented as known.

## The `\r\n` debugging story

The first end-to-end run of login looked like a textbook bug:

```
AdventOS login: guest
password: guest
Welcome, guest (uid=1000 gid=1000)
login: exec failed
```

The shell exec returned `-1`. I traced through `task_exec_inplace` — opens the path, loads ELF, switches PD — nothing obviously wrong, no uid checks. Added a debug print of the shell name byte-by-byte:

```
login: exec 'sh.elf' (len=7, bytes: 73 68 2e 65 6c 66 0d)
```

`73 68 2e 65 6c 66` is `s h . e l f`. Then `0d` — `\r`. The shell name was `"sh.elf\r"`. The file `sh.elf` exists on the FS; the file `sh.elf\r` doesn't.

Root cause: Python's `open('w')` on Windows automatically translates `\n` → `\r\n` when writing text. mkfs.py was writing `/etc/passwd` in text mode, so the file content on disk was:

```
root:...:0:0:/:sh.elf\r\n
guest:...:1000:1000:/:sh.elf\r\n
```

login.c's parser strips the trailing `\n` from each line, but leaves the `\r`, which then ends up in the last field of every record. The hash field doesn't care (still parses as 64 hex chars + nothing extra), but the `shell` field is "sh.elf\r" → exec fails.

Fix: `open('fs/passwd', 'wb').write(content.encode('ascii'))` — binary mode, no platform-specific newline translation.

This is the same shape as session 19's git CRLF warnings, just at the filesystem image layer. Worth a paragraph in the doc because it's the kind of bug that'll bite anyone doing similar host-built FS images on Windows.

## Test results

`[t30]` covers seven things in one pass:

1. **`/etc/passwd` readable** — print the file contents (verifies mkfs.py produced valid data and the on-disk FS layout is intact).
2. **Default creds** — `sys_getuid()` returns 0 in the boot-time selftest (kernel-spawned tasks default to root). ✓
3. **fork+exec inherits root** — `id.elf` spawned as root prints uid=0. ✓
4. **`sys_setuid(1000)` drops privileges** — `id.elf` after the syscall prints uid=1000. ✓
5. **Non-root `setuid(0)` is rejected** — the child tries it post-drop and the call returns -1. ✓
6. **File ownership stamps current uid** — root-written file shows uid=0, guest-written file shows uid=1000. ✓
7. **Full login flow** — keystrokes "guest\nguest\nid.elf\nexit\n" injected; login authenticates, drops priv, execs sh.elf, sh runs id.elf (which prints uid=1000), then exits cleanly. ✓

cryptotest 27/27 still passes. Real-world Cloudflare HTTPS GET still works (we didn't touch TLS). usbtest, vi `[t9b]`, USB hub + HID kbd + mouse — all unaffected.

## What's next

- **Permission bits + enforcement.** Currently the uid/gid is informational. Add rwx mode bits and have `fs_open` consult them against the calling task's uid/gid. ~50 LOC.
- **`chown` / `chmod`** syscalls and user-space commands.
- **Real boot-time login.** Swap `once sh.elf selftest` for `once login.elf` in inittab and call it a day — but that breaks the automated test harness. A two-mode inittab (one for selftest, one for production) is the right shape.
- **`/etc/group`** for supplementary groups.
- **Saved-uid / effective-uid** if we ever want setuid-bit binaries.
- **A `users` shell built-in** that lists who's logged in (we don't track that today).
- **A `passwd` command** that updates the user's row in /etc/passwd. Currently you'd boot, `vi /etc/passwd`, regenerate the salt+hash by hand, and write it.
