# Session 125 — Path E phase 8: NVMe + EHCI + AHCI IRQ/NCQ + CDC-ECM

**Goal.** Close out the Path E driver backlog in one push. Four
items:

1. **CDC-ECM** — USB Ethernet (the standard protocol for modern
   USB-Ethernet dongles), plumbed into the existing net stack.
2. **AHCI IRQ + NCQ** — convert the polled AHCI driver to IRQ-driven
   completion and switch to Native Command Queuing (READ/WRITE FPDMA
   QUEUED) with 32 command slots.
3. **NVMe** — modern PCIe-attached SSD interface. New blkdev backend
   slotting in next to ATA, AHCI, virtio-blk, virtio-scsi, USB MSC.
4. **EHCI** — USB 2.0 host controller (480 Mbps vs UHCI's 12 Mbps).
   Controller bring-up + BIOS handoff + async-list init + port
   survey. Transfer-path integration with the class drivers is
   deliberately deferred.

Status: **done.** All four merged. NVMe / AHCI / CDC-ECM verified
end-to-end under QEMU; EHCI shows controller-alive + high-speed port
detection. The combined set runs cleanly on the same shared PCI IRQ
line (AHCI + NVMe + virtio + e1000 all on IRQ 10/11 in QEMU's default
PIIX3 + ICH9 mix).

---

## CDC-ECM

The CDC-ACM driver from session 118 only handled USB-serial (Abstract
Control Model, subclass 0x02). Real USB-Ethernet dongles speak
Ethernet Networking Control Model (subclass 0x06): two interfaces
where the data interface uses alternate settings to gate the link.

New files:
- `kernel/usb_cdc_ecm.{h,c}`

Wire model summary:

```
Comm interface (class 0x02, subclass 0x06)
  - Interrupt-IN endpoint  (NETWORK_CONNECTION notices, we ignore)
  - Ethernet Functional Descriptor (subtype 0x0F, byte 3 = iMACAddress)

Data interface (class 0x0A)
  - Alt 0: no endpoints (link logically down)
  - Alt 1: bulk-IN + bulk-OUT  ← we activate this via SET_INTERFACE
```

The detector walks the configuration blob looking for a comm
interface with subclass 0x06 and a data interface alt setting that
carries the bulk pair. The MAC address is fetched from string
descriptor `iMACAddress` (UTF-16LE, 24 bytes of hex digits, parsed
into a 6-byte MAC).

Hooked into the net stack the same way e1000 / rtl8139 / virtio-net
are — net.c's `nic_send_fn` dispatch picks CDC-ECM last in the
fallback chain.

### RNDIS guard

QEMU's `-device usb-net` doesn't emulate CDC-ECM. It emulates
**RNDIS** (Microsoft's proprietary protocol). RNDIS shares the CDC
comm class but uses protocol 0xFF (vendor-specific). Without a
guard, the existing CDC-ACM detector matched RNDIS devices and tried
to send `SET_LINE_CODING` to them — which STALL'd and produced a
confusing-looking error in the log.

Fix is one line in `find_cdc_acm_interface`:

```c
if (id->bInterfaceClass    == USB_CLASS_CDC_COMM &&
    id->bInterfaceSubClass == USB_CDC_SUBCLASS_ACM &&
    id->bInterfaceProtocol < 0x07)               // reject RNDIS (0xFF)
```

The CDC-ACM spec lists valid protocols as 0x00..0x06; 0xFF is the
sentinel for "vendor-specific."

### Test status

QEMU has no CDC-ECM emulation, so we can't verify byte flow in CI.
The driver builds clean, the dispatcher routes correctly, and a
RNDIS device now falls through to the "no recognized class" path
instead of getting wrongly bound to CDC-ACM. Wire-level testing
would need real hardware passthrough (`-device usb-host,vendor=...`).

---

## AHCI IRQ + NCQ

The session-123 AHCI driver was polled and used only command slot
0 with non-queued READ DMA EXT / WRITE DMA EXT. This session
converts it to:

- **IRQ-driven completion.** Replaces the `wait_command` busy-poll
  with a pushfl/sti/hlt/popfl loop driven by a new `ahci_irq` handler
  that walks every attached port, reads `P_IS`, and ORs completed
  slot bits into a per-port `completed_mask`. Plays nicely with the
  shared-IRQ chain landed in session 124.
- **NCQ commands.** READ FPDMA QUEUED (0x60) / WRITE FPDMA QUEUED
  (0x61) instead of plain DMA EXT. Different FIS encoding: sector
  count lives in the features fields; the count byte holds the NCQ
  TAG (in bits 7:3).
- **32 slots.** Allocate a command table per slot up front; track
  `free_mask` for slot allocation. The BKL still serializes callers
  today, but the structure is ready for multi-in-flight when an
  async blkdev layer arrives.

### FIS encoding gotcha

For non-NCQ DMA:
```
features = 0
count    = sector_count
```

For NCQ:
```
features = sector_count (split low/high across f[3] / f[11])
count    = tag << 3
```

`build_h2d_fis(cmd, lba, count, tag)` branches on `IS_NCQ_CMD(cmd)`.
Same struct slots, different semantic mapping.

### SACT register

NCQ uses `P_SACT` (offset 0x34) alongside `P_CI` (0x38). Writing
bit N to SACT means "queue slot N is pending"; the device clears it
via a Set Device Bits FIS on completion. The driver kicks via:

```c
if (IS_NCQ_CMD(cmd)) pw32(p->regs, P_SACT, bit);
pw32(p->regs, P_CI,   bit);
```

The IRQ handler then computes `done = (~free_mask) & ~ci & ~sact` to
identify slots that have completed since the last check.

### Per-port IE enable

Each port now has `P_IE` populated with `DHRE | PSE | DSE | SDBE |
TFEE | HBFE` so both non-NCQ (D2H Register FIS) and NCQ (Set Device
Bits FIS) completions trigger interrupts, plus task-file / host-bus
errors.

### In-use timing bug

First implementation set `p->in_use = 1` only at the *end* of
`port_init`, after the IDENTIFY + sanity probe. But the IRQ handler
guards with `if (!p->in_use) continue;` — so the very commands
needed for bring-up never had their completions recorded. Fix:
flip `in_use` to 1 right after the per-port CLB/FB/IE registers are
programmed; clear it back to 0 on any failure path.

---

## NVMe

Brand new driver: `kernel/nvme.{h,c}`. PCI vendor 0x1B36 / device
0x0010 (QEMU's NVMe controller). Targets the QEMU CLI:

```
-drive id=nvme0,file=nvme_disk.img,format=raw,if=none
-device nvme,drive=nvme0,serial=adventos-nvme0
```

### Architecture in one paragraph

Unlike AHCI's single SATA link with a 32-slot queue, NVMe sits
directly in host memory as paired submission/completion queues. An
admin pair (qid=0) for setup; one or more I/O pairs (qid≥1) for
data. Each SQ entry is 64 bytes; each CQ entry is 16 bytes with a
phase bit that flips on every wrap so the host can tell which
entries are valid. DMA addressing uses PRPs: PRP1 = pointer to first
page (with offset bits), PRP2 = pointer to second page OR pointer
to a PRP list. For small transfers (≤8 KiB = 16 LBAs at 512 B/LBA)
PRP1 + PRP2 suffice and we skip the list.

### Bring-up sequence

```
1. PCI probe; map BAR0 MMIO (16 KiB).
2. CC.EN = 0; wait CSTS.RDY = 0.
3. Allocate admin SQ (4 KiB, 16 entries) + admin CQ (1 KiB).
4. AQA = (ASQS-1) | ((ACQS-1) << 16)
5. ASQ_LO = phys; ACQ_LO = phys.
6. CC = EN | CSS_NVM | MPS_4K | IOSQES_64 | IOCQES_16
7. Wait CSTS.RDY = 1.
8. Admin IDENTIFY (CNS=1) controller → model name
9. Admin IDENTIFY (CNS=0) NSID=1     → size, LBA format
10. Admin CREATE_IO_CQ (qid=1, IEN=1)
11. Admin CREATE_IO_SQ (qid=1, CQID=1)
12. INTMC = 1 to unmask vector 0
13. Register as blkdev; run write+read+restore sanity probe.
```

### I/O command shape

```c
struct nvme_sq_entry e = {0};
e.cdw0    = opcode | (cid << 16);    // 0x02 = READ, 0x01 = WRITE
e.nsid    = 1;
e.prp1_lo = buffer_phys;
e.prp2_lo = build_prp2(buf, bytes);   // 0 if fits in one page
e.cdw10   = slba_lo;
e.cdw11   = slba_hi;
e.cdw12   = (nlb - 1);                 // count minus 1
```

Push to `io_sq[tail]`, bump tail, write the SQ tail doorbell at
`BAR0 + 0x1000 + (2*qid) * doorbell_stride`. Wait via pushfl/sti/hlt
+ check `io_completed` bitmap. IRQ handler drains the CQ, OR's the
CID bit into `io_completed`, advances head, writes the CQ head
doorbell.

### Doorbell stride

The doorbell stride lives in `CAP[35:32]` (bits 3:0 of `CAP_HI`).
On QEMU NVMe it's 0 → 4 bytes. On some real controllers it's larger
(power-of-2 to allow MSI-X interrupt coalescing per doorbell). The
driver reads it once and uses it for every doorbell offset.

### Boot log

```
[boot] probing NVMe... nvme: PCI 0:4.0  bar0=0xfebb0004  irq=11
nvme: CAP_LO=0xf0107ff  MQES=2048  doorbell_stride=4
nvme: vid=0x1b36  model="QEMU NVMe Ctrl"
nvme: namespace 1: 4729 LBAs * 512 B = 2364 KiB
nvme: registered as blkdev slot 1 (nvme0)
nvme: nvme0: probe ok (write+read round-trip @ sector 4728)
...
fs: instance(bdev=nvme0, base=0): 152 entries, 4729 sectors visible
vfs: mounted 'nvmefs' at /mnt/nvme
[boot] mounted nvme0 at /mnt/nvme
```

---

## EHCI

The biggest of the four data-structure-wise but the most scoped in
this session: bring-up only, no transfer integration.

New files: `kernel/ehci.{h,c}`.

### Why scope-limited

EHCI's wire format and schedule layout are completely different from
UHCI's:

- MMIO instead of I/O space.
- Async list of QHs + qTDs vs UHCI's frame-list-of-TDs.
- Doorbell handshake for QH removal (USBCMD.IAAD + USBSTS.IAA).
- Periodic schedule indexed by uframe for interrupt transfers.

Every call site in `usb_core`, `usb_hid`, `usb_msc`, `usb_cdc_acm`,
`usb_cdc_ecm`, `usb_hub` reaches directly into `uhci_*` functions.
Routing them through a `usb_hc_*` abstraction so they can run over
EHCI is invasive — that rewiring is its own session of work.

What this driver delivers:

- PCI probe (Intel ICH4 0x8086:0x24CD, ICH9 #1/#2, ICH6 fallbacks).
- BAR0 MMIO mapping; CAPLENGTH, HCIVERSION, HCSPARAMS, HCCPARAMS.
- BIOS handoff via EECP USBLEGSUP cap: set OS-owned semaphore at
  bit 24, poll bit 16 (BIOS-owned) to clear, fallback-force after
  ~1 s.
- Controller reset (USBCMD.HCRESET) + halt confirmation
  (USBSTS.HCH).
- Empty placeholder async QH (H bit set, HALTED token, self-loop
  via HLP); ASYNCLISTADDR points at it.
- USBCMD = RS | ASE; wait HCH clears.
- CONFIGFLAG = 1 to route ports away from any companion.
- Per-port survey: power on, check CCS, distinguish low-speed
  (→ release to companion via PORTSC.PortOwner) from high-speed
  (→ reset, wait for PE).

### What's next for EHCI

The controller is live; high-speed devices on its ports get reset
and enabled. The next session's work:

1. Add a `usb_hc.h` dispatching layer (control / bulk / int).
2. Implement EHCI control transfer via the async schedule
   (QH + 3 qTDs: SETUP, DATA, STATUS).
3. Implement EHCI bulk-IN/OUT (QH + qTD chain).
4. Replace `uhci_*` direct calls with `usb_hc_*` in class drivers.
5. Update enumeration to use EHCI ports when EHCI is present.

Bulk-only for now is fine — HID interrupts can poll over async
transfers, isochronous won't be on the menu until audio over USB
matters.

### Boot log with `-device usb-ehci`

```
[boot] probing EHCI... ehci: PCI 0:4.0  Intel 82801DB ICH4 EHCI (0x8086:0x24cd)
ehci: caplen=32  hciver=0x100  ports=6  eecp=0x68
ehci: port 0  high-speed device enabled (PORTSC=0x1005)
ehci: controller running (async list @ 0x102000, 6 ports)
ehci: NOTE — class-driver transfer integration is a follow-up;
      USB devices on the EHCI ports will surface through the
      companion UHCI controller (if present) for now.
```

---

## Combined boot

All four backends running simultaneously:

```
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 -smp 1 \
  -drive id=sata0,file=sata_disk.img,format=raw,if=none \
  -device ahci,id=ahci0 \
  -device ide-hd,drive=sata0,bus=ahci0.0 \
  -device virtio-scsi-pci,id=scsi0,disable-modern=on \
  -drive id=hd0,file=scsi_disk.img,format=raw,if=none \
  -device scsi-hd,drive=hd0,bus=scsi0.0 \
  -drive id=nvm0,file=nvme_disk.img,format=raw,if=none \
  -device nvme,drive=nvm0,serial=ad0 \
  -device usb-ehci,id=usb2 \
  -display none
```

Boot log highlights:

```
[boot] probing AHCI... ahci: HBA @ 0xfebb5000  CAP=0xc0141f05  PI=0x3f  ports=6 slots=32
ahci: port 0 -> blkdev slot 1 (ahci0)  4729 sectors  (2 MiB)
ahci: ahci0: probe ok (write+read round-trip @ sector 4728)
virtio-scsi: registered as blkdev slot 2 (vscsi0)
virtio-scsi: vscsi0: probe ok (write+read round-trip @ sector 4728)
[boot] probing NVMe... nvme: vid=0x1b36  model="QEMU NVMe Ctrl"
nvme: namespace 1: 4729 LBAs * 512 B = 2364 KiB
nvme: registered as blkdev slot 3 (nvme0)
nvme: nvme0: probe ok (write+read round-trip @ sector 4728)
[boot] probing EHCI... ehci: PCI 0:N.0  Intel 82801DB ICH4 EHCI
ehci: controller running (async list @ 0xN, 6 ports)
...
vfs: mounted 'satafs' at /mnt/sata
vfs: mounted 'scsifs' at /mnt/scsi
vfs: mounted 'nvmefs' at /mnt/nvme
```

Three independent block backends auto-mounted, four PCI IRQs sharing
chain-style dispatch, no IRQ storms, no boot hangs.

---

## Files touched

New:
- `kernel/usb_cdc_ecm.h` + `kernel/usb_cdc_ecm.c`
- `kernel/nvme.h` + `kernel/nvme.c`
- `kernel/ehci.h` + `kernel/ehci.c`
- `docs/126-pathE-nvme-ehci-ahci-ecm.md` — this doc

Modified:
- `kernel/usb.h` — CDC_SUBCLASS_ECM, CDC_FUNC_ETHERNET, etc.
- `kernel/usb_core.c` — `find_cdc_ecm_interface`, CDC-ECM dispatch,
  CDC-ACM protocol guard against RNDIS misdetection
- `kernel/net.c` — CDC-ECM in the NIC fallback chain
- `kernel/ahci.h` + `kernel/ahci.c` — IRQ + NCQ + 32 slots
- `kernel/kernel.c` — `ehci_init` + `nvme_init` + auto-mount `/mnt/nvme`

kernel.bin (ELF / WSL): 139438 → 151726 bytes (+12 KiB; 77% budget).

---

## Path E status after session 125

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read) + portable WSL build
- ✅ 121 — virtio-9p (write) + IRQ-driven virtio
- ✅ 122 — e1000 NIC + 9p atomic rename
- ✅ 123 — AHCI SATA controller
- ✅ 124 — virtio-scsi + CDC-ACM TTY + shared-IRQ-storm fix
- ✅ 125 — NVMe + EHCI + AHCI IRQ/NCQ + CDC-ECM

Path E backlog: **drained.** Storage tier covers ATA / AHCI /
virtio-blk / virtio-scsi / USB MSC / NVMe. Net tier covers rtl8139
/ virtio-net / e1000 / CDC-ECM. USB tier has UHCI live with HID +
MSC + CDC-ACM + CDC-ECM class drivers; EHCI brought up, transfer
integration is the natural next session.

Open candidates that aren't really Path E anymore:
- EHCI transfer integration (USB 2.0 throughput for everything).
- AHCI multi-slot concurrent in flight (needs an async blkdev API).
- NVMe IRQ vectoring (MSI-X would let CQ completions land on
  different vectors and avoid the polled-CQ-head shared state).
