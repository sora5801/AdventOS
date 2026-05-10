# Session 44 — HID mouse + USB hub

**Goal:** finish off the deferred follow-ups from sessions 40 and 41. Session 40 shipped UHCI + USB core + HID *boot keyboard*, leaving HID mouse and USB hub support as "next session" items. Session 41 added USB mass storage but had to pin both `usb-kbd` and `usb-storage` to explicit root-hub ports (`bus=usb0.0,port=1` and `port=2`) because QEMU would otherwise insert a virtual hub between them and our enumerator couldn't walk through it. This session adds both — HID mouse as a parallel class to the existing keyboard, and a USB-1.1 hub class driver that walks downstream ports and recurses back into the core enumerator for each connected device.

End state — boot trace with `-device usb-hub -device usb-kbd,port=1.1 -device usb-mouse,port=1.2`:

```
[uhci] PIIX3 controller @ I/O 0xc100  IRQ 11
[usb] port 1: full-speed device attached
[usb] addr 1  full-speed  vid=409  pid=55aa  class=9  ep0_max=8
[usb] addr 1: USB hub
[hub] addr 1: 8 ports, pwr2good=2ms
[hub] addr 1 port 1: full-speed device, enumerating
[usb] addr 2  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 2: HID iface=0 proto=1 ep=IN1 max=8 int=10ms
[usb] HID keyboard registered (polling starts late)
[hub] addr 1 port 2: full-speed device, enumerating
[usb] addr 3  full-speed  vid=627  pid=1  class=0  ep0_max=8
[usb] addr 3: HID iface=0 proto=2 ep=IN1 max=4 int=10ms
[usb] HID mouse registered (polling starts late)
[usb] port 2: no device
[usb] HID keyboard polling task started
[usb] HID mouse polling task started
```

The root-hub UHCI port 1 sees the hub (class 0x09, vendor 0x0409 = NEC, product 0x55AA = QEMU's stock virtual hub). The hub class driver fetches the hub descriptor (8 ports for QEMU's `usb-hub`), powers each downstream port, polls connect status, resets, and calls back into `usb_enumerate_default` with the new device's speed and a "hub 1 port N" tag for logging. The recursive enumeration finds:

- **Hub port 1**: full-speed device, vid=0x0627 (QEMU's "QEMU keyboard"), interface class 0x03/proto=1 (HID keyboard) — binds to `usb_hid_attach`.
- **Hub port 2**: full-speed device, same vid, interface class 0x03/proto=2 (HID mouse) — binds to `usb_hid_attach` on the new mouse path.

Both polling tasks start at end of kmain. cryptotest still passes 27/27 — the new code doesn't perturb the system.

## What's in scope

In:

- **`kernel/mouse.{h,c}`** — adds `mouse_inject(int32_t dx, int32_t dy, uint32_t buttons)`. Mirrors what the PS/2 IRQ handler does to the shared `g_x`/`g_y`/`g_buttons` state: clamp inside the screen rectangle, flip the Y axis (USB and PS/2 both send "up = +dy"; framebuffer "+y = down"), bump packets counter. `sys_mouse_state` reads from the same storage, so the GUI demo and `[t24]` selftest see the mouse cursor regardless of which physical input drove it.
- **`kernel/usb_hid.c`** — adds a parallel HID-mouse code path alongside the keyboard. Boot-protocol mouse reports are 3 bytes (some devices 4 with a wheel byte — we ignore the wheel): `report[0]` button bitmap, `report[1]` signed dx, `report[2]` signed dy. The poll task runs at 50 ms cadence per device just like the keyboard. The class binding switches on `bInterfaceProtocol` (1 = kbd, 2 = mouse) and instantiates the right slot.
- **`kernel/usb_hub.{h,c}`** — new (~230 LOC). USB 1.1 §11 hub class driver:
  - Get class-specific hub descriptor (DT 0x29) via class-typed `GET_DESCRIPTOR`.
  - Per-port: `SET_PORT_FEATURE(PORT_POWER)`, wait `bPwrOn2PwrGood × 2 ms` (floored at 20 ms — cheap hubs lie about this), `GET_PORT_STATUS`, on connect: `SET_PORT_FEATURE(PORT_RESET)`, wait 60 ms, poll status until `PORT_RESET` clears, check `PORT_ENABLE` + `PORT_LOW_SPEED`, hand the device speed to `usb_enumerate_default()` for the standard chapter-9 dance.
  - Acknowledges `C_PORT_CONNECTION` and `C_PORT_RESET` change indicators after each step.
- **`kernel/usb_core.{c,h}`** — `enumerate_one()` becomes the public `usb_enumerate_default(int low_speed, const char *origin)` so the hub driver can call it once per port. Binds `dd->bDeviceClass == USB_CLASS_HUB` (= 0x09) to `usb_hub_attach()` — checked at the *device* level (not the interface level) since hub class is a `bDeviceClass` value, not a `bInterfaceClass`.
- **`kernel/usb.h`** — adds `USB_RECIP_OTHER = 3`, the recipient code the hub class uses for per-port control requests (vs `USB_RECIP_DEVICE` for hub-level requests).

Out — bigger follow-ups not on this session's path:

- **Nested hubs.** USB spec allows hub-behind-hub up to 5 deep. The `usb_hub_attach` → `usb_enumerate_default` path is already recursive: a hub plugged into a hub port would just bind to `usb_hub_attach` again. Not tested in QEMU — the typical setup is one tier — but should Just Work given the existing structure.
- **Hot-plug.** Hub class enumeration runs at boot only. A device plugged into the hub later isn't detected. Real hubs have a status-change endpoint (interrupt-IN endpoint 1) that signals which ports changed; we'd add a polling task for that endpoint and re-enumerate on a connect event. Out of session-44 scope.
- **Port-power individual control.** Some hubs are `PortPwrCtrlMask`-driven: power is granted per-port via individual `SET_PORT_FEATURE(PORT_POWER)` calls (compound power switching). We do call SET_PORT_FEATURE per port, which works for both compound-switched and gang-switched hubs.
- **HID report-descriptor parser.** We still rely on boot protocol — the fixed 8-byte keyboard report and 3-byte mouse report. Devices that only support report protocol (some gaming peripherals) would need a report-descriptor parser, which is HID 1.11's "Forth in bytes" with usage pages, usage IDs, logical min/max, etc. Out of scope.

## Architecture

```
QEMU CLI:
    -device piix3-usb-uhci,id=usb0
    -device usb-hub,bus=usb0.0,port=1
    -device usb-kbd,bus=usb0.0,port=1.1
    -device usb-mouse,bus=usb0.0,port=1.2

                root hub (UHCI controller, 2 ports)
                ┌────────────────────────┐
                │   port 1    │   port 2 │
                └─────┬───────┴──────────┘
                      │ ↓ usb-hub attached
                      ▼
                ┌─────────────────────────────┐
                │  QEMU usb-hub (8 ports)     │
                │  vid=0x0409 pid=0x55AA      │
                │  class=0x09 (HUB)           │
                │  ┌────┬────┬────┬────┬──┐   │
                │  │ 1  │ 2  │ 3  │ 4  │..│   │
                │  └─┬──┴─┬──┴────┴────┴──┘   │
                │    │    │                  │
                │    ▼    ▼                  │
                │   kbd  mouse               │
                └─────────────────────────────┘

Enumeration flow (boot):

  uhci_probe_ports()
   │
   ▼   "port 1: full-speed device attached"
  usb_enumerate_default(low=0, "port 1")
   │
   ▼   GET_DESCRIPTOR(DEVICE,8) → ep0_max
   ▼   SET_ADDRESS(1) + 2ms wait
   ▼   GET_DESCRIPTOR(DEVICE,18) → bDeviceClass=9 (HUB)
   ▼   SET_CONFIGURATION(1)
   ▼
  usb_hub_attach(addr=1)
   │
   ▼   GET_DESCRIPTOR(HUB)
   ▼   bNbrPorts=8, bPwrOn2PwrGood=1 (=2ms)
   │
   for p in 1..8:
   ▼   SET_PORT_FEATURE(PORT_POWER, p)
   ▼   sleep max(20ms, bPwrOn2PwrGood*2ms)
   ▼   GET_PORT_STATUS(p)
   ▼   if PORT_CONNECTION:
       │
       ▼  SET_PORT_FEATURE(PORT_RESET, p)
       ▼  sleep 60ms
       ▼  poll GET_PORT_STATUS until PORT_RESET clears
       ▼  CLEAR_PORT_FEATURE(C_PORT_RESET, p)
       ▼  check PORT_ENABLE + PORT_LOW_SPEED
       │
       ▼  usb_enumerate_default(low_speed, "hub 1 port p")
       │  ─── recurses into core enumerator ───
       ▼
       device-class branch:
         class 0x03 + bInterfaceProtocol 1 → usb_hid_attach (kbd)
         class 0x03 + bInterfaceProtocol 2 → usb_hid_attach (mouse)
         class 0x08 → usb_msc_attach (mass storage)
         class 0x09 → usb_hub_attach (nested hub — untested)

usb_start_polling() at end of kmain:
   spawns usb-hid-kbd task (50ms poll)
   spawns usb-hid-mouse task (50ms poll, if any mouse attached)
```

## Hub class request encoding

USB 1.1 §11.16 distinguishes between two kinds of class-typed control requests by `bmRequestType.recipient`:

- `USB_RECIP_DEVICE` for hub-level requests (e.g., `GET_DESCRIPTOR(HUB)`).
  Affects the hub as a whole.
- `USB_RECIP_OTHER` (= 3) for per-port requests. The port number goes in
  `wIndex`. This is the recipient code we added to `usb.h` this session.

Feature selectors (`wValue` for SET_FEATURE/CLEAR_FEATURE):
```
   PORT_CONNECTION     0
   PORT_ENABLE         1
   PORT_SUSPEND        2
   PORT_OVER_CURRENT   3
   PORT_RESET          4
   PORT_POWER          8
   PORT_LOW_SPEED      9
   C_PORT_CONNECTION  16    (clear-the-change-bit features)
   C_PORT_ENABLE      17
   C_PORT_SUSPEND     18
   C_PORT_OVER_CURRENT 19
   C_PORT_RESET       20
```

`GET_PORT_STATUS` returns four bytes — `wPortStatus` (current state) and `wPortChange` (which bits changed since last query). Our `enumerate_port` reads both, acts on the status, and clears the change bits we observed via `CLEAR_PORT_FEATURE(C_PORT_*)`.

The reset sequence after a connect indication:
```c
hub_set_port_feature(hub, port, PORT_RESET);
pit_sleep(60);                                 /* spec says ≥10ms */
for retries:
    hub_get_port_status(hub, port, &status, &change);
    if (!(status & PS_RESET)) break;           /* hub cleared reset → done */
    pit_sleep(10);
hub_clear_port_feature(hub, port, C_PORT_RESET);
```

The hub does its own reset signaling on the wire; the host doesn't pulse the UHCI controller's reset bit. After the hub sees the device come back from reset, the hub itself sets `PORT_ENABLE` and clears `PORT_RESET`. Then the device speaks at the default address 0 just like a freshly-reset root-hub port.

## HID boot mouse

Boot-protocol mouse reports are dramatically simpler than the keyboard's:
```
   byte 0   : buttons         (bit 0 left, bit 1 right, bit 2 middle)
   byte 1   : dx (int8)
   byte 2   : dy (int8)
   byte 3?  : wheel (int8)    — only if wMaxPacketSize >= 4; ignored here
```

Unlike the keyboard's "always send the full set of currently-pressed keys" level-triggered model, the mouse sends *deltas* that always get applied. NAK polls (no movement) leave the cursor untouched. The polling task is identical in shape to the keyboard's:

```c
static void poll_mouse(struct hid_mouse *m) {
    uint8_t report[8] = {0};
    int rc = uhci_int_in(m->dev->addr, m->dev->low_speed, m->ep_max,
                         m->ep, report, m->ep_max < 8 ? m->ep_max : 8,
                         &m->toggle);
    if (rc < 3) return;     /* NAK / timeout / runt */

    uint32_t buttons = report[0] & 0x07;
    int32_t  dx = (int32_t)(int8_t)report[1];
    int32_t  dy = (int32_t)(int8_t)report[2];
    mouse_inject(dx, dy, buttons);
}
```

`mouse_inject` lives in `kernel/mouse.c` next to the existing PS/2 IRQ handler — same clamping logic, same `g_x`/`g_y`/`g_buttons`, same Y-axis flip. The shell's `[t24]` selftest's `sys_mouse_state` syscall reads from the same shared state and gets the right answer regardless of whether the cursor was driven by PS/2 or USB.

The `int8_t` cast on `report[1]` / `report[2]` is the load-bearing detail. The raw byte is unsigned, but the spec semantics are signed two's-complement, so a left motion comes through as `0xF8` and we want the int conversion to sign-extend to `-8`. Cast to `int8_t` first, then promote to `int32_t`, and the sign survives.

## Testing

End-to-end with `-device piix3-usb-uhci -device usb-hub,port=1 -device usb-kbd,port=1.1 -device usb-mouse,port=1.2`:

| Check | Result |
|---|---|
| Hub class enumerated as device class 0x09 | ✓ `[usb] addr 1: USB hub` |
| `GET_DESCRIPTOR(HUB)` returns 8 ports | ✓ `[hub] addr 1: 8 ports` |
| Hub port 1 powered + reset + enumerated | ✓ `[hub] addr 1 port 1: full-speed device, enumerating` |
| Keyboard found behind hub port 1 | ✓ `[usb] addr 2: HID iface=0 proto=1 ep=IN1` |
| Hub port 2 powered + reset + enumerated | ✓ `[hub] addr 1 port 2: full-speed device, enumerating` |
| Mouse found behind hub port 2 | ✓ `[usb] addr 3: HID iface=0 proto=2 ep=IN1 max=4` |
| Both polling tasks start | ✓ `[usb] HID keyboard polling task started` + `HID mouse polling task started` |
| cryptotest 27/27 (system stability with all 3 USB devices) | ✓ |
| `[t26]` httpsget→httpsd TLS 1.3 (network + TLS unaffected) | ✓ handshake OK, 501 bytes |
| `[t28]` usbtest (if `-device usb-storage` also attached) | ✓ |

Headless QEMU monitor `mouse_move` commands don't reliably route to the
USB mouse — they go through QEMU's input subsystem which defaults to
PS/2 unless explicitly targeted. Verifying actual mouse delivery
end-to-end needs interactive QEMU mode (`-display sdl` or `-display gtk`)
+ real movement; the code path is symmetric with the keyboard's which
*does* deliver under interactive QEMU per session 40's testing, and
the polling task ran cleanly through the entire selftest without
crashing.

## What's next

- **USB hot-plug.** Listen on the hub's status-change endpoint (interrupt-IN
  endpoint 1) for port-state change events; on connect, run a single port
  through the reset-and-enumerate dance. Lets a USB device plugged in
  *after* boot get picked up. ~100 LOC.
- **Nested-hub testing.** `-device usb-hub,bus=usb0.0,port=1 -device usb-hub,bus=usb0.0,port=1.1`
  exercises the recursive `usb_hub_attach → usb_enumerate_default → usb_hub_attach`
  path that we wrote but didn't test.
- **HID report-descriptor parser.** Boot protocol covers stock keyboards
  and mice; devices that only export report protocol (most game
  controllers, multi-touch pads) need the actual HID report parser. ~300 LOC
  of HID Usage Tables + report item parser. Mostly bookkeeping.
- **Wheel + 5-button mice.** The current code ignores wheel deltas and
  caps buttons at left/right/middle. Easy to extend once `mouse_inject`
  grows the parameter.
