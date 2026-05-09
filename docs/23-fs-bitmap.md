# Session 23 — Free-sector bitmap for AdventFS

**Goal:** Pay back the technical debt session 19 left behind. `fs_write_all` had a leak-on-rewrite design — every save allocated a fresh contiguous run from `g_high_water` and forgot the old range, banking on never running out of disk. After 11 saves of a 1-sector file, the FS would have spent 11 sectors of "live" content on disk plus 10 leaked sectors. Add a real free-sector bitmap so rewrites reuse the old range, fix the leak, and surface the result via a tiny `SYS_FS_FREE_SECTORS` syscall + a t14 selftest.

End state — the new t14 line:

```
[t14] fs free-sector bitmap: rewrites reuse sectors
  free at start: 910
  after  1 write : 909  (delta 1)
  after 11 writes: 909  (delta 1 — should equal first delta)
```

Eleven rewrites of a 13-byte file consume **one** sector total, not eleven. `httpd.elf` keeps serving curl on :80 throughout (`status=200 bytes=317`).

## What's in scope

In:
- `static uint8_t g_bitmap[FS_BITMAP_BYTES]` — 128 bytes covering 1024 sectors (1 bit per sector); bit set = allocated
- `bitmap_alloc_run(n)` — first-fit allocator that finds the lowest contiguous run of `n` free sectors and marks them used
- `bitmap_free_run(start, n)` — frees a run; refuses to touch sector 0
- Mount-time bitmap reconstruction by walking the file table (no on-disk bitmap)
- `fs_write_all` rewritten: allocate-first → write → free-old → persist superblock (in that order — atomicity-safe against partial-write failures)
- `fs_free_sectors` re-implemented over the bitmap (no more `g_high_water` arithmetic)
- `SYS_FS_FREE_SECTORS = 40` syscall + `sys_fs_free_sectors()` libuser wrapper
- `[t14]` selftest demonstrating reuse on rewrite

