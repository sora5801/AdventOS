# Session 34 — PS/2 mouse, mmap("/dev/fb0"), and a tiny GUI

**Goal:** Get pixels and clicks under userspace control. Session 32 added the framebuffer and a kernel-side console; the FB was kernel-only — `kprintf` painted glyphs, but no user process had access. This session opens both gates: the kernel learns to talk to the PS/2 mouse, and the framebuffer becomes a user-mappable resource via a tiny `SYS_FB_MMAP` syscall. With those two primitives, a 350-line user program (`gui.elf`) draws an interactive demo: gradient header, three colored "tile" widgets that highlight on click, a 12×12 arrow cursor that tracks the mouse, and a 5×7 status bar showing coordinates and button state.

End state — the boot lines:

```
mouse: PS/2 ready, clamp box 1024x768
```

And the new `[t24]` selftest:

```
[t24] PS/2 mouse + framebuffer mmap from userspace
  mouse_state: alive=1  x=512 y=384 btns=0 packets=0
  fb mmap returned VA 0x50000000 (size 2304 KiB)
  PASS: wrote blue pixels at bottom-left (no #PF)
gui: 1024x768 @ 24-bpp, FB mapped at 0x50000000
gui: 240 frames done, exiting
  gui.elf exited (code=0)
```

A QMP `screendump` 14 seconds into boot confirms the GUI is actually painting:

| Sample point | Color | Expected |
|--------------|-------|----------|
| header (10, 30) | (16, 127, 191) | cyan-blue gradient ✓ |
| tile 0 (210, 170) | (64, 128, 224) | blue (0x4080E0) ✓ |
| tile 1 (440, 170) | (224, 160, 48) | orange (0xE0A030) ✓ |
| tile 2 (670, 170) | (48, 192, 64) | green (0x30C040) ✓ |
| status bar (10, 760) | (0, 0, 0) | black ✓ |
| cursor sprite at (512, 384) | white triangle pattern | classic arrow ✓ |

All 24 selftests still pass; `curl http://localhost:8211/` continues to serve `httpd.elf`.

## What's in scope

In:
- **`kernel/mouse.{h,c}`** — PS/2 mouse driver. Initializes the controller (cmd 0xA8 → enable AUX port, set IRQ-enable bit in config, set defaults, enable reporting), wires IRQ12, decodes 3-byte packets into (x, y, buttons), clamps the cursor to the screen.
- **`kernel/syscall.{h,c}`** — Two new syscalls: `SYS_MOUSE_STATE = 54` returns (x, y, buttons, packets); `SYS_FB_MMAP = 55` eagerly maps the LFB physical pages into the calling task's PD with PTE_USER + PTE_WRITABLE.
- **`kernel/kernel.c`** — `mouse_init()` called after `vbe_init` so the cursor's clamp box matches the actual screen dimensions.
- **`user/libuser.{h,c}`** — `sys_mouse_state()`, `sys_fb_mmap()` wrappers.
- **`user/gui.c`** — The demo. ~350 LOC: `put_pixel`/`fill_rect`/`rect_outline` primitives, a tiny 5×7 digit font, a 12×12 arrow cursor sprite with separate outline + fill bitmaps, save/restore of the under-cursor pixel block, and a 16ms-per-frame poll loop.
- **`mkfs.py`, `build.sh`** — `gui.elf` joins the user-program list.
- **`user/sh.c`** — `[t24]` selftest exercises both syscalls and forks `gui.elf`.

Out:
- **PS/2 mouse hot-plug.** We initialize once at boot. If the mouse isn't plugged at that point, we stay disabled forever.
- **Scroll wheel / 5-button packets.** Standard 3-byte streaming mode only — the mouse never gets the IntelliMouse extension dance (sample-rate ramp 200/100/80) that switches it to 4-byte packets.
- **Per-process FB mappings.** `SYS_FB_MMAP` always maps the WHOLE FB. No region selection, no read-only mode, no shared mmap with copy-on-write.
- **A real GUI toolkit.** No widget tree, no event dispatch, no z-order. The demo's "click highlight" is hand-rolled hit-testing in the main loop.
- **Compositing.** `gui.elf` shares the FB with the kernel's `fbcon` console — there's no display server arbitrating writes. In practice the user paints over fbcon and vice versa; the screenshots show the GUI dominant during the demo's 4-second window because nothing else is calling `kprintf` in that interval.
- **fork-safe FB mappings.** `paging_clone_user_pd` does a deep copy of every present PTE — including FB pages — which would copy the framebuffer's contents into a fresh anonymous page and map *that* into the child. Don't fork after `sys_fb_mmap`.

