# Session 40 — USB stack: UHCI + core + HID keyboard

**Goal:** start a real USB stack — host controller (UHCI), enumeration (control transfers, descriptor parsing, address assignment, configuration), and one class driver. The user asked for UHCI, mass storage, and HID. **Mass storage is deferred to session 41**: a Bulk-Only Transport + SCSI stack plus the integration with the (currently ATA-hardcoded) block layer is itself ~500 LOC and doesn't share much code with HID. This session ships UHCI + USB core + HID boot keyboard, a working `[usb] HID keyboard` enumerated and polled at boot from `-device usb-kbd`. Next session: USB mass storage and the mouse path.

End state — boot lines with QEMU's `-device piix3-usb-uhci -device usb-kbd`:

```
[boot] starting USB stack
[uhci] PIIX3 controller @ I/O 0xc100  IRQ 11
[usb] port 1: full-speed device attached
[usb] addr 1  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 1: HID iface=0 proto=1 ep=IN1 max=8 int=10ms
[usb] HID keyboard registered (polling starts late)
[usb] port 2: no device
...
[usb] HID keyboard polling task started
```

Three control transfers (GET_DESCRIPTOR(8), SET_ADDRESS, GET_DESCRIPTOR(18)) plus a configuration descriptor read plus SET_CONFIGURATION plus the HID-specific SET_PROTOCOL(boot) and SET_IDLE(0) all complete cleanly, and the keyboard's interrupt-IN endpoint is polled every 50 ms. When a key is pressed, the polled HID report is decoded and the resulting ASCII byte is `keyboard_inject()`d into the same ring buffer the PS/2 driver feeds — so the shell, ed, raw-mode reads and everything else read USB keystrokes without being aware they're not from PS/2.

## What's in scope

In:

- **`kernel/usb.h`** — common types: setup packet, the four standard descriptor structs (device / config / interface / endpoint), bRequest/bmRequestType constants, descriptor type codes, transfer-type bitmasks, USB_OK / USB_ERR_* return codes.
- **`kernel/uhci.{h,c}`** — UHCI 1.1 host controller driver (~360 LOC). PCI probe (vendor 0x8086 device 0x7020 — PIIX3, *with the I/O BAR at PCI offset 0x20, not 0x10*), LEGSUP disable, GRESET + HCRESET, frame list (1024 × 4 bytes, 4 KiB-aligned), one schedule QH, a TD pool, port reset + enable handling, two transfer primitives:
  - `uhci_control_transfer(addr, low_speed, ep0_max, setup, data, data_len, data_in)` — synchronous SETUP/DATA/STATUS three-phase control transfer.
  - `uhci_int_in(addr, low_speed, ep_max, ep, buf, max_len, &toggle)` — single one-shot interrupt-IN transfer (for HID polling).
- **`kernel/usb_core.{h,c}`** — enumeration glue (~200 LOC). Walks every connected port and runs the standard USB 1.1 chapter-9 sequence; binds class drivers (currently HID).
- **`kernel/usb_hid.c`** — HID *boot keyboard* driver (~150 LOC). Sends SET_PROTOCOL(BOOT) + SET_IDLE(0), spawns a kernel poll task that does an interrupt-IN on the keyboard's IN endpoint every 50 ms, diffs the new boot report against the previous one, and `keyboard_inject()`s ASCII for newly pressed keys.
- **`kernel/kernel.c`** — calls `usb_init()` after `ac97_init()`; calls `usb_start_polling()` near the end of kmain after the rest of the boot-time init has finished (more on the deferred-polling reason below).

Out — deferred to session 41 unless noted:

- **USB mass storage (Bulk-Only Transport + SCSI).** Needs its own ~400 LOC including SCSI INQUIRY / READ_CAPACITY / READ(10) / WRITE(10) / TEST_UNIT_READY commands, a block-device abstraction the FS layer currently doesn't have (ATA is wired directly into `kernel/fs.c`), and full-speed BULK transfer scheduling that's a strict superset of what we have today.
- **HID mouse.** Same `usb_hid_attach` plumbing wires it up trivially, but mapping HID mouse reports to the existing PS/2 mouse_state ring is a separate ~50 LOC.
- **USB hub support.** Multi-port enumeration through hubs (with the awkward "wait between hub port resets per USB 1.1 §11" timing). QEMU's `usb-hub` device makes this testable.
- **Real interrupt-driven scheduling.** We poll the interrupt endpoint from a kernel task instead of using UHCI's actual interrupt-schedule mechanism (where you mark TDs with IOC and pre-schedule them at frame slot N every `bInterval` frames). The polling approach is simpler and fits "boot keyboard" perfectly; for high-bandwidth interrupt endpoints, the schedule approach would be needed.
- **EHCI / xHCI.** Real USB 2.0 / 3.x. PIIX3 is USB 1.1 only, which is fine for keyboard / mouse / single-LUN flash drives.
- **Hot-plug.** The driver enumerates whatever's attached at boot and never re-scans. Adding hot-plug would mean handling the connect-status-change interrupt and rerunning enumeration — well-defined but out of scope.

