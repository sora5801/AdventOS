# Session 8 — AdventFS + ELF32 loader

**Goal:** Stop baking user programs into the kernel image. Put them on disk in a real format, parse the format at runtime, load programs the way an OS actually does it.

Up to here, "userspace" was a half-trick: user code lived inside `.up1`/`.up2` sections of the kernel binary, and `userprog` did `memcpy` of raw bytes into a page. There was no separation between "the kernel" and "the programs the kernel runs". This session draws that line.

## The minimum thing that counts as a filesystem + loader

- A disk layout the kernel can mount: superblock + named files at known sector ranges.
- A real ELF32 parser that reads program headers from disk, allocates pages per PT_LOAD segment, copies file content + zero-fills `.bss`.
- A host-side build tool that produces ELF binaries and packs them into a disk image.
- Shell commands that exercise it: `ls`, `cat`, `exec`.

Everything else (directories, write support, multiple PT_LOAD segments, dynamic linking, fragmentation, mount points) is intentionally absent.

## Disk layout

```
LBA 0           Boot sector (MBR)
LBA 1..N        Kernel image (loaded by bootloader at boot)
LBA 200         AdventFS superblock
LBA 201+        File data
```

200 is a round number well past the kernel (which currently lives at LBA 1..N where N≈80). The build pipeline pads `os.img` to 102,400 bytes (= 200 × 512) and appends the FS image.

Picking 200 is over-provisioning. The kernel currently fits in ~80 sectors and the bootloader DAP loads 112. We could push the FS down to 128 or even tighter, but the gap is free margin for kernel growth.

## Superblock

```c
struct fs_super {
    char            magic[8];         /* "ADVENTFS"            */
    uint32_t        file_count;
    struct fs_entry files[16];
} __attribute__((packed));

struct fs_entry {
    char     name[16];                /* NUL-padded            */
    uint32_t start_sector;            /* relative to LBA 200   */
    uint32_t size;                    /* bytes                 */
} __attribute__((packed));
```

8 + 4 + 16×24 = 396 bytes. Fits in one sector with room to spare.

`start_sector` is relative to the FS area. Sector 0 of the FS area is the superblock; files start at sector 1+. The kernel adds `FS_DISK_OFFSET_SECTORS` (= 200) when issuing the actual ATA read.

## Why a packed struct is enough

The on-disk layout and the in-memory C struct are bit-identical, packed, little-endian. `fs_init` does:

```c
ata_read_sector(FS_DISK_OFFSET_SECTORS, sec_buf);
memcpy(&g_super, sec_buf, sizeof(g_super));
```

No serialization layer, no field-by-field parsing. The Python `mkfs.py` writes bytes via `struct.pack('<I'...)` matching the same little-endian field order, which gives you the same bit pattern the C struct will read.

This works *only* because:
- We're on x86 (little-endian).
- The struct is `__attribute__((packed))` (no compiler-inserted alignment holes).
- We don't care about endianness portability.

The day we cross-compile this for ARM big-endian, we replace this with a real reader. Today we don't.

## fs_read across sector boundaries

Files don't have to be sector-multiples in size, and reads can start mid-file. `fs_read(idx, offset, buf, n)` walks one sector at a time, copies the relevant slice:

```c
while (n > 0) {
    uint32_t abs_off = offset + total;
    uint32_t lba     = FS_DISK_OFFSET_SECTORS + e->start_sector
                                              + (abs_off / 512);
    uint32_t off_in  = abs_off % 512;
    ata_read_sector(lba, sec_buf);
    uint32_t take = 512 - off_in;
    if (take > n) take = n;
    memcpy(out + total, sec_buf + off_in, take);
    total += take;  n -= take;
}
```

Each iteration reads one full sector into a stack buffer, copies the slice that lies inside `[offset, offset+n)`. O(N/512) ATA reads. The ELF loader does many small reads (header, then per-phdr, then per-segment-page) — every one of those goes through this.

A future optimization is to cache the last-read sector. We don't bother; ATA reads are microseconds in QEMU.

## ELF32 — the absolute minimum subset

```c
struct elf32_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));
```

The loader cares about exactly these fields:

- **From ehdr:** `ident[0..5]` (magic + class + endian), `machine`, `type`, `entry`, `phoff`, `phentsize`, `phnum`. Everything else is ignored.
- **From phdr:** `type` (only `PT_LOAD` matters), `offset`, `vaddr`, `filesz`, `memsz`, `flags`. `paddr` and `align` are not used.