Out:
- On-disk bitmap persistence (we recompute at mount; it's free and avoids a coherence problem)
- Best-fit / buddy / free-list allocators — first-fit is enough at 1024-sector scale
- Shrinking a file (we always allocate a fresh run; if the new size is smaller we still alloc-then-free; we don't try to chop off the tail of an existing run)
- Defragmentation
- File deletion (no `unlink` syscall yet — the bitmap supports it, but the user-facing API doesn't)
- Sparse files (we always allocate `ceil(size/512)` contiguous sectors)
- Block sharing / copy-on-write
- Quotas

## The bitmap

```c
#define FS_DISK_TOTAL_SECTORS   1024u
#define FS_BITMAP_BYTES         ((FS_DISK_TOTAL_SECTORS + 7) / 8)
static uint8_t g_bitmap[FS_BITMAP_BYTES];

static inline int bitmap_get(uint32_t s) {
    if (s >= FS_DISK_TOTAL_SECTORS) return 1;     /* off-disk = used */
    return (g_bitmap[s >> 3] >> (s & 7)) & 1;
}
static inline void bitmap_set(uint32_t s)   { g_bitmap[s >> 3] |=  (uint8_t)(1u << (s & 7)); }
static inline void bitmap_clear(uint32_t s) { g_bitmap[s >> 3] &= (uint8_t)~(1u << (s & 7)); }
```

128 bytes total. The "off-disk = used" return for out-of-range queries is a small safety net so `bitmap_get(huge_number)` doesn't accidentally report a sector as free.

## First-fit allocator

```c
static int bitmap_alloc_run(uint32_t n) {
    if (n == 0) n = 1;
    uint32_t run_start = 0;
    uint32_t run_len   = 0;
    for (uint32_t s = 1; s < FS_DISK_TOTAL_SECTORS; s++) {
        if (!bitmap_get(s)) {
            if (run_len == 0) run_start = s;
            run_len++;
            if (run_len >= n) {
                for (uint32_t k = 0; k < n; k++) bitmap_set(run_start + k);
                return (int)run_start;
            }
        } else {
            run_len = 0;
        }
    }
    return -1;
}
```

Sweeps from sector 1 (skipping the superblock at sector 0) looking for `n` consecutive zero bits, sets them to 1, returns the start. -1 on failure. Walking the full bitmap is O(N) in sectors = 1024 — fast enough that we don't bother with a hint pointer.

`bitmap_free_run` is the inverse — clear the bits — with a one-liner safeguard against ever clearing sector 0:

```c
static void bitmap_free_run(uint32_t start, uint32_t n) {
    if (start == 0) return;
    for (uint32_t k = 0; k < n; k++) {
        if (start + k < FS_DISK_TOTAL_SECTORS) bitmap_clear(start + k);
    }
}
```

## Mount-time reconstruction

The bitmap lives only in RAM. Each boot, `fs_init` recomputes it from the superblock:

```c
for (uint32_t i = 0; i < FS_BITMAP_BYTES; i++) g_bitmap[i] = 0;
bitmap_set(0);                                 /* superblock */
for (uint32_t i = 0; i < g_super.file_count; i++) {
    struct fs_entry *e = &g_super.files[i];
    if (e->size == 0) continue;
    uint32_t n = (e->size + 511) / 512;
    for (uint32_t s = 0; s < n; s++) bitmap_set(e->start_sector + s);
}
```

Why not persist the bitmap to disk? Three reasons:

1. **No coherence problem.** A persistent bitmap that disagrees with the file table is corruption — and doubles your atomicity surface (every `fs_write_all` would need to update both atomically). Recomputing from the file table, which IS the source of truth, sidesteps the whole class of bugs.
2. **It's cheap.** 1024 sectors × probe-bit work = a few KiB of operations at mount. That's a rounding error against the disk I/O of reading the superblock.
3. **It survives a corrupted bitmap.** If for any reason `g_bitmap` got trashed in RAM (a future bug), the next boot would rebuild it correctly from the persisted file table.

The downside is leaked sectors stay leaked across reboots — if a previous boot's `fs_write_all` failed in a state that left a sector run allocated to nothing (no file points at it), the recomputed bitmap won't see those sectors as used and will hand them out again. That's actually a self-healing property here, not a bug — orphan sector runs get recycled automatically.

## fs_write_all, rewritten

The new flow:

```
fs_write_all(name, data, size):
  1. find_or_create_entry(name)         -> idx
  2. snapshot old_start, old_size, old_n
  3. needed = ceil(size / 512)  (>= 1)
  4. new_start = bitmap_alloc_run(needed)
                   -> -1 means out of contiguous space; bail
  5. for s in 0..needed-1:
        write sector at FS_DISK_OFFSET_SECTORS + new_start + s
        on failure: bitmap_free_run(new_start, needed); return -1
  6. files[idx].start_sector = new_start
     files[idx].size         = size
  7. if old_n > 0: bitmap_free_run(old_start, old_n)
  8. fs_write_super()
```

**Allocate-first / free-old-last** is the safety property:

- If step 4 fails (no contiguous run), the file is unchanged and readable through its old entry.
- If any sector write in step 5 fails, we roll back the freshly-allocated bitmap bits (so the sector run goes back to the free pool). The file is still pointing at its old contents.
- If step 8 (superblock persist) fails, the in-memory file table is correct but disk is stale. We return -1; next boot would re-read the superblock and see the OLD state. The new sectors are still in the bitmap (in-RAM) but the disk-side superblock doesn't reference them — they'd appear as "free but containing stale data." The mount-time recompute would correctly see them as free. Acceptable.

The "always allocate ≥ 1 sector" rule from session 19 stays — a zero-byte file still gets one sector allocated (storing nothing). It's the simplest way to keep `start_sector` meaningful for empty files.

## fs_free_sectors and the SYS_FS_FREE_SECTORS hook

```c
uint32_t fs_free_sectors(void) {
    uint32_t free = 0;
    for (uint32_t s = 0; s < FS_DISK_TOTAL_SECTORS; s++) {
        if (!bitmap_get(s)) free++;
    }
    return free;
}
```

Exact count of zero bits in the bitmap. The kernel mount line uses it for the diagnostic banner:

```
fs: AdventFS mounted, 10 files, 911/1024 sectors free
```

`SYS_FS_FREE_SECTORS = 40` exposes the count to userspace. The selftest reads it before/after writes to verify reuse:

```c
uint32_t before = sys_fs_free_sectors();
sys_fs_write("reuse.txt", data, len);
uint32_t after_first = sys_fs_free_sectors();
for (int i = 0; i < 10; i++) sys_fs_write("reuse.txt", data, len);
uint32_t after_eleven = sys_fs_free_sectors();
/* expect after_first == after_eleven */
```

## What the [t14] test demonstrates

```
[t14] fs free-sector bitmap: rewrites reuse sectors
  free at start: 910
  after  1 write : 909  (delta 1)
  after 11 writes: 909  (delta 1 — should equal first delta)
```

The write is a 13-byte string; one sector covers it. Each rewrite:
- Allocates a fresh sector run (1 sector — `bitmap_alloc_run(1)`)
- Writes the 13 bytes (zero-padded to 512)
- Frees the previous sector run (1 sector — `bitmap_free_run(old, 1)`)

Net: each rewrite consumes 0 additional sectors. Free count stays at 909.

The `delta 1` after 11 writes is **the** demonstration that the bitmap is doing its job. With session 19's leak-on-rewrite, it would have been `delta 11`.

The 910 → 909 first delta is "we permanently consumed 1 sector for reuse.txt." That sector keeps being reused on every subsequent write. If the test wrote a 600-byte string instead (= 2 sectors), the first delta would be 2 and the per-rewrite delta would still be 0.

## Files added / modified

| File | Change |
|---|---|
| `kernel/fs.c` | Bitmap data + helpers; `fs_init` recomputes from superblock; `fs_write_all` rewritten alloc→write→free; `fs_free_sectors` over bitmap |
| `kernel/syscall.{h,c}` | `SYS_FS_FREE_SECTORS = 40` |
| `user/libuser.{h,c}` | `sys_fs_free_sectors()` wrapper |
| `user/sh.c` | `[t14]` selftest |

## Design decisions

**Bitmap, not free list.** A free list of `(start, length)` runs would handle bigger disks more efficiently. At 1024 sectors a 128-byte bitmap is overwhelmingly cheaper than the linked-list bookkeeping; first-fit on 1024 entries is microseconds.

**Recompute at mount, no on-disk bitmap.** Removes a coherence problem and makes orphan sectors self-heal across reboot.

**First-fit, not best-fit.** Best-fit reduces internal fragmentation but requires walking the whole bitmap on every allocation. First-fit returns as soon as it sees a fitting run. The fragmentation cost shows up only when many similar-sized runs interleave — not our use case.

**Allocate-first, free-old-last.** Atomicity-safe at the cost of needing 2× the file's size in free space during the write. With 1024 sectors and small files, no problem. A nearly-full disk could fail a rewrite that would have succeeded under free-first/alloc-second; documented in pitfalls.

**`bitmap_alloc_run` skips sector 0.** The superblock lives there; allocating it would corrupt the FS table. The skip is in the loop, not in `bitmap_set` — we DO mark sector 0 as used at mount time; we just never hand it out.

**`bitmap_free_run` refuses sector 0.** Defense against a corrupt entry whose `start_sector` is 0 with `size > 0`. We could have asserted instead — silent no-op is safer for a demo OS.

**No SYS_FS_BITMAP_DUMP or similar.** The free-count syscall is enough for the test; surfacing the full bitmap layout to userspace would mean a copy out and a syntax for the user side.

**The mount banner format changed from `(free sec X..Y)` to `N/M sectors free`.** Old format implied a high-water contiguous free region; new format better describes what the bitmap actually represents (any sector might be free).

**Empty-file write still consumes 1 sector.** The `if (needed == 0) needed = 1` rule from session 19 is preserved. A truly empty file would otherwise have `start_sector = ?` ambiguous; one sector keeps the on-disk model uniform.

## Pitfalls

1. **Allocate-first temporarily doubles space usage.** Rewriting a 500-sector file requires 500 sectors of free space even though net consumption is 0. On a nearly-full disk, in-place rewrites would fail.
2. **No `unlink`.** The bitmap supports freeing a file's sectors, but there's no syscall path to invoke it. A file that's no longer wanted stays allocated until the FS is rebuilt at boot.
3. **`fs_write_all` is whole-file replace.** Partial writes / appends would need to copy the existing data into the new range first; we don't.
4. **Bitmap reconstruction trusts the superblock.** A malicious or corrupted superblock entry with `start_sector` past the disk would be silently capped (`bitmap_set` checks bounds), but two entries claiming overlapping ranges would both be marked used and allocator would never hand out those sectors. There's no consistency check at mount.
5. **No defragmentation.** If a 16-sector file frees its run, then a 4-sector file allocates 4 of those, then we want a 16-sector run again — we'd find a NEW 16-sector range past the high-water if available, OR fail. We don't try to compact existing files.
6. **First-fit gets greedy at the front.** A 1-sector file allocated near sector 1 stays there forever (rewrites prefer the same low sector). Many small files cause low-sector clustering and force big files to allocate further out. Best-fit or "next-fit" (start scan from the last allocation) would distribute better.
7. **`SYS_FS_FREE_SECTORS` returns int, signed.** With `FS_DISK_TOTAL_SECTORS = 1024` we never get close to int-max so this works. A future bigger FS might overflow; switching the syscall return to `uint32_t` cast through a buffer would fix it.
8. **The superblock write at the end of `fs_write_all` is a single sector write.** Not crash-safe — a power loss during that write could leave the file table half-updated. A real FS would journal the metadata change. We don't.
9. **The bitmap is rebuilt at mount, not after each operation.** If the kernel crashes mid-`fs_write_all`, the in-RAM bitmap might disagree with the file table. Reboot heals it. Without a reboot, the discrepancy could cause bad allocations until the bitmap is re-walked.

## What might come next

`unlink` (one syscall, one bitmap-free call). Then partial/append writes (read existing → modify → write whole — extension of the same primitive). Then a real journal so metadata writes are crash-safe. After that, larger disks (the bitmap scales linearly to 4 KiB / 32 K sectors / 16 MiB FS area; beyond that a multi-level allocator — buddy or extent-tree — earns its keep).
