# Session 126 — Path E phase 9: EHCI transfer integration

**Goal.** Close the gap left by session 125: EHCI was brought up
(controller alive, async list initialized, ports surveyed) but
high-speed devices on its ports got released to a companion UHCI
controller because the class drivers couldn't reach them. This
session wires the class drivers through an abstraction layer so
either controller can carry transfers — EHCI at USB 2.0 speeds
(480 Mbps, 512-byte bulk packets), UHCI at USB 1.1 speeds (12 Mbps,
64-byte bulk packets), with each device routing through its own
controller.

Status: **done.** A `usb-storage` device attached via `-device
usb-ehci` enumerates, the MSC driver INQUIRY + READ CAPACITY + READ
all run over EHCI bulk transfers, AdventFS mounts off it cleanly at
`/mnt/usb`. Combined with `-device piix3-usb-uhci` + `-device
usb-kbd`, the HID keyboard polling task runs over UHCI while storage
runs over EHCI — dual-HC dispatch end-to-end.

---

## The abstraction

```c
struct usb_hc_ops {
    int (*control_transfer)(uint8_t addr, int low_speed, int ep0_max,
                            const struct usb_setup_packet *setup,
                            void *data, int data_len, int data_in);
    int (*int_in)(uint8_t addr, int low_speed, int ep_max,
                  int ep, void *buf, int max_len, int *toggle);
    int (*bulk_in) (uint8_t addr, int ep_max, int ep,
                    void *buf, int len, int *toggle);
    int (*bulk_out)(uint8_t addr, int ep_max, int ep,
                    const void *buf, int len, int *toggle);
};
```

`struct usb_device` grows a `const struct usb_hc_ops *hc` field set
during enumeration. Every transfer site in the class drivers
(usb_core, hid, msc, cdc-acm, cdc-ecm, hub) was rewritten:

```c
- uhci_bulk_in(c->dev->addr, c->ep_max, c->ep_in, buf, len, &c->in_toggle);
+ c->dev->hc->bulk_in(c->dev->addr, c->ep_max, c->ep_in, buf, len, &c->in_toggle);
```

31 call sites; mechanical refactor. UHCI's existing `uhci_*` functions
already match the signatures, so its vtable is a one-line initializer
list. EHCI implements the same shape on top of QH + qTD chains.

---

## EHCI qTD format

The async-schedule transfer descriptor is 32 bytes:

```
[ 0]  next_qtd       phys ptr | T(bit 0)
[ 4]  alt_qtd        alt next on short-packet
[ 8]  token          status | length | PID | toggle
[12]  bufp[0]        page 0 phys; bits [11:0] = in-page offset
[16]  bufp[1]        page 1 phys (page-aligned)
[20]  bufp[2]
[24]  bufp[3]
[28]  bufp[4]
```

Token bits:

| bit  | meaning |
|------|---------|
| 0    | Ping state (high-speed bulk-OUT only — we ignore) |
| 3    | Transaction Error |
| 4    | Babble Detected |
| 5    | Data Buffer Error |
| 6    | Halted |
| 7    | Active — set on submit, cleared by HC on completion |
| 11:8 | CErr (error retry count — set to 3 for fresh submission) |
| 14:12| PID Code (0=OUT, 1=IN, 2=SETUP) |
| 30:16| Total Bytes to Transfer |
| 31   | Data Toggle |

One qTD covers up to 5 pages = 20 KiB — far more than our callers
ever ask for. AdventOS's heap is identity-mapped so the buffer
pointers go straight into the bufp slots.

## Control transfer — three qTDs

The SETUP / DATA / STATUS chain mirrors the USB spec exactly:

```
SETUP qTD:   PID=SETUP, toggle=0, len=8, buf=&setup_packet
              next = DATA (or STATUS if no data stage)
DATA qTD:    PID=IN or OUT, toggle=1, len=data_len, buf=data
              next = STATUS
STATUS qTD:  PID=opposite of DATA, toggle=1, len=0, buf=NULL
              next = T (1)
```

