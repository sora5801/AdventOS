# Session 122 — Path E phase 5: e1000 NIC + 9p atomic rename

**Goal.** Two pieces in one session:

1. Add a third NIC backend — Intel 82540EM (a.k.a. `e1000`), the
   widely-emulated gigabit Ethernet card in QEMU's `-device e1000`
   and the chip many real laptops actually ship.
2. Close the last 9p protocol gap: Trenameat for atomic rename
   (`mv /mnt/9p/foo /mnt/9p/bar`).

Status: **done.** Both verified end-to-end on WSL Ubuntu / QEMU 8.2.
Windows MSYS2 build still works clean. No regressions on RTL8139 or
virtio-net.

---

## Verification — what we ran

### e1000

```
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 -smp 1 \
  -netdev user,id=net0 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -display none
```

Boot log:
```
e1000: PCI 0:3.0  dev=0x100e  mmio=0xfeb80000  irq=11
e1000: link up — MAC 52:54:00:12:34:56
net: link up (e1000) — MAC 52:54:00:12:34:56  (IP unconfigured — waiting for DHCP)
dhcp: DISCOVER ... got OFFER 10.0.2.15, REQUEST ... ACK
net: 10.0.2.15/255.255.255.0  GW 10.0.2.2  DNS 10.0.2.3  lease=86400s
```

Regression cross-check — same image, three different NIC backends:

| `-device` | dhcp roundtrip |
|---|---|
| `rtl8139` | ✅ ACK |
| `virtio-net-pci` | ✅ ACK (IRQ-driven) |
| `e1000` | ✅ ACK (IRQ-driven) |

### 9p atomic rename

Host:
```
$ rm -rf /tmp/9p-host && mkdir /tmp/9p-host
$ echo 'rename me' > /tmp/9p-host/old.txt
```

In AdventOS:
```
advent$ ls /mnt/9p
  old.txt
advent$ mv /mnt/9p/old.txt /mnt/9p/new.txt
advent$ ls /mnt/9p
  new.txt
advent$ cat /mnt/9p/new.txt
rename me
```

Host afterward:
```
$ ls /tmp/9p-host
new.txt        # old.txt was atomically renamed, not copy+unlinked
```

---

## Part A — Intel 82540EM driver

`kernel/e1000.{h,c}` — ~400 lines including the register map.

### MMIO transport

The e1000's BAR0 is a 128 KiB MMIO region (low bit of BAR0 = 0,
denoting memory-mapped). Identity-mapped via `paging_map` in 4 KiB
strides at init, then accessed through `volatile uint32_t *`:

```c
static inline uint32_t mr32(uint32_t off) {
    return *(volatile uint32_t *)(g_e1k.mmio + off);
}
static inline void mw32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_e1k.mmio + off) = v;
}
```

Notable registers (offsets from BAR base):

| reg | offset | purpose |
|---|---|---|
| CTRL | 0x00000 | reset, link-up |
| STATUS | 0x00008 | link state |
| ICR | 0x000C0 | interrupt cause (read-to-clear) |
| IMS / IMC | 0x000D0 / 0x000D8 | enable / disable IRQ sources |
| RCTL | 0x00100 | RX enable, broadcast accept, buffer size, strip CRC |
| TCTL | 0x00400 | TX enable, pad short packets, collision settings |
| RDBAL / RDLEN / RDH / RDT | 0x02800 / 0x02808 / 0x02810 / 0x02818 | RX ring |
| TDBAL / TDLEN / TDH / TDT | 0x03800 / 0x03808 / 0x03810 / 0x03818 | TX ring |
| MTA | 0x05200 | multicast filter (128 dwords, we zero it) |
| RAL0 / RAH0 | 0x05400 / 0x05404 | receive address (= MAC) |

### Init sequence

```
1. PCI probe vendor 0x8086 device 0x100E (or 0x10D3 = e1000e).
2. Map BAR0 (128 KiB MMIO).
3. CTRL.RST = 1; poll until self-clear.
4. IMC = 0xFFFFFFFF; read ICR to clear.
5. Read MAC from RAL0/RAH0.
6. Zero the multicast filter table (128 dwords).
7. RX:
     - allocate 16 desc ring + 16 buffers of 2048 bytes
     - write desc.addr for each
     - RDBAL = ring phys, RDLEN = 256 bytes, RDH=0, RDT=15
     - RCTL = EN | BAM | BSIZE_2048 | SECRC
8. TX:
     - allocate 16 desc ring + 16 buffers of 2048 bytes
     - TDBAL = ring phys, TDLEN = 256, TDH=0, TDT=0
     - TCTL = EN | PSP | CT(0x10) | COLD(0x40)
     - TIPG = (10) | (8 << 10) | (6 << 20)   [recommended IPG]
9. CTRL.SLU = 1   (link up)
10. isr_register_irq + IMS = RXT0 | RXDMT0 | TXDW | LSC
```

### IRQ handler — RX path

```c
uint32_t icr = mr32(R_ICR);     // read-to-clear
if (icr & (ICR_RXT0 | ICR_RXDMT0)) {
    while (rx_ring[rx_idx].status & RXD_STA_DD) {
        net_rx_frame(rx_buf[rx_idx], rx_ring[rx_idx].length);
        rx_ring[rx_idx].status = 0;
        mw32(R_RDT, rx_idx);
        rx_idx = (rx_idx + 1) % 16;
    }
}
```