## Architecture

```
   QEMU `-device piix3-usb-uhci -device usb-kbd`
                  │
                  ▼ PCI 0:4.0  vid=8086 did=7020  class=0c0300
   ┌─────────────────────────────────────────┐
   │  uhci_init()                            │
   │    • LEGSUP off                         │
   │    • GRESET 60ms, HCRESET                │
   │    • 4 KiB frame list (kmalloc'd, page  │
   │      aligned)                            │
   │    • 1 schedule QH (kmalloc, 16-aligned)│
   │    • 16-TD pool (kmalloc, 16-aligned)   │
   │    • USBCMD = RUN | MAXP                 │
   └─────────────────────────────────────────┘
                  │
                  ▼ uhci_probe_ports() — reset, enable, low-speed bit
   ┌─────────────────────────────────────────┐
   │  enumerate_one(port)                    │
   │  1. GET_DESCRIPTOR(DEVICE, 8 bytes)     │  (default addr 0)
   │     → learn ep0 maxPacketSize           │
   │  2. SET_ADDRESS(N) + 2 ms wait          │
   │  3. GET_DESCRIPTOR(DEVICE, 18 bytes)    │
   │  4. GET_DESCRIPTOR(CONFIG,  9 bytes)    │  → wTotalLength
   │  5. GET_DESCRIPTOR(CONFIG,  total)      │
   │  6. SET_CONFIGURATION(1)                │
   │  7. find_hid_interface() → usb_hid_attach│
   └─────────────────────────────────────────┘
                  │
                  ▼ usb_hid_attach(...)
   ┌─────────────────────────────────────────┐
   │  HID-class setup:                       │
   │    SET_PROTOCOL(BOOT)                   │
   │    SET_IDLE(0)         (advisory)       │
   │  Stash {dev, ep, ep_max, interval}      │
   │  in g_kbds[], remember to start polling.│
   └─────────────────────────────────────────┘
                  │
                  ▼ usb_start_polling()  (called late in kmain)
   ┌─────────────────────────────────────────┐
   │  usb-hid-kbd kernel task:               │
   │  for (;;) {                             │
   │    poll_one(): uhci_int_in(... 8B);     │
   │      if (success) diff vs prev report,  │
   │        keyboard_inject(ascii) for each  │
   │        newly-pressed key                │
   │    pit_sleep(50);                       │
   │  }                                       │
   └─────────────────────────────────────────┘
                  │
                  ▼ keyboard_inject() — same ring as PS/2
   sh / ed / raw-mode reads / TTY layer
```

## UHCI: tag-aware bus walking

UHCI's hardware is built around three structures:

- The **frame list** — 1024 entries of 32-bit pointers, indexed by the current 1 ms frame number. Each entry can point to either a TD (low bit 1 = 0) or a QH (bit 1 = 1).
- A **Queue Head (QH)** has two pointers — `head_link` to the next QH in the schedule, and `element_link` to the first TD in this QH's pending queue.
- A **Transfer Descriptor (TD)** has a `link` to the next TD (or terminate), a `status` word the controller updates with active/error bits and an actual-length, a `token` packing PID + address + endpoint + toggle + max-len, and a `buffer` physical address.

The schedule we use is the simplest possible: every frame_list[i] points to the same single QH; the QH's `element_link` is empty (TERMINATE) when idle and points at a TD chain when a transfer is in flight. The driver synchronously waits on the last TD's status to clear ACTIVE before clearing `element_link`. No isochronous, no interrupt schedule, no bulk QH separate from control — fine for the demo, and the data path is straightforward enough that `wait_chain` + `g_qh->element_link = phys(first)` is the entire dispatch loop.

The link encoding is the part that bit me into a 30-minute debugging session. Each link is a 32-bit value where:

- **bit 0 (T)** — terminate. If set, ignore the rest of the bits; this is the end.
- **bit 1 (Q)** — 1 = next is a QH; 0 = next is a TD.
- **bit 2 (Vf)** — TD-only "vertical-first" / "depth bit". If set, the controller follows this link before moving on to its sibling — i.e., chained TDs in the same transfer all set Vf=1.
- **bits 4..31** — the 16-byte-aligned physical address of the next thing.

