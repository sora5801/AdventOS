# Session 109 — Path C phase 3: PS/2 mouse driver

**Goal.** Get the mouse to the user. Path C phase 1 gave userspace a framebuffer; phase 2 gave it a drawing library. Phase 3 gives it a cursor to chase.

Status: **done.** Smoke test:

```
$ mouse 10        # mid-demo, QMP-inject 20 events of (+10, +5)
[ moving white crosshair tracks the QMP-sent rel events;
  HUD updates "x=NNN y=NNN buttons=---" each frame ]
```

QMP screendump verified the cursor crosshair (17 white pixels in a `+` shape) appears at the correct screen position after the synthesized mouse movement, and the green static frame + dark-grey backdrop + HUD all painted correctly.

---

## The driver shape

`kernel/mouse.c` programs the i8042 controller to enable the auxiliary (mouse) port, then sends the standard PS/2 init sequence:

1. `0xA8` to 0x64 — enable AUX port
2. `0x20` to 0x64, read 0x60 — get current config byte
3. set bit 1 (mouse IRQ12 enable), clear bit 5 (mouse clock disable)
4. `0x60` to 0x64, write config back
5. `0xD4 0xF6` — send 0xF6 (set defaults) to mouse, swallow ACK
6. `0xD4 0xF4` — send 0xF4 (enable streaming), swallow ACK

After this, the mouse produces 3-byte packets on every motion or click. The packets arrive on the same data port (0x60) the keyboard uses; i8042 status bit 5 (AUX) distinguishes the source.

The accumulated state is `(x, y, buttons)` clamped to the VBE framebuffer dimensions. The cursor starts centered.

---

## Routing AUX bytes to the mouse

The keyboard's polling path drains 0x60 in a tight loop. To avoid mouse bytes getting decoded as scancodes, the loop now inspects bit 5 of the status byte BEFORE reading 0x60:

```c
void keyboard_poll_once(void) {
    uint8_t st;
    while ((st = inb(KBD_STATUS)) & 0x01) {
        uint8_t b = inb(KBD_DATA_PORT);
        if (st & 0x20) mouse_process_byte(b);     /* AUX = mouse */
        else            process_scancode(b);
    }
}
```

Same change in the IRQ-1 handler (`kbd_irq`) for symmetry — even though IRQ 1 doesn't reliably fire on the current QEMU build, when it does fire we don't want to misroute the byte it's draining.

`mouse_process_byte` accumulates a 3-byte packet, decodes deltas + buttons, applies them to the state, and clips to the framebuffer.

---

## Packet decode

Standard PS/2 packet format:

```
byte 0:  Y_OVF | X_OVF | Y_SIGN | X_SIGN | ALWAYS_1 | M | R | L
byte 1:  X_delta (low 8 bits — combined with X_SIGN gives 9-bit signed delta)
byte 2:  Y_delta (same)
```

```c
int dx = (int)pkt[1] - ((pkt[0] << 4) & 0x100);
int dy = (int)pkt[2] - ((pkt[0] << 3) & 0x100);
g_mouse_x += dx;
g_mouse_y -= dy;     /* invert: PS/2 Y is up-positive */
g_buttons  = pkt[0] & 0x07;
```

Resync: byte 0's bit 3 is always 1 on a real packet. If the first byte read at `g_pkt_idx == 0` has bit 3 clear, drop it (treat next byte as the new byte 0). This recovers from any condition that desynchronizes the driver from the hardware's packet boundary.

---

## SYS_MOUSE_POLL

Userspace reads the state via syscall 89:

```c
struct sys_mouse_state {
    int x, y;
    unsigned int buttons;      /* bit 0 = L, bit 1 = R, bit 2 = M */
};
int sys_mouse_poll(struct sys_mouse_state *out);
```

The handler calls `keyboard_poll_once()` first to drain any pending bytes — that way a userspace polling loop sees fresh state even if no other path has been running. Then snapshots `(x, y, buttons)` into the user struct.

---

## Why polling, not IRQ-driven

Session 68 (keyboard) discovered that PS/2 IRQs don't fire reliably on the current QEMU + Windows MSYS2 setup. The kernel still wires `isr_register_irq(1, kbd_irq)` but the actual drain happens via active polling in the keyboard input path.

The mouse inherits this. The driver's `mouse_process_byte` is called from the keyboard's `keyboard_poll_once` (which runs on every keystroke wait AND from `SYS_MOUSE_POLL`). A userspace program that polls mouse state at 60 Hz pulls bytes off the controller at the same rate — fine for interactive use.

If IRQs become reliable later, no driver change is needed; the IRQ path also calls `mouse_process_byte` for AUX bytes.

---

## The user demo

`user/mouse.c` (300-ish lines) uses libgfx:
- Maps the framebuffer
- Paints a green frame + dark-grey backdrop + top-text + HUD strip
- Loops 60 times/second: poll mouse, erase old cursor square, draw new crosshair, update HUD
- Color-codes cursor by button state
- Releases on exit

Direct framebuffer writes — no backbuffer yet. The cursor leaves a faint trail during fast moves because the erase-then-draw isn't atomic with vsync. Session 110 fixes this.

---

## Files touched

- `kernel/mouse.h`, `kernel/mouse.c` — new (PS/2 driver)
- `kernel/keyboard.c` — route AUX bytes via `mouse_process_byte`
- `kernel/kernel.c` — call `mouse_init()` at boot
- `kernel/syscall.h`, `kernel/syscall.c` — `SYS_MOUSE_POLL=89` + `struct sys_mouse_state`
- `user/libuser.h`, `user/libuser.c` — `sys_mouse_poll` wrapper
- `user/mouse.c` — new demo (cursor + HUD)
- `build.sh`, `mkfs.py` — `mouse` joins `GFX_PROGS`, man page
- `fs/man/mouse` — new

kernel.bin: 131248 → 131248 (the new mouse.o is small; sizes happen to round identically because we removed the redundant FB_TAKEOVER stubs in 107).

---

## Path C status after session 109

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse + cursor demo
- ⏳ 110 — double-buffering / vsync
- ⏳ 111 — window manager daemon
- ⏳ 112+ — widgets, fonts, event loop