## Architecture

```
                 USER (gui.elf)               KERNEL                     HARDWARE
                 ──────────────               ──────                     ────────
                                                                                   
   sys_fb_mmap() ──────────────► syscall 55                                          
                                  │                                                   
                                  ▼                                                   
                          paging_map_in(user_pd,                                       
                            user_va, fb_phys,                                          
                            PTE_USER|PTE_WRITABLE)                                     
                                  │                                                   
                                  ▼                                                   
                          for each FB page:                                            
                            invlpg(user_va)        ◄─────────────  LFB at fb_phys     
                                                                   (e.g. 0xFD000000)  
   ◄────── user_va = 0x50000000 ─                                                     
                                                                                       
   *(volatile uint32_t*)(user_va + offset) = pixel ─────────────► writes go directly  
                                                                   to FB MMIO          
                                                                                       
                                                                                       
   sys_mouse_state(out) ────────► syscall 54                                            
                                  │                                                    
                                  ▼                                                    
                          mouse_get_state(out)                                         
                                  │                                                    
                                  ▼                                                    
                          (x, y, buttons, packets) ◄─────── IRQ12 → mouse_irq        
                                                              │                       
                                                              ▼                       
                                                          decode 3-byte packet,       
                                                          update g_x/g_y/g_buttons    
                                                              ▲                       
                                                              │                       
                                                              │  PS/2 ports 0x60/0x64
                                                              │                       
                                                                       MOUSE silicon
```

The gap session 34 closes is the *user-side access* gap. Mouse + framebuffer existed already (mouse silicon was always there; FB came in session 32). What was missing: a kernel-mediated way for ring 3 to read mouse events and write FB pixels. Both are tiny syscalls — the heavy lifting (controller handshake, page-table mutation) happens kernel-side.

## The PS/2 mouse handshake

The PS/2 controller (the "8042" silicon, descended from the original IBM PC keyboard controller) multiplexes two devices: the keyboard on the primary channel, the mouse on the AUX channel. Both share I/O ports `0x60` (data) and `0x64` (command/status), but the controller's status register bit 5 marks each byte's source ("AUX data available").

Bringing the mouse online is a multi-step dialog with the controller:

```c
/* 1. Enable the AUX port. */
ps2_send_command(0xA8);

/* 2. Read current config, enable IRQ12 + clock for AUX, write back. */
ps2_send_command(0x20);
uint8_t cfg; ps2_read_data(&cfg);
cfg |=  (1u << 1);    /* enable AUX IRQ */
cfg &= ~(1u << 5);    /* don't disable AUX clock */
ps2_send_command(0x60);
ps2_send_data(cfg);

/* 3. Tell the mouse to use defaults — sample rate 100Hz, scaling 1:1. */
mouse_write(0xF6);    /* expects 0xFA ack */

/* 4. Enable streaming reporting. After this, IRQ12 fires per event. */
mouse_write(0xF4);    /* expects 0xFA ack */
```

`mouse_write` is the mouse-vs-keyboard discriminator. Every byte you want to send to the mouse must be preceded by controller command `0xD4` ("the next byte you write to data goes to the AUX device"). Bytes coming back are read from the same port `0x60` and discriminated by status-register bit 5 in the IRQ handler.

**Three things that bit me, all small:**

1. **Drain the output buffer first.** The BIOS leaves stale bytes in the controller's output buffer after its own POST tests. Without `while (status & 1) inb(0x60);` at init, the first `ps2_read_data(&cfg)` returns garbage and the config-byte write goes wrong.

2. **Bound the wait loops.** `ps2_wait_input` and `ps2_wait_output` spin on the controller's busy bits. A buggy or absent controller could spin forever; we use a 100,000-iteration cap and silently give up. The driver then logs "disabled" and userspace's `sys_mouse_state` keeps reporting (0, 0, 0) — the GUI handles this gracefully.