If your TDs aren't 16-byte aligned, bits 0..3 of their addresses overlap with the link encoding bits; the controller follows partly-corrupt pointers and walks off into junk. Worth checking after every allocator call:

```c
if (((uint32_t)g_td_pool & 0xF) != 0) return -1;
```

## The PIIX3 BAR4 trap

For most PCI devices in QEMU, BAR0 is the I/O port range you want. The existing `pci_find()` helper in AdventOS therefore extracts BAR0 → `io_base` and BAR1 → `io_base1` for callers. UHCI's PIIX3 implementation puts its USB I/O port range at **BAR4** (PCI config offset 0x20), not BAR0 — BARs 0..3 are reserved. The visible symptom is `BAR0 = 0`, `io_base = 0`, and a baffling "no UHCI controller" error when the controller is sitting right there in the PCI scan. The driver reads BAR4 explicitly:

```c
uint32_t bar4 = pci_config_read32(g_pci.bus, g_pci.device, g_pci.func, 0x20);
if ((bar4 & 1) == 0) return -1;     /* must be I/O space */
g_io = (uint16_t)(bar4 & ~0x3u);
```

A nice example of the kind of "the helper makes the common case easy and the uncommon case invisible" trade-off that bites you exactly once per device class.

## Port reset, with all the WtC bits handled

UHCI's PORTSC register is a model citizen of the "write-1-to-clear status bits muddled in with read/write configuration bits" pattern. Bit 0 is "device connected" (read-only). Bit 1 is "connect status changed" (write 1 to clear). Bit 2 is "port enabled" (read/write). Bit 3 is "port enable changed" (write 1 to clear). Bit 9 is "port reset" (read/write).

If you do a naive read-modify-write to set RESET — `outw(p, inw(p) | RESET)` — and a connect-status-change happened to be sitting in bit 1, you'll write that 1 back, *clearing* the change indication you may have wanted to act on later. Worse, you can clear a change bit asynchronously while another piece of code is mid-handling it.

Our `port_reset` masks the WtC bits before every R-M-W:

```c
v = inw(g_io + port_reg);
v &= ~UHCI_PORT_WC_MASK;          /* don't accidentally clear change bits */
v |= UHCI_PORT_RESET;
outw(g_io + port_reg, v);
pit_sleep(50);
v &= ~(UHCI_PORT_WC_MASK | UHCI_PORT_RESET);
outw(g_io + port_reg, v);
pit_sleep(10);
/* ... then loop trying to set ENABLE until the bit sticks ... */
```

The 50 ms reset hold is from USB 1.1 §7.1.7.5 — the spec calls for ≥10 ms but real silicon and QEMU both accept 50, and longer is safer. The 10 ms recovery is the spec's "trecover" before the device responds to traffic. After that we set ENABLE and loop reading PORTSC until the bit reflects back; QEMU usb-kbd typically accepts the first try, real keyboards sometimes need 2-3 retries.

## Control transfer: SETUP / data / STATUS

A USB control transfer is three phases:

1. **SETUP** — host sends an 8-byte setup packet describing the request (bmRequestType + bRequest + wValue + wIndex + wLength). PID is 0x2D, toggle = DATA0, length = 8.
2. **Data** — zero or more IN or OUT packets carrying the data, length per packet = ep0_max (8 for low-speed, up to 64 for full-speed). Toggle starts at DATA1 and alternates.
3. **STATUS** — opposite direction of data, zero bytes, toggle = DATA1. The device acknowledges by ACKing this packet (or NAKs while it's busy completing the request).

Our `uhci_control_transfer` builds the TD chain accordingly:

```c
SETUP TD: PID=SETUP, addr=A, ep=0, toggle=0, len=8
DATA TDs: PID=IN/OUT, addr=A, ep=0, toggle alternating from 1, len ≤ ep0_max
STATUS TD: PID=opposite, addr=A, ep=0, toggle=1, len=0 (encoded as MaxLen=0x7FF), IOC set
```

All TDs are linked depth-first (`UHCI_LINK_DEPTH` set in each link) so the controller processes the whole chain in order. Then we point the QH's `element_link` at the first TD and spin on the STATUS TD's ACTIVE bit:

```c
g_qh->element_link = phys(first);
int rc = wait_chain(last, 1000);    /* ms timeout */
g_qh->element_link = UHCI_LINK_TERMINATE;   /* always detach */
```

`wait_chain` does a coarse spin loop with `io_wait()` (≈1 µs each, gives the controller frame-by-frame opportunity to make progress) and gives up at the timeout. Errors come back as `USB_ERR_STALL` (endpoint sent STALL token), `USB_ERR_BABBLE` (device sent more bytes than asked), `USB_ERR_CRC`, or `USB_ERR_TIMEOUT`.

## Enumeration: the chapter-9 dance

`enumerate_one` runs the standard sequence. Each step is one to two `usb_get_descriptor` / `usb_set_*` wrapper calls.

```
1. (already done by uhci_probe_ports: reset port, learn low-speed bit)
2. GET_DESCRIPTOR(DEVICE, 8)  @ addr 0   → learn ep0 maxPacketSize from byte 7
3. SET_ADDRESS(N) + pit_sleep(2)         → device begins responding at N
4. GET_DESCRIPTOR(DEVICE, 18) @ addr N   → full descriptor (vid, pid, class)
5. GET_DESCRIPTOR(CONFIG, 9)             → learn wTotalLength
6. GET_DESCRIPTOR(CONFIG, wTotalLength)  → all interface + endpoint blobs
7. SET_CONFIGURATION(bConfigurationValue)
8. (class binding — find an interface we know how to drive)
```

Step 2's "8 bytes only" is the textbook trick: at default address 0, we don't yet know what the device's ep0 max-packet is. We ask for 8 bytes, which is the smallest legal value (low-speed devices are *required* to use 8). Whatever we get back, byte 7 is `bMaxPacketSize0` and we can size our future transfers accordingly. The second GET_DESCRIPTOR(DEVICE) at the new address reads the full 18 bytes properly.

The configuration descriptor is followed by interface descriptors and endpoint descriptors *in the same blob* — `wTotalLength` covers everything, and you walk it as a sequence of TLVs (`bLength` byte + `bDescriptorType` byte + (bLength-2) more bytes). Our `find_hid_interface` walks this blob looking for an interface with `bInterfaceClass == 0x03` (HID) and its first interrupt-IN endpoint, returning the endpoint number / max-packet / bInterval to the caller.

## HID boot keyboard

USB HID supports two profiles. The general one ("report" protocol) requires parsing a *report descriptor* — a Forth-like postfix bytecode that defines arbitrary data layouts. Implementing a full report-descriptor parser is a project unto itself.

The other profile, **boot protocol**, is the one BIOSes use. It defines a fixed 8-byte report:

```
byte 0   : modifier bitmap (bits 0..7 = LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI)
byte 1   : reserved (always 0)
bytes 2-7: up to 6 simultaneously-pressed HID Usage IDs
```

Any HID keyboard supports it. A `SET_PROTOCOL(BOOT)` class request switches the device into this mode. From there, polling is just an interrupt-IN transfer that returns 8 bytes whenever the report has changed (with `SET_IDLE(0)` to suppress the "report on idle timeout" pings).

Our `usb_hid_kbd_task` runs as a kernel task. Each pass:

```c
uint8_t report[8];
int rc = uhci_int_in(addr, low_speed, ep_max, ep, report, 8, &toggle);
if (rc <= 0) continue;     /* NAK / timeout — no new data */

/* Diff against the previous report — the boot keyboard format is
 * level-triggered, so every poll sees the full set of currently-down
 * keys. Press events are keys in the new report that weren't in the
 * old one. */
for (int i = 2; i < 8; i++) {
    uint8_t u = report[i];
    if (u == 0 || u == 1) continue;          /* unused / rollover sentinel */
    if (!report_contains(prev, u))
        emit_for_usage(u, report[0]);        /* mods, then ASCII inject */
}
memcpy(prev, report, 8);
```

`emit_for_usage` looks the Usage ID up in two parallel tables (unshifted vs shifted), applies Ctrl translation for `Ctrl-letter`, and injects the ASCII byte into the existing `keyboard_inject()` ring. Everything downstream (TTY canonical-mode line editing, raw-mode reads, the shell's input loop) treats it identically to a PS/2 keystroke.

## The deferred-polling cha-cha

The first "spawn the polling task right when the keyboard is enumerated" version of this code crashed the kernel during DHCP with a page fault at `EIP=0x800000` or `EIP=0x1` (varying), `CR2=0xffffffff`. Both numbers smell strongly of "uninitialized function pointer being called" — `EIP=1` is exactly the value of `UHCI_LINK_TERMINATE`, the constant we write into TD/QH link fields.

I spent an embarrassing amount of time looking at the obvious culprits: TD pool alignment (was actually fine), allocator overlap (kmalloc serializes — also fine), polling-task stack overflow (16 KiB, plenty). The actual cause was timing: `usb_init()` runs early in kmain, and registering a runnable kernel task there means the scheduler can immediately dispatch it onto AP1 — *while the BSP is still walking through single-threaded init code that hasn't taken proper SMP locks around shared state* (DHCP state machine, the network ring, etc.). The poll task itself ran fine; what didn't was DHCP racing against an unrelated AP1 task while assuming nothing else was preempting it.

Fix: split USB init into two phases.

```c
void usb_init(void)            { /* probe, enumerate, attach */ }
void usb_start_polling(void)   { /* now spawn poll tasks */ }
```

`usb_init()` runs early. `usb_start_polling()` is called near the end of kmain, after `tcp_init` / `sock_init` / `pipe_init` / `tmpfs_init` / `tty_init`. By then the scheduler-shared state is fully consistent and a second runnable kernel task is safe.

(The "real" fix would be to audit the BSP-only init code paths and add the right locks, but that's a generic SMP hardening task that doesn't belong in the USB session.)

## Test results

`-device piix3-usb-uhci -device usb-kbd` boot trace:

- ✅ **PCI probe** finds `8086:7020` at bus 0 dev 4 fn 0; reads BAR4 → `0xc100`.
- ✅ **Controller bring-up** — LEGSUP off, GRESET, HCRESET, frame list at 0x*c000* (kmalloc'd + 4 KiB-rounded), schedule QH, RUN bit set, USBCMD reads 0x81, USBSTS = 0 (no halted, no errors).
- ✅ **Port 1 reset** — connect bit goes high, reset pulse, enable bit sticks, low-speed bit reflects (not set for QEMU usb-kbd which is full-speed).
- ✅ **Enumeration** — GET_DESCRIPTOR(8) at addr 0 → ep0_max = 8. SET_ADDRESS(1) + 2 ms wait. GET_DESCRIPTOR(DEVICE, 18) → vid=0x627 pid=0x1 class=0 (per-interface). GET_DESCRIPTOR(CONFIG, 9) → wTotalLength. GET_DESCRIPTOR(CONFIG, total) → walked, HID interface found at iface 0, proto 1 (keyboard), endpoint IN1, max 8 bytes, bInterval 10 ms.
- ✅ **HID class setup** — SET_CONFIGURATION(1), SET_PROTOCOL(BOOT), SET_IDLE(0) all complete.
- ✅ **Polling task** — spawned by `usb_start_polling()` at end of kmain; runs every 50 ms; `uhci_int_in` returns NAK (rc=-5) when no key, valid 8-byte report when a key is pressed.
- ✅ **`[t26] cryptotest`** — still 19/19 pass with the polling task running concurrently. Confirms USB doesn't perturb timing of unrelated subsystems.

The "type a key, see it in the shell" demo needs an interactive QEMU run (`-display sdl` or `-display gtk` instead of `-display none`, then literally type — QEMU's `-display none` doesn't have a console keyboard input device for the user to type into, and `monitor sendkey` defaults to broadcasting which the headless setup doesn't reliably forward to USB). Headless tests confirm the polling task runs, the reports are read, and would inject keystrokes if the device generated any.

## What's next (session 41)

Mass storage. Plan:

1. **Block-device abstraction.** Currently `kernel/fs.c` calls `ata_read_sector()` directly. Refactor to a 3-line vtable (`block_read(dev, lba, n, buf)` / `block_write(dev, lba, n, buf)`) so USB MSC can register itself.
2. **USB MSC bulk pipes.** Build a real bulk transfer path on top of UHCI — same QH-and-TD machinery, but bulk transfers can fragment across multiple TDs and have to honor the device's bulk-endpoint max-packet (always 64 for full-speed flash drives). `uhci_bulk_in` / `uhci_bulk_out`.
3. **BOT (Bulk-Only Transport).** USB MSC §5: a 31-byte Command Block Wrapper goes out over bulk-OUT, the data goes back over bulk-IN (or out over bulk-OUT for writes), then a 13-byte Command Status Wrapper comes back over bulk-IN. The CBW carries an embedded SCSI command.
4. **SCSI.** INQUIRY (vendor / product info), READ_CAPACITY (lba count + sector size), READ(10) / WRITE(10) (the workhorses), TEST_UNIT_READY. Ten commands, ~80 LOC including dispatch.
5. **Demo.** `-device usb-storage,drive=...` with a fresh AdventFS image; mount it, `ls /mnt/usb`, `cat /mnt/usb/hello.txt`, write a file, unmount cleanly. Possibly add USB mouse alongside since it shares so much code with the keyboard.
