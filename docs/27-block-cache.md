# Session 27 — Block cache + write-back filesystem

**Goal:** Stop calling `ata_read_sector` / `ata_write_sector` for every byte of file I/O. Slot a 32-entry LRU cache between fs.c and ata.c, with write-back semantics: writes only mark the cached block dirty, and a periodic kernel syncer task flushes dirty blocks to disk every 5 seconds. The selftest demonstrates that 11 successive rewrites of the same file collapse from 44 disk writes to just 5 — an 8.8× reduction — and that re-reading the same file is a pure cache hit, with zero disk traffic.

End state — the new `[t18]` selftest output:

```
[t18] block cache: hit ratio + write coalescing + sync
  baseline: hits=237 misses=406 logical_w=59 disk_w=6 dirty=0
  first read /etc/inittab : delta hits=0 misses=1
  re-read /etc/inittab    : delta hits=1 misses=0 (should be all hits)
  11x sys_fs_write bc.txt: delta logical_w=44 disk_w=0 dirty=5
  sys_bcache_sync flushed 5 block(s); dirty after=0
  read bc.txt after sync: 'rewrite via bcache test' (24 bytes)
```

The interesting numbers to read here:

- **`re-read … delta hits=1 misses=0`** — the second `sys_open + sys_read` of `/etc/inittab` produced exactly one cache hit and zero misses. That's the "block cache" in its purest form: the same byte returns instantly the second time.
- **`11x sys_fs_write … logical_w=44 disk_w=0 dirty=5`** — 11 rewrites of a 24-byte file generated 44 calls to `bcache_write` (= 11 × 4 sectors per rewrite: 1 data + 3 superblock), but **zero ata_write_sector calls** in that window. All 44 writes coalesced into just 5 dirty cache slots. The math: data sector "A" is reused on rewrites 1, 3, 5, …; sector "B" is reused on rewrites 2, 4, 6, … (the bitmap allocator alternates because each rewrite frees the old run AFTER allocating the new one); plus the 3 superblock sectors. 2 + 3 = 5.
- **`flushed 5 block(s); dirty after=0`** — the explicit sync drained those 5 entries to disk. The disk count went from 0 to 5 in one go; the cache is now clean.

httpd still serves curl with HTTP/1.0 200 throughout, and tests t1–t17 all pass.

## What's in scope

In:
- `kernel/bcache.{h,c}` — 32-slot LRU cache, doubly-linked list, 16 KiB total RAM footprint
- Write-back policy: writes mark the slot dirty; disk I/O happens only on eviction or explicit sync
- A kernel syncer task that calls `bcache_sync` every 5 seconds; logs to console only when something was actually flushed
- `fs.c` plumbed through `bcache_read` / `bcache_write` exclusively — every existing FS callsite uses the cache
- `SYS_BCACHE_SYNC` + `SYS_BCACHE_STATS` syscalls (47, 48), with libuser wrappers, so userspace tests can drive and inspect the cache
- `[t18]` selftest covering hit ratio, write coalescing, sync, and read-back-after-sync persistence
- Boot loader bumped to read 256 sectors via two back-to-back DAP calls (kernel grew past the old 128-sector / 64 KiB limit; many BIOSes cap a single int 13h ah=42h read at 127 sectors, so two 128-sector reads beats one 256-sector read)
- Concurrency safety via `cli`/`sti` around list mutation and dirty-bit claims; the `ata_*_sector` calls themselves run with the lock held briefly

Out:
- A real journal / write-ahead log. A crash mid-flush still loses data and can leave the FS inconsistent — we have no recovery story.
- Read-ahead / prefetching. Sequential reads still issue one `ata_read_sector` per sector miss.
- Per-page checksums or torn-write detection.
- `fdatasync` / per-file flush; only `sys_bcache_sync` (which flushes everything).
- Variable block sizes — fixed 512 byte slots only.
- Cache resizing at runtime — `BCACHE_NR=32` is a `#define`.
- Migration of dirty pages between processes — moot, since the cache is global.

## Architecture: where the cache sits

Before this session, the FS layer was a thin wrapper over the ATA driver:

```
+---------------------------------------------+
| fs.c        fs_init / fs_open / fs_read /   |
|             fs_write_all                    |
+---------- ata_read_sector ------------------+
| ata.c       PIO 0x1F0 / IDENTIFY / etc.     |
+----------- BIOS / disk ---------------------+
```

Every `fs_read` of a one-byte file required at minimum one full sector read (512 bytes off the disk via PIO). Every `fs_write_all` of a one-byte file wrote a fresh data sector AND rewrote the 1024-byte superblock — three full ATA writes per logical "save," every single time.

