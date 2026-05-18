# Session 123 — Path E phase 6: AHCI SATA controller

**Goal.** Add a real-hardware SATA storage driver. The modern block
interface every PC has shipped with for the last ~15 years; what most
laptops actually expose to a freshly-installed OS. Slots into the
`blkdev` table the same way ATA and virtio-blk do, so bcache, the FS
layer, and `SYS_BLOCK_*` syscalls all work uniformly.

Status: **done.** Probe + READ DMA EXT + WRITE DMA EXT + IDENTIFY all
working end-to-end on WSL Ubuntu / QEMU 8.2. Stretch goal landed too:
AdventFS now mounts off an AHCI-attached SATA disk at `/mnt/sata`,
fully browsable from the shell.

---

## Verification — what we ran

QEMU CLI:

```
qemu-system-i386 -drive format=raw,file=os.img -serial stdio -m 32 -smp 1 \
  -drive id=sata0,file=sata_disk.img,format=raw,if=none \
  -device ahci,id=ahci0 \
  -device ide-hd,drive=sata0,bus=ahci0.0 \
  -display none
```

Boot log (with `sata_disk.img` = `cp fs.img sata_disk.img`):

```
[boot] probing AHCI... ahci: HBA @ 0xfebb1000  CAP=0xc0141f05  PI=0x3f  ports=6
ahci: port 0 -> blkdev slot 1 (ahci0)  4679 sectors  (2 MiB)
ahci: ahci0: probe ok (write+read round-trip @ sector 4678)
fs: instance(bdev=ahci0, base=0): 148 entries, 4679 sectors visible
vfs: mounted 'satafs' at /mnt/sata
[boot] mounted ahci0 at /mnt/sata
```

From the shell:

```
advent$ ls /mnt/sata
  init.elf
  sh.elf
  ...
  etc/passwd
advent$ cat /mnt/sata/etc/passwd
root:...:0:0:/:sh.elf
guest:...:1000:1000:/:sh.elf
```

The same files visible at `/` — because in this test we mounted a
copy of the boot FS image. The point isn't the content; it's that
the FS layer reads + writes through the AHCI block device cleanly.

---

## AHCI in one paragraph

The HBA is a single PCI device with a 4-KiB MMIO register file at
BAR5. It exposes up to 32 SATA "ports," each with its own register
block (offset `0x100 + port*0x80`). Per-port memory layout: a
**command list** (1 KiB, 32 × 32-byte slot headers), a **FIS receive
area** (256 bytes where the HBA writes incoming Frame Information
Structures), and one **command table** per active slot (a 64-byte
CFIS + 16-byte ACMD + a PRD list of physical-region descriptors).
To issue a command: fill slot header + table, write the CFIS, drop
PRD entries pointing at your DMA buffers, set the corresponding bit
in the **CI** (Command Issue) register, poll until that bit clears.
That's it.

### Register cheat-sheet

HBA (offsets from BAR5):

| reg | purpose |
|---|---|
| `0x00` CAP | capabilities (port count, NCQ depth, etc.) |
| `0x04` GHC | global host control (`AE` = AHCI enable, `HR` = reset) |
| `0x08` IS  | interrupt status, one bit per port |
| `0x0C` PI  | ports implemented (bitmap) |
| `0x10` VS  | AHCI version |

Per-port (offset `0x100 + port*0x80`):

| reg | purpose |
|---|---|
| `0x00` CLB / `0x04` CLBU | command-list base address (1-KiB aligned) |
| `0x08` FB / `0x0C` FBU | FIS-receive base (256-byte aligned) |
| `0x10` IS / `0x14` IE | per-port IRQ status/enable |
| `0x18` CMD | start (ST), FIS-receive-enable (FRE), CR, FR |
| `0x20` TFD | task-file data (BSY, DRQ, ERR) |
| `0x24` SIG | device signature (`0x101` = SATA disk) |
| `0x28` SSTS | SATA status; low 4 bits = DET |
| `0x2C` SCTL | SATA control |
| `0x30` SERR | SATA errors (write-1-to-clear) |
| `0x38` CI | command issued; one bit per slot |

### Command flow (slot 0)

```
1. Stop port: CMD.ST = 0, wait CMD.CR = 0.
2. Stop FIS receive: CMD.FRE = 0, wait CMD.FR = 0.
3. Allocate clist (1 KiB), fis (256 B), ctbl (256 B) — kmalloc with
   alignment slack; AdventOS's heap is identity-mapped so phys = virt.
4. Write CLB / CLBU / FB / FBU.
5. Clear SERR + IS (write 0xFFFFFFFF).
6. CMD.FRE = 1; CMD.ST = 1.

For each I/O:
7. Fill clist[0]: CFL=5 (5-dword H2D Register FIS), W=write?, prdtl=1,
   ctba = ctbl phys.
8. Fill ctbl.cfis: H2D Register FIS with ATA opcode + 48-bit LBA + count.
9. Fill ctbl.prdt[0]: data buffer phys + (byte_count - 1).
10. Write CI = 1<<0.
11. Poll CI bit 0 until clear (DMA done). Check TFD for ERR.
```

