# Session 41 — USB Mass Storage (BOT + SCSI), block-device abstraction, mount-it demo

**Goal:** finish the USB stack from session 40 — add USB Mass Storage Class on top of UHCI bulk transfers, expose it to user space as a block device, and demonstrate read/write to a QEMU-attached USB drive end to end. Session 40 shipped UHCI + USB core enumeration + HID boot keyboard. This session adds the second leg: bulk transfers, BOT (USB MSC §5), a small SCSI command set (`INQUIRY`, `READ CAPACITY`, `READ(10)`, `WRITE(10)`, `TEST UNIT READY`), a `blkdev` vtable that the existing ATA driver also adopts, three new syscalls (`SYS_BLOCK_INFO/READ/WRITE`), and a `usbtest` user program that reads sector 0, does a write/read round-trip, and a 4-sector multi-block read.

End state — `[t28]` selftest with `-device piix3-usb-uhci -device usb-kbd,port=1 -device usb-storage,drive=usbfs,port=2`:

```
[uhci] PIIX3 controller @ I/O 0xc100  IRQ 11
[usb] port 1: full-speed device attached
[usb] addr 1  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 1: HID iface=0 proto=1 ep=IN1 max=8 int=10ms
[usb] HID keyboard registered (polling starts late)
[usb] port 2: full-speed device attached
[usb] addr 2  full-speed  vid=46f4  pid=1  class=0  ep0_max=8
[usb] addr 2: MSC iface=0  ep_in=IN1  ep_out=OUT2  max=64
[msc] addr 2  vendor="QEMU"  product="QEMU HARDDISK"
[msc] addr 2  capacity = 512 blocks * 512 B = 256 KiB
[msc] registered as blkdev[1] = usb0
...
[t28] USB Mass Storage: SCSI READ/WRITE round-trip via blkdev
== usbtest ==
  blkdev[0] = 'ata0'  (1048576 blocks * 512 bytes = 524288 KiB)
  blkdev[1] = 'usb0'  (512 blocks * 512 bytes = 256 KiB)
  using blkdev[1]
  sector 0 first 16 bytes:
    41 44 56 45 4e 54 46 53 01 00 00 00 00 00 00 00
  PASS  AdventFS superblock magic on USB sector 0
  PASS  USB write+read round-trip (sector 100, 512B pattern)
  PASS  multi-block read (4 sectors at once)
  usbtest exit code = 0
```

The QEMU virtual flash drive enumerates as a USB mass storage device, identifies itself as "QEMU HARDDISK" via SCSI INQUIRY, reports 512 sectors × 512 bytes via SCSI READ_CAPACITY, registers as `blkdev[1]` with name `usb0`, and a user-space program reads its actual content (the AdventFS magic from a separate `usbfs.img` built by `mkfs.py`), writes a known pattern to a high-LBA sector, reads it back, and verifies the round-trip — all through the new `sys_block_*` syscalls layered on top of UHCI bulk transfers, BOT, and SCSI.

## What's in scope

In:

- **`kernel/blkdev.{h,c}`** — block-device vtable + tiny registry (~50 LOC). Drivers register a `struct blkdev { name, block_size, n_blocks, read_fn, write_fn, driver_data }`; consumers (the new syscalls, future FS-on-USB) call through the table.
- **`kernel/ata.c`** — wraps the existing `ata_read_sector` / `ata_write_sector` in `blkdev` adapter functions, registers as `blkdev[0] = ata0`. No behavior change for code that still calls `ata_*_sector()` directly (e.g., `bcache.c`).
- **`kernel/uhci.{c,h}`** — adds `uhci_bulk_in` and `uhci_bulk_out` (~100 LOC). Multi-TD chains (one TD per `ep_max`-byte chunk), depth-linked, alternating data toggle that persists across calls (USB §5.5.4 — the toggle state is per-endpoint, owned by the host).
- **`kernel/usb_msc.{h,c}`** — Bulk-Only Transport + a small SCSI command set (~270 LOC). Builds the 31-byte CBW, drives the optional data phase, validates the 13-byte CSW; SCSI commands `0x00 TEST UNIT READY`, `0x12 INQUIRY`, `0x25 READ CAPACITY (10)`, `0x28 READ (10)`, `0x2A WRITE (10)` are enough to drive a flash drive.
- **`kernel/usb_core.c`** — adds `find_msc_interface` parallel to `find_hid_interface`; binds `bInterfaceClass=0x08` to `usb_msc_attach`.
- **`kernel/syscall.{c,h}`** — three new syscalls:
  - `SYS_BLOCK_INFO  (idx, struct sys_block_info *)` — name, block_size, n_blocks
  - `SYS_BLOCK_READ  (struct sys_block_args { idx, lba, n, buf })` — read N blocks
  - `SYS_BLOCK_WRITE (struct sys_block_args)` — write N blocks
  Both READ and WRITE bounce through a `kmalloc`'d kernel buffer because UHCI DMAs by physical address and user pages aren't identity-mapped.
