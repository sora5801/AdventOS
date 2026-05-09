#include "bcache.h"
#include "ata.h"
#include "kprintf.h"
#include "string.h"
#include "task.h"
#include "pit.h"

/*
 * Per-slot state. valid + dirty fully describe the entry's relation
 * to the disk:
 *   valid=0           slot is empty; data[] is meaningless
 *   valid=1 dirty=0   data[] matches sector `lba` on disk
 *   valid=1 dirty=1   data[] is newer than disk; sync will flush it
 */
struct bcache_entry {
    int                  valid;
    int                  dirty;
    uint32_t             lba;
    uint8_t              data[BCACHE_BLKSZ];
    struct bcache_entry *prev;     /* toward head (more recent)   */
    struct bcache_entry *next;     /* toward tail (less recent)   */
};

static struct bcache_entry  g_entries[BCACHE_NR];
static struct bcache_entry *g_head;
static struct bcache_entry *g_tail;
static int                  g_inited;

static uint32_t g_hits;
static uint32_t g_misses;
static uint32_t g_logical_writes;
static uint32_t g_disk_writes;

void bcache_init(void) {
    /* Build the initial LRU list: every slot empty, linked in order
     * 0 (head) -> 1 -> ... -> BCACHE_NR-1 (tail). The first miss
     * pulls slot 31 (LRU end), the second pulls 30, and so on. */
    for (int i = 0; i < BCACHE_NR; i++) {
        g_entries[i].valid = 0;
        g_entries[i].dirty = 0;
        g_entries[i].lba   = 0;
        g_entries[i].prev  = (i > 0)              ? &g_entries[i - 1] : 0;
        g_entries[i].next  = (i < BCACHE_NR - 1)  ? &g_entries[i + 1] : 0;
    }
    g_head = &g_entries[0];
    g_tail = &g_entries[BCACHE_NR - 1];

    g_hits = g_misses = g_logical_writes = g_disk_writes = 0;
    g_inited = 1;
}

/* Move `e` to the head of the LRU list. Safe to call when `e` is
 * already the head (early return). */
static void touch(struct bcache_entry *e) {
    if (e == g_head) return;

    /* Unlink. */
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (e == g_tail) g_tail = e->prev;

    /* Splice in at head. */
    e->prev = 0;
    e->next = g_head;
    if (g_head) g_head->prev = e;
    g_head = e;
    if (!g_tail) g_tail = e;
}

static struct bcache_entry *lookup(uint32_t lba) {
    for (int i = 0; i < BCACHE_NR; i++) {
        if (g_entries[i].valid && g_entries[i].lba == lba) return &g_entries[i];
    }
    return 0;
}

/* Cache state mutation is short-but-non-atomic: a miss path does
 * lookup -> evict -> ata I/O -> link manipulation. Any of those can
 * be preempted by the timer, the scheduler runs another task, that
 * task may issue a bcache call that touches the SAME entry, then
 * we resume mid-mutation with the world out from under us. We
 * coarsely disable interrupts around the metadata manipulation;
 * the disk I/O itself (ata_*_sector) takes its own lock and must
 * run with interrupts on so PIO polling can yield. */