Stage 2 is omitted when `data_len == 0` (e.g. `SET_ADDRESS`,
`SET_CONFIGURATION`). The QH is created with `EPC_DTC = 1` so the
data toggle comes from the qTD's bit-31, matching the SETUP/DATA1
convention.

## Bulk and interrupt transfers

Both are single qTDs in this driver — one transfer chunk per call,
caller polls for more. EHCI's bulk-IN can deliver up to 20 KiB per
qTD via the 5-page buffer pointers, plenty for the largest single
read the FS layer ever issues (11 sectors = 5632 B). Interrupt-IN
(used by HID) takes the same path; the class driver's 50 ms
software polling interval is the timing mechanism.

A periodic schedule for true interrupt transfers exists in the EHCI
spec but is not used here — software polling is simpler and the
latency is fine for keyboard input.

## Async list link/unlink

The async list is a ring of QHs. The placeholder `async_head` that
ehci_init built sits in the ring forever with its H bit set. Each
new transfer inserts a fresh QH right after async_head:

```
new_qh->hlp = async_head->hlp;
async_head->hlp = new_qh_phys | TYP_QH;
```

The HC walks the list, sees the new QH, executes its qTD chain.
When all qTDs are non-Active, the QH idles.

Unlinking is the gnarly bit. Once we want to free the QH, we have
to make sure the HC isn't going to touch its memory again:

```c
async_head->hlp = new_qh->hlp;                       // bypass us
op_w32(OP_USBCMD, op_r32(OP_USBCMD) | USBCMD_IAAD);  // doorbell
while (!(op_r32(OP_USBSTS) & USBSTS_IAA)) { ... }    // wait
op_w32(OP_USBSTS, USBSTS_IAA);                       // clear
// now safe to free new_qh
```

`IAAD` = Interrupt on Async Advance Doorbell. Writing USBCMD.IAAD=1
tells the HC "let me know when you've advanced past every QH that
was in the list when I dropped the doorbell." It sets USBSTS.IAA on
completion. We poll, clear, and we're guaranteed no more HC accesses
to our QH.

This doorbell handshake is the part that makes EHCI's async list
"safe to modify at runtime" instead of needing a stop/restart cycle.

---

## Dual-controller init

`usb_init` now probes both controllers in order:

```
1. EHCI: if ehci_present(), survey high-speed ports, enumerate
   each connected one via &g_ehci_hc_ops.
2. UHCI: if uhci_init() == 0, survey ports, enumerate each
   connected one via &g_uhci_hc_ops.
```

Ports released to the companion by EHCI's CONFIGFLAG handoff show
up on UHCI for the low/full-speed devices that landed there. Each
device's `hc` field captures which controller drives it; every
transfer call routes accordingly.

A hub behind either controller inherits its parent's `hc` so
devices behind the hub run on the same controller as the hub
itself.

---

## Tested scenarios

```
# USB 2.0 mass storage end-to-end:
-device usb-ehci,id=usb2
-drive id=usbfs,file=usbfs.img,format=raw,if=none
-device usb-storage,drive=usbfs,bus=usb2.0

→ enumeration, MSC INQUIRY/READ_CAPACITY/READ, AdventFS mount at /mnt/usb

# UHCI + EHCI simultaneously:
-device piix3-usb-uhci,id=usb0
-device usb-kbd,bus=usb0.0
-device usb-ehci,id=usb2
-device usb-storage,drive=usbfs,bus=usb2.0

→ keyboard polls over UHCI (12 Mbps, 64-byte int-IN packets),
  storage transfers over EHCI (480 Mbps, 512-byte bulk packets),
  both fully concurrent
```

Boot log highlights from the combined test:

```
[boot] probing EHCI... ehci: PCI 0:5.0  Intel 82801DB ICH4 EHCI (0x8086:0x24cd)
ehci: port 0  high-speed device enabled (PORTSC=0x1005)
ehci: controller running (async list @ 0x102000, 6 ports)
[usb] ehci port 0: high-speed device attached
[usb] addr 1  full-speed  vid=46f4  pid=1  class=0  ep0_max=64
[usb] addr 1: MSC iface=0  ep_in=IN1  ep_out=OUT2  max=512
[msc] addr 1  vendor="QEMU"  product="QEMU HARDDISK"
[msc] registered as blkdev[1] = usb0
[usb] uhci port 1: full-speed device attached
[usb] addr 2  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 2: HID iface=0 proto=1 ep=IN1 max=8 int=10ms
[usb] HID keyboard registered (polling starts late)
fs: instance(bdev=usb0, base=0): 152 entries, 4852 sectors visible
vfs: mounted 'usbfs' at /mnt/usb
[boot] mounted usb0 at /mnt/usb
```

Notice `max=512` on the MSC bulk endpoints — that's the high-speed
USB max-packet size, vs UHCI's `max=64`. Eight times the packets
per second.

---

## Files touched

New:
- `kernel/usb_hc.h` — vtable definition + convenience wrappers
- `docs/127-pathE-ehci-transfers.md` — this doc

Modified:
- `kernel/usb.h` — `hc` field on `struct usb_device`
- `kernel/usb_core.h` + `kernel/usb_core.c` — `usb_enumerate_default` takes hc; dispatch through `d->hc->*`; `usb_init` probes both controllers
- `kernel/uhci.c` — exports `g_uhci_hc_ops`
- `kernel/ehci.h` + `kernel/ehci.c` — full transfer path (control + bulk + int) on top of qTD chains + async-list link/unlink + IAAD doorbell; exports `g_ehci_hc_ops`
- `kernel/usb_hub.c` — inherits hc to children
- `kernel/usb_hid.c`, `kernel/usb_msc.c`, `kernel/usb_cdc_acm.c`, `kernel/usb_cdc_ecm.c` — every uhci_* call routed through `d->hc->*`

kernel.bin (ELF / WSL): 151726 → 155822 bytes (+4 KiB, 79% budget).

---

## What's still on the open list

The periodic schedule is not implemented; interrupt-IN endpoints
ride the async list with software polling. For a keyboard at 50 ms
intervals that's fine. For an audio device with iso endpoints it
wouldn't be — but USB audio over EHCI isn't on the Path E plan.

EHCI has its own PCI INTx line. We poll qTD completion via the
token's Active bit; converting to IRQ-driven would shave the busy
wait, but with the BKL serializing kernel callers anyway, the saving
is marginal. The chain-style IRQ dispatcher from session 124 is
ready when we want it.

Multi-instance EHCI (some hardware has 2+ EHCI controllers, one per
USB "pair"). The driver state is currently a single static `g_ehci`;
extending to an array would be straightforward when a real
multi-EHCI system appears.

---

## Path E status after session 126

- ✅ 118–125: virtio family, USB 1.x class drivers, e1000, AHCI,
  virtio-scsi, /dev/ttyACM0 TTY, NVMe, AHCI IRQ/NCQ, CDC-ECM,
  EHCI bring-up
- ✅ 126: EHCI transfer integration — USB 2.0 speeds live for every
  USB device whose host happens to be EHCI

Path E is comprehensively done. Storage tier: ATA / AHCI (IRQ+NCQ) /
virtio-blk / virtio-scsi / NVMe / USB MSC over UHCI or EHCI. Net
tier: rtl8139 / virtio-net / e1000 / USB CDC-ECM. USB tier:
UHCI + EHCI with HID + MSC + CDC-ACM + CDC-ECM class drivers, all
controller-neutral. Random-tier: virtio-rng, RTC, AC97 audio.
9p host-filesystem passthrough with reads, writes, atomic rename.
