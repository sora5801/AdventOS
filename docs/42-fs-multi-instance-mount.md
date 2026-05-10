# Session 42 — Finishing the "mount it" promise: multi-instance AdventFS, USB mounted at /mnt/usb

**Goal:** finish what session 41 deferred — make `cat /mnt/usb/readme.txt` actually work. Session 41 added USB Mass Storage end-to-end (UHCI bulk transfers, BOT, SCSI READ(10) / WRITE(10), the `blkdev` vtable, raw `sys_block_*` syscalls), but the file-system layer still hardcoded the boot disk as a singleton — `g_super`, `g_initialized`, and the global sector bitmap in `kernel/fs.c` plus the bcache→ATA call chain meant only one AdventFS could exist at a time. This session refactors `fs.c` into a multi-instance design, threads instance ownership through VFS into the file-descriptor table, and mounts the USB drive's AdventFS at `/mnt/usb` so the existing `cat`, `ls`, `cat /proc/mounts` all just work across the mount boundary.

End state — boot trace:

```
[boot] mounting AdventFS... fs: instance(bdev=boot, base=256): 37 entries, 1024 sectors visible
[boot] mounting VFS... vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
...
[usb] addr 1: MSC iface=0  ep_in=IN1  ep_out=OUT2  max=64
[msc] addr 1  vendor="QEMU"  product="QEMU HARDDISK"
[msc] addr 1  capacity = 512 blocks * 512 B = 256 KiB
[msc] registered as blkdev[1] = usb0
fs: instance(bdev=usb0, base=0): 1 entries, 512 sectors visible
vfs: mounted 'usbfs' at /mnt/usb
[boot] mounted usb0 at /mnt/usb
```

And the t28 selftest tail:

```
[t28] USB Mass Storage: SCSI READ/WRITE round-trip via blkdev
== usbtest ==
  PASS  AdventFS superblock magic on USB sector 0
  PASS  USB write+read round-trip (sector 100, 512B pattern)
  PASS  multi-block read (4 sectors at once)
  usbtest exit code = 0

  --- AdventFS mounted at /mnt/usb (via VFS) ---
  ls /mnt/usb:
readme.txt

  cat /mnt/usb/readme.txt:
Hello from a USB Mass Storage device!

QEMU exposes this file via:
    -drive id=usbfs,file=usbfs.img,format=raw,if=none
    -device usb-storage,drive=usbfs

AdventOS's UHCI driver enumerates the device, the Bulk-Only
Transport layer wraps SCSI commands, and the new sys_block_*
syscalls expose block-level access to user space.

  cat /proc/mounts:
/ rootfs
/proc procfs
/mnt/usb usbfs
```

`cat`, `ls`, and `/proc/mounts` had zero changes. The whole chain from the user-space binary's `open("/mnt/usb/readme.txt")` syscall down to the SCSI READ(10) on the USB drive flows through the existing `vfs_open` → `rootfs_open` → `fs_iread_inst` → `inst_read_sector` → `blkdev->read` → `usb_msc.scsi_read10` → `uhci_bulk_in` pipeline because every layer is now instance-aware.

## What's in scope

In:

- **`kernel/fs.c` rewrite** — file-scope globals (`g_super`, `g_initialized`, `g_bitmap`) replaced by `struct fs_instance` with the same three fields. A static array `g_instances[FS_INSTANCE_MAX=4]` holds the slots; `g_root_inst` points at the boot disk's slot. Every existing fs operation got a per-instance variant (`fs_iopen_inst`, `fs_iread_inst`, `fs_imkdir_inst`, `fs_iwrite_all_inst`, `fs_idir_iter_inst`); the public singleton API (`fs_open`, `fs_read`, `fs_mkdir`, `fs_write_all`, `fs_dir_iter`) is now thin wrappers around `&g_root_inst`.
- **`kernel/fs.h`** — new public surface: `struct fs_instance` (opaque), `fs_create_instance(blkdev*, base_lba, n_sectors)`, `fs_root_instance()`, `fs_make_ops_for(inst)`, `fs_read_at(fs_data, idx, off, buf, n)` (used by SYS_READ).
- **Per-instance block I/O dispatch** — `inst_read_sector` / `inst_write_sector` checks `inst->bdev`. The boot fs has `bdev == NULL` and goes through the existing `bcache_read` / `bcache_write` (which still hits ATA via `ata_*_sector` — no change to the boot path). USB instances have `bdev = &usb_msc.bdev` and call `bdev->read/write` directly (no caching, fine for the demo).
- **`kernel/vfs.h`** — `struct vfs_fs_ops` gained a `void *fs_data` field, and every op signature got a `void *fs_data` first argument. `struct vfs_inode` gained a matching `void *fs_data` field that ops set on success — `vfs_open` defaults it to NULL and `rootfs_open` overwrites it with the instance pointer.
- **`kernel/vfs.c`** — every dispatch site now passes `m->ops->fs_data` to the op function.
- **`kernel/procfs.c`** — three op functions got the `void *fs_data` first arg (ignored — procfs is single-instance).
- **`kernel/task.h`** — `struct task_fd` gained a `void *fs_data` field. SYS_OPEN copies `vfs_inode.fs_data → task_fd.fs_data`; SYS_READ for FD_FS calls `fs_read_at(e->fs_data, ...)` so the read goes to the right instance.
- **`kernel/kernel.c`** — after `usb_start_polling()`, walks the `blkdev` table looking for the first `usb*` device, calls `fs_create_instance(b, 0, b->n_blocks)` to mount its AdventFS, and `vfs_mount("/mnt/usb", "usbfs", ...)` to expose it.
- **`mkfs.py`** — adds `mnt` to `DIRECTORIES` so the rootfs has an actual `/mnt` entry. Not strictly required for `/mnt/usb/...` paths to resolve (VFS doesn't check that the mount point exists in the parent fs), but it makes `ls /` show `mnt` alongside `etc`, which is the convention.
- **`user/sh.c`** — `[t28]` selftest extended with `ls /mnt/usb`, `cat /mnt/usb/readme.txt`, and `cat /proc/mounts` after the existing `usbtest` round-trip. Demonstrates the real-userspace path with the unmodified `cat` and `ls` binaries.

Out — deferred to follow-up sessions:

- **bcache for non-boot mounts.** The USB instance reads and writes go directly to the SCSI bulk pipe with no caching — fine for `cat /mnt/usb/readme.txt` (one open, a couple of sequential reads), but a full-bore workload would benefit from the same LRU cache the boot fs has. Easiest fix: tag bcache entries with their owning blkdev pointer in addition to the LBA, and add `bcache_read(blkdev*, lba, ...)` / `bcache_write(blkdev*, lba, ...)`. The existing bcache_read/write become wrappers that pass `g_ata_blkdev`. About 60 LOC.
- **chdir into a non-rootfs mount.** Right now `task->cwd_dir` is an entry index into the boot fs's superblock — it carries no instance pointer, so cwd-relative paths only resolve against the boot disk. `cd /mnt/usb` works as far as the path resolution goes, but writes and reads relative to that cwd would silently target the boot fs. Fix: extend `cwd_dir` into a `(fs_instance *, entry_idx)` pair. Touches every `task_current()->cwd_dir` reader plus fork/exec inheritance. About 80 LOC.
- **USB write-back.** `fs_iwrite_all_inst` works on the USB instance, but the t28 selftest doesn't exercise it because there's no convenient user-space "write a file" syscall yet (`>` writes to tmpfs, not real fs; `ed` exists but its scripted-input path is fragile). Easy add later.
- **Hot-plug remount.** USB enumeration runs once at boot. A drive plugged in afterward isn't detected — we'd need the connect-status-change interrupt + a re-enumerate path. Per session 40's deferred list.

## Architecture

```
   /mnt/usb/readme.txt              /                 /proc/mounts
        │                            │                     │
        └──────────────┐  ┌──────────┘                     │
                       ▼  ▼                                │
              ┌─────────────────────────────────┐          │
              │  vfs_open(path)                 │          │
              │    longest-prefix match over    │          │
              │    g_mounts[]:                  │          │
              │      "/"        rootfs          │          │
              │      "/proc"    procfs          │          │
              │      "/mnt/usb" usbfs ◄─────────┼─NEW─────┐│
              │    out->fs_data = inst pointer  │         ││
              └─────────┬───────────────────────┘         ││
                        │                                 ││
                        ▼                                 ││
              ┌─────────────────────────────────┐         ││
              │  rootfs_open(fs_data, rel)      │         ││
              │    inst = fs_data ?: g_root_inst│         ││
              │    fs_iopen_inst(inst, rel)     │         ││
              │    out->fs_data = inst          │         ││
              └─────────┬───────────────────────┘         ││
                        │                                 ││
                        ▼                                 ││
              ┌─────────────────────────────────┐         ││
              │  SYS_READ:                      │         ││
              │    fs_read_at(fd->fs_data, ...) │         ││
              │      → fs_iread_inst(inst, ...) │         ││
              │      → inst_read_sector(...)    │         ││
              │          ┌──────┴──────┐         │         ││
              │          │bdev == NULL?│         │         ││
              │          │   yes  no   │         │         ││
              │          ▼             ▼         │         ││
              │        bcache_read   bdev->read  │         ││
              │            ↓             ↓       │         ││
              │       ata_sector     usb_msc /   │         ││
              │                      SCSI/UHCI   │         ││
              └─────────────────────────────────┘         ││
                                                          ││
   The path that opens the readme.txt goes:               ││
   1. cat opens "/mnt/usb/readme.txt"                     ││
   2. vfs_open finds usbfs mount, strips "/mnt/usb"       ││
   3. rootfs_open(fs_data=uinst, "readme.txt") → idx 0    ││
   4. fd table: kind=FD_FS, obj_idx=0, fs_data=uinst      ││
   5. cat reads → SYS_READ → fs_read_at(uinst, 0, ...)    ││
   6. fs_iread_inst routes through inst_read_sector       ││
   7. inst->bdev != NULL → bdev->read(usb0, lba, 1, buf)  ││
   8. SCSI READ(10) over UHCI bulk-IN pipe                ││
                                                          ││
   The /proc/mounts read goes through procfs, which       ││
   walks g_mounts[] and prints mount_point + fs_name. ────┴┘
```

## What to thread, where

The refactor is structurally simple: take all the per-fs state and put it behind a struct pointer. The work is in the *threading* — making sure the instance pointer flows through every layer from path resolution to disk I/O. Five places carry it:

1. **`vfs_fs_ops`** has a new `void *fs_data` field. The ops table for the boot fs has `fs_data = NULL` (means "use g_root_inst"); the ops table for `/mnt/usb` has `fs_data = &g_instances[1]`.
2. **VFS dispatch** (`vfs_open`, `vfs_readdir`, `vfs_mkdir`, `vfs_write_all`) reads `m->ops->fs_data` and passes it as the first argument to the op.
3. **Per-fs op functions** (`rootfs_open`, `rootfs_read`, `rootfs_readdir`, `rootfs_mkdir`, `rootfs_write_all`) cast it back to `struct fs_instance *` and route the work to that instance.
4. **`vfs_inode`** has a matching `void *fs_data` field. `rootfs_open` writes the inst pointer into `out->fs_data` so the caller (SYS_OPEN) can preserve it across the open→read boundary.
5. **`task_fd`** has a matching `void *fs_data` field. SYS_OPEN copies `vfs_inode.fs_data → task_fd.fs_data`; SYS_READ on FD_FS reads it back and calls `fs_read_at(e->fs_data, ...)` which dispatches to the right instance.

The only "interesting" code is the per-instance block-I/O switch:

```c
static int inst_read_sector(struct fs_instance *inst, uint32_t lba, void *buf) {
    if (inst->bdev) {
        /* USB or any other future blkdev — bypass the cache. */
        return inst->bdev->read(inst->bdev, lba, 1, buf);
    }
    /* Boot fs — use the existing LRU cache + ATA path. */
    return bcache_read(lba, buf);
}
```

That single function preserves the boot path completely (bcache + ata, untouched) while letting USB MSC plug in cleanly. Adding a third blkdev (a future SD card driver, say) needs zero changes here — the dispatch is already there.

## Path resolution: cwd, root, and the relative-path trap

The original `fs_open` had a feature: bare names like `"readme.txt"` resolve against `task_current()->cwd_dir`. That works for the boot fs where `cwd_dir` is meaningful, but VFS-passed mount-relative paths shouldn't go through cwd at all — when `cat /mnt/usb/readme.txt` becomes `rootfs_open(uinst, "readme.txt")`, the `"readme.txt"` is relative to the USB fs *root*, not relative to whatever directory the calling task is in.

Fix: every per-instance walker takes a `from_root` flag. The public singleton API (`fs_open(path)`) calls with `from_root=0` (preserve cwd semantics for kernel-internal callers like `dyld` and `elf` that open `/lib/libc.bin` etc.), but the VFS adapter (`rootfs_open`) calls with `from_root=1`. So the same `"foo"` argument resolves against cwd when an in-kernel caller asks, and against the fs root when VFS asks. Bonus: it also fires for the boot instance accessed via VFS — `vfs_open("foo")` from a userspace `open("foo")` still has the right root-relative semantics.

## Why fs.c is now ~580 LOC instead of ~470

A cleaner-but-more-invasive design would have been to make every public fs API take `struct fs_instance *` as the first argument, deleting the singleton entirely. The cost would be touching every `fs_open`, `fs_read`, etc. caller in the kernel — `kernel/dyld.c` (loads libc.bin), `kernel/elf.c` (loads user binaries), `kernel/syscall.c` (chdir, getcwd), `kernel/kernel.c` (LAUNCH macro), shell built-ins. ~30 call sites total. Mechanical but real.

The middle path I took: keep the singleton API for everyone who only cares about the boot fs (which is everyone except VFS and the new USB mount), and add per-instance functions for everyone who cares about which fs. The wrappers are five lines each:

```c
int fs_open(const char *path) {
    return fs_iopen_inst(g_root_inst, path, /*from_root=*/0);
}
int fs_read(int idx, uint32_t offset, void *buf, uint32_t n) {
    return fs_iread_inst(g_root_inst, idx, offset, buf, n);
}
```

The whole point of the refactor was that *VFS* could mount two AdventFS instances, not that the elf loader needed to know which fs the kernel was running on. Worth the duplication.

## VFS-inode and task_fd round-tripping

The flow:

```
user: open("/mnt/usb/readme.txt")
  → SYS_OPEN
      struct vfs_inode ino = { .fs_data = NULL };  /* default in vfs.c */
      vfs_open(name, &ino)
        → resolve longest mount = "/mnt/usb" mount → strip prefix → "readme.txt"
        → m->ops->open(m->ops->fs_data, "readme.txt", &ino)
            (m->ops->fs_data is &g_instances[1] for usbfs)
          → rootfs_open writes ino.fs_data = &g_instances[1]
                            ino.kind = FD_FS, ino.obj_idx = 0
      t->fds[fd].kind    = FD_FS
      t->fds[fd].obj_idx = 0
      t->fds[fd].fs_data = &g_instances[1]   /* NEW */

user: read(fd, buf, n)
  → SYS_READ → kind == FD_FS
      → fs_read_at(e->fs_data /* = &g_instances[1] */, e->obj_idx /* 0 */, ...)
          → fs_iread_inst(&g_instances[1], 0, off, buf, n)
              → inst_read_sector → inst->bdev->read → SCSI READ(10) → done
```

The fd table now carries the instance pointer alongside the entry index. Because fork copies the whole `task_fd` struct verbatim, an fd opened before `fork()` keeps pointing at the right instance after fork. exec resets fds, which is fine — exec'd binaries open their own files.

## Test results

After all of the above, with `-device piix3-usb-uhci -drive id=usbfs,file=usbfs.img,format=raw,if=none -device usb-storage,drive=usbfs,bus=usb0.0,port=1`:

```
fs: instance(bdev=boot, base=256): 37 entries, 1024 sectors visible
vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
fs: instance(bdev=usb0, base=0): 1 entries, 512 sectors visible
vfs: mounted 'usbfs' at /mnt/usb
[boot] mounted usb0 at /mnt/usb

[t26] cryptotest: 19 passed, 0 failed     ← TLS / crypto unaffected
  httpsget exit code = 0                   ← TLS handshake unaffected
  usbtest exit code = 0                    ← session 41 SCSI tests pass

  --- AdventFS mounted at /mnt/usb (via VFS) ---
  ls /mnt/usb:
readme.txt
  cat /mnt/usb/readme.txt:
Hello from a USB Mass Storage device!
... (entire 338-byte file) ...

  cat /proc/mounts:
/ rootfs
/proc procfs
/mnt/usb usbfs
```

Three plain user-space binaries (`ls`, `cat`, `cat /proc/mounts`) running under the unmodified syscall layer transparently see the USB drive's filesystem at `/mnt/usb`, and the crypto/network/USB-block-device tests from sessions 36-41 keep passing — confirming the refactor didn't break anything below or beside the FS layer.

## What's next

- **bcache per-blkdev** so USB reads get cached too. ~60 LOC.
- **chdir/cwd that crosses mounts** — extend `task->cwd_dir` into a (fs_instance, idx) pair. ~80 LOC. Lets `cd /mnt/usb; ls; cat readme.txt` work as expected.
- **USB hot-plug** so a drive plugged in after boot becomes available. Needs the connect-status-change interrupt path that's also blocked on hub support — same mechanism, recyclable.
- **A real `umount` syscall.** Right now mounts are forever. `umount /mnt/usb` would let the user safely yank the (virtual) drive.
