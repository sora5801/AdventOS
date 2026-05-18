# Session 119 — Path E phase 2: virtio-rng, virtio-console, virtio-balloon

**Goal.** Three more virtio devices, with userspace tools for each.
Originally planned as virtio-9p, but this QEMU build (MSYS2 ucrt64 on
Windows) doesn't ship 9p support — pivoted to three smaller virtio
devices that all do exercise the existing legacy transport in
interesting ways.

Status: **done.** All three drivers initialize, the per-device I/O
probe succeeds, and the userspace tools (`rand`, `hvc`, `balloonctl`)
build into the disk image.

---

## Verification — what we ran

```
[boot] probing virtio-rng... virtio-rng: PCI 0:5.0  io=0xc1a0  irq=10
virtio-rng: probe ok (73 f3 1d 72 24 31 6c 64)

[boot] probing virtio-console... virtio-console: PCI 0:7.0  io=0xc140  irq=11
virtio-console: probe ok (TX 38 bytes)

[boot] probing virtio-balloon... virtio-balloon: PCI 0:6.0  io=0xc100  irq=10
virtio-balloon: ready (target=0 pages)
...
virtio-console: RX polling task started
virtio-balloon: cooperation task started
```

Plus a no-regression check: same boot still completes under bare
`-device rtl8139 + -device usb-kbd`, no virtio devices listed.

---

## The three devices

### virtio-rng (PCI device id `0x1005`)

The simplest virtio device that exists. One queue, no feature bits,
no device-specific config space. Each request is a single writable
descriptor — the device fills the buffer with random bytes and
acknowledges via the used ring. QEMU caps each request at the
rng-backend's rate-limit (typically 32 bytes default).

Our API: a single `virtio_rng_get(buf, len)` primitive plus a
`SYS_GETRANDOM` syscall that wraps it with a fallback path. If the
device isn't present, the syscall fills the buffer via xorshift32
seeded with `pit_ticks() XOR rtc_epoch_corrected()` and returns -1 —
that -1 is the signal to callers that the bytes are weak.

Userspace tool: `rand [n] [raw]`:
```
$ rand 16
a0780f78dec50cc9...
$ rand 4 raw | xxd
00000000: 87 12 a3 cf                                ....
```

QEMU CLI:
```
-object rng-builtin,id=rng0
-device virtio-rng-pci,rng=rng0
```

On a Linux host you'd swap `rng-builtin` for `rng-random,filename=/dev/urandom`.

### virtio-console (PCI device id `0x1003`)

Two queues — `rx_vq` (host -> guest) and `tx_vq` (guest -> host) — for
the default console port. We skip `VIRTIO_CONSOLE_F_MULTIPORT` and
the control queues that come with it, which means port 0 works on
its own without sending any control messages.

RX strategy: 4 buffers of 512 bytes pre-armed at init. A kernel
polling task (`vcons-rx`) wakes every 50 ms, drains the used ring,
copies incoming bytes into an in-kernel 2-KiB byte ring, and re-arms
each consumed descriptor.

TX strategy: per-call, allocate one descriptor, memcpy from the
caller into a kernel-side scratch buffer, submit, wait, free. The
caller blocks until the host's RX ring has been consumed.

Userspace tool: `hvc {write "text" | read | echo}`:
```
$ hvc write "hello over hvc0"
hvc: wrote 15 bytes
$ hvc read
(no data)
$ hvc echo
hvc echo: reading + echoing until ^C
[hvc] got: from-host
```

QEMU CLI:
```
-chardev socket,id=hvc0,host=localhost,port=5556,server=on,wait=off
-device virtio-serial-pci,id=vsbus
-device virtconsole,chardev=hvc0,bus=vsbus.0
```

From the host: `nc localhost 5556`. Anything typed on either side
appears on the other.

### virtio-balloon (PCI device id `0x1002`)