3. **Tolerate missing ACKs.** Per the spec, the mouse acks every command with `0xFA`. In QEMU we always get the ack; on some real BIOS-virtualized stacks the ack on `MOUSE_CMD_RESET` is missing. We `mouse_write` returns -1 if no ack, and the caller treats that as fatal — but we skip RESET entirely (using SET_DEFAULTS instead) which works on every box we tried.

## Decoding the 3-byte packet

After enable-reporting, IRQ12 fires once per movement or button event, and the controller queues a 3-byte packet:

```
byte 0:  Y-overflow:7  X-overflow:6  Y-sign:5  X-sign:4  always_1:3  middle:2  right:1  left:0
byte 1:  dx (signed; sign-extend from byte 0 bit 4)
byte 2:  dy (signed; sign-extend from byte 0 bit 5)
```

The sign bits in byte 0 extend the dx/dy from 8-bit to 9-bit signed values. With clamping to a 1024×768 screen and ~100Hz sample rate, even fast mouse motion never produces dx > ±127, so the overflow bits (6, 7) practically never fire. We drop overflowed packets (data is unreliable) just to be safe.

The IRQ handler:

```c
static void mouse_irq(struct registers *r) {
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) return;
    if (!(status & PS2_STATUS_AUX_DATA))    return;   /* belongs to keyboard */

    uint8_t b = inb(PS2_DATA);

    /* Resync: byte 0 of every packet has bit 3 set. If we're at
     * cycle 0 and don't see it, the stream is out of phase. Drop
     * until alignment recovers. */
    if (g_pkt_cycle == 0 && !(b & 0x08)) return;

    g_pkt[g_pkt_cycle++] = b;
    if (g_pkt_cycle < 3) return;
    g_pkt_cycle = 0;

    uint8_t flags = g_pkt[0];
    if (flags & 0xC0) return;     /* drop overflowed */

    int32_t dx = g_pkt[1];
    int32_t dy = g_pkt[2];
    if (flags & 0x10) dx |= 0xFFFFFF00;     /* X-sign */
    if (flags & 0x20) dy |= 0xFFFFFF00;     /* Y-sign */

    /* Mouse +Y is "up"; framebuffer +Y is "down". Invert. */
    int32_t nx = g_x + dx;
    int32_t ny = g_y - dy;

    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > g_screen_w - 1) nx = g_screen_w - 1;
    if (ny > g_screen_h - 1) ny = g_screen_h - 1;

    g_x = nx;
    g_y = ny;
    g_buttons = flags & 0x07;
    g_packets++;
}
```

The Y-axis flip is a recurring subtle thing: every device that reports motion has to pick a convention for "up positive" or "down positive". The PS/2 mouse picks "up positive" (mathematical convention, consistent with its trackpad heritage). Framebuffers universally pick "down positive" (raster scanline order from top of display). We flip in the IRQ so userspace sees screen coordinates directly.

The resync logic matters more than it sounds. If the IRQ ever fires with `g_pkt_cycle = 1` because of a missed byte, all subsequent packets misalign — `g_pkt[1]` becomes byte 0 of the next packet, every read is shifted. The "always 1" bit at position 3 of byte 0 is the resync token: if cycle==0 and bit 3 is clear, the byte is from the middle of some other packet, drop it and stay at cycle 0 until a real byte 0 arrives.

## Framebuffer mmap into user space

`SYS_FB_MMAP` is half the size of the file-backed `SYS_MMAP` because there's no lazy-loading machinery — every FB page exists in physical memory already (it's MMIO; the GPU's video RAM, decoded by the chipset). We just need to add a USER-flagged PT entry pointing at it.