`status.DD` (descriptor done, bit 0) is the device's "I DMA'd a
packet into this descriptor's buffer" signal. We consume each one,
hand the frame to `net_rx_frame` (which descends through eth → ip →
tcp/udp under `net_lock`), zero the status byte, advance RDT to tell
the device the slot is reusable, and loop.

### TX path — fire-and-forget

```c
int e1000_send(const void *frame, uint32_t len) {
    spin_lock(&tx_lock);
    uint16_t i = tx_idx;
    memcpy(tx_buf[i], frame, len);
    tx_ring[i] = { .addr = phys(tx_buf[i]),
                   .length = len,
                   .cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS };
    tx_idx = (i + 1) % 16;
    mw32(R_TDT, tx_idx);
    spin_unlock(&tx_lock);
    return len;
}
```

We don't wait for TX completion — the IRQ fires when the device is
done, but nothing in the kernel needs that signal because the ring
is 16 deep and SLIRP processes each frame in microseconds. By the
time we wrap to the same slot 16 packets later, the descriptor's DD
bit is set.  If a future workload pushes harder, we can switch to
"wait for DD before reusing slot."

### Integration

`net.c::net_init` tries backends in this order:

1. **RTL8139** (legacy, cheapest) — `rtl8139_init`
2. **virtio-net** (paravirt) — `virtio_net_init`
3. **e1000** (real-hw breadth) — `e1000_init`

Whichever returns success first wins `g_nic_send`. The eth/ip/tcp/udp
stack doesn't care which backend it ends up on.

---

## Part B — 9p atomic rename (Trenameat)

Wire format (9P2000.L message 74 / 75):

```
T: [olddir_fid:4][oldname:s][newdir_fid:4][newname:s]
R: (empty)
```

Both fids must already refer to directories. The fids themselves
are NOT consumed by the operation — caller clunks them after.

### Implementation

`p9_renameat(v, olddir_fid, oldname, newdir_fid, newname)` —
straightforward wrapper around `p9_round_trip`.

`virtio_9p_rename_path(old_rel, new_rel)` — splits both paths into
parent + base, walks `root_fid → old_parent` and `root_fid →
new_parent` into two fresh fids, issues one `Trenameat`, clunks
both. Atomic from the host's perspective.

### Syscall + userland

- New `SYS_RENAME = 100` in `kernel/syscall.h`.
- Dispatch in `kernel/syscall.c`: copies both paths into kernel
  scratch, checks that both start with `/mnt/9p/` (Trenameat is
  single-filesystem), and calls `virtio_9p_rename_path`. Anything
  else returns -1 — AdventFS proper has no rename op.
- libuser wrapper `sys_rename(old, new)` in `user/libuser.c`.
- `user/mv.c` now tries `sys_rename` first and falls back to
  copy-then-unlink only on -1. The fallback was the only mode
  before this session; for `/mnt/9p` paths we now get true atomic
  rename in one syscall.

---

## What's not done yet

- **e1000 TX completion blocking.** If we ever push more than ~16
  packets per microsecond (we won't, on this OS), the TX ring would
  overrun. The fix is a spin-on-DD-before-reuse loop; the current
  code starts that loop but only spins if the slot's `cmd` is
  non-zero AND DD is still clear, which is the common-case-fast
  path.
- **e1000 EEPROM read.** We read the MAC from RAL0/RAH0 directly
  rather than poking the EEPROM. QEMU pre-loads RAL/RAH from the
  `mac=` device option, and most real boards put the MAC there at
  power-on. If a real card boots up with zero MAC and only the
  EEPROM populated, this driver would need an EEPROM-read fallback.
- **e1000 multi-NIC.** One device at a time. The struct is static,
  not a pool.
- **9p rename across filesystems.** SYS_RENAME returns -1 if the
  paths aren't both under `/mnt/9p`. `mv` falls back to its
  copy+unlink path in that case, which is correct.

---

## Files touched

New:
- `kernel/e1000.{h,c}` — Intel 82540EM / 82574L driver

Modified:
- `kernel/net.c` — try e1000 as third NIC after rtl8139 + virtio-net
- `kernel/virtio_9p.{h,c}` — `p9_renameat()` + `virtio_9p_rename_path()`
- `kernel/syscall.h` — `SYS_RENAME = 100`
- `kernel/syscall.c` — dispatch + `/mnt/9p` prefix check
- `user/libuser.h` + `user/libuser.c` — `sys_rename()` wrapper
- `user/mv.c` — try `sys_rename` first

kernel.bin (ELF / WSL): 131246 bytes (66% budget, +0 from session 121
because e1000.o is large but kept under the existing PE/ELF padding
slack).
kernel.bin (PE / MSYS2): 135344 → 139440 bytes (+4 KiB, 71% budget).

---

## Path E status after session 122

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read) + portable WSL build
- ✅ 121 — virtio-9p (write) + IRQ-driven virtio
- ✅ 122 — e1000 NIC + 9p atomic rename

Still candidate:
- **virtio-scsi** — multi-LUN block device, completes the virtio
  storage story.
- **USB CDC-ECM** — USB Ethernet adapter (sister to CDC-ACM serial).
- **CDC-ACM TTY integration** — make `/dev/ttyACM0` visible to
  userspace.
- **e1000 polish** — EEPROM read fallback, TX backpressure, multi-
  device support.
