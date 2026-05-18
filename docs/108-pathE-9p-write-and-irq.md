# Session 121 — Path E phase 4: 9p writes + IRQ-driven virtio

**Goal.** Two pieces in one session:

1. Finish the virtio-9p write path so `/mnt/9p` is fully read-write.
2. Replace polling-based virtio completion with PCI INTx handlers.
   All six virtio drivers (blk, net, rng, console, balloon, 9p) move
   off `pit_sleep` polling loops onto real interrupts.

Status: **done.** Verified end-to-end on WSL Ubuntu 24.04 / QEMU 8.2;
Windows MSYS2 build still works clean.

---

## Verification — what we ran

### Part A: 9p writes

```
$ rm -rf /tmp/9p-host && mkdir /tmp/9p-host
$ echo 'initial host file' > /tmp/9p-host/host.txt

  [boot into AdventOS]

advent$ cp /etc/passwd /mnt/9p/copied.txt
advent$ cat /mnt/9p/copied.txt
root:ABCDef01$2fbe4871...:0:0:/:sh.elf
guest:GH23ij45$e2ea47ad...:1000:1000:/:sh.elf

advent$ mkdir /mnt/9p/subdir
advent$ ls /mnt/9p
  host.txt
  subdir
  copied.txt

advent$ rm /mnt/9p/copied.txt
advent$ rmdir /mnt/9p/subdir
advent$ ls /mnt/9p
  host.txt
```

On the host afterward:
```
$ ls /tmp/9p-host
host.txt        # copied.txt + subdir were created and removed
```

### Part B: IRQ-driven virtio

```
[boot] probing virtio-rng... virtio-rng: PCI 0:5.0  io=0xc060  irq=10
virtio-rng: probe ok (e2 b8 67 65 5e 64 d2 8a)
[boot] probing virtio-9p... virtio-9p: PCI 0:4.0  io=0xc000  irq=11
virtio-net: PCI 0:3.0  io=0xc040  irq=11  subsys=0x1
virtio-net: link up — MAC 52:54:00:12:34:56
virtio-net: RX is IRQ-driven (IRQ 11)
dhcp: DISCOVER ... got OFFER 10.0.2.15, REQUEST ... ACK
```

Note virtio-net + virtio-9p both on IRQ 11 — the shared-line dispatcher
correctly fans out to each device.

---

## Part A — 9p writes

Four new message types implemented (9P2000.L):

| code | name | shape |
|---|---|---|
| 14 / 15 | Tlcreate / Rlcreate | `[fid:4][name:s][flags:4][mode:4][gid:4]` → `[qid:13][iounit:4]` |
| 118 / 119 | Twrite / Rwrite | `[fid:4][offset:8][count:4][data]` → `[count:4]` |
| 72 / 73 | Tmkdir / Rmkdir | `[dfid:4][name:s][mode:4][gid:4]` → `[qid:13]` |
| 76 / 77 | Tunlinkat / Runlinkat | `[dfid:4][name:s][flags:4]` → (empty) |

### VFS plumbing

`v9p_vfs_write_all(path, data, n)`:
1. Split `path` into `parent/base` (new helper `split_parent_basename`).
2. Walk `root_fid → parent` into a fresh fid.
3. Try `Tlcreate parent base flags=O_WRONLY|O_TRUNC mode=0644`.
   After Tlcreate succeeds, the SAME fid now refers to the opened
   newly-created file. (This is 9P's slightly cute reuse of the
   directory fid for the new file.)
4. If create fails (file exists), clunk and retry as `Twalk → Tlopen
   O_WRONLY|O_TRUNC`.
5. Loop `Twrite fid offset count` until all bytes are sent. Each
   chunk capped at `msize - 23` bytes (header + Twrite fixed fields).
6. `Tclunk` and free the fid.

`v9p_vfs_mkdir(path)`: split → walk to parent → Tmkdir → clunk.

### Unlink/rmdir routing

`SYS_UNLINK` and `SYS_RMDIR` previously called `fs_unlink`/`fs_rmdir`
directly (the AdventFS-specific functions, no VFS dispatch). Added a
small prefix check at the syscall layer: paths starting with `/mnt/9p`
route to the new `virtio_9p_unlink_path(rel, is_dir)` which calls
`Tunlinkat`. Everything else falls through to the existing AdventFS
code path. This avoids changing the broader VFS interface.

### What's not wired

- Shell `>` redirect still creates a tmpfs file (the existing
  AdventOS behavior). Tools that go through `SYS_FS_WRITE` (cp, ed,
  agentd, cc) do write through to 9p. Fixing `>` would require
  changing the shell, out of scope here.
- File renames (Trename / Trenameat). 9P has dedicated messages
  but no AdventOS callers exist yet.
- Permission propagation. We pass `0644` / `0755` literally — the
  guest doesn't yet do POSIX uid/gid lookups against the share.

---

## Part B — IRQ-driven virtio

Before this session every virtio driver completed I/O by polling.
Sync waiters (`virtio_wait_used` in blk / rng / 9p / net-TX) used a
`pushfl; sti; hlt; popfl` loop that was woken by the PIT IRQ every
~10 ms. RX-driven drivers (net, console) ran dedicated polling tasks
that called `rx_drain` every 20–50 ms.

That worked but burned CPU when idle and capped RX latency at the
poll interval. Real PCI INTx fixes both.

