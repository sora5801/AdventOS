# Session 6 — Free-list kmalloc + ATA + sync primitives + RTC

**Goal:** Replace the bump kmalloc with a real allocator (kfree included), get block storage working, add locks, read wall-clock time.

Four mostly-independent subsystems. The connecting thread is **lock everything that's now multi-task-aware**.

## Free-list kmalloc

The bump allocator from session 2 was always temporary. Real kmalloc/kfree wants:

- O(1) free with neighbor coalescing
- O(N) first-fit allocation (or first-fit on a free list — we picked first-fit-on-everything)
- Deterministic header per block, recoverable from the payload pointer
- A magic word for double-free / corruption detection

Block layout (16 bytes header + payload):

```
struct kmblock {
    uint32_t        size;        // payload size
    uint16_t        free;
    uint16_t        magic;       // 0xCAFE
    struct kmblock *prev;        // physical-address order
    struct kmblock *next;
};
```

`prev`/`next` chain blocks in **physical address order**, not free-list order. This is the boundary-tag idiom but with explicit pointers instead of computing them from sizes. Lets `kfree` find both neighbors in O(1).

Allocation walks the chain looking for the first free block of sufficient size. Splits if the leftover exceeds `KM_HDR_SIZE + KM_MIN_PAYLOAD = 32 bytes`.

```c
size_t leftover = b->size - want;
if (leftover >= KM_HDR_SIZE + KM_MIN_PAYLOAD) {
    struct kmblock *n = (struct kmblock *)((uintptr_t)b + KM_HDR_SIZE + want);
    n->size  = (uint32_t)(leftover - KM_HDR_SIZE);
    n->free  = 1;
    n->magic = KM_MAGIC;
    n->prev  = b;
    n->next  = b->next;
    if (b->next) b->next->prev = n;
    b->next  = n;
    b->size  = (uint32_t)want;
}
b->free = 0;
```

Free is symmetric: mark free, then look at `prev` and `next`. If either is free, merge.

```c
b->free = 1;
if (b->next && b->next->free) {           // forward coalesce
    struct kmblock *n = b->next;
    b->size += KM_HDR_SIZE + n->size;
    b->next  = n->next;
    if (n->next) n->next->prev = b;
    n->magic = 0;
}
if (b->prev && b->prev->free) {           // backward coalesce
    struct kmblock *pp = b->prev;
    pp->size += KM_HDR_SIZE + b->size;
    pp->next  = b->next;
    if (b->next) b->next->prev = pp;
    b->magic = 0;
}
```

`kmtest` verifies:

```
[init]              used=32800 free=4161488  blocks=2/1
[post-alloc]        used=36432 free=4157856  blocks=5/1
[after kfree(b)]    used=34368 free=4159904  blocks=4/2     ← b's hole
[after kfree(a)]    used=33328 free=4160944  blocks=3/2     ← a coalesces fwd into b's hole
[after kfree(c)]    used=32800 free=4161488  blocks=2/1     ← back to one big free block
```

The terminal state matches the initial state byte-for-byte. Coalescing is bidirectional and complete.

The 2 used blocks at "init" are the demo_a and demo_b kernel stacks (16 KiB each, allocated by `task_create` in session 4) — they outlive `kmtest`.

## Spinlock

UP-correct, IRQ-aware:

```c
typedef struct {
    volatile uint32_t locked;
    uint32_t          saved_eflags;
} spinlock_t;

static inline void spin_lock(spinlock_t *l) {
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    while (spin_xchg(&l->locked, 1) != 0) __asm__ volatile ("pause");
    l->saved_eflags = flags;
}

static inline void spin_unlock(spinlock_t *l) {
    uint32_t flags = l->saved_eflags;
    spin_xchg(&l->locked, 0);
    if (flags & 0x200) __asm__ volatile ("sti");
}
```

Two correctness properties:

1. **The xchg is atomic** even on UP. We don't strictly need it (cli serializes us), but it's cheap and forward-compatible with SMP.