- **`user/libuser.{c,h}`** — `sys_block_info / sys_block_read / sys_block_write` wrappers + the matching ABI structs.
- **`user/usbtest.c`** — the t28 selftest body. Walks `blkdev[]`, finds the `usb*` device, reads sector 0 (expects ADVENTFS magic), saves+writes+reads-back+restores sector 100, and does a 4-sector multi-block read.
- **`mkfs.py`** — refactored into a generic `build_image(...)` that's called twice: once with the full file list to produce the boot disk's `fs.img` (unchanged), and once with a single `readme.txt` to produce a small (256 KiB, 512-sector) AdventFS-formatted `usbfs.img` that QEMU exposes as the USB drive.
- **`build.sh`** — adds `usbtest` to `USER_PROGS`, prints the new QEMU command line that wires both `usb-kbd` and `usb-storage`.
- **`user/sh.c`** — `[t28]` selftest hook spawns `usbtest.elf`.

Out — deferred to follow-up sessions:

- **True "mount on /mnt/usb so `cat /mnt/usb/readme.txt` works".** The existing `kernel/fs.c` is a single-instance singleton — `g_super` and `g_initialized` are file-scope globals, every read goes through `bcache_read(FS_DISK_OFFSET_SECTORS + offset, ...)`, and `bcache.c` calls `ata_read_sector` directly without a device parameter. To mount the USB drive at a second path you'd need to either (a) make `fs.c` device-parameterized — every call takes a `struct fs_instance *`, with `fs_init(blkdev *)` returning one — and have `bcache.c` cache per-device or just let USB MSC bypass bcache entirely, or (b) duplicate fs.c and bcache.c into a per-device variant. Either is mechanical refactor work, not interesting USB-stack work, so it sits out for now. The `sys_block_*` syscalls give user space block-level access to the USB drive today; tomorrow's FS-on-USB code reads the same blocks by going through the (yet-to-be-written) FS layer instead of `usbtest`.
- **USB hub support.** With both `usb-kbd` and `usb-storage` attached, QEMU will route them through a virtual hub by default unless you pin them to explicit ports of `piix3-usb-uhci` (`bus=usb0.0,port=1` / `port=2`). Walking through a hub means handling class 0x09, the GET_PORT_STATUS / SET_PORT_FEATURE class requests, the spec's "wait between port resets" timing, and a recursive enumeration — well-understood territory but ~150 LOC of new code we don't need for the demo.
- **Asynchronous I/O / interrupt-driven completion.** Both ATA (polled PIO) and USB MSC (synchronous bulk transfers with `wait_chain` spinning until ACTIVE clears) block the calling task. Real systems use IRQ-driven completion + per-device queues + bottom halves. Our usbtest finishes its three rounds of read/write in single-digit milliseconds, so the simple synchronous path is fine for the demo.
- **Multiple LUNs.** USB MSC supports up to 16 logical units behind one device via the `bCBWLUN` field. We hardcode LUN 0. A real card reader (where slots map to LUNs) would need `GET_MAX_LUN` (class request 0xFE) and one `blkdev` per LUN.

## Architecture

```
        QEMU `-device usb-storage,drive=usbfs,bus=usb0.0,port=2`
                                │
                                ▼
                ┌──────────────────────────────────┐
                │  uhci enumerates port 2          │
                │  usb_core: full chapter-9 dance, │
                │  binds class 0x08 → usb_msc_attach│
                └──────────────────────────────────┘
                                │
                                ▼
                ┌──────────────────────────────────┐
                │  usb_msc: bulk reset, INQUIRY,   │
                │  READ_CAPACITY → fills cached    │
                │  m->n_blocks, m->block_size      │
                │  blkdev_register(&m->bdev) → 1   │
                └──────────────────────────────────┘
                                │
                                ▼
                ┌──────────────────────────────────┐
                │  blkdev table:                   │
                │    [0] = ata0  (boot disk)       │
                │    [1] = usb0  (QEMU HARDDISK)   │
                └──────────────────────────────────┘
                       ▲                  ▲
                       │                  │
                  blkdev_get(0)      blkdev_get(1)
                       │                  │
                       │                  │
        ┌──────────────┴──────────────────┴────────┐
        │  syscall.c — SYS_BLOCK_INFO/READ/WRITE   │
        │  bounce buffer: kmalloc → d->read/write  │
        │  → (for USB) → bot_command → CBW/data/CSW│
        │  → uhci_bulk_in / uhci_bulk_out          │
        └─────────────────────┬────────────────────┘
                              │
                              ▼
                       user/usbtest.c
       walks blkdev[] looking for "usb*", does read+write
```

