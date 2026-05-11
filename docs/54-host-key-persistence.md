# Session 54 — Host-key persistence on disk

**Goal:** stop baking the SSH host private key into the sshd binary. Generate a fresh ed25519 keypair on first boot, persist the 32-byte seed to `/etc/ssh_host_key` mode 0600, and load it back on every subsequent boot. The fingerprint a client stores in `known_hosts` then stays stable for the lifetime of the disk — which is what `known_hosts` was designed for.

End state — manual two-boot verification of the same `os.img`:

```
$ qemu ... os.img    # boot 1
sshd: loaded host key from /etc/ssh_host_key
sshd: host key sha256: 5a8e11382bd0ee6a3550b9e83ff62921c81356f769db75da876d165450a6a866

$ qemu ... os.img    # boot 2 (no rebuild)
sshd: loaded host key from /etc/ssh_host_key
sshd: host key sha256: 5a8e11382bd0ee6a3550b9e83ff62921c81356f769db75da876d165450a6a866
```

Identical fingerprint. Plus `[t36]` covers file properties:

```
[t36] sshd: host-key persistence on disk (/etc/ssh_host_key)
  /etc/ssh_host_key mode=0600 owner=uid:0 gid:0
  PASS  host key mode = 0600 (owner-only)
  PASS  host key owner uid = 0 (root)
  PASS  open /etc/ssh_host_key succeeds for root
  PASS  host key file is exactly 32 bytes (ed25519 seed)
  PASS  host key bytes aren't uniform (looks like rand_bytes output)
  PASS  non-root open() of host key denied by 0600 mode
```

## What's in scope

In:

- **`user/sshd.c`** — replace the baked-in `HOSTKEY_SEED` constant with `load_or_generate_host_key()` that reads `/etc/ssh_host_key` if present (deriving the keypair via `ed25519_keypair_from_seed`), or generates a fresh 32-byte seed with `rand_bytes`, persists it via `sys_fs_write`, tightens mode to 0600 with `sys_chmod`, and forces an immediate `sys_bcache_sync` so the file survives an unclean shutdown.
- **`user/sshd.c`** — `print_host_fingerprint()` prints the SHA-256 of the public key as 64 hex chars at startup. Same byte sequence on every boot of the same image = persistence working.
- **`build.sh`** — pad `os.img` to `fs_lba + 4096` sectors (was 2048). This was the actual bug fix that made the whole thing work — see the gotcha section.
- **`user/sh.c`** — `[t36]` selftest covering file existence, size, mode, owner, and the negative case (non-root open is denied).

Out:

- **OpenSSH-format private key files.** Real sshd writes an PEM-wrapped multi-section blob; AdventOS stores just the 32-byte ed25519 seed. We re-derive the public key + 64-byte expanded private key from the seed every boot. Trade-off: not interchangeable with `ssh-keygen`-generated key files, but 90% less code and zero ambiguity about the wire format.
- **Multiple host keys.** Real sshd usually has `_rsa`, `_ecdsa`, AND `_ed25519` — AdventOS uses ed25519 exclusively (session 51 picked that and clients haven't complained).
- **Host-key rotation / `ssh-keygen -A` regen tool.** To rotate: delete `/etc/ssh_host_key` and reboot. Next boot generates a fresh key. (Doc TODO if anyone ever needs this in production-shaped form.)

## The flow

```c
static int load_or_generate_host_key(void) {
    uint8_t seed[32];
    int fd = sys_open("/etc/ssh_host_key");
    if (fd >= 0) {
        int n = sys_read(fd, seed, 32);
        sys_close(fd);
        if (n == 32) {
            ed25519_keypair_from_seed(g_host_pk, g_host_sk, seed);
            return 0;
        }
        /* present but malformed → regenerate */
    }
    rand_bytes(seed, 32);
    ed25519_keypair_from_seed(g_host_pk, g_host_sk, seed);
    if (sys_fs_write("/etc/ssh_host_key", seed, 32) < 0) {
        /* couldn't persist; use the in-memory keypair anyway,
         * warn the operator that the fingerprint will reset */
        return 0;
    }
    sys_chmod("/etc/ssh_host_key", 0600);
    sys_bcache_sync();
    return 0;
}
```

Five things to notice:

1. **Mode 0600.** The seed IS the private key. Mode 0644 (the AdventFS default for `fs_write` outputs) would let any process read it; 0600 restricts to the owner. `fs_check_perm` (session 48) enforces this on every `sys_open` from a non-root task — `[t36]` asserts the denial.

2. **`sys_bcache_sync()` after the write.** The block cache holds dirty sectors in RAM and flushes lazily (either when a slot is evicted or when a periodic syncer task wakes up). If QEMU is killed `-9` before the syncer fires, the file's metadata persists (its FS entry got written along with the superblock) but its data sectors don't. The explicit sync after write closes that window.

3. **Owner is whoever wrote it.** sshd is launched by init at boot, before any setuid; it runs as uid 0. So when `sys_fs_write` stamps the new entry's owner, that's 0:0 (session 47 file-ownership). The `[t36]` test pins this — non-root sshds couldn't write a 0:0 host key file.

4. **`ed25519_keypair_from_seed` is deterministic.** The 32-byte seed expands to the 64-byte expanded private key (seed + SHA-512(seed)-derived bits) and 32-byte public key in one shot. Two boots with the same seed produce bit-identical `g_host_pk` and `g_host_sk` — and therefore bit-identical signatures over identical exchange-hash inputs.

5. **`HOSTKEY_SEED` is gone.** The previous hardcoded 32-byte constant in `sshd.c` made every install of AdventOS use the same host key. That's a useful property for a tutorial but a footgun in any other context — first-boot generation is what real Unix sshd does for a reason. The new code path is the same shape (read-or-init-then-derive); just the source-of-truth is the disk instead of a `.text` constant.

## The gotcha that made this an actual project — qemu drives don't grow

I had the code working in a single boot — the file was written, `same-boot reread` returned 32 bytes — but every "second boot" of the same image showed `present but -1 bytes (need 32); regenerating`. File metadata persisted; file *data* didn't. Not a permissions issue: probe showed mode 0600 / owner 0, and `sys_open` returned a valid fd. The failure was inside `fs_iread_inst` itself.

After adding a kprintf to `inst_read_sector`, the diagnostic was clear:

```
[fs_iread] idx=44 type=1 size=32 start=2407 offset=0 n=32
[fs_iread] read_sector lba=2663 rc=-1
```

LBA 2663 came back with `-1`. Why? Because `os.img` was **exactly 2663 sectors long**:

```
$ ls -la os.img
-rw-r--r-- 1 ... 1363456 ... os.img    # = 2663 × 512
```

The kernel's FS bitmap allows 4096 sectors (session 51 bumped that). But `build.sh` was still padding `os.img` to `fs_lba + 2048 = 2304` sectors (the cap from session 46, which never got bumped to match the kernel side). The freshly-built image had room for the mkfs-shipped files but NOT for any runtime-created file whose data lands past sector 2303.

`sys_fs_write` for the host key:
1. Found a free slot in the bitmap (start_sector = 2407, well within the 4096-bit bitmap).
2. Updated the file entry in the superblock and called `inst_write_sector` for the data block at LBA `256 + 2407 = 2663`.
3. **Within QEMU**, the write went to QEMU's buffer cache (cache=writeback) and silently dropped because LBA 2663 is past the host file's EOF — QEMU doesn't auto-extend raw drives.
4. The bcache "thought" the write succeeded (no error from the layer below). The bcache flush did nothing for that sector because QEMU never gave back an error.
5. On boot 2, fs_init reads the superblock (within the 2663-sector window — survives), reconstructs the file entry table (sees `/etc/ssh_host_key` at start=2407), but trying to read LBA 2663 fails because reads beyond EOF return error.

The fix is two lines:

```bash
# build.sh
# was: final_size=$(( (fs_lba + 2048) * 512 ))
       final_size=$(( (fs_lba + 4096) * 512 ))
```

With the image padded to 4352 sectors, LBA 2663 is in-bounds. Writes go through. Reads on the next boot see the data. The persistence works.

The lesson: any time `FS_BITMAP_BYTES_MAX` in `kernel/fs.c` changes, the build-script padding for `os.img` has to track it. They're two views of the same number and silently disagreeing is exactly the kind of bug that survives unit tests (writes "succeed") and only shows up when something tries to read back from the FS after a power cycle.

## Verifying it manually

Two-boot fingerprint match, using the standard QEMU invocation (no special cache flag needed, default writeback works once the image is properly sized):

```bash
$ bash build.sh
$ rm os.img && bash build.sh    # fresh start

$ qemu-system-i386 -drive format=raw,file=os.img -serial stdio -display none \
    ... | grep "host key"
sshd: generated fresh host key (saved to /etc/ssh_host_key, mode 0600, 6 block(s) synced)
sshd: host key sha256: <some random 64-hex-char value>

$ qemu-system-i386 -drive format=raw,file=os.img ...   # SAME os.img
sshd: loaded host key from /etc/ssh_host_key
sshd: host key sha256: <same 64-hex-char value>
```

If the fingerprints differ between boots, the disk write didn't persist — start checking `os.img` size, padding, and the bcache_sync return value. (The session-50/51/52/53 SSH-2 wire stack on top of this isn't affected: client signatures are computed against whatever the current host_pk is, so a fingerprint change is invisible to the protocol — it just invalidates client-side `known_hosts` cache.)

## Files touched

```
user/sshd.c                +60 -10   load_or_generate_host_key + fingerprint helper
                                     deletes the baked-in HOSTKEY_SEED constant
user/sh.c                  +60       [t36] selftest
build.sh                   +5 -1     pad os.img to fs_lba + 4096 sectors
docs/54-host-key-persistence.md +new this file
```