static inline uint32_t bcache_lock_save(void) {
    uint32_t f;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void bcache_unlock_restore(uint32_t f) {
    __asm__ volatile ("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

/* Reserve the LRU slot under lock: pop it from g_tail, mark it
 * invalid so concurrent lookups can't see stale lba/data, AND
 * also writeback the slot's existing dirty contents (still under
 * lock — keeps the lookup table consistent across the disk I/O).
 *
 * Returns the slot pointer with valid=0, OR NULL on disk failure
 * (the slot's previous state is preserved in that case).
 *
 * The eviction's writeback runs INSIDE the locked region; that
 * forces every other bcache call to spin-wait through the disk
 * I/O. ata_*_sector takes its own short critical section but its
 * polling happens with interrupts on, so we need to keep the
 * lock for it (otherwise a preempting task could re-enter bcache
 * and find a half-rebuilt slot). The trade-off is tolerable
 * because: (a) cooperative kernel; (b) eviction is the cold path
 * — most accesses hit the cache. */
static struct bcache_entry *reserve_lru(void) {
    struct bcache_entry *e = g_tail;
    if (!e) return 0;
    if (e->valid && e->dirty) {
        if (ata_write_sector(e->lba, e->data) == 0) {
            g_disk_writes++;
        } else {
            kprintf("bcache: writeback failed for lba=%u\n",
                    (unsigned)e->lba);
        }
        e->dirty = 0;
    }
    e->valid = 0;
    return e;
}

int bcache_read(uint32_t lba, void *out) {
    if (!g_inited) bcache_init();

    uint32_t f = bcache_lock_save();
    struct bcache_entry *e = lookup(lba);
    if (e) {
        memcpy(out, e->data, BCACHE_BLKSZ);
        touch(e);
        g_hits++;
        bcache_unlock_restore(f);
        return 0;
    }
    /* Miss — reserve a slot with valid=0. Read from disk into the
     * slot's data buffer (still under lock — see reserve_lru). */
    e = reserve_lru();
    if (!e) { bcache_unlock_restore(f); return -1; }

    if (ata_read_sector(lba, e->data) != 0) {
        bcache_unlock_restore(f);
        return -1;
    }
    e->lba   = lba;
    e->valid = 1;
    e->dirty = 0;
    memcpy(out, e->data, BCACHE_BLKSZ);
    touch(e);
    g_misses++;
    bcache_unlock_restore(f);
    return 0;
}

int bcache_write(uint32_t lba, const void *in) {
    if (!g_inited) bcache_init();

    uint32_t f = bcache_lock_save();
    g_logical_writes++;

    struct bcache_entry *e = lookup(lba);
    if (e) {
        memcpy(e->data, in, BCACHE_BLKSZ);
        e->dirty = 1;
        touch(e);
        bcache_unlock_restore(f);
        return 0;
    }

    /* Write-miss. Whole-sector overwrite, so no read-modify-write
     * needed. reserve_lru handles writeback of the LRU slot. */
    e = reserve_lru();
    if (!e) { bcache_unlock_restore(f); return -1; }
    e->lba   = lba;
    e->valid = 1;
    e->dirty = 1;
    memcpy(e->data, in, BCACHE_BLKSZ);
    touch(e);
    bcache_unlock_restore(f);
    return 0;
}

uint32_t bcache_sync(void) {
    /* One slot at a time: snapshot the (lba, data) pair under
     * lock, claim the dirty bit, drop the lock, do the disk
     * write, re-acquire to bump the counter. The buf lives on
     * the stack — 512 bytes, no __chkstk needed. */
    uint32_t n = 0;
    for (int i = 0; i < BCACHE_NR; i++) {
        uint8_t  buf[BCACHE_BLKSZ];
        uint32_t lba = 0;
        int      do_write = 0;

        uint32_t f = bcache_lock_save();
        if (g_entries[i].valid && g_entries[i].dirty) {
            lba = g_entries[i].lba;
            memcpy(buf, g_entries[i].data, BCACHE_BLKSZ);
            g_entries[i].dirty = 0;
            do_write = 1;
        }
        bcache_unlock_restore(f);

        if (!do_write) continue;
        if (ata_write_sector(lba, buf) == 0) {
            f = bcache_lock_save();
            g_disk_writes++;
            bcache_unlock_restore(f);
            n++;
        } else {
            /* Disk write failed — restore the dirty bit on the
             * slot if it still holds the same lba. (The slot may
             * have been re-purposed during our unlocked window.) */
            f = bcache_lock_save();
            if (g_entries[i].valid && g_entries[i].lba == lba) {
                g_entries[i].dirty = 1;
            }
            bcache_unlock_restore(f);
        }
    }
    return n;
}

uint32_t bcache_hits          (void) { return g_hits; }
uint32_t bcache_misses        (void) { return g_misses; }
uint32_t bcache_logical_writes(void) { return g_logical_writes; }
uint32_t bcache_disk_writes   (void) { return g_disk_writes; }

uint32_t bcache_dirty(void) {
    uint32_t n = 0;
    for (int i = 0; i < BCACHE_NR; i++) {
        if (g_entries[i].valid && g_entries[i].dirty) n++;
    }
    return n;
}

/* Periodic flush task. Sleeps 5 seconds between sweeps; if anything
 * was dirty at the time, prints a one-line summary so the demo
 * shows the syncer is alive. With no writes happening, nothing
 * gets logged — the task stays quiet. */
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
