# Session 124 — Path E phase 7: virtio-scsi + CDC-ACM TTY

**Goal.** Two driver-shaped items in the Path E backlog:

1. **virtio-scsi** — the *other* paravirtualized block device. Most
   cloud images expose disk via SCSI, not virtio-blk. Multi-target /
   multi-LUN on the wire (we use one of each). End-to-end probe +
   READ(10) + WRITE(10) + AdventFS auto-mount.

2. **CDC-ACM TTY** — wire the existing USB serial driver into the FD
   table so userspace can `open("/dev/ttyACM0")` and the result reads
   bytes from / writes bytes to a real USB-serial dongle.

Status: **done.** virtio-scsi probes, registers as a `blkdev`, passes
the same write+read+restore sanity probe AHCI uses, and the FS layer
auto-mounts a SCSI-attached AdventFS image at `/mnt/scsi`.
CDC-ACM `/dev/ttyACMn` returns a proper `FD_CDC_ACM` descriptor;
`SYS_READ` blocks on a per-port ring buffer the polling task fills,
`SYS_WRITE_FD` queues bulk-OUT.

Surprise bug fixed along the way: a shared-PCI-IRQ storm — *covered
last in this doc because it was the most interesting bit*.

---

## virtio-scsi

### Wire format

The data path uses three virtqueues — control (vq 0), event (vq 1),
request (vq 2). We only touch the request queue; control is for task-
management commands (abort, reset), event for unit-attention notices,
neither of which a polled boot-time block driver cares about. They
still get configured because virtio refuses `DRIVER_OK` until every
advertised queue has a valid PFN.

Each I/O is a descriptor chain that wraps a SCSI command. The cmd
header the driver sends is:

```c
struct vscsi_req_cmd_hdr {
    uint8_t  lun[8];        // SAM 8-byte LUN
    uint64_t tag;            // we never reuse, set to 0
    uint8_t  task_attr;      // 0 = SIMPLE
    uint8_t  prio;           // 0
    uint8_t  crn;            // 0
    uint8_t  cdb[32];        // SCSI Command Block
};
```

The response header the device writes is:

```c
struct vscsi_req_resp_hdr {
    uint32_t sense_len;
    uint32_t residual;
    uint16_t status_qualifier;
    uint8_t  status;         // 0 = GOOD
    uint8_t  response;       // 0 = COMPLETED, 3 = BAD_TARGET
    uint8_t  sense[96];
};
```

Direction is encoded by which descriptors are device-readable vs
device-writable. For READ(10): `[cmd READ] [resp WRITE] [data WRITE]`.
For WRITE(10): `[cmd READ] [data READ] [resp WRITE]`. Spec §5.6 makes
it sound complicated; with one in-flight command at a time it's just
chain three descriptors in the right order.

### LUN gotcha

SAM's 8-byte LUN format is *not* "lun[0] = target id." It's a tagged
encoding where:

- `lun[0]` = address method (top 2 bits) | top 6 bits of bus number
- `lun[1]` = remaining LUN byte
- `lun[2..7]` = zero for the simple single-level case

QEMU's virtio-scsi handler rejects any request where `lun[0] != 1`
with `response = 3` (BAD_TARGET) — no useful sense data, just a
silent rejection. The first round of testing hit this with `lun[0] =
0`. Fix is one line:

```c
for (int i = 0; i < 8; i++) v->lun[i] = 0;
v->lun[0] = 1;
```

### SCSI command flow

Five commands suffice for a polled boot-time driver:

| CDB | mnemonic | purpose |
|---|---|---|
| 0x00 | TEST UNIT READY | "are you alive?" — sometimes returns CHECK CONDITION on first call (UNIT ATTENTION clear), try 5x |
| 0x12 | INQUIRY | 36 bytes — vendor / product / version strings |
| 0x25 | READ CAPACITY(10) | 8 bytes: last-LBA + block-size, both big-endian |
| 0x28 | READ(10) | 32-bit LBA, 16-bit count |
| 0x2A | WRITE(10) | same layout as READ(10) |

READ(10) / WRITE(10) cap at 2 TiB which is far more than anything
AdventOS will ever see. Bumping to 16-byte CDBs (`0x88` / `0x8A`)
would extend that, but there's no point.

### Sanity probe

Right after `blkdev_register`, do AHCI's trick: save the last sector,
write a known pattern (`i ^ 0x5A`), read back, compare, restore. The
boot log line is:

```
virtio-scsi: vscsi0: probe ok (write+read round-trip @ sector 4678)
```

If the round-trip fails the driver still keeps the blkdev registered
but flags the probe so any latent FS corruption is at least visible.

---

## CDC-ACM TTY

The CDC-ACM driver has been around since session 118 but it routed
incoming bytes to `kprintf` and exposed nothing to userland — useful
for "echo from the host appears in the kernel log" but not for an
actual character device.

### Per-port ring buffer

Each `cdc_device` gets a 4 KiB RX ring + spinlock. The polling task
pushes bytes into the head; the head walks forward and bumps the tail
on overflow (lose oldest unread — same trade-off the TTY driver
already makes). 4 KiB at 20 Hz × 64-byte packets = ~3 seconds of
buffer, plenty for line-edit latency.

```c
#define CDC_RX_RING  4096

struct cdc_device {
    ...
    spinlock_t rx_lock;
    uint8_t    rx_buf[CDC_RX_RING];
    uint32_t   rx_head;
    uint32_t   rx_tail;
};
```

### New public API

```c
int usb_cdc_acm_port_count(void);
int usb_cdc_acm_read(int port, void *buf, int n);
int usb_cdc_acm_write_port(int port, const void *data, int len);
```

`port` is 0-indexed (matches `/dev/ttyACM<n>`). Read is non-blocking
and returns 0 when the ring is empty; kernel `SYS_READ` wraps this in
a `task_yield` loop so userland blocks naturally.

### FD plumbing

`task.h` grows one enum entry:

```c
enum {
    ...
    FD_9P,
    FD_CDC_ACM,  // obj_idx = port number
};
```

`SYS_OPEN` short-circuits `/dev/ttyACM<n>` before VFS dispatch (there's
no on-disk path), validates the digit, checks the port count, installs
the FD. `SYS_READ` / `SYS_WRITE_FD` get one new case each. `SYS_CLOSE`
needs no per-kind cleanup — the port is a static rather than refcounted
object.

```
open("/dev/ttyACM0")        -> fd or -1 if no device
read(fd, buf, n)             -> bytes from RX ring (blocks)
write(fd, "hello\r\n", 7)    -> bulk-OUT to dongle
close(fd)                    -> just marks slot FREE
```

No test setup that actually exercises bytes — QEMU doesn't emulate
CDC-ACM (its `usb-serial` is FTDI), and we don't have hardware
passthrough wired. But the file lookup, FD allocation, and dispatch
all work, so plugging a real Arduino in via `-device usb-host,...`
would just work.

---

## Shared-PCI-IRQ storm (the actual hard bug)

When the first iteration of virtio-scsi went in, the *probe* (TUR +
INQUIRY + READ CAPACITY) succeeded and registered the blkdev — and
then boot hung silently between the driver init block and the banner.
No timeout, no crash, just nothing.

### Symptoms

```
virtio-scsi: registered as blkdev slot 1 (vscsi0)
[boot] mounting AdventFS... fs: instance(bdev=boot, ...): 148 entries
[boot] mounting VFS... vfs: mounted 'rootfs' at /
...
[boot] spawning reaper + demo tasks A, B
<silence forever>
```

Removing the `scsi-hd` backing (controller present but no disk
attached) → boot completes normally to the shell. So the bug needed a
real target.

### Diagnosis

Adding `kprintf("[boot] auto-mount: %d blkdev(s)\n", n);` showed we
reach the auto-mount loop and then disappear *inside* the first
`fs_create_instance(vscsi0, ...)` call. Which means the hang is in
the read path — `vscsi_command` → `virtio_submit` → `virtio_wait_used`.

`virtio_wait_used` has a 5-second deadline. Even with a broken
device it should return -1 after 5s. It didn't — 60 seconds in, no
"command timed out" line.

What changed between the early-boot probe (which worked) and the
auto-mount read (which hung)? Just one thing: `e1000_init` ran in
between. e1000 is on **PCI IRQ 11, same line as virtio-scsi**.

```c
// e1000.c (before fix):
isr_register_irq(e->pci.irq_line, e1000_irq);
```

`isr_register_irq` *overwrote* the previously-installed
`virtio_master_irq`. So now when virtio-scsi's INTx asserts:

1. PIC fires IRQ 11.
2. CPU jumps to `e1000_irq`.
3. `e1000_irq` reads e1000's ICR — gets 0 (no e1000 event) — returns.
4. PCI INTx is **level-triggered**. The line stays asserted because
   nobody cleared virtio-scsi's ISR.