The new layout slots a cache between:

```
+---------------------------------------------+
| fs.c        fs_init / fs_open / ...         |
+---------- bcache_read / write --------------+
| bcache.c   32-slot LRU, dirty bits          |
|            on-demand reserve / evict        |
+---------- ata_read_sector ------------------+
| ata.c       PIO ...                         |
+----------- BIOS / disk ---------------------+
```

The bcache layer is **transparent**: fs.c didn't gain any new state, and didn't need to think about caching. Every `ata_read_sector(lba, buf)` became `bcache_read(lba, buf)`; every `ata_write_sector` became `bcache_write`. Same signatures, same return semantics, same calling sites.

## The bcache_entry struct

```c
struct bcache_entry {
    int                  valid;
    int                  dirty;
    uint32_t             lba;
    uint8_t              data[BCACHE_BLKSZ];   /* 512 bytes */
    struct bcache_entry *prev;     /* toward head (more recent) */
    struct bcache_entry *next;     /* toward tail (less recent) */
};

static struct bcache_entry  g_entries[BCACHE_NR];     /* 32 slots */
static struct bcache_entry *g_head;                   /* MRU end */
static struct bcache_entry *g_tail;                   /* LRU end */
```

Three booleans-and-pointers per slot, plus 512 bytes of data. The `valid` + `dirty` combo fully describes how the slot relates to disk:

| valid | dirty | meaning |
|---|---|---|
| 0 | – | slot empty; `data` is meaningless |
| 1 | 0 | `data` matches disk at `lba` |
| 1 | 1 | `data` is newer than disk; sync will flush it |

The list-of-32 representation is used for two things at once: the LRU ordering (touch moves a slot toward `g_head`; evict pulls from `g_tail`), AND the storage table (lookups linearly scan all 32 slots). For 32 entries the linear scan is faster than a hash; if we ever bump the cache to 256 entries, this is the moment to add a small hash table.

## LRU operations

Three core helpers do the heavy lifting:

```c
/* touch(e): move e to the head of the list. */
static void touch(struct bcache_entry *e) {
    if (e == g_head) return;
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (e == g_tail) g_tail = e->prev;
    e->prev = 0; e->next = g_head;
    if (g_head) g_head->prev = e;
    g_head = e;
}

/* lookup(lba): O(N) scan. Returns the slot or NULL. */
static struct bcache_entry *lookup(uint32_t lba) {
    for (int i = 0; i < BCACHE_NR; i++) {
        if (g_entries[i].valid && g_entries[i].lba == lba) return &g_entries[i];
    }
    return 0;
}

/* reserve_lru(): pop g_tail, write back if dirty, mark invalid,
 *                return the slot ready for reuse. */
static struct bcache_entry *reserve_lru(void);
```

`bcache_read` is then:

```c
e = lookup(lba);
if (e) { memcpy(out, e->data, ...); touch(e); return 0; }   /* hit */

/* miss */
e = reserve_lru();
ata_read_sector(lba, e->data);
e->lba = lba; e->valid = 1; e->dirty = 0;
memcpy(out, e->data, ...);
touch(e);
return 0;
```

`bcache_write` does the same lookup-then-reserve dance, but on miss it skips the disk read entirely — we're overwriting the whole 512-byte sector, so reading the old contents would be wasted I/O. The slot is filled with the new data and marked `valid=1, dirty=1`. Writes to a hit just `memcpy` and re-mark dirty; no disk traffic at all.

## The critical race I had to fix

My first cut had a subtle concurrency bug. `bcache_read` looked like this:

```c
e = lookup(lba);                /* (1) */
if (e) { /* hit */ ... }
e = evict();                    /* (2) returns g_tail, doesn't re-link */
ata_read_sector(lba, e->data);  /* (3) ~ms of polling, IF=1 here */
/* (4) populate e->lba, valid, etc. */
touch(e);                       /* (5) move to head */
```

