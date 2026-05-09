#ifndef ADVENTOS_BCACHE_H
#define ADVENTOS_BCACHE_H

#include "../include/types.h"

/*
 * Block cache (session 27).
 *
 * Sits between fs.c and ata.c, caching 512-byte sectors in RAM.
 * Reads check the cache first (hit -> memcpy out, no disk I/O).
 * Writes mark the slot dirty WITHOUT going to disk; a periodic
 * syncer kernel task flushes dirty blocks every few seconds, and
 * eviction of a dirty slot writes it back synchronously.
 *
 * Replacement policy: LRU via a doubly-linked list. The `head` is
 * the most-recently-used end; eviction picks `tail`. Lookups are
 * a linear scan over BCACHE_NR slots — for 32 entries that's
 * cheaper than a hashtable.
 *
 * Coherency: fs.c calls bcache_read / bcache_write for every
 * sector touched; nothing else in the FS path goes near
 * ata_read_sector / ata_write_sector. The shell's raw `ata read`
 * / `ata write` debug commands bypass the cache on purpose — they
 * exist to inspect the underlying disk, not the cached view.
 */

#define BCACHE_NR     32      /* 32 slots * 512B = 16 KiB total */
#define BCACHE_BLKSZ  512

void     bcache_init(void);

/* Cached sector I/O. Returns 0 on success, -1 on disk failure. */
int      bcache_read (uint32_t lba, void *out);
int      bcache_write(uint32_t lba, const void *in);

/* Force every dirty slot out to disk synchronously. Returns the
 * number of disk writes issued. Idempotent — calling twice in a
 * row returns 0 the second time. */
uint32_t bcache_sync(void);

/* Spawn the kernel task that calls bcache_sync() on a fixed
 * interval (currently 5 seconds). Call once, after task_init. */
void     bcache_start_syncer(void);

/* Diagnostics, mostly for the [t18] selftest. All counts are
 * cumulative since boot.
 *   hits          successful lookup in the cache (no disk read)
 *   misses        lookup that had to fall through to ata_read_sector
 *   logical_writes  total calls to bcache_write
 *   disk_writes   actual ata_write_sector calls (writebacks + sync)
 *   dirty         live count of slots currently marked dirty
 */
uint32_t bcache_hits        (void);
uint32_t bcache_misses      (void);
uint32_t bcache_logical_writes(void);
uint32_t bcache_disk_writes (void);
uint32_t bcache_dirty       (void);

#endif
