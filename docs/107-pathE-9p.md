# Session 120 — Path E phase 3: WSL build + virtio-9p host filesystem

**Goal.** Cross the long-deferred 9p gap. AdventOS now mounts a host
directory at `/mnt/9p` over virtio-9p / 9P2000.L. Includes a portable
build (WSL or MSYS2 → same source, same `os.img`).

Status: **done.** End-to-end verified on WSL Ubuntu 24.04 / QEMU 8.2.
The MSYS2 Windows path still builds cleanly and boots — the 9p
driver silently no-ops there because the Windows QEMU port lacks
virtio-9p support.

---

## Verification — what we ran

Host setup:
```
$ mkdir -p /tmp/9p-host
$ echo 'Hello from the WSL host!' > /tmp/9p-host/greeting.txt
$ mkdir -p /tmp/9p-host/sub
$ echo 'nested'                    > /tmp/9p-host/sub/nested.txt
```

QEMU CLI:
```
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 -smp 1 \
  -fsdev local,id=hostfs,path=/tmp/9p-host,security_model=none \
  -device virtio-9p-pci,fsdev=hostfs,mount_tag=hostshare,disable-modern=on \
  -display none
```

In the guest:
```
advent$ ls /mnt/9p
  sub
  foo.txt
  greeting.txt
advent$ cat /mnt/9p/greeting.txt
Hello from the WSL host!
advent$ cat /mnt/9p/sub/nested.txt
nested
```

Kernel boot log:
```
virtio-9p: PCI 0:4.0  io=0xc040  irq=11
virtio-9p: mount_tag="hostshare" (len=9)
9p: negotiated msize=8192 version=9P2000.L
virtio-9p: ready (root_fid=1)
vfs: mounted '9p' at /mnt/9p
virtio-9p: mounted at /mnt/9p
```

---

## Part 1 — WSL build path

The existing `build.sh` was MSYS2-only (mingw32 PE/COFF toolchain).
This session makes it auto-detect:

```bash
case "$(uname -s)" in
    Linux*)            TARGET_FORMAT=elf ;;
    MSYS*|MINGW*|CYGWIN*) TARGET_FORMAT=pe ;;
esac
```

Two things change between targets:
- **`ld -m` emulation.** PE wants `i386pe`, ELF wants `elf_i386`. A
  single `LD_EMUL` variable threads through all nine `ld` invocations.
- **`gcc -fleading-underscore` on ELF.** The hand-written assembly
  stubs (`entry.S`, `isr_stubs.S`, `task_switch.S`,
  `ap_trampoline.S`) reference C symbols with the PE-style `_kmain`
  / `_bss_start` underscore prefix. Native i386-elf gcc omits that
  prefix, so the assembly's `call _kmain` would fail to resolve.
  `-fleading-underscore` forces gcc to emit the underscores even
  under ELF, keeping the assembly portable.

The linker scripts (`linker_kernel.ld`, `linker_boot.ld`, `libc/libc.ld`,
`user/user.ld`) didn't need changes — they already use a section glob
(`*(.rdata*)` and `*(.rodata*)` in the same output section) that
catches both PE's `.rdata` and ELF's `.rodata`.

Two smaller WSL-isms:
- `python` → `python3` (Ubuntu ships only the latter by default). The
  script now picks whichever is on PATH.
- A `.gitattributes` enforces LF line endings on `*.sh` / `*.py` /
  Makefile / source files. Without it the `set -o pipefail` in
  `build.sh` failed in WSL because the CRLF made the shebang read
  `bash\r` instead of `bash`.

### Build smoke results

| host | TARGET_FORMAT | kernel.bin |
|--|--|--|
| WSL Ubuntu 24.04 | elf | 127150 bytes |
| MSYS2 ucrt64     | pe  | 135344 bytes |

The PE build is 8 KiB larger due to per-section padding overhead.

---

## Part 2 — virtio-9p driver

### Wire format

9P2000.L messages are `[size:4][type:1][tag:2][payload]`, all
little-endian. Strings are `[len:2][bytes:len]`. The qid (file
identity) is 13 bytes: `[type:1][version:4][path:8]`.

Implemented message types (subset of 9P2000.L):

| code | name | purpose |
|---|---|---|
| 100 / 101 | Tversion / Rversion | handshake; tag = `NOTAG` = `0xFFFF` |
| 104 / 105 | Tattach / Rattach | attach a fid to FS root |
| 110 / 111 | Twalk / Rwalk | resolve a path, allocate a new fid |
| 12 / 13 | Tlopen / Rlopen | open existing fid (Linux flags) |
| 116 / 117 | Tread / Rread | read at offset |
| 120 / 121 | Tclunk / Rclunk | close fid |
| 24 / 25 | Tgetattr / Rgetattr | stat-ish |
| 40 / 41 | Treaddir / Rreaddir | directory listing |
| 7 | Rlerror | error reply (any T-msg) |

### Transport

Single virtqueue. Each request occupies two descriptors:
- `desc[0]` = T-message, device READS (size = t_len)
- `desc[1]` = R-message, device WRITES (size = msize, negotiated 8 KiB)

Round-trip is fully synchronous: hold a per-device spin around
build + submit + `virtio_wait_used`. The new `hlt`-based yield from
session 118 means a single CPU isn't pegged while waiting.