```c
case SYS_FB_MMAP: {
    const struct vbe_state *v = vbe_state();
    if (!v->enabled) { ret = 0; break; }

    struct task *t = task_current();
    uint32_t fb_size = v->fb_size;
    uint32_t aligned = (fb_size + 0xFFFu) & ~0xFFFu;
    if (t->mmap_brk + aligned > USER_MMAP_MAX) { ret = 0; break; }

    uint32_t va_start = t->mmap_brk;
    uint32_t va_end   = va_start + aligned;
    uint32_t fb_phys  = v->fb_phys & ~0xFFFu;

    for (uint32_t va = va_start; va < va_end; va += 0x1000) {
        uint32_t phys = fb_phys + (va - va_start);
        if (paging_map_in((uint32_t *)(uintptr_t)t->cr3,
                          va, phys,
                          PTE_USER | PTE_WRITABLE) != 0) {
            /* roll back any pages we mapped, return 0 */
            ...
            ret = 0;
            goto fb_mmap_done;
        }
        __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
    }

    t->mmap_brk = va_end;
    /* Track in the mmap region table so SYS_MUNMAP can find it.
     * fs_idx = -1 marks device-backed (no file source). */
    for (int i = 0; i < TASK_MMAP_MAX; i++) {
        if (!t->mmaps[i].in_use) {
            t->mmaps[i].in_use      = 1;
            t->mmaps[i].va_start    = va_start;
            t->mmaps[i].va_end      = va_end;
            t->mmaps[i].fs_idx      = -1;
            ...
            break;
        }
    }
    ret = (int32_t)va_start;
fb_mmap_done:
    break;
}
```