Two virtqueues — `inflate` (guest gives pages to host) and `deflate`
(host gives pages back). Plus two u32 fields in the device-specific
config space: `num_pages` (host's request, RO from the driver) and
`actual` (driver's report, WO from the driver).

Cooperation task (`virtio-balloon`) wakes every 1 second:
- Reads `config.num_pages` (the host's target).
- If `target > actual`: allocates pages from PMM, sends their PFNs
  via the inflate queue, updates `config.actual`.
- If `target < actual`: pops pages from the LIFO tracking array,
  sends their PFNs via the deflate queue, returns them to PMM,
  updates `config.actual`.

Inflated pages are tracked in a kmalloc'd `uint32_t pfns[512]` array
— the static cap is 2 MiB out of our 32-MiB guest. Anything beyond
that the driver silently refuses (the host can `balloon 24` and we'll
only give back what fits).

Userspace tool: `balloonctl`:
```
$ balloonctl
actual:   0 pages (0 KiB)
target:   0 pages (0 KiB)
inflated: 0 pages cumulative
deflated: 0 pages cumulative
```

Then from the QEMU monitor: `balloon 30`. The cooperation task notices
the new target on its next tick, inflates by 512 pages (2 MiB cap),
and `balloonctl` shows the new state.

QEMU CLI: `-device virtio-balloon-pci`.

---

## What's new in the syscall surface

```c
#define SYS_GETRANDOM             96
#define SYS_VIRTIO_CONSOLE_WRITE  97
#define SYS_VIRTIO_CONSOLE_READ   98
#define SYS_VIRTIO_BALLOON_STATS  99
```

`SYS_GETRANDOM` returns the byte count on success (== requested
length when virtio-rng is present); returns -1 to signal that the
buffer was filled by the weak fallback instead. Code that needs
crypto entropy should check the return value and refuse to proceed
on -1.

The other three return 0 / count / -1 in the obvious way; see the
syscall.h comments.

---

## Bugs found while landing this

### BSS overflow into VGA RAM (`0xA0000`)

First version of virtio-console kept the 4 KiB byte ring as a static
member of `struct vcons`. Plus virtio-balloon kept a 4 KiB
`uint32_t pfns[1024]` array. Together they pushed kernel .bss past
the VGA RAM hard limit at `0x9FFFF` — the build's BSS check caught
it at link time:

```
ERROR: kernel .bss ends at 0xa18c8 — overlaps VGA RAM (0xA0000+)
```

Fix: both arrays moved to `kmalloc()`-backed pointers (kernel heap
lives above 0x100000 and isn't subject to the VGA-RAM ceiling). Also
shrunk the console ring 4096 -> 2048 and balloon cap 1024 -> 512 pages
as a belt-and-suspenders measure. Net change in kernel.bin size:
+4 KiB for the three new drivers, well within the 196-KiB budget
(62% -> 64% used).

---

## Files touched

New:
- `kernel/virtio_rng.h` + `kernel/virtio_rng.c`
- `kernel/virtio_console.h` + `kernel/virtio_console.c`
- `kernel/virtio_balloon.h` + `kernel/virtio_balloon.c`
- `user/rand.c`
- `user/hvc.c`
- `user/balloonctl.c`

Modified:
- `kernel/virtio.h` — add `VIRTIO_LEGACY_BALLOON` constant
- `kernel/kernel.c` — probe the three new devices at boot + start
  their tasks alongside the existing virtio-net polling
- `kernel/syscall.h` — `SYS_GETRANDOM = 96`, `_VIRTIO_CONSOLE_*` = 97/98,
  `_VIRTIO_BALLOON_STATS = 99`
- `kernel/syscall.c` — dispatch for the new syscalls
- `user/libuser.h` + `user/libuser.c` — userspace wrappers
- `build.sh` — add `rand`, `hvc`, `balloonctl` to USER_PROGS

kernel.bin: 123056 -> 127152 bytes (+4 KiB).

---

## Path E status after session 119

- ✅ 118 — virtio-blk
- ✅ 118 — virtio-net
- ✅ 118 — USB CDC-ACM
- ✅ 118 — aplay sound consumer
- ✅ 119 — virtio-rng (+ SYS_GETRANDOM + `rand`)
- ✅ 119 — virtio-console (+ SYS_VIRTIO_CONSOLE_* + `hvc`)
- ✅ 119 — virtio-balloon (+ SYS_VIRTIO_BALLOON_STATS + `balloonctl`)

Still candidate (deferred):
- virtio-9p (host filesystem passthrough — blocked on QEMU
  ucrt64 build not shipping 9p support; works on Linux hosts)
- USB CDC-ECM (USB Ethernet adapter — sister to CDC-ACM)
- Full TTY integration of CDC-ACM
- virtio-scsi (more capable than virtio-blk for multi-LUN devices)