5. PIC fires IRQ 11 again. Immediately. Forever.

Even `pit_ticks()` couldn't advance, because the CPU's instruction
boundary check at every step kept seeing IRQ 11 pending and trapping
back into `e1000_irq`. The result was a complete IRQ storm — exactly
the failure mode level-triggered PCI INTx is famous for.

### Fix

`isr_register_irq` was 1-handler-per-line; we needed a chain.
Converted `irq_handlers[16]` into `irq_handlers[16][IRQ_CHAIN_LEN]`
with a per-IRQ count, and walked the chain in `irq_handler`:

```c
static irq_handler_fn irq_handlers[16][IRQ_CHAIN_LEN];
static int            irq_handler_count[16];

void isr_register_irq(int irq, irq_handler_fn h) {
    if (irq < 0 || irq >= 16) return;
    if (irq_handler_count[irq] >= IRQ_CHAIN_LEN) return;
    irq_handlers[irq][irq_handler_count[irq]++] = h;
}

void irq_handler(struct registers *r) {
    int irq = (int)r->int_no - 32;
    if (irq < 0 || irq >= 16) return;
    pic_send_eoi(irq);
    int n = irq_handler_count[irq];
    for (int i = 0; i < n; i++) irq_handlers[irq][i](r);
}
```

Append-only. No removal API — no driver ever unregisters. `IRQ_CHAIN_LEN
= 4` covers the worst real case (rtl8139 + e1000 + virtio-blk +
virtio-scsi on the same QEMU line).

Each handler is responsible for checking its own status register and
no-op'ing if the IRQ wasn't for it. e1000 already did this (read R_ICR,
return if 0); the virtio master dispatcher reads each device's ISR
and only calls the per-device function if bit 0 was set. So the chain
"just works" — both handlers run for every IRQ on the shared line,
the one whose device actually fired clears its latch, the line
de-asserts, forward progress resumes.

### Why this didn't bite before

Before session 124, the only PCI-INTx devices in the codebase were
rtl8139 (IRQ 11) and virtio-net (also IRQ 11 if present in the same
boot). But rtl8139 and virtio-net were almost never run together —
the build-script-generated QEMU commands attach exactly one of them.
e1000 (added session 122) is the first net device users routinely
boot *alongside* a virtio device, because the test pattern is "boot
with e1000 for DHCP + virtio-blk or virtio-scsi for storage."

The virtio master-IRQ dispatcher (session 121) had the right
*structure* to handle sharing — it walks a slot table and only invokes
each per-device hook when that device's ISR bit is set — but it
assumed *all* sharers were virtio. The moment a non-virtio sharer
overwrote `isr_register_irq[N]`, the whole scheme collapsed.

The real fix lives one layer down (the IRQ chain itself), so any
future driver on a shared line — AHCI's IRQ, NVMe later, EHCI — just
works.

---

## Files touched

New:
- `kernel/virtio_scsi.h` + `kernel/virtio_scsi.c` — driver
- `docs/122-pathE-vscsi-cdc-tty.md` — this doc

Modified:
- `kernel/isr.c` — chain-style IRQ handler installation
- `kernel/virtio.c` — comment now refers to the chain
- `kernel/usb_cdc_acm.h` + `kernel/usb_cdc_acm.c` — per-port ring +
  `usb_cdc_acm_{read,write_port,port_count}` API
- `kernel/task.h` — `FD_CDC_ACM` enum
- `kernel/syscall.c` — `/dev/ttyACMn` path resolution + FD_CDC_ACM
  read/write dispatch
- `kernel/kernel.c` — `virtio_scsi_init()` boot probe; auto-mount loop
  extended from USB+AHCI to USB+AHCI+vscsi

kernel.bin (ELF / WSL): 135342 → 139438 bytes (+4 KiB, 71% budget).

---

## Path E status after session 124

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read) + portable WSL build
- ✅ 121 — virtio-9p (write) + IRQ-driven virtio
- ✅ 122 — e1000 NIC + 9p atomic rename
- ✅ 123 — AHCI SATA controller
- ✅ 124 — virtio-scsi + CDC-ACM TTY integration

Still candidate:
- **USB EHCI** — USB 2.0 host controller (480 Mbps vs UHCI's 12)
- **USB CDC-ECM** — USB Ethernet adapter
- **AHCI polish** — IRQ-driven completion, NCQ, multi-slot in flight
- **NVMe** — modern PCIe-attached SSDs