We **don't** read section headers at all. Section headers are for the linker and debugger; the runtime loader only needs program headers.

## Loader algorithm

```
1. Read ELF header (52 bytes) from file offset 0.
2. Validate magic, class=ELFCLASS32, data=little-endian, machine=EM_386,
   type=ET_EXEC, phnum>0, phentsize>=32.
3. Allocate a fresh user page directory.
4. For each PT_LOAD program header:
     a. Reject vaddr below USER_MIN_VA (0x40000000).
     b. Reject filesz > memsz.
     c. Compute page span [align_down(vaddr), align_up(vaddr+memsz)].
     d. Build PTE flags from p_flags (USER always, WRITABLE if PF_W).
     e. For each page in the span:
          - pmm_alloc_page
          - memset 0  (handles .bss-equivalent zero-fill for free)
          - paging_map_in(user_pd, va, page, flags)
          - If this page overlaps [vaddr, vaddr+filesz):
              copy_va_start = max(va, vaddr)
              copy_va_end   = min(va+PAGE_SIZE, vaddr+filesz)
              fs_read(file_off=p_offset+(copy_va_start-vaddr),
                      dest=page+(copy_va_start-va),
                      len=copy_va_end-copy_va_start)
5. Allocate a one-page user stack at USER_STACK_VA (0x40100000).
6. Return { entry=eh.entry, cr3=user_pd_phys, user_esp=stack_top }.
```

The "memset 0 then read filesz bytes" pattern is how `.bss` works for free. Pages backing virtual addresses past `filesz` (but still within `memsz`) just stay zero — no separate code path.

The "copy_va_start/end" intersection logic handles partial-page overlap: if a 5000-byte segment spans two pages, the first page gets 4096 file bytes copied into it, the second gets 904 file bytes, and any tail (memsz > filesz) is already zero.

## Direct write to physical addresses

```c
void *page = pmm_alloc_page();          // returns physical address P
memset(page, 0, PAGE_SIZE);             // writes via P (kernel identity-map)
paging_map_in(user_pd, va, P, flags);   // user PD now maps va → P
fs_read(fd, file_off, page + offs, n);  // writes via P
```

The PMM returns physical addresses. The kernel's master PD identity-maps everything, so dereferencing `page` as a `void *` works. The user PD has the same physical page mapped at `va` (with USER), but the kernel only writes via the kernel mapping.

After all PT_LOADs are mapped, the user PD has user pages where the user task expects them. CR3 swap on the next `task_switch` makes them visible to ring 3.

## Error path: free what we mapped

Any failure mid-load (out of physical memory, failed map, short fs_read) calls `paging_destroy_user_pd(user_pd)` to roll back. That walks PDEs 8+ in the user PD, frees every PTE-mapped page, frees the PT, then the PD itself. From session 7. Without this we'd leak everything every time an `exec` failed.

## mkfs.py

The host-side tool. Three jobs:

1. **Extract `.up1` and `.up2` from `kernel/kernel.elf`.** Run `objcopy -O binary -j .up1 kernel/kernel.elf _tmp_up1.bin`. This dumps just the bytes of that section, no headers. Same for `.up2`.

2. **Wrap each in a minimal ELF32.** Hand-construct the 52-byte ELF header and 32-byte program header in Python via `struct.pack('<...')`. The PT_LOAD segment is at vaddr 0x40000000, filesz=memsz=len(code), flags=R+W+X. Code goes right after the headers.

   ```python
   ehdr = ident + struct.pack('<HHIIIIIHHHHHH',
       2, 3, 1, USER_VA, 52, 0, 0, 52, 32, 1, 0, 0, 0)
   phdr = struct.pack('<IIIIIIII',
       1, 84, USER_VA, USER_VA, len(code), len(code), 7, 0x1000)
   ```

   Total ELF size is 52 + 32 + len(code), e.g., 84 + 192 = 276 bytes for `hello.elf`.

3. **Build the FS image.** Pack the superblock (magic + file count + 16 entries), then concatenate each ELF padded to the next sector boundary. Write `fs.img`.

The build pipeline runs `python mkfs.py` after the kernel build, then appends `fs.img` to `os.img` at byte offset 102400.

## Why the same user_program bytes work after ELF load