2. **`saved_eflags` makes nested locks compose.** Outer lock saved IF=1 (interrupts were on); inner lock saved IF=0 (outer's cli already disabled them). Inner's unlock restores IF=0 — no glitch. Outer's unlock restores IF=1. If we just blindly `sti`'d on every unlock, the inner unlock would re-enable interrupts mid-critical-section.

On UP, the `while` loop is dead code in the absence of bugs — cli prevents preemption, and a single CPU can't be in two places at once. The `pause` instruction is forward-compatibility for SMP, where it hints to the CPU that we're spin-waiting and it should back off the L1 cache prefetcher.

## Sleeping mutex

Spinlocks are wrong for anything that holds the lock longer than a few instructions — they busy-wait with IRQs disabled. Anything that wants to wait without burning the CPU needs a real sleeping primitive.

New task state:

```c
enum {
    TASK_STATE_UNUSED  = 0,
    TASK_STATE_READY   = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,    // ← new
    TASK_STATE_DEAD    = 4,
};
```

Scheduler skips both DEAD and BLOCKED:

```c
while (next != g_current
       && (next->state == TASK_STATE_DEAD ||
           next->state == TASK_STATE_BLOCKED)) {
    next = next->next;
}
```

Mutex with FIFO wait queue:

```c
typedef struct mutex {
    int          locked;
    struct task *holder;
    struct task *waiters_head;
    struct task *waiters_tail;
} mutex_t;

void mutex_lock(mutex_t *m) {
    cli;
    if (!m->locked) {
        m->locked = 1; m->holder = task_current();
        sti;
        return;
    }
    /* enqueue self, mark BLOCKED, schedule */
    self->wait_next = NULL;
    m->waiters_tail = (m->waiters_tail ? (m->waiters_tail->wait_next = self, self) : self);
    if (!m->waiters_head) m->waiters_head = self;
    self->state = TASK_STATE_BLOCKED;
    schedule();
    /* schedule sti's at end; we're back from being awoken with the lock held */
}
```

The key invariant in `mutex_unlock`:

```c
if (m->waiters_head) {
    struct task *w = m->waiters_head;
    /* dequeue */
    w->state  = TASK_STATE_READY;
    m->holder = w;
    /* m->locked stays 1 — ownership transferred */
}
```

The lock **never returns to "free"** between unlock and the next acquirer's wakeup. If we set `m->locked = 0` and let the awoken waiter re-acquire, a third task could `mutex_lock` and steal it. Direct ownership transfer prevents the race.

Test (`mtest` command spawns two tasks contending on a mutex; each prints `[MA-IN ... MA-OUT]` / `[MB-IN ... MB-OUT]` around a `pit_sleep(120)` while holding the lock):

After ~6 seconds and 34 IN/OUT pairs, **zero interleavings** — A's IN and OUT always bracket each other before B's IN appears. The unrelated kernel demo tasks `[A]/[B]` interleave freely throughout, since they don't touch the mutex.

## ATA driver

LBA28, primary master only, polled PIO. The 8 useful registers:

```
0x1F0  data (16-bit)
0x1F1  error (read), features (write)
0x1F2  sector count
0x1F3  LBA[7:0]
0x1F4  LBA[15:8]
0x1F5  LBA[23:16]
0x1F6  drive/head
0x1F7  status (read), command (write)
0x3F6  control (alternate status, device control)
```

Read sequence:
1. Wait for BSY=0
2. Write 0xE0 | (LBA[27:24] & 0xF) to drive register (master, LBA mode, top 4 LBA bits)
3. 4× read of 0x3F6 = 400 ns delay (canonical post-drive-select pause)
4. Write 1 to sector count, LBA[23:0] across the LBA registers
5. Write 0x20 (READ_SECTORS) to command register
6. Wait for BSY=0 ∧ DRQ=1
7. `inw` 256 times from data register → 512 bytes

Write is identical except command 0x30 (WRITE_SECTORS), `outw` 256 times, then 0xE7 (FLUSH_CACHE) and another BSY wait — without the cache flush, writes can sit in the controller's buffer indefinitely.

`atatest` reads sector 0 (the MBR), confirms 0xAA55 signature; writes a known pattern to sector 100, reads back, byte-compares. Both succeed.

```
Sector 0 (first 64 bytes):
  0000: fa 31 c0 8e d8 8e c0 8e d0 bc 00 7c fb 88 16 dd  .1.........|...
  ...
  MBR signature at offset 510 = 0xaa55 (valid)

ata: wrote sector 100; readback MATCHES (write/read OK)
```

The first bytes of sector 0 are exactly our `boot/boot.S` — `cli; xor ax,ax; mov ax,ds; mov ax,es; mov ax,ss; mov sp,0x7c00`.

Spinlock-protected: concurrent shell + ATA test + future user tasks don't conflict.

## RTC

CMOS access via index port 0x70 + data port 0x71:

```c
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}
```

Registers: 0x00=seconds, 0x02=minutes, 0x04=hours, 0x07=day, 0x08=month, 0x09=year, 0x32=century, 0x0A=status A (UIP bit), 0x0B=status B (BCD/binary, 12/24).

The hazard: the RTC ticks every second. If you read seconds, minutes, hours sequentially and a tick happens between your reads, the values are inconsistent (e.g., 23:59:60 — minutes hasn't rolled over yet but seconds did).

Two defenses:
1. Wait for UIP=0 (update-in-progress bit clear) before reading.
2. Read everything **twice** and only accept the result if both reads agree:

```c
do {
    while (rtc_updating()) {}
    s   = cmos_read(0x00); ...
    while (rtc_updating()) {}
    s2  = cmos_read(0x00); ...
} while (s != s2 || m != m2 || ...);
```

BCD conversion (default on most machines):

```c
if (!(status_b & STATUS_B_BIN)) {
    s = bcd2bin(s);
    /* Hour: PM bit (0x80) preserved through conversion */
    h = (h & 0x80) | bcd2bin(h & 0x7F);
    ...
}
```

12-hour mode (rare but possible) maps PM-flagged hour back to 24-hour.

`rtc_to_epoch` computes UNIX time using only constant shifts and adds:

```c
for (int y = 1970; y < t->year; y++) days += is_leap(y) ? 366 : 365;
for (int m = 1; m < t->month; m++) {
    days += days_per_month[m - 1];
    if (m == 2 && is_leap(t->year)) days += 1;
}
days += (uint32_t)t->day - 1;
return days * 86400u + ...;
```

Tested against today's date (2026-05-08) and matched a hand-calculated epoch to the second.

## Bootloader DAP bumped

Kernel grew past 32 KiB after this session — the bootloader was loading 64 sectors. Bumped `dap.sectors` from 64 to 112 (56 KiB). Plenty of headroom for now.

## Files added

| File | Role |
|---|---|
| `kernel/spinlock.h` | UP spinlock with saved EFLAGS |
| `kernel/mutex.{c,h}` | Sleeping mutex with FIFO wait queue |
| `kernel/kmalloc.{c,h}` | Free-list rewrite with kfree |
| `kernel/ata.{c,h}` | LBA28 polled PIO read/write/flush |
| `kernel/rtc.{c,h}` | CMOS read with UIP guard, BCD/binary, epoch conversion |
| `kernel/task.{c,h}` | `TASK_STATE_BLOCKED` + `wait_next` |
| `kernel/shell.c` | `kfree`, `kmtest`, `ata`, `time`, `mtest` commands |
| `kernel/kernel.c` | `ata_init`, RTC boot message |
| `boot/boot.S` | DAP sector count 64 → 112 |

## Design decisions

**Free-list over buddy.** Buddy gives O(log N) alloc and clean fragmentation behavior, but the bookkeeping is heavier (2× free lists keyed by size class). Free-list is what fits in 100 lines. Coalescing keeps fragmentation manageable.

**Sleeping mutex over spin-yielding.** Earlier draft had `mutex_lock` busy-yield via `task_yield()`. That works but burns CPU when contention is real (the unblocked task gets immediately re-scheduled and re-fails). FIFO wait queue + BLOCKED state is the textbook approach.

**Polled PIO ATA.** Simplest possible disk driver. IRQ-driven would mean handling IRQ 14 (primary controller), which is on the slave PIC and needs cascade IRQ-2. For our boot-time-only-ish I/O, polling is fine. We'd lose this in a multi-process system that's actually contention-heavy on disk.

**Spinlock-protected ATA.** Concurrent disk I/O is doubly-bad without serialization (the ATA registers are shared state). Even with one shell task, there's no harm in being correct.

**RTC read on every `time` invocation rather than at boot + cached + uptime delta.** RTC reads are slow (a few microseconds) but rare. Caching adds bug surface (clock skew between cached value and real time). Re-read each time.

## Deferred

- Block cache (never)
- IRQ-driven ATA (never)
- ATA secondary controller, slave drives (never)
- Filesystem (session 8)
- Read-write/condition variables (never)

## Pitfalls

1. **`spin_unlock` must restore the EFLAGS the matching `spin_lock` saved**, or nested locks corrupt IF state. Storing in the lock struct works only because UP guarantees one holder; SMP would need stack-allocated state.
2. **Mutex ownership transfer on unlock is mandatory.** Setting `locked = 0` and letting the waiter re-acquire is racy.
3. **ATA write must end with FLUSH CACHE.** Otherwise data sits in the controller buffer until evicted.
4. **CMOS reads need UIP-aware retry**, not just a single read. The "read twice and compare" pattern is the simplest correct version.
5. **BCD conversion of the hours register must preserve the PM bit (0x80)** before masking the BCD digits, then OR it back after.
6. **Constant shifts only** for any 64-bit math, even with the new allocator. Variable shifts and divides still pull libgcc.
