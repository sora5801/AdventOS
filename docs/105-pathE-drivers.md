# Session 118 — Path E: drivers (virtio-blk, virtio-net, CDC-ACM, aplay)

**Goal.** Land the entire Path E driver bundle in one session: a
virtio-blk block device, a virtio-net NIC, a USB CDC-ACM serial
class driver, and a userspace `aplay` consumer for the existing AC97
codec. All four pieces wire into the existing abstractions (blkdev
table, net.c NIC backend, usb_core class dispatcher, SYS_AUDIO_PLAY)
so they look just like any other device from userspace.

Status: **done.** All four drivers compile clean, kernel.bin is 62%
of its budget (123 KiB / 196 KiB), and the bring-up sequence below
is verified under QEMU 10.x.

---

## Verification — what we ran

| QEMU CLI fragment | Result |
|--|--|
| `-device virtio-blk-pci,drive=vd` | `virtio-blk: registered as blkdev slot 1 (vblk0)` + sector-0 probe ok |
| `-device virtio-net-pci,netdev=net0,mac=...` | `net: link up (virtio-net)` → DHCP DISCOVER/OFFER/REQUEST/ACK round-trip ok |
| `-device rtl8139,netdev=net0` (regression) | RTL8139 still preferred over virtio-net; DHCP unchanged |
| no virtio devices | virtio-blk/net init log "probing... done" then no-op, kernel boots as before |

Concrete trace from a virtio-blk + ATA + RTL8139 boot:

```
[boot] initializing ATA driver... ok
[boot] probing virtio-blk... virtio-blk: PCI 0:4.0  io=0xc100  irq=11  subsys=0x2
virtio-blk: capacity = 8192 sectors (4096 KiB)
virtio-blk: registered as blkdev slot 1 (vblk0)
virtio-blk: probe sector 0 ok (00 00 00 00 ...)
...
rtl8139: PCI 0:3.0  io=0xc000  irq=11
net: link up (rtl8139) — MAC 52:54:00:12:34:56  (IP unconfigured — waiting for DHCP)
dhcp: DISCOVER ... got OFFER 10.0.2.15, REQUEST ... ACK
```

And the virtio-net replacement:

```
virtio-net: PCI 0:3.0  io=0xc000  irq=11  subsys=0x1
virtio-net: link up — MAC 52:54:00:12:34:56
net: link up (virtio-net) — MAC 52:54:00:12:34:56  (IP unconfigured — waiting for DHCP)
virtio-net: RX polling task started
dhcp: DISCOVER ... got OFFER 10.0.2.15, REQUEST ... ACK
```

---

## Architecture

Shared substrate (`kernel/virtio.{h,c}`) covers the legacy PCI
transport. Two device-class drivers sit on top:

```
                  [ blkdev table ]   [ net.c g_nic_send ]
                         ^                  ^
                  +------+------+    +------+------+
                  | virtio_blk  |    | virtio_net  |
                  +------+------+    +------+------+
                         |                  |
                         +--------+---------+
                                  |
                          [ virtio.c common ]
                                  |
                          [ pci.c legacy I/O ]
                                  |
                          [ in/out instructions ]
```

The common layer handles:
- feature negotiation (`virtio_negotiate`)
- status handshake (`virtio_status_{reset,ack,driver,driver_ok,failed}`)
- virtqueue setup (`virtio_queue_init`)
- descriptor pool (`virtio_alloc_desc` / `virtio_free_desc_chain`)
- submit + wait (`virtio_submit` / `virtio_wait_used`)

Each device driver provides its own per-request marshalling and
plugs into the right kernel abstraction:

- `virtio_blk.c` registers a `struct blkdev` with name `"vblk0"`
- `virtio_net.c` registers itself as the net stack's NIC backend
  via the new `g_nic_send` function pointer in `net.c`

---

## The legacy transport, in one paragraph

QEMU's transitional virtio devices expose the legacy I/O register
interface at BAR0 (a low-bit-set I/O BAR). The driver walks PCI for
the right vendor (`0x1AF4`) + device id (`0x1000` net, `0x1001` blk,
etc.), then talks to a 20-byte register window plus device-specific
config space at offset `0x14`. Setup is: reset → ACK → DRIVER →
read host features → write back our subset → for each queue (QUEUE_SEL +
read QUEUE_NUM + write QUEUE_PFN) → DRIVER_OK. Submission is: write
the descriptor head index into `avail.ring[avail.idx % qsize]`, bump
`avail.idx`, and `outw(notify, qidx)`. Completion: poll `used.idx`.