`user_program_1` is naked + position-independent (`call 1f; pop %ebx; add $(msg-1b), %ebx`). Its byte sequence works at any virtual address. The session-7 path copied those bytes to a freshly allocated page mapped at 0x40000000. The session-8 path:

1. mkfs extracts the same bytes via objcopy.
2. mkfs wraps them in an ELF with PT_LOAD vaddr=0x40000000.
3. The kernel reads the ELF from disk, allocates a page, copies the same bytes in, maps at 0x40000000.

Result: the same bytes at the same virtual address, just delivered through a richer pipeline.

The proof: `exec hello.elf` produces byte-for-byte the same output as `userprog`:

```
Hello from ring 3! (pid=4)
...woke back up after yield. exiting.
[user task pid=4 exited code=0]
[reaper] freed pid=4 (user task), slot 4 now UNUSED
```

## Build pipeline

`build.sh` step list:

```
[1/6] compile C sources
[2/6] assemble
[3/6] link bootloader
[4/6] link kernel  →  kernel/kernel.elf, kernel/kernel.bin
[5/6] build disk image (boot + kernel)
[6/6] mkfs + append AdventFS at LBA 200
```

`os.img` total goes from 64 KB to 103 KB. The first 200 sectors are `boot.bin + kernel.bin + zero padding`; the rest is `fs.img`.

QEMU's `-drive format=raw,file=os.img` exposes this whole thing as the primary IDE master disk. The bootloader reads sectors 1..112 to get the kernel; the kernel later reads sector 200 for the superblock and 201..N for files. Same disk, two different access patterns.

## Files added

| File | Role |
|---|---|
| `kernel/fs.{c,h}` | Superblock + lookup + sector-walking read |
| `kernel/elf.{c,h}` | ELF32 ehdr/phdr structs + `elf_load` |
| `kernel/shell.c` | `ls`, `cat`, `exec` commands |
| `kernel/kernel.c` | `fs_init` after `ata_init` |
| `mkfs.py` | Build-time FS image generator |
| `build.sh`, `Makefile` | Step 6 = mkfs + append FS at LBA 200 |

## Design decisions

**On-disk format = packed C struct.** Costs portability, saves a lot of code. We accept little-endian-only and x86-only.

**`start_sector` is FS-relative, not disk-absolute.** Lets us move the FS area (`FS_DISK_OFFSET_SECTORS`) without rewriting every entry. Adding 200 once at access time is free.

**Single static superblock copy in kernel BSS.** Reading the superblock once at mount avoids re-issuing ATA reads for every `fs_open`. Tradeoff: file table is fixed at mount time; no live additions. We don't have writes anyway.

**No directories, no path separators.** Files are flat: `hello.elf`, `count.elf`. 16-character names. A real OS layers tree structure on top of an inode store; we just have the inode-equivalents.

**Read-only.** Adding write would mean: free-sector tracking on disk, sector-allocator, dirty-superblock writeback, fsck for crash recovery. Easy to add — but not the goal of "minimal".

**Hand-rolled ELF in mkfs.py.** Two reasons:
1. `mingw-w64` GCC doesn't have an `i386-elf` cross-compiler available in our toolchain. Producing a real ELF from C source would need a separate cross-toolchain or PE-to-ELF conversion via objcopy that produces program headers (which it doesn't).
2. The ELF we want is trivial — one segment, no relocations, no dynamic linking, no sections that the runtime cares about. A 36-line Python function emits exactly the bytes we need.

If we ever want to compile real C user programs as ELF, we either install a cross-compiler or do PE → custom-format conversion. For demonstration of the loader, the hand-rolled wrap is sufficient.

**`USER_MIN_VA = 0x40000000` enforced by loader.** Refuse ELFs that try to map below this. Otherwise a malicious or broken ELF could overlap the kernel-shared low PDEs. With this limit, all PT_LOAD segments land in PDE 256+, well clear of the kernel.

**User stack hard-coded at 0x40100000, single page.** No dynamic stack growth, no guard page, no `sbrk`. 4 KiB is enough for our user programs. Extending this means an `_start` runtime that asks for more pages; outside scope.

## What about `cat`?

`cat hello.elf` dumps the first 128 bytes hex+ASCII. Useful as a sanity check that the file content is what mkfs.py wrote:

```
hello.elf (276 bytes):
  0000: 7f 45 4c 46 01 01 01 00 00 00 00 00 00 00 00 00  .ELF............
  0010: 02 00 03 00 01 00 00 00 00 00 00 40 34 00 00 00  ...........@4...
  0020: 00 00 00 00 00 00 00 00 34 00 20 00 01 00 00 00  ........4. .....
  ...
```

Magic `7f 45 4c 46`, class=01 (ELFCLASS32), data=01 (LSB), version=01, OSABI=00, padding zeros, type=02 (ET_EXEC), machine=03 (EM_386), version=01, entry=`00 00 00 40` (0x40000000 LE), phoff=52, ... checks out.

## Test trace

```
[boot] mounting AdventFS... fs: AdventFS mounted, 2 files

advent> ls
Total 2 files on AdventFS:
  NAME             SIZE
  hello.elf        276 bytes
  count.elf        260 bytes

advent> exec hello.elf
exec: pid=4  cr3=0x0000b000  entry=0x40000000  esp=0x40101000  (loaded hello.elf)
Hello from ring 3! (pid=4)
...woke back up after yield. exiting.
[user task pid=4 exited code=0]
[reaper] freed pid=4 (user task), slot 4 now UNUSED

advent> exec count.elf
exec: pid=5  cr3=0x0000b000  entry=0x40000000  esp=0x40101000  (loaded count.elf)
Counter: 01234 (epoch=1778271578)
[user task pid=5 exited code=0]
[reaper] freed pid=5 (user task), slot 4 now UNUSED
```

`pid=5` reused slot 4 (and the same physical PD page 0x0000b000) because the reaper had finished reaping `pid=4` between the two `exec` calls.

## A non-bug we hit anyway: bursty input + UART FIFO

First test of `ls / cat / exec` had this:

```
advent> exec hello.elf
exec count.elf
exec: hello.elfexec count.elf: not found
```

Two commands sent in quick succession via `printf 'cmd1\n'; sleep 1; printf 'cmd2\n'`. The `\n` between them got dropped — UART RX FIFO is 16 bytes, host stuffs ~20 bytes worth into the kernel's queue before the IRQ handler can drain. Same class of bug as session 1's "echo hello AdventOS" truncation.

Workaround: type input one byte at a time with sleeps between. Not a kernel bug — a test-harness limitation. With real keyboard input (humans type slowly) this never matters.

## Deferred

- Multiple PT_LOAD segments (allocator is ready; just no program needs it yet)
- Dynamic linking (would need the dynamic section, GOT, PLT, ld.so equivalent)
- ELF section headers (we ignore them; never needed at runtime)
- File writes / mkfs from inside the running OS
- Directories, path separators
- More-than-16-files support
- Fork / exec replacement (we have spawn but no fork+exec)
- `argv` / `envp` to user programs (need to push them on the user stack before iret)

## Pitfalls

1. **PE/COFF section names ≤ 8 chars** is the recurring lesson. `mkfs.py` uses `objcopy -j .up1` and `-j .up2` — both 4 characters, safely under the limit. If we'd kept `.usrcode` we'd have only one extractable section.
2. **`__attribute__((packed))` on every on-disk struct.** Without it, the compiler aligns fields and the on-disk layout no longer matches the C struct after a `memcpy`.
3. **Endianness of `struct.pack` must match the struct's natural order.** All `<...` (little-endian, std sizes) in mkfs.py.
4. **`memset` first, then `fs_read filesz` bytes.** This is how `.bss` works in ELF, and it also means we don't have to track which page bytes the file covers vs which are zero-fill.
5. **Refuse low-VA PT_LOADs.** Without `USER_MIN_VA`, a hand-crafted ELF could map over the kernel's identity-mapped low PDEs.
6. **Reaper handles the cleanup.** `exec` doesn't have to do anything special when the user task exits — the same path that handles `userprog` cleanup handles `exec`-loaded tasks: SYS_EXIT → state=DEAD → reaper frees PD + stack.

## What "finishing the userspace path" really means now

Sessions 5 and 7 made user programs run, exit cleanly, and reuse resources. Session 8 made the programs themselves first-class artifacts: stored on disk, parsed at runtime, loadable by name. The kernel binary no longer ships its own programs; it ships a *loader*. Adding a new user program means writing it, getting it into `mkfs.py`, and rebuilding — no kernel changes needed.

The shape of an OS suddenly looks more like an OS.