## The `blkdev` vtable

The smallest abstraction that lets ATA and USB MSC coexist:

```c
struct blkdev {
    char            name[BLKDEV_NAME_LEN];   /* "ata0", "usb0", ... */
    uint32_t        block_size;              /* always 512 here       */
    uint32_t        n_blocks;                /* total addressable     */
    blkdev_read_fn  read;                    /* fn(d, lba, n, buf)    */
    blkdev_write_fn write;                   /* fn(d, lba, n, buf)    */
    void           *driver_data;             /* per-driver private    */
};
```

Drivers register their `struct blkdev` at boot. Consumers iterate via `blkdev_count() / blkdev_get(idx)` and call `d->read(d, lba, n, buf)`. Both ATA and USB MSC pass single-sector calls under the hood — ATA via repeated `ata_read_sector`, USB via one SCSI READ(10) per call.

`bcache.c` is left alone: it still calls `ata_*_sector` directly. That keeps the boot path untouched and means USB MSC has no caching today. If the FS-on-USB refactor lands later, `bcache` would either become device-aware (each entry tagged with a `blkdev *`) or USB MSC would get its own small write-through cache — both are easy.

## UHCI bulk transfers

Bulk endpoints carry chunks of `ep_max` bytes (always 64 for full-speed BOT). For a 512-byte SCSI READ(10) data phase, that's 8 TDs in a depth-linked chain:

```c
/* TD i carries bytes [i*64 .. (i+1)*64), token has alternating
 * DATA0/DATA1, last TD has IOC set so wait_chain notices completion. */
for (int i = 0; i < n_tds; i++) {
    g_td_pool[i].status = make_status(0, /*ioc=*/(i == n_tds-1));
    g_td_pool[i].token  = make_token(IN/OUT, addr, ep, t, chunk);
    g_td_pool[i].buffer = phys(buf + i*chunk);
    t ^= 1;     /* toggle alternates per TD */
}
for (int i = 0; i < n_tds-1; i++)
    g_td_pool[i].link = phys(&g_td_pool[i+1]) | UHCI_LINK_DEPTH;
g_td_pool[n_tds-1].link = UHCI_LINK_TERMINATE;

g_qh->element_link = phys(&g_td_pool[0]);
int rc = wait_chain(&g_td_pool[n_tds-1], 500);  /* 500 ms */
g_qh->element_link = UHCI_LINK_TERMINATE;
```

The data toggle (`*toggle` in the public API) is in/out — caller persists it across calls because the device tracks it per-endpoint and a missed flip would cause every subsequent transfer to STALL. Each TD that completes flips the toggle; if the chain stalls partway, the toggle ends up reflecting only the TDs that ran.

The pool-size limit (`UHCI_TD_POOL_SIZE = 16`) caps any one call at 16×64 = 1024 bytes. Larger transfers loop in `bulk_xfer` — for SCSI READ(10) of 32 sectors = 16 KiB that's 16 outer-loop iterations, each doing 16 inner TDs. Plenty for the demo. A real driver would either grow the pool or implement proper queueing.

## Bulk-Only Transport (BOT)

USB MSC §5.1/§5.2 defines the simplest possible mass-storage protocol:

```
            Host                                    Device
              │                                        │
              │  Bulk-OUT: CBW (31 bytes)              │
              │ ─────────────────────────────────────► │
              │                                        │
              │  Bulk-IN  or  Bulk-OUT: data phase     │
              │ ◄────────────────────────────────────► │
              │                                        │
              │  Bulk-IN:  CSW (13 bytes)              │
              │ ◄─────────────────────────────────────  │
              │                                        │
```

The CBW carries an embedded SCSI command (1..16 bytes) plus enough metadata for the device to know how big the data phase will be and which direction it goes:

```
CBW [31 bytes]
  [0..3]   dCBWSignature      = 'USBC'  (0x43425355 little-endian)
  [4..7]   dCBWTag            arbitrary; device echoes in CSW
  [8..11]  dCBWDataTransferLen total bytes in data phase
  [12]     bmCBWFlags         bit 7 = direction (1=IN, 0=OUT)
  [13]     bCBWLUN            target LUN, 0..15
  [14]     bCBWCBLength       SCSI command length, 1..16
  [15..30] CBWCB              the SCSI command itself
```

The CSW the device sends back is the BOT-level success/failure indication (the SCSI sense data, when there's a SCSI-level error, comes via a separate REQUEST SENSE command we don't bother implementing):

```
CSW [13 bytes]
  [0..3]   dCSWSignature      = 'USBS'
  [4..7]   dCSWTag            echoed dCBWTag
  [8..11]  dCSWDataResidue    bytes NOT transferred
  [12]     bCSWStatus         0=passed, 1=failed, 2=phase
```

Our `bot_command` sends one CBW, drives the optional data phase in either direction, reads the CSW, validates the signature + status. ~70 LOC. The whole driver wraps this with one-line SCSI helpers:

```c
static int scsi_inquiry(struct msc_device *m, uint8_t out[36]) {
    uint8_t cb[6] = {0x12, 0, 0, 0, 36, 0};
    return bot_command(m, cb, 6, out, 36, /*data_in=*/1);
}
static int scsi_read10(struct msc_device *m, uint32_t lba, uint16_t n,
                       void *buf) {
    uint8_t cb[10] = {0x28, 0,
        (uint8_t)(lba>>24), (uint8_t)(lba>>16),
        (uint8_t)(lba>> 8), (uint8_t) lba,
        0, (uint8_t)(n>>8), (uint8_t)n, 0 };
    return bot_command(m, cb, 10, buf, n * m->block_size, 1);
}
```

After attach completes, the driver synthesizes a `struct blkdev` whose `read` and `write` adapter funcs unwrap the `driver_data` to a `struct msc_device *` and call the right SCSI command. From the rest of the kernel's POV, USB MSC and ATA look identical.

## QEMU-virtual-USB-disk: `usbfs.img`

`mkfs.py` got refactored into a parameterized `build_image(directories, user_programs, raw_blobs, data_files, out_name, log_prefix)` that's now called twice:

```python
def build():     build_image(DIRECTORIES, USER_PROGRAMS, RAW_BLOBS, DATA_FILES,
                             'fs.img', 'FS')
def build_usb(): build_image([], [], [], [('readme.txt', 'fs/usb-readme.txt', None)],
                             'usbfs.img', 'USB')
```

`usbfs.img` ends up as a perfectly normal AdventFS-formatted disk with one file (`readme.txt`) — superblock magic at offset 0, one entry in the table, the file's data at sector 5. That magic is what `usbtest` looks for to confirm "the bytes the USB drive returns are actually the bytes we wrote into the disk image, not some bogus device-internal cache or zeros". After the build, `usbfs.img` is padded to 256 KiB so SCSI READ_CAPACITY returns a sensible value (511 last-LBA, 512 bytes/block) and the write test at LBA 100 fits comfortably.

QEMU exposes it via:

```
-drive id=usbfs,file=usbfs.img,format=raw,if=none
-device usb-storage,drive=usbfs,bus=usb0.0,port=2
```

The `bus=usb0.0,port=2` matters: QEMU's default behavior when both `usb-kbd` and `usb-storage` are present is to insert a virtual hub and put the second device behind it. Pinning each to an explicit root-hub port avoids the hub entirely, which means the (still-not-implemented) USB hub class driver isn't on the critical path for the demo.

## The DMA-vs-user-pointer trap

The first end-to-end test result was `sector 0 first 16 bytes: 00 00 00 00 ...` — every bit zero, no AdventFS magic, write/read round-trip a guaranteed mismatch. Took five minutes of double-checking the SCSI byte layout to remember why.

UHCI is a bus-mastering DMA controller: when you set a TD's `buffer` field to a 32-bit physical address, the controller goes off and DMAs to that address with no help from the CPU. *Physical* is the operative word. Inside the kernel everything is identity-mapped, so `phys(p) = (uint32_t)p` is correct. But user-space pointers live at virtual address 0x40000000+ where the underlying physical pages live wherever PMM happened to allocate them. `phys(user_buf)` therefore returns 0x40000000+, which the controller dutifully DMAs to — landing somewhere completely unrelated to the user buffer. The user reads zeros because nothing ever wrote to their buffer.