---

## Bugs found while landing this

### 1. The qsize cap that broke everything

Initial implementation capped queue size at 64 to keep ring memory
modest. The result: virtio-blk's modal kick reached QEMU but the
device handler returned NULL from `virtqueue_pop` — silent drop, no
trace event, no IRQ raised. After many hours of tracing every read
and write of `avail.idx`, the answer was finally in the QEMU source:

```c
case VIRTIO_PCI_QUEUE_NUM:
    /* not writable in legacy */
    break;
```

In **legacy mode the queue size is fixed by the device.** The driver
MUST use exactly the size advertised by `QUEUE_NUM` — there is no
mechanism (in legacy) to override it. QEMU defaults to 128 for
virtio-blk and 256 for virtio-net, computes `avail`/`used` base
offsets from *its* qsize, and reads garbage if the guest's descriptor
table is smaller. Symptom is `avail.idx` updates the guest writes
never being seen on the host side (because QEMU reads avail from
desc-base + 16*device-qsize, not + 16*driver-qsize).

Fix in [kernel/virtio.c](kernel/virtio.c): bump `VIRTIO_QSIZE_MAX`
to 256 and have `virtio_queue_init` accept whatever the device
advertises. Allocation switched from a fixed 2-page buffer to a
dynamic `kmalloc(total_pages + 1, page-aligned)` to fit the bigger
rings (virtio-net at qsize=256 needs ~10 KiB, three pages).

### 2. Busy-wait + IRQ-off = QEMU starvation

First versions of `virtio_wait_used` busy-polled `used.idx` with
interrupts off (because the caller held a spin_lock). On QEMU TCG
this starves the host main loop — virtio-blk requests use AIO
coroutines that resume from the main loop, so the request never
completes and our timeout (paced by `pit_ticks()`) doesn't tick
either since the PIT IRQ is masked.

Fix: replaced the busy-loop body with an EFLAGS-preserving yield
that briefly enables interrupts long enough for the host to be
serviced:

```c
__asm__ volatile (
    "pushfl\n\t"
    "sti\n\t"
    "hlt\n\t"
    "popfl\n\t"
    ::: "memory", "cc"
);
```

This works regardless of whether the caller had IRQs enabled
(task context — IF was on, stays on after `popfl`) or disabled
(init context before `sti` in `kmain` — IF was off, hlt waits for
one IRQ, then `popfl` restores off). At PIT 100 Hz the per-iteration
delay is at most 10 ms, which is plenty for virtio's BH-driven
completion path. The bonus: `pit_ticks()` advances during the wait,
so the timeout actually works.

### 3. Compiler hoisting the used.idx load

`vq->used->idx` had no `volatile` on its access in the wait loop.
gcc cached the initial load in a register on `-O2`, turning what
should have been a polling loop into an infinite-spin. Fixed by
casting to `volatile uint16_t *` for the inner read:

```c
volatile uint16_t *p_used_idx = &vq->used->idx;
while ((uint16_t)(*p_used_idx - vq->last_used) == 0) { ... }
```

### 4. Low-memory PMM pages and virtio queue placement

Initial allocations used `pmm_alloc_contiguous(2)`, which sometimes
gave back pages at 0xC000 / 0xD000 (well below the kernel image).
QEMU's PCI/PIO emulation refused to process queues placed in that
range — most likely because the legacy SeaBIOS + option ROMs treat
those pages as part of their working set even after the kernel runs.

Symptom was identical to the qsize bug (silent dropped requests),
which made the diagnosis take longer than it should have. Final
fix uses `kmalloc(total + page)` with manual 4 KiB alignment, which
returns memory from the kmalloc heap (always >= 0x100000) and
sidesteps the question entirely.

---

## USB CDC-ACM

CDC-ACM is the "USB serial port" class — Arduino-style boards,
USB modems, BLE radios. The protocol has two interfaces:

| interface | class | endpoints |
|--|--|--|
| Comm | 0x02 (CDC) subclass 0x02 (ACM) | one interrupt-IN for notifications |
| Data | 0x0A (CDC Data) | one bulk-IN, one bulk-OUT |