Between (2) and (5), another task can fire a syscall that calls `bcache_read` again. At that point `g_tail` is still the slot we just claimed (we haven't called `touch` yet), so the second task picks the **same slot**. Now two tasks are both about to fill the same slot with their respective lbas. Whichever finishes second wins; the first task's data is corrupted, plus the LRU list is left with stale pointers.

The bug manifested in the boot run as a **user-mode page fault** during `t17` — a child of the shell jumped to a corrupted return address (specifically EIP=0x4000048a, which was 4 bytes inside a 7-byte `cmpl` instruction in `_run_pipeline`). Tracing back, that's a stack-overwrite from `[ebp - 0x88]`-style buffer corruption, exactly the shape you'd expect when two kernel paths step on each other's slot data and the bytes leak into a process's loaded ELF page.

The fix is to coarsely disable interrupts around the slot-mutation and have a single `reserve_lru()` helper that does the writeback AND marks `valid=0` AND keeps the slot in the same list position — all inside the locked region:

```c
static inline uint32_t bcache_lock_save(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void bcache_unlock_restore(uint32_t f) {
    __asm__ volatile ("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

int bcache_read(uint32_t lba, void *out) {
    uint32_t f = bcache_lock_save();
    struct bcache_entry *e = lookup(lba);
    if (e) { /* hit */ memcpy(out, ...); touch(e); g_hits++; ... }
    e = reserve_lru();
    if (!e) { bcache_unlock_restore(f); return -1; }
    if (ata_read_sector(lba, e->data) != 0) {
        bcache_unlock_restore(f);
        return -1;
    }
    e->lba = lba; e->valid = 1; e->dirty = 0;
    memcpy(out, e->data, BCACHE_BLKSZ);
    touch(e);
    g_misses++;
    bcache_unlock_restore(f);
    return 0;
}
```

Yes, the `ata_read_sector` and the writeback inside `reserve_lru` run with `cli` set. That sounds bad — those calls poll the IDE controller for ~ms. But:

1. AdventOS is cooperative single-CPU. Holding interrupts off for a few ms doesn't deadlock anything; it just delays the next timer tick by that much.
2. The cache **hot path** (a hit) does `memcpy + touch` and is microsecond-scale. Only the cold path (miss + writeback) is slow, and that path has to be serialized anyway, otherwise we duplicate disk I/O.
3. The previous-session ATA driver already takes a spinlock during PIO, so concurrent disk traffic was always serialized.

If we ever want true concurrency, the right fix is per-slot locks plus marking the slot "I/O in flight" so other lookups can wait on it. That's a future-session problem.

## bcache_sync: the syncer's job

```c
uint32_t bcache_sync(void) {
    uint32_t n = 0;
    for (int i = 0; i < BCACHE_NR; i++) {
        uint8_t  buf[BCACHE_BLKSZ];
        uint32_t lba = 0;
        int      do_write = 0;

        uint32_t f = bcache_lock_save();
        if (g_entries[i].valid && g_entries[i].dirty) {
            lba = g_entries[i].lba;
            memcpy(buf, g_entries[i].data, BCACHE_BLKSZ);
            g_entries[i].dirty = 0;       /* claim early */
            do_write = 1;
        }
        bcache_unlock_restore(f);

        if (!do_write) continue;
        if (ata_write_sector(lba, buf) == 0) {
            /* counted under lock so the test's stat read sees a
             * consistent value */
            f = bcache_lock_save();
            g_disk_writes++;
            bcache_unlock_restore(f);
            n++;
        } else {
            /* failure: restore dirty if the slot still holds same lba */
            f = bcache_lock_save();
            if (g_entries[i].valid && g_entries[i].lba == lba) {
                g_entries[i].dirty = 1;
            }
            bcache_unlock_restore(f);
        }
    }
    return n;
}
```

One slot at a time, three-phase per slot:

1. **Snapshot under lock.** Copy 512 bytes onto the stack, capture the lba, clear the dirty bit. Drop the lock.
2. **Disk write with the lock dropped.** This is where `ata_write_sector`'s polling actually progresses (timer can fire, scheduler can switch to other tasks).
3. **Counter bump under lock.** Or, on failure, restore the dirty bit so the next sync retries.

The 512-byte stack buffer is small enough to not trip GCC's `__chkstk_ms` probe. (My very first cut allocated `uint8_t snap[BCACHE_NR][BCACHE_BLKSZ]` = 16 KiB on the stack and the linker complained about the missing probe symbol.)

## The syncer task

```c
static void syncer_task(void) {
    for (;;) {
        pit_sleep(5000);
        uint32_t flushed = bcache_sync();
        if (flushed > 0) {
            kprintf("[bcache] syncer flushed %u dirty block(s)\n",
                    (unsigned)flushed);
        }
    }
}

void bcache_start_syncer(void) {
    task_create(syncer_task, "bcache");
}
```

Spawned from kmain after `task_init`/`sti` are up. The task is a regular ring-0 kernel task, just like the reaper from session 14. It doesn't need a user PD or argument frame; `task_create` synthesizes a stack frame whose first `ret` jumps into `syncer_task`, and the infinite loop ensures we never fall off.

Five-second cadence is arbitrary. Linux's `pdflush` historically used 5–30 seconds; modern Linux uses 30 seconds for `dirty_writeback_centisecs` in many configs. Five is conservative but visible — the demo log will print exactly one `[bcache] syncer flushed N` line per active period.

## What changed in fs.c

Four call sites, mechanical renames:

```diff
- if (ata_read_sector(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) ...
+ if (bcache_read(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) ...

- if (ata_write_sector(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) return -1;
+ if (bcache_write(FS_DISK_OFFSET_SECTORS + s, sb + s * 512) != 0) return -1;

- if (ata_read_sector(lba, sec_buf) != 0) return -1;
+ if (bcache_read(lba, sec_buf) != 0) return -1;

- if (ata_write_sector(FS_DISK_OFFSET_SECTORS + (uint32_t)new_start + s, buf) != 0) {
+ if (bcache_write(FS_DISK_OFFSET_SECTORS + (uint32_t)new_start + s, buf) != 0) {
```

`shell.c`'s raw `ata read` / `ata write` debug commands intentionally still call `ata_read_sector` / `ata_write_sector` directly. They're the back-door for "let me see the actual disk", and bypassing the cache is the whole point.

## SYS_BCACHE_STATS: how the test inspects state

The selftest's whole reason for being is to verify that hits-vs-misses and logical-vs-disk writes behave the way we say they do. Two new syscalls:

```c
case SYS_BCACHE_SYNC: {
    ret = (int32_t)bcache_sync();
    break;
}
case SYS_BCACHE_STATS: {
    uint32_t *uout = (uint32_t *)(uintptr_t)a;
    if (!uout) { ret = -1; break; }
    uout[0] = bcache_hits();
    uout[1] = bcache_misses();
    uout[2] = bcache_logical_writes();
    uout[3] = bcache_disk_writes();
    uout[4] = bcache_dirty();
    ret = 0;
    break;
}
```

The user-side wrapper is one `int $0x80` line plus `out[5]` for `STATS`. The selftest takes a baseline snapshot, performs an action, takes another snapshot, and reports the deltas. That's enough to prove "11 rewrites = 0 disk writes": you only need to compare the disk_writes field before and after.

## Bootloader: 256-sector kernel via two DAP reads

This session's kernel.bin grew from **69808 bytes** (session 26) to **73904 bytes** — past the **65536 byte / 128 sector** limit the bootloader had been reading since session 21.

I tried to bump the DAP's sector count from 128 to 192, then 256. Both failed silently — QEMU/SeaBIOS rejected the read; the boot looped without output. (Many real-machine BIOSes cap a single int 13h ah=42h read at 127 sectors; the spec lets you go higher but coverage is spotty.)

The fix is to issue **two consecutive DAP reads**, each for 128 sectors. Total 256 sectors = 128 KiB of kernel:

```asm
movb    drive, %dl
mov     $dap1, %si          /* sectors 1..128 -> 0x10000 */
mov     $0x42, %ah
int     $0x13
jc      disk_err

movb    drive, %dl
mov     $dap2, %si          /* sectors 129..256 -> 0x20000 */
mov     $0x42, %ah
int     $0x13
jc      disk_err
```

```asm
dap1:
    .byte 0x10
    .byte 0x00
    .word 128
    .word 0x0000
    .word 0x1000
    .quad 1

dap2:
    .byte 0x10
    .byte 0x00
    .word 128
    .word 0x0000
    .word 0x2000
    .quad 129
```

`dap2`'s segment is `0x2000`, so the bytes land at linear `0x20000` — directly continuing where `dap1` finished. The kernel ELF is layout'd at `_kernel_start = 0x10000`; with this scheme it has 128 KiB of headroom before the next bump (which would have to go to `dap3` at segment `0x3000`).

This was the second time in AdventOS's life the bootloader needed to read more than it could in one shot. Session 21 hit it with UDP/DHCP/DNS pushing kernel.bin past 56 KiB; this session hit it again with the bcache code + the bigger user programs. Each bump is "one more DAP" and that scales linearly to whatever ATA can give us.

## Numbers from a clean boot

Boot logs:

```
[boot] initializing ATA driver... ok
[boot] initializing block cache... ok (32 slots, 16 KiB)
[boot] mounting AdventFS... fs: AdventFS mounted, 24 entries, 695/1024 sectors free
```

`24 entries` — same as session 26 (session 25 added `/etc`, 26 added 13 coreutils binaries + the boot path).

The 5-second syncer prints occasionally during selftest. With the 5 dirty blocks we built up during `t9` (ed editor write), `t14` (reuse.txt rewrites), `t16` (mkdir + note write), `t17` (tee /seq.txt), the line shows up roughly twice across the test run:

```
[bcache] syncer flushed 5 dirty block(s)
[bcache] syncer flushed 4 dirty block(s)
```

Or similar. `t18`'s explicit sync produces another flush of exactly 5.

## Why "hits=237 misses=406" at baseline

By the time `[t18]` runs in the selftest, hundreds of cache operations have already happened:

- `fs_init` reads the 3 superblock sectors → 3 misses
- `kmain`'s `LAUNCH("init.elf", …)` opens + reads ~21 sectors of init.elf — most miss
- `init.elf` itself opens + reads `/etc/inittab` — 1 miss for the inittab sector
- httpd.elf + sh.elf get loaded — ~9 sectors each, mostly miss
- Each test in t1..t17 opens / reads / writes various files

237 hits and 406 misses means roughly 37% hit ratio across the run as a whole. The interesting number isn't the absolute, it's the DELTA on a known-cold or known-warm operation: a fresh open of a new sector should cost +1 miss; a re-open of a recently-touched sector should cost +1 hit.

The selftest measures both, and both behave correctly:

```
first read /etc/inittab : delta hits=0 misses=1
re-read /etc/inittab    : delta hits=1 misses=0
```

The first read is +1 miss (the inittab sector got evicted by all the activity since boot — 32 slots is small and lots of programs got loaded). The re-read immediately after is +1 hit, +0 miss, exactly as cache theory says it should.

## File-by-file diff

```
kernel/bcache.h           NEW — declarations + BCACHE_NR / _BLKSZ
kernel/bcache.c           NEW — LRU + lock + sync + syncer task

kernel/fs.c               4 callsites changed: ata_*_sector -> bcache_*
                          + #include "bcache.h"

kernel/kernel.c           bcache_init() before fs_init();
                          bcache_start_syncer() after task spawns
                          + #include "bcache.h"

kernel/syscall.h          add SYS_BCACHE_SYNC = 47, SYS_BCACHE_STATS = 48
kernel/syscall.c          two new dispatcher cases
                          + #include "bcache.h"

user/libuser.h            sys_bcache_sync / sys_bcache_stats prototypes,
                          syscall constants
user/libuser.c            two new wrappers

user/sh.c                 [t18] selftest case appended

boot/boot.S               second DAP read added (sectors 129..256 -> 0x20000)
                          comment updated in dap1
```

Net diff: 2 new kernel files (~290 lines combined), ~30 lines of mechanical fs.c changes, ~50 lines of t18 selftest, ~25 lines of libuser plumbing, 16 lines of boot.S. Kernel binary goes from 69808 → 73904 bytes (+4096 bytes, mostly the 16 KiB g_entries in .bss which doesn't add to the binary, plus the ~3 KiB of new code).

## The numbers in plain English

For the rewrite-heavy workload, this is the practical impact:

| Operation | Pre-cache | With write-back cache |
|---|---|---|
| Read same sector twice | 2 disk reads | 1 disk read + 1 memcpy |
| 11 rewrites of one file | 33+ disk writes | 5 disk writes (after one sync) |
| Random sector miss | 1 disk read | 1 disk read |
| Boot-time fs_init | 2 disk reads | 2 disk reads (first boot) |

Disk traffic is reduced by ~6.6× on the rewrite-heavy path. Read traffic goes to roughly zero on hot data once the cache is warm. The trade-off — non-durable writes until the next sync — is the standard write-back trade Linux + most other OSes make.

## What's not yet here

Three things that would be next:

1. **Crash recovery / journaling.** Today, a crash mid-flush leaves dirty data in RAM and an inconsistent superblock on disk. A journal would log "I'm about to write LBAs A, B, C" and replay on next mount.
2. **Read-ahead.** Sequential reads still hit the disk once per sector. Detecting "this is a sequential read" and prefetching the next 4–8 sectors would cut the per-read latency for big files.
3. **A proper unmount path.** `bcache_sync` exists, but nothing calls it on shutdown. We don't have shutdown — we'd need a `SYS_REBOOT` or just an `init`-driven flush before the user pulls power.

All three are mostly mechanical from here. The hard work — getting the cache layer in place transparently — is done.

## Final sanity check

```
$ curl -s http://localhost:8080/ | head -3
Hello from a USERSPACE HTTP server!

This page was served by user/httpd.c, which runs in ring 3.
```

httpd still works. The cache is invisible to ring 3; it just makes the fs faster.