### VFS integration

The VFS in AdventOS has no `close()` op on `struct vfs_fs_ops`, so
the 9p driver can't keep server-side fids alive across a
`SYS_OPEN` / `SYS_READ` / `SYS_CLOSE` sequence. Instead, every
high-level operation re-walks from `root_fid`:

```
op = "v9p_vfs_read"
  alloc temp_fid from 16-entry pool
  Twalk root_fid → temp_fid by path
  Tlopen temp_fid (O_RDONLY)
  Tread temp_fid offset count
  Tclunk temp_fid
  free temp_fid in pool
```

It's slower than a real Linux 9p client (which caches fids), but
it's correct and matches the VFS shape. The 4 KiB chunks `cat` and
`ls` use are well within `msize`, so a typical file read is one
`Twalk → Tlopen → Tread → Tclunk` round trip per buffer.

A new FD kind `FD_9P` was added to the task fd table so `SYS_READ`
dispatches into 9p instead of `fs_read_at`. The fd's `obj_idx`
indexes a 16-slot inode-cache (path + size + is_dir) that
`v9p_vfs_open` populates; `release_fd` frees the slot on `SYS_CLOSE`.

### What's not yet wired

- Write / create / unlink / mkdir / rename: `v9p_vfs_write_all` and
  `v9p_vfs_mkdir` return -1. The protocol pieces exist (Tlcreate,
  Twrite, Tunlinkat) — adding them is mechanical, deferred for
  scope.
- Multiple 9p mounts. Only one virtio-9p device is supported.
- Caching. Every VFS op is a fresh round trip.
- Async / pipelined requests. Tag = 0 everywhere, one outstanding
  request at a time.

---

## Bugs found mid-session

### BSS overflow once 9p code landed

The 9p driver added ~250 bytes of static state plus ~2.3 KiB of
inode-cache to `.bss`. That alone wouldn't have been a problem, but
the kernel image was already 64% of budget and the PE/COFF build
is 8 KiB larger than the ELF build (per-section padding overhead).
On Windows the build failed with:

```
ERROR: kernel .bss ends at 0xa1a08 — overlaps VGA RAM (0xA0000+)
```

The root cause was the PMM bitmap at `1 << 20` pages = 128 KiB
covering the entire 32-bit address space. The PMM has only ever
managed actual RAM (E820 USABLE regions); the upper 99% of the
bitmap is wasted on MMIO holes the allocator never touches anyway.
Shrunk `PMM_MAX_PAGES` to `1 << 16` (256 MiB / 8 KiB bitmap) which
recovers 120 KiB of BSS headroom — far more than the 9p code needed.

Tradeoff: the kernel now refuses to track RAM above 256 MiB. With
QEMU's default `-m 32` we use 32 MiB, so we have ~8× headroom. If
AdventOS ever wants `-m 256+`, bump back up.

The inode-cache itself was moved to a `kmalloc`'d pointer to keep
the v9p struct small, as belt-and-suspenders.

### Build script CRLF on WSL

The repo's working tree had been entirely CRLF-line-ending under
the MSYS2 git config that converts LF → CRLF on Windows checkout.
On WSL that broke `build.sh`: the shebang `#!/usr/bin/env bash\r`
made the kernel try to exec `bash\r` instead of `bash`, falling
back to `/bin/sh` which doesn't accept `set -o pipefail`.

Fixed by adding a `.gitattributes` that pins LF for the
file-types Linux executes (`*.sh`, `*.py`, `Makefile`, `*.ld`)
plus source code for portability. A one-time `sed -i 's/\r$//'`
on the working copy unblocked the local test.

---

## Files touched

New:
- `.gitattributes` — LF-only for shell scripts, Python, Makefile, source
- `kernel/virtio_9p.h` + `kernel/virtio_9p.c` — driver + 9P client
- `docs/107-pathE-9p.md` — this file

Modified:
- `build.sh` — `TARGET_FORMAT` auto-detect, `LD_EMUL` parameterization, `-fleading-underscore` for ELF, `python` → `python|python3`
- `kernel/virtio.h` — `VIRTIO_LEGACY_9P` device id constant
- `kernel/kernel.c` — `virtio_9p_init()` after vfs probe stack, `virtio_9p_mount("/mnt/9p")` after `vfs_init`
- `kernel/task.h` — new `FD_9P` enum value
- `kernel/syscall.c` — `case FD_9P` in `SYS_READ` and `release_fd`
- `kernel/pmm.c` — `PMM_MAX_PAGES`: 4 GiB → 256 MiB (saves 120 KiB of `.bss`)

kernel.bin (ELF / WSL): 123054 → 127150 bytes (+4 KiB).
kernel.bin (PE / MSYS2): 127152 → 135344 bytes (+8 KiB) — was 64%
of budget before this session, 69% after.

---

## Path E status after session 120

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read + readdir + getattr) + portable build

Still candidate (deferred):
- 9p write path (Tlcreate / Twrite / Tunlinkat / Tmkdir)
- IRQ-driven virtio completion (replace `hlt`-yielding poll loops with
  PCI INTx handlers; cuts RX latency from ~20 ms to microseconds)
- USB CDC-ECM (USB Ethernet — sister to CDC-ACM)
- virtio-scsi (multi-LUN block device)
- Real-hardware NIC: e1000 / e1000e