The driver follows the existing usb_core dispatch pattern (see
`find_msc_interface` for the prior art): walk the configuration
descriptor blob looking for an interface with class 0x0A + an
adjacent class-0x02 ACM interface, find its bulk-IN/OUT endpoints,
and attach. At attach time we issue:

1. `SET_LINE_CODING(115200, 8N1)` — most CDC-ACM firmware gates TX
   on a configured baud, even though USB doesn't really carry one
2. `SET_CONTROL_LINE_STATE(DTR=1, RTS=1)` — Linux's `ttyACM` driver
   does the same; without it many devices drop TX

RX is handled by a kernel polling task at 50 ms cadence (same
pattern as `usb_hid_kbd_task`). Bytes coming from the device land
in `kprintf` so the host sees them on serial. TX is exposed as a
kernel API:

```c
int usb_cdc_acm_write(const void *data, int len);
```

callable from any kernel context. We expect to wire this into a
proper userland TTY in a future session — for now it's a
"works when a device shows up, otherwise stays quiet" driver. QEMU
doesn't ship an emulated CDC-ACM device (its `usb-serial` uses the
FTDI vendor protocol), so the driver is most useful with
`-device usb-host,...` passthrough of a real dongle.

---

## aplay — the AC97 sound consumer

The kernel AC97 driver (`kernel/ac97.c`) and `SYS_AUDIO_PLAY` syscall
have been in place since session 37 (docs/37-ac97-pcm.md). The only
userspace consumer was `beep.elf`, which generates a sine wave inline
and feeds it to the syscall. Path E adds `aplay.elf`, a proper
PCM streamer:

- `aplay <file.wav>` parses the 44-byte canonical PCM WAV header,
  warns on format mismatch (AC97 wants 48 kHz, 16-bit, stereo),
  and streams the data chunk through `sys_audio_play` in 4 KiB chunks
- `aplay` (no args) reads raw PCM from stdin — useful for piping
  generators: `gen | aplay`

The chunked streaming matters because long clips would otherwise
exceed kernel staging (32 × 4 KiB pages ≈ 680 ms at 192 KB/s). With
4 KiB chunks `aplay` blocks on the next `sys_audio_play` only when
the staging slot it wants is still being DMA'd — so playback is
back-pressured naturally.

---

## Files touched

New kernel files:
- `kernel/virtio.h` + `kernel/virtio.c` — common transport
- `kernel/virtio_blk.h` + `kernel/virtio_blk.c` — block driver
- `kernel/virtio_net.h` + `kernel/virtio_net.c` — NIC driver
- `kernel/usb_cdc_acm.h` + `kernel/usb_cdc_acm.c` — USB serial driver

Modified:
- `kernel/pci.h` + `kernel/pci.c` — `pci_find_nth`, `subsystem_id` capture, 8/16-bit config-space reads
- `kernel/usb.h` — CDC class + subclass + request constants
- `kernel/usb_core.c` — `find_cdc_acm_interface` + dispatch into `usb_cdc_acm_attach`
- `kernel/net.c` — pluggable NIC backend (`g_nic_send`); RTL8139 tried first, virtio-net fallback
- `kernel/kernel.c` — `virtio_blk_init()` after ATA, `virtio_net_start_polling()` before DHCP, `usb_cdc_acm_start_polling()` alongside USB HID

New userspace:
- `user/aplay.c` — WAV + raw PCM consumer

Build:
- `build.sh` — `aplay` joins `USER_PROGS`

---

## Path E status after session 118

- ✅ 118 — virtio-blk
- ✅ 118 — virtio-net
- ✅ 118 — USB CDC-ACM
- ✅ 118 — aplay sound consumer

Out of scope, deferred:
- virtio-rng (entropy device — would feed the existing CSPRNG path)
- virtio-console (replacement for the serial UART; would let us drop COM1)
- USB CDC-ECM (USB Ethernet — different from CDC-ACM)
- Stretch: full TTY integration of CDC-ACM (so userland sees it as
  another `/dev/tty*` rather than just kprintf bridging)

kernel.bin: 114864 → 123056 bytes (+8192). aplay.bin: 10056 bytes
(new). All four drivers gracefully no-op when their device isn't
present, so the same os.img boots under any QEMU CLI from
"-smp 1 with nothing" up to the full feature set.
