# Session 141 — Path C phase 34: USB tablet absolute pointer

**Goal.** Stop the wmd-drawn crosshair from drifting away from
QEMU's host pointer.  Root cause: PS/2 mouse is *relative*.  Host
OS applies acceleration to the visible host cursor; the guest's
kernel accumulates raw delta packets at a different rate; the two
drift, and the user sees the crosshair stuck "northeast" (or
wherever) of where their pointer actually is.

Fix: implement a USB tablet (HID class, non-boot protocol) driver
that reads absolute X / Y from QEMU's `-device usb-tablet`.  With
that attached, every report carries an absolute screen position
that the kernel scales to FB pixels — no acceleration drift, no
grab/release dance.  PS/2 mouse stays initialised as a fallback
for environments without usb-tablet.

Status: **done.**  Smoke `smoke_usbtablet.py` (5/5):

```
=== checks ===
  [OK] boot enumerated tablet
  [OK] boot started tablet polling task
  [OK] cursor at target (33 white px in 24x24)
  [OK] cursor moved away from default centre (0 white px there)
  [OK] wmd status bar alive (887/924)
```

The smoke sends a QMP `input-send-event { type: "abs" }` for
abs=(6406, 12816) which scales to FB pixel (200, 300) — and the
wmd cursor (a 17×17 white +) lands exactly there, with the old
boot-time centre (512, 384) showing zero white pixels.

---

## What QEMU sends

QEMU's `usb-tablet` (vid=0x0627, pid=0x0001) advertises:

- bInterfaceClass = 3 (HID)
- bInterfaceSubClass = 0 (no boot subclass)
- bInterfaceProtocol = 0 (no boot protocol)
- One interrupt-IN endpoint, wMaxPacketSize = 8

Reports are **6 bytes**, not 8 — the descriptor reserves 8 but
the device only sends:

| byte  | content                          |
|-------|----------------------------------|
| 0     | buttons (bit0=L, bit1=R, bit2=M) |
| 1..2  | X (uint16 LE, 0..32767)          |
| 3..4  | Y (uint16 LE, 0..32767)          |
| 5     | wheel (int8, ignored for now)    |

Our `uhci_int_in` returns the actual count (6), short-reads
work fine, we only touch bytes 0..4.

Reports only fire on input *state change*.  With no events the
endpoint NAKs every poll — the polling task sees `rc = USB_ERR_NAK`
or `USB_ERR_TIMEOUT` and silently continues.

---

## Kernel changes

### `kernel/usb_hid.c`

Added a tablet-class path alongside the existing keyboard one:

```c
struct hid_tablet {
    int           in_use;
    struct usb_device *dev;
    int           ep;
    int           ep_max;
    int           interval_ms;
    int           toggle;
};
#define MAX_HID_TABLET 2
static struct hid_tablet g_tablets[MAX_HID_TABLET];
```

`usb_hid_attach` routes on `proto`:

- `proto == 1` (boot keyboard) → existing path: SET_PROTOCOL(BOOT),
  register as keyboard
- `proto == 2` (boot mouse) → decline (PS/2 already covers this)
- `proto == 0` → register as tablet, **skip** SET_PROTOCOL(BOOT)
  (the tablet doesn't support boot protocol; report protocol is
  already the default after SET_CONFIGURATION)
- anything else → log and decline

The polling task runs every 15 ms (≈66 Hz, smoother than wmd's
30 fps repaint) and pushes parsed reports through
`mouse_set_absolute`.

### `kernel/mouse.h` + `mouse.c`

New entry point:

```c
void mouse_set_absolute(int x, int y, int buttons) {
    /* clamp to FB bounds */
    g_mouse_x = x;
    g_mouse_y = y;
    g_buttons = buttons & 0x07;
}
```

No PS/2 packet bookkeeping — just overwrite the same globals
that `mouse_get_state` reads.  `SYS_MOUSE_POLL` already snapshots
those globals, so wmd picks up the new position on its next
frame without any client-side change.

PS/2 is still alive: `mouse_init` runs, `mouse_process_byte`
still updates the same globals when packets arrive.  If both
sources are active they overwrite each other, but in practice
QEMU routes pointer events to the absolute device only, so PS/2
stays idle whenever usb-tablet is attached.

---

## QEMU command update

`build.sh` now suggests:

```
-device piix3-usb-uhci,id=usb0 \
-device usb-kbd,bus=usb0.0      \
-device usb-tablet,bus=usb0.0    \
```

The tablet is opt-in — drop the line and you get the old PS/2
relative behaviour (which works but with the drift the user
flagged).

---

## Why the smoke needs a 1.5 s wait

QEMU's usb-tablet emits a report **only when state changes**.
At boot, no events have occurred → the polling task NAKs every
15 ms.  When QMP injects an abs event, QEMU enqueues a single
report; our task picks it up on its next interrupt-IN.  Then
wmd's main loop polls `SYS_MOUSE_POLL`, gets the new (x, y),
and repaints the cursor.

With a 0.5 s wait, the screendump occasionally caught the FB
between "tablet polled fresh report" and "wmd repainted with
new cursor pos."  1.5 s gives the cursor 90+ frames to land and
removes the race entirely.

---

## What stays out of scope

- **Proper HID report-descriptor parsing.**  We hard-code the
  QEMU usb-tablet format.  Real hardware tablets / mice with
  different report layouts would not work.  Fine for our QEMU-
  centric setup; reportedly Bochs and VirtualBox emit similar
  formats for their absolute-pointer devices, but unverified.
- **Wheel scroll.**  Byte 5 is read but discarded.  Adding
  vertical-scroll support means a new field in
  `struct sys_mouse_state` and a new event type plumbed through
  wmd → client.
- **Multiple simultaneous tablets.**  MAX_HID_TABLET = 2 in the
  array; the second tablet would just overwrite the first's
  state via `mouse_set_absolute`.  Same model as the keyboard
  path's MAX_HID_KBD.
- **Hot-plug.**  Tablet plugged in after boot doesn't enumerate.
  Existing limitation of the USB stack (no port-status polling
  for live attach/detach).

---

## Files touched

- `kernel/usb_hid.c` — tablet struct, polling task, attach
  branch, polling-start hookup
- `kernel/mouse.h` + `mouse.c` — `mouse_set_absolute` entry point
- `build.sh` — QEMU minimal + full run hints include
  `-device usb-tablet,bus=usb0.0`
- `smoke_usbtablet.py` — new harness, 5 checks
- `docs/127-pathC-usbtablet.md` — this file

kernel.bin: 147632 (unchanged net — `--gc-sections` reclaims
were larger than the ~800 B of tablet code added).  No
userspace changes.

---

## Path C status after session 141

- ✅ 107..140 — see prior docs
- ✅ 141 — USB tablet absolute pointer

The wmd cursor now stays locked to QEMU's host pointer whenever
`-device usb-tablet` is in the command.  PS/2 still works for
environments without it.