The choice to *eagerly* map (vs lazy with #PF) is twofold:

1. **No backing pages need allocation.** File-backed mmap allocates fresh anonymous pages and copies file content into them on demand. The FB is already there at a fixed physical address; we just need PTEs.
2. **No #PF in the demo's hot loop.** A 60fps cursor that takes a #PF on the first pixel write of every frame would burn 16μs of latency per frame on the fault handler. Eager mapping pays the cost once at `sys_fb_mmap()` time.

The mapping uses `paging_map_in` (operates on a specific PD) rather than `paging_map` (operates on the kernel master PD). The user task's PD already mirrors the kernel's PDEs for memory in the kernel's identity-mapped range, but `0xFD000000` is way above that range and the user PD doesn't share a PT with the kernel for that region. We allocate a fresh PT in the user PD, fill it with USER-flagged PTEs pointing at the FB physical pages, and `invlpg` each new VA so the TLB picks up the change.

**Cleanup safety:** when `paging_destroy_user_pd` tears down the task's PD on exit, it walks every present PTE and calls `pmm_free_page(phys)`. The FB physical pages are at `0xFD000000+` — way past the PMM's tracked RAM range (= 32 MiB on QEMU). `pmm_free_page` has a guard: `if (p < g_total_pages && bit_get(p))` — it silently ignores pages outside the bitmap. So freeing a "user FB page" no-ops cleanly. The PT page itself (from `pmm_alloc_page` during the map) IS in PMM range and gets properly freed.

## The user-side painter

`user/gui.c` is straight C with no surprises once you have the FB pointer. Hot path:

```c
static inline void put_pixel(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || (unsigned)x >= g_w || (unsigned)y >= g_h) return;
    volatile unsigned char *row = g_fb + (unsigned)y * g_pitch;
    if (g_bpp == 32) {
        ((volatile uint32_t *)row)[x] = rgb;
    } else if (g_bpp == 24) {
        volatile unsigned char *p = row + x * 3;
        p[0] = (unsigned char)(rgb);          /* B */
        p[1] = (unsigned char)(rgb >> 8);     /* G */
        p[2] = (unsigned char)(rgb >> 16);    /* R */
    } else { /* 16 bpp R5 G6 B5 */
        unsigned int r = (rgb >> 16) & 0xFF;
        unsigned int g = (rgb >>  8) & 0xFF;
        unsigned int b =  rgb        & 0xFF;
        unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        ((volatile uint16_t *)row)[x] = (uint16_t)v;
    }
}
```

The `volatile` matters even for the FB — without it, GCC at -O2 happily hoists pixel writes out of the loop or batches them in unpredictable ways, which usually still works but can race the hardware refresh in subtle modes (and bites on real LCDs where the chipset latches PHY-bound pixels on a clock edge).

The bitmap-cursor render is two passes for the 1px black outline:

```c
static const unsigned short cursor_outline[12] = { 0x0001, 0x0003, ..., 0x0099 };
static const unsigned short cursor_fill[12]    = { 0x0000, 0x0001, ..., 0x0028 };

static void draw_cursor(int x, int y) {
    for (int r = 0; r < 12; r++) {
        unsigned short ob = cursor_outline[r];
        unsigned short fb = cursor_fill[r];
        for (int c = 0; c < 12; c++) {
            if (ob & (1 << c)) put_pixel(x + c, y + r, 0x000000);
            if (fb & (1 << c)) put_pixel(x + c, y + r, 0xFFFFFF);
        }
    }
}
```

Outline first (1 pixel wider in every direction than fill), then fill on top. The result is a classic arrow cursor with 1px black contour visible against any background. Verified by grepping the screenshot at the expected cursor location:

```
12x12 sample at expected cursor location (512, 384):
  ............
  #...........
  ##..........
  ###.........
  ####........
  #####.......
  ######......
  #######.....
  ########....
  #########...
  .#.#..#.....
  ...#.#......
```

The save-restore dance for under-cursor pixels:

```c
static unsigned char g_save[12 * 12 * 4];   /* up to 32-bpp per pixel */
static int  g_last_x = -1, g_last_y = -1;

/* Per frame: */
if (g_last_x >= 0) cursor_restore(g_last_x, g_last_y);  /* old pos: paint back */
cursor_save(mx, my);                                     /* new pos: snapshot */
draw_cursor(mx, my);                                     /* new pos: paint cursor */
g_last_x = mx; g_last_y = my;
```

This is the canonical "blit-and-store" cursor pattern. Without `cursor_restore`, the old cursor positions stay on screen as a trail. The 12×12 buffer (×4 bytes for 32bpp safety) is small enough to keep on the stack.

## The selftest

```
[t24] PS/2 mouse + framebuffer mmap from userspace
  mouse_state: alive=1  x=512 y=384 btns=0 packets=0
  fb mmap returned VA 0x50000000 (size 2304 KiB)
  PASS: wrote blue pixels at bottom-left (no #PF)
gui: 1024x768 @ 24-bpp, FB mapped at 0x50000000
gui: 240 frames done, exiting
  gui.elf exited (code=0)
```

Three things verified:
1. **`sys_mouse_state` returns sensible coordinates** — at boot the cursor is centered (x=512, y=384 for a 1024×768 screen), packets=0 confirms no ghost movement events (a misconfigured controller can spam phantom packets).
2. **`sys_fb_mmap` returns a usable VA** — 0x50000000 is the base of the user mmap region (`USER_MMAP_START`). Writing to it doesn't page-fault.
3. **`gui.elf` runs to completion** — 240 frames at 16ms each = 3.84 seconds of cursor tracking, no panics.

The "PASS: wrote blue pixels at bottom-left" check is the cheapest possible "user-side FB write actually lands on the hardware" smoke test. Without USER+WRITABLE on the PT entry, the write would page-fault with a protection violation in user mode. The fact that it doesn't fault is the entire correctness bar for the mmap path.

## Composition with fbcon (the elephant in the room)

`fbcon` (kernel side, session 32) and `gui.elf` (user side, this session) both write the same physical FB. There's no display server, no z-order, no clipping. They cooperate by mostly NOT being active at the same time:

- During the 4-second `gui.elf` window, the kernel doesn't generate `kprintf` output (no boot messages, no IRQ debug logs unless something goes wrong). So `fbcon`'s painter is dormant and the GUI's frame buffer survives.
- After `gui.elf` exits, `fbcon` immediately paints the next shell prompt on top of the GUI's leftover background.

Real OSes solve this with a display server (X11, Wayland) that owns the FB exclusively and treats both terminals and graphical apps as clients. We don't have one, and won't until it's the right session to write one. For now, "polite cooperation by absence" is enough to demonstrate the underlying primitives work.

If you wanted to be slightly less polite, the GUI could call `sys_fbcon_disable()` (a hypothetical syscall) on entry and re-enable on exit. Easy to add, but not the point of this session — the point is "a user process can paint pixels and read mouse events."

## Bugs and lessons

**1. `printf` doesn't support `%p`.** First test run printed:
```
fb mmap returned VA %p (size 1342177280 KiB)
```
The `%p` got passed through literally; the next `%u` consumed the FIRST varargs (the FB pointer 0x50000000 = 1342177280). libuser's printf was added in session 9 and only handles the formats it needed at the time. Worked around with `0x%x` and explicit `(unsigned)(unsigned long)ptr` casts. Adding `%p` properly is a 6-line patch we'll do whenever it next blocks.

**2. `gui.elf` not in the FS image.** Added it to `build.sh`'s USER_PROGS list but forgot to add to `mkfs.py`'s `USER_PROGRAMS`. The kernel's `init.elf` happily found `httpd.elf`, then sh.elf, then sh's `[t24]` tried to `exec("gui.elf")` and got -1 (file not found), exited 127. Fix was a one-line addition to `mkfs.py`.

**3. `mouse_init` running before `vbe_init`.** First placement was right after `keyboard_init` — both before `paging_init`/`vbe_init`. The cursor-clamp box defaulted to 1024×768 (a sensible guess) because `vbe_state()->enabled` was 0. Worked but lost the auto-detection. Moved `mouse_init` to right after `fbcon_init` so the actual screen dimensions feed the clamp.

**4. `volatile` on the user-side FB pointer.** GCC at -O2 was hoisting some pixel-clearing loops (specifically the cursor `restore` path that copies bytes back) into a memcpy that the optimizer couldn't prove was needed. Adding `volatile` to the FB pointer types fixed it. Subtle bug: without `volatile` the demo's first frames might paint the cursor BUT not its outline, because the optimizer sees adjacent writes to the same address and elides one.

## What's left

- **`sys_munmap` on a FB region.** Currently the FB region IS tracked in the mmap table (`fs_idx = -1` marks it), so `mmap_unregister` will find it and walk the pages. `paging_unmap` clears the PTE and `pmm_free_page(fb_phys)` correctly no-ops thanks to the PMM range guard. But we haven't tested it — the demo never calls unmap.
- **Mouse cursor in fbcon.** The kernel-side text console doesn't draw a cursor at the text-cell position. With the FB pointer already mapped, fbcon could blink a 1×8 underline at `(g_cur_col*8, g_cur_row*8 + 7)` on every PIT tick. Easy follow-up.
- **`fork` after `sys_fb_mmap`.** `paging_clone_user_pd` deep-copies every present PTE — including the FB. The child ends up with copies of the FB contents in fresh anonymous pages; writes to its "FB" don't reach the hardware. Fix: teach `paging_clone_user_pd` about device-backed regions (mark via `fs_idx == -1` or a new region flag) and share-by-reference instead of copy. Rare in practice (you fork before mapping the FB), but a footgun if anyone ever does.
- **Real GUI toolkit.** Widgets, event dispatch, focus, z-order, drag-and-drop. Each is a session in its own right.
- **Wheel + 5-button packets.** The IntelliMouse extension dance (set sample rate to 200, then 100, then 80; query device-id; if returns 0x03, switch to 4-byte packets). Adds ~30 lines to mouse_init.
- **Mouse acceleration.** Currently 1:1. Standard PS/2 acceleration is a piecewise-linear curve applied to dx/dy after sign extension; trivial to add.

## Files touched

- `kernel/mouse.h`, `kernel/mouse.c` — driver (~200 LOC).
- `kernel/kernel.c` — wires `mouse_init()` after `fbcon_init()`.
- `kernel/syscall.h`, `kernel/syscall.c` — `SYS_MOUSE_STATE = 54`, `SYS_FB_MMAP = 55` (~80 LOC of handler code).
- `user/libuser.h`, `user/libuser.c` — `sys_mouse_state()`, `sys_fb_mmap()` wrappers.
- `user/gui.c` — the demo (~350 LOC).
- `mkfs.py` — adds `gui.elf` to the FS image.
- `build.sh` — adds `gui` to USER_PROGS.
- `user/sh.c` — `[t24]` selftest.
- `docs/34-mouse-fb-gui.md` — this document.

About 700 LOC net. The kernel side is ~280; the rest is the user demo and documentation.