Fix in the syscall layer: bounce through a kernel buffer.

```c
uint32_t bytes = n * d->block_size;
void *kbuf = kmalloc(bytes);    /* identity-mapped */
if (r->eax == SYS_BLOCK_READ) {
    ret = d->read(d, lba, n, kbuf);
    if (ret == 0) memcpy(ubuf, kbuf, bytes);
} else {
    memcpy(kbuf, ubuf, bytes);
    ret = d->write(d, lba, n, kbuf);
}
kfree(kbuf);
```

It costs a memcpy on every block transfer. For a 512-byte single-sector SCSI READ that's negligible. For large bulk transfers a real implementation would either pin the user pages and use their physical addresses directly (Linux's `get_user_pages`) or DMA into a per-process pre-pinned region. Out of scope.

## Test results

`-device piix3-usb-uhci -device usb-kbd,bus=usb0.0,port=1 -device usb-storage,drive=usbfs,bus=usb0.0,port=2`:

```
[uhci] PIIX3 controller @ I/O 0xc100  IRQ 11
[usb] port 1: full-speed device attached
[usb] addr 1  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 1: HID iface=0 proto=1 ep=IN1 max=8 int=10ms
[usb] HID keyboard registered (polling starts late)
[usb] port 2: full-speed device attached
[usb] addr 2  full-speed  vid=46f4  pid=1  class=0  ep0_max=8
[usb] addr 2: MSC iface=0  ep_in=IN1  ep_out=OUT2  max=64
[msc] addr 2  vendor="QEMU"  product="QEMU HARDDISK"
[msc] addr 2  capacity = 512 blocks * 512 B = 256 KiB
[msc] registered as blkdev[1] = usb0
...
[t28] USB Mass Storage: SCSI READ/WRITE round-trip via blkdev
== usbtest ==
  blkdev[0] = 'ata0'  (1048576 blocks * 512 bytes = 524288 KiB)
  blkdev[1] = 'usb0'  (512 blocks * 512 bytes = 256 KiB)
  using blkdev[1]
  sector 0 first 16 bytes:
    41 44 56 45 4e 54 46 53 01 00 00 00 00 00 00 00
  PASS  AdventFS superblock magic on USB sector 0
  PASS  USB write+read round-trip (sector 100, 512B pattern)
  PASS  multi-block read (4 sectors at once)
  usbtest exit code = 0
```

`41 44 56 45 4e 54 46 53` is `ADVENTFS` in ASCII — the superblock magic at offset 0 of `usbfs.img`. `01 00 00 00` after it is the `file_count` field — exactly one file (the readme). The bytes the controller returned over the bulk-IN endpoint are the bytes `mkfs.py` wrote into `usbfs.img`. End-to-end, the USB stack works.

Both USB devices coexist on the same UHCI controller because they're pinned to explicit ports (1 and 2). With `port=` left unspecified QEMU inserts a virtual hub on whichever port the second device claims, and our enumerate-once-no-hub-walk driver then sees the hub instead of the device behind it — known limitation, called out above.

`cryptotest` still passes 19/19 — USB MSC traffic doesn't perturb the network or crypto code paths.

## What's next

- **FS-on-USB.** Refactor `fs.c` so its globals (`g_super`, `g_initialized`, `FS_DISK_OFFSET_SECTORS`) become per-instance state, then mount a second AdventFS rooted at `/mnt/usb` reading from `blkdev[1]`. After that, `cat /mnt/usb/readme.txt` works out of the box because the rest of the kernel (open/read/readdir syscalls, the shell, every coreutil) already goes through VFS. Order-of-magnitude scope: ~150 LOC of mechanical refactor.
- **USB hub class driver.** Walk class 0x09 devices: `GET_PORT_STATUS`, `SET_PORT_FEATURE(PORT_RESET)`, `CLEAR_PORT_FEATURE(C_PORT_*)`, recursive enumeration. Lets `-device usb-kbd -device usb-storage` work without explicit `port=` pinning, and matches what real-world hubs (which are everywhere — every laptop has internal hubs) actually look like.
- **HID mouse.** Mostly the same wiring as the keyboard, slightly different report decoding, push deltas into the existing `mouse_state` struct. ~80 LOC.
- **USB hot-plug.** Handle the connect-status-change interrupt, re-enumerate on insertion, mark devices gone on removal. Also lets `usb_kbd` and friends survive a `-monitor device_del` from QEMU.