### Shared-line dispatcher

`isr_register_irq` allows one handler per PIC line. Virtio devices in
QEMU regularly share IRQs (in our test rig virtio-net and virtio-9p
both got IRQ 11). To handle that, `kernel/virtio.c` now keeps a slot
table indexed by IRQ:

```c
struct virtio_irq_slot {
    int       irq;
    uint16_t  io_base;
    void    (*fn)(void *);
    void     *cookie;
};
static struct virtio_irq_slot g_irq_slots[VIRTIO_MAX_IRQ_SLOTS];
```

A single master dispatcher `virtio_master_irq` is installed per IRQ
line the first time `virtio_install_irq` is called for that line.
On every interrupt the master walks every slot for that line and:

1. Reads each device's `VIRTIO_PCI_ISR` (read-to-clear). ISR bit 0
   = "queue had completions". Bit 1 = "config changed" (ignored).
2. If bit 0 is set and the slot has a non-NULL `fn`, calls it
   with the per-device cookie.

Slots without an `fn` (the sync-only drivers) only get the ISR read,
which still serves a real purpose: clearing the latched line so the
hlt-waker in `virtio_wait_used` doesn't loop forever after one edge.

### Per-driver hookup

| driver | fn | reason |
|---|---|---|
| virtio-blk | NULL | sync only; ISR clear is enough |
| virtio-rng | NULL | sync only |
| virtio-9p | NULL | sync only |
| virtio-balloon | NULL | cooperation task still polls config |
| virtio-net | `virtio_net_irq_drain` | RX path drains used ring + reposts buffers |
| virtio-console | `virtio_console_irq_drain` | RX into byte ring |

For the RX-driven cases, `rx_drain` runs in IRQ context. That's fine —
the existing RTL8139 driver already calls into the IP/TCP stack from
its IRQ handler. The net stack takes spinlocks (`net_lock`) which
disable interrupts, so a packet arriving mid-handler queues until
the lock releases.

### What we kept on the polling path

- `virtio_wait_used`'s `sti; hlt; popfl` loop. We still poll
  `used.idx`. The improvement is that the device's IRQ wakes hlt
  immediately instead of waiting for the next PIT tick (~10 ms).
  Latency drops from "up to 10 ms" to "microseconds."
- The virtio-balloon cooperation task. It checks the host's `target`
  page count every 1 second — that's a config-space poll, not a
  used-ring poll, so the IRQ doesn't help here.

### What's deleted

- `virtio_net_rx_task` — gone, RX is now IRQ-driven.
- `virtio_console_rx_task` — gone, RX is now IRQ-driven.
- The `usb_start_polling`-style `virtio_net_start_polling` /
  `virtio_console_start_polling` entry points kept their names + a
  banner kprintf for symmetry, but no longer create kernel tasks.

---

## Bugs that didn't bite this round

A few worth recording because the next refactor in this area might
hit them:

- **Lock taken from IRQ context.** `rx_drain` calls `net_rx_frame`
  which descends through eth/ip/tcp/udp/sock. All of those take
  `net_lock`, which is a spinlock that disables interrupts. So
  IRQ-context callers and task-context callers don't deadlock — the
  IRQ handler waits if a task is holding the lock, but since the
  task-context caller can't be preempted (IF=0) while holding it,
  this resolves in microseconds. The same pattern is in rtl8139.
- **Static state on the IRQ stack.** The virtio-net per-RX scratch
  is `v->rx_buf[idx]`, kmalloc'd at init. No re-entrancy issue —
  same IRQ can't fire twice on the same line before its handler
  returns.

---

## Files touched

Modified:
- `kernel/virtio.h` — `virtio_install_irq()` prototype
- `kernel/virtio.c` — shared-line dispatcher + slot table
- `kernel/virtio_blk.c` — install IRQ (NULL fn)
- `kernel/virtio_rng.c` — install IRQ (NULL fn)
- `kernel/virtio_9p.c` — 4 new T-messages + `virtio_9p_unlink_path()`
  + install IRQ (NULL fn) + `split_parent_basename` + write-side
  VFS ops + Tlcreate / Twrite / Tmkdir / Tunlinkat
- `kernel/virtio_9p.h` — `virtio_9p_unlink_path()` prototype
- `kernel/virtio_balloon.c` — install IRQ (NULL fn)
- `kernel/virtio_net.c` — install IRQ + remove `virtio_net_rx_task`
- `kernel/virtio_console.c` — install IRQ + remove `virtio_console_rx_task`
- `kernel/syscall.c` — `/mnt/9p` prefix routing in `SYS_UNLINK` and `SYS_RMDIR`

kernel.bin (ELF / WSL): 127150 → 131246 bytes (+4 KiB, 66% budget).
kernel.bin (PE / MSYS2): 135344 → 135344 bytes (unchanged after
  --gc-sections; PE pads differently than ELF).

---

## Path E status after session 121

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read) + portable WSL build
- ✅ 121 — virtio-9p (write) + IRQ-driven virtio

Still candidate:
- USB CDC-ECM (USB Ethernet — sister to CDC-ACM)
- virtio-scsi (multi-LUN block device)
- e1000 NIC (Intel gigabit ethernet for real-hw coverage)
- Trenameat for 9p
- Full TTY integration of CDC-ACM (/dev/ttyACM0 as a real userspace device)