### H2D Register FIS (the actual ATA command frame)

20 bytes packed into the first slot of the 64-byte CFIS area:

```
[ 0] fis_type = 0x27   (Host-to-Device Register)
[ 1] pmp_c    = 0x80   (C=1: this is a command, not a control)
[ 2] command           (ATA opcode — 0x25 read, 0x35 write, 0xEC identify)
[ 3] features-low
[ 4..6]   LBA[ 0..23]
[ 7] device = 0x40     (LBA mode)
[ 8..10]  LBA[24..47]
[11] features-high
[12..13]  count
[14..15]  ICC + control
[16..19]  aux + rsv
```

48-bit LBA + 16-bit count gets us 32 MiB per command (count * 512).
The PRD byte-count caps at ~4 MiB per PRD, but the driver caps at
`n <= 256` sectors per call which is well under both ceilings.

---

## Why polled (no IRQ)

Every AHCI op runs at DMA speeds — even on QEMU, completion is
microseconds. The polling loop `for 5 million iterations watch CI`
finishes in much less time than even one PIT tick, so converting to
IRQ-driven would buy us less than the complexity costs. The hook
point is there if someone wants it later (the HBA has a single PCI
IRQ line in BAR6's IRQ register; the virtio shared-IRQ dispatcher
from session 121 would handle it cleanly).

The bigger reason to leave it polled for now: AHCI's command-list
already has 32 slots, but our driver only uses slot 0. Adding async
completion would also want NCQ-style multiple-in-flight commands,
which is a separate concern. For a uniprocessor OS reading filesystem
metadata, polled-slot-0 is the right tier of complexity.

---

## Filesystem auto-mount

`kmain` already had a USB-only loop that looked for `usb*` blkdevs
and tried to `fs_create_instance + vfs_mount` them at `/mnt/usb`.
Generalized to also handle `ahci*` blkdevs, mounting them at
`/mnt/sata`. Same `fs_create_instance + fs_make_ops_for + vfs_mount`
call shape — the AdventFS layer is filesystem-instance-aware, so
nothing FS-side needed to change.

So with one image-as-AdventFS attached, the kernel boots, finds the
disk, detects the AdventFS magic at sector 0, mounts it, and `ls
/mnt/sata` works from the very first userland shell.

---

## Bugs that didn't bite this round

Two close calls worth recording:

- **Alignment.** Command list = 1 KiB aligned; FIS RX = 256 B
  aligned; command table = 128 B aligned. kmalloc returns 16-B
  alignment; the `alloc_aligned(size, align)` helper oversizes by
  `align` bytes and rounds up. Trivial but get-it-wrong-and-the-HBA-
  refuses-everything important.
- **BSY on first command.** Newly-reset SATA disks come up with
  TFD.BSY=1 for a few hundred microseconds. The `port_start` path
  spin-waits for BSY+DRQ to clear *before* setting CMD.ST. Without
  that pre-wait, the first IDENTIFY DEVICE silently failed about
  half the time.

---

## Files touched

New:
- `kernel/ahci.h` + `kernel/ahci.c` — driver

Modified:
- `kernel/kernel.c` — `ahci_init()` in the boot probe block; the
  `/mnt/usb` auto-mount loop generalized to also try `/mnt/sata` for
  AHCI disks

kernel.bin (ELF / WSL): 131246 → 135342 bytes (+4 KiB, 69% budget).
kernel.bin (PE / MSYS2): 139440 → 143536 bytes (+4 KiB, 73% budget).

---

## Path E status after session 123

- ✅ 118 — virtio-blk, virtio-net, USB CDC-ACM, aplay
- ✅ 119 — virtio-rng, virtio-console, virtio-balloon
- ✅ 120 — virtio-9p (read) + portable WSL build
- ✅ 121 — virtio-9p (write) + IRQ-driven virtio
- ✅ 122 — e1000 NIC + 9p atomic rename
- ✅ 123 — AHCI SATA controller

Still candidate:
- **virtio-scsi** — multi-LUN block device on the virtio family
- **USB EHCI** — USB 2.0 host controller (480 Mbps vs UHCI's 12)
- **USB CDC-ECM** — USB Ethernet adapter
- **CDC-ACM TTY integration** — `/dev/ttyACM0` userspace device
- **AHCI polish** — IRQ-driven completion, NCQ, multi-slot in flight
