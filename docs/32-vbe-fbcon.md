# Session 32 — VBE/VESA graphics + a framebuffer console

**Goal:** Stop being shackled to the 80×25 VGA text mode. AdventOS has spent 31 sessions writing characters into the colour-attribute words of the legacy VGA buffer at `0xB8000`. This session asks the BIOS for a real linear framebuffer via VBE (VESA BIOS Extensions), maps it into the kernel address space, and adds a small framebuffer-backed console (`fbcon`) that can paint glyphs anywhere on a 1024×768 24-bit-colour canvas. The boot log now scrolls a 128×96-cell, 8×8-glyph terminal at full screen instead of being capped at 25 lines of yellow PC text.

End state — the new boot lines:

```
[boot] reading VBE summary from bootloader... vbe: 1024x768 24-bpp linear FB at 0xfd000000 (2304 KiB mapped)
fbcon: 128x96 glyph cells (24-bpp)
```

And the new `[t23]` selftest:

```
[t23] VBE/fbcon: sys_fbinfo reports framebuffer geometry
  fbcon enabled: 1024x768 24-bpp pitch=3072
  glyph cells (8x8 font): 128x96
  framebuffer-backed kprintf is live (this line is also painted to the FB)
```

The serial log + VGA text mode console + framebuffer console run in **parallel** — every `kputc` fans out to all three sinks. Headless boots still get the same serial output that the harness has used since session 1; QEMU's graphical window now renders boot messages in a 128-column terminal instead of an 80-column one. All 23 selftests pass under `-smp 2` with `-vga std` (the QEMU default), and `curl http://localhost:8094/` continues to serve the userspace `httpd.elf` page.

This deep dive walks through the whole pipeline: the 70-byte VBE call we wedged into the bootloader, why we had to capture its output before the PMM ate it, the BGR-pixel packing and 8×8-font rendering for `fbcon`, and the off-by-one page-fault that ate an evening's debugging.

## What's in scope

In:
- **`boot/boot.S`** — Adds an `int 0x10 ax=0x4F01/0x4F02` block that asks the BIOS for the VBE ModeInfoBlock for mode `0x118` and then sets the mode with the linear-framebuffer bit. Stashes a 12-byte summary at physical `0x9100`.
- **`kernel/vbe.{h,c}`** — Two-phase init: an early `vbe_capture_bootinfo()` snapshots the bootloader's summary into a static struct *before* `pmm_init` runs, then `vbe_init()` identity-maps the framebuffer pages into the kernel PD.
- **`kernel/font8x8.h`** — Embedded 8×8 monospace bitmap font for ASCII 0x20..0x7F, derived from the public-domain IBM-PC ROM font.
- **`kernel/fbcon.{h,c}`** — Framebuffer console: `putc`/`write`/`clear`/`scroll`, BGR pixel packing, line-wrap and scroll-on-overflow, configurable colours, plus a `fill_rect` primitive for future GUI bits.
- **`kernel/kprintf.c`** — `kputc` now writes to serial **and** VGA text mode **and** fbcon; the cost when fbcon is disabled is one branch.
- **`kernel/syscall.{h,c}`** — `SYS_FBINFO = 52`, returns whether fbcon is enabled and copies width/height/bpp/pitch into a user-supplied buffer.
- **`user/libuser.{h,c}`** — `sys_fbinfo()` wrapper.
- **`user/sh.c`** — `[t23]` selftest.

Out:
- **Direct user-space framebuffer access.** The FB is mapped into the kernel PD only — user PDs don't mirror it. A future `mmap("/dev/fb0", …)` could expose it; for now any "graphics" comes through `kprintf`.
- **Hardware-accelerated scroll.** Our scroll is a CPU `memcpy` of 760 scanlines × 3072 bytes = 2.3 MB per line. On QEMU that's microseconds; on a real 1995 box it'd be visibly slow. A bochs-vbe BLT op or a line-buffer ring would fix it.
- **Anti-aliasing, true colour cursor, mouse, anything that resembles a GUI.** This is a console, not a window system.
- **Mode fallback.** If mode 0x118 (1024×768×24) isn't available we fall back to text mode immediately; we don't try 0x117 / 0x115. The boot stays usable; the framebuffer just stays dark. (See §"What's left" for the patch shape.)
- **PCI BAR walk.** We trust whatever physical address the BIOS reports for the LFB (typically `0xFD000000` on QEMU). Real hardware may have it elsewhere; we'd need to walk PCI config space to confirm.

## Architecture: from BIOS to glyphs

```
        16-bit real mode          32-bit pmode + paging         user space
        (boot.S)                  (kernel)                      (sh.elf)
        ─────────────             ────────────────────          ───────────
   load kernel
   E820 scan
   ↓
   xor ax,ax;mov ax,es
   movb 0,0x910B            ─── ok flag = 0 ────────►
   rep stosw 0x9000,256
   ↓
   AX=0x4F01
   CX=0x118
   ES:DI=0x9000
   int 0x10                 ◄── BIOS writes 256-byte ModeInfoBlock
   ↓
   reload DS=ES=0
   copy +10 +12 +14 +19 +28
       to summary @0x9100   ─── width, height, pitch, bpp, fb_phys
   ↓
   AX=0x4F02
   BX=0x4118  (mode|LFB)
   int 0x10                 ─── BIOS sets mode, FB now writable
   ↓
   movb 1,0x910B            ─── ok flag = 1
   ↓
   pmode handoff to kernel ─►   kmain
                                ↓
                                vbe_capture_bootinfo()
                                  read 12 bytes from 0x9100
                                  save into g_boot
                                ↓
                                pmm_init()
                                  (now safe to overwrite 0x9100)
                                ↓
                                paging_init()
                                ↓
                                vbe_init()
                                  validate g_boot
                                  paging_map fb_phys..fb_phys+size
                                ↓
                                fbcon_init()
                                  cache pitch, bpp, dims
                                  fbcon_clear()
                                ↓
                                kprintf(...)  ──── kputc ───►  serial_putc
                                                          ─►  vga_putc
                                                          ─►  fbcon_putc
                                                                ↓
                                                                draw_glyph(col,row,c)
                                                                  for r in 0..7:
                                                                    for x in 0..7:
                                                                      put_pixel(...)
                                                                              ─►  fb[y*pitch + x*3]
                                                                                  = R/G/B byte triplet

                                                       ◄── int 0x80
                                                           SYS_FBINFO   sh.elf t23
                                                           returns 1, dims
```

Three transitions matter:
1. **BIOS-time mode-set.** Only real mode can call the VESA BIOS. We have to do this BEFORE the kernel's pmode entry, BEFORE the kernel turns on paging, BEFORE the kernel's PMM claims the low-memory area we use as scratch.
2. **Bootloader → kernel handoff** of the framebuffer's geometry. The summary block is the contract.
3. **Kernel-side mapping** of the high-physical-address LFB region into the kernel PD before any pixel write.

## Step 1: VBE mode-set in the bootloader

The VESA BIOS Extensions give us three int-0x10 functions:

| AX | What | Buffer |
| -- | ---- | ------ |
| `0x4F00` | Get VBE controller info | ES:DI = 512-byte VBEInfoBlock |
| `0x4F01` | Get mode info for CX | ES:DI = 256-byte ModeInfoBlock |
| `0x4F02` | Set mode BX (BX bit 14 = use LFB) | — |

We skip `0x4F00` (we don't need the controller capabilities — we know the QEMU BIOS supports VBE 2.0+) and go straight to `0x4F01` for mode `0x118` (1024×768×24bpp), then `0x4F02` to actually switch.

The added bootloader code, in `boot/boot.S`, sits between `e820_done:` and the protected-mode entry `cli`:

```asm
    /* Pre-zero the 256-byte ModeInfoBlock at 0x9000. If the BIOS
     * silently refuses to write the block but returns AX=0x004F
     * (some VGABIOS quirk), we'll still read all-zeros and the
     * kernel's plausibility check will reject it cleanly. */
    push    %di
    mov     $0x9000, %di
    xor     %ax, %ax
    mov     $128, %cx                    /* 128 words = 256 bytes */
    rep stosw
    pop     %di

    /* AX=0x4F01, CX=mode, ES:DI=info-block buffer. */
    mov     $0x9000, %di
    mov     $0x118, %cx
    mov     $0x4F01, %ax
    int     $0x10
    /* Restore segment registers — the BIOS may have touched them. */
    push    %ax
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    pop     %ax
    cmp     $0x004F, %ax
    jne     vbe_skip

    /* ModeInfoBlock fields (VBE 2.0 spec):
     *   +0x10 w pitch   +0x12 w width   +0x14 w height
     *   +0x19 b bpp     +0x28 d fb_phys */
    mov     0x9010, %ax
    mov     %ax, 0x9108
    ...
    mov     0x9028, %eax
    mov     %eax, 0x9100

    /* Set the mode with linear-FB bit (BX bit 14 = 0x4000). */
    mov     $(0x118 | 0x4000), %bx
    mov     $0x4F02, %ax
    int     $0x10
    push    %ax
    xor     %ax, %ax
    mov     %ax, %ds
    pop     %ax
    cmp     $0x004F, %ax
    jne     vbe_skip
    movb    $1, 0x910B                   /* ok = 1 */
vbe_skip:
```

A few subtleties worth pausing on:

**Why pre-zero the buffer.** The BIOS-int contract says `int 10h ax=0x4F01` returns `AX=0x004F` on success. In practice, some VGABIOS implementations return `0x004F` for unknown modes too — they just don't fill the block. If we trusted the return code blindly and the buffer happened to contain a pointer that decoded as `bpp=24, width=1024, height=768`, we'd "succeed" with garbage. Pre-zeroing is cheap insurance: the kernel's plausibility check (`bpp ∈ {16,24,32}`, dims ≤ 8192) rejects the all-zero block cleanly.

**Why reload DS/ES after the int.** The IBM-PC convention is that BIOS interrupts preserve segment registers, but it's not contractual — and QEMU's SeaBIOS has historically been fussy about whether DS comes back as we left it. Our subsequent `mov 0x9010, %ax` is a `disp16` direct-addressing instruction; the assembler emits it as `[DS:0x9010]`. If DS were anything other than 0, we'd be reading 64 KiB elsewhere in conventional memory and copying *that* into the summary. The push/pop dance preserves the BIOS-returned status while we reset DS=ES=0.

**Why one mode, not three.** The boot sector is hard-capped at 512 bytes by the BIOS (see the trailing `0x55AA` magic). Adding a fall-back loop that tried `0x118 → 0x117 → 0x115` blew the limit by ~30 bytes. The single-mode form fits in 142 bytes of code; if mode 0x118 isn't available the kernel notices `ok=0` and falls back to VGA text mode without complaint.

**The 12-byte summary at `0x9100`.** Layout:
```
0x9100  uint32_t  fb_phys      (PhysBasePtr from VBE block @ +0x28)
0x9104  uint16_t  width        (XResolution             @ +0x12)
0x9106  uint16_t  height       (YResolution             @ +0x14)
0x9108  uint16_t  pitch        (BytesPerScanLine        @ +0x10)
0x910A  uint8_t   bpp          (BitsPerPixel            @ +0x19)
0x910B  uint8_t   ok           (1 if both BIOS calls succeeded)
```

Compact, sufficient, addressable from disp16 in real mode.

**`0x9000` is the ONLY location you can put the buffer.** Conventional memory layout below the EBDA:
```
0x000-0x4FF   IVT + BIOS data area
0x500-0x7BFF  free
0x7C00-0x7DFF boot sector
0x7E00-0x9FBFF free
0x9FC00+      EBDA (extended BIOS data area)
```
We've already used `0x8000-0x84B0` for the E820 map. `0x9000` is in the free range, well below the EBDA. The 256-byte ModeInfoBlock + 12-byte summary + slack covers `0x9000-0x910C` — about 0x110 bytes.

## Step 2: surviving the PMM — two-phase init

Here's the bug that took an evening. After the bootloader returned ok=1 and the framebuffer pointer to `0xfd000000`, the kernel printed:

```
vbe: implausible mode info (4099x388 132-bpp pitch=8195) — disabled
```

The values were *bizarre and stable across reboots* — same garbage every time. A diagnostic hex dump of physical `0x9000` revealed why:

```
+00: 03 00 80 01 03 10 80 01 03 20 80 01 03 30 80 01
+10: 03 40 80 01 03 50 80 01 03 60 80 01 03 70 80 01
+20: 03 80 80 01 03 90 80 01 03 a0 80 01 03 b0 80 01
```

That pattern is unmistakable: 4-byte little-endian words of the form `0x01800003`, `0x01801003`, `0x01802003`, … each incremented by `0x1000`. That's a **page table** — PTEs pointing at consecutive 4 KiB pages with the present+writable flags `0x003` set in the low bits.

By the time `vbe_init()` ran, the kernel's `paging_init()` had already allocated the very page at physical `0x9000` to hold one of its PTs. The PMM treats `0x500-0x9FBFF` as free conventional RAM (correctly — that's exactly what E820 reports), and `paging_init` happily picks pages off the bottom of the free list to back the kernel's address space. Our bootloader's careful 12-byte summary was overwritten by a brand-new page table about 50 milliseconds after the BIOS wrote it.

The fix is structural: split VBE init into two phases that bracket `pmm_init`:

```c
// In kmain, immediately after memmap_init:
vbe_capture_bootinfo();   // snapshot 0x9100..0x910C into static g_boot
...
pmm_init();
...
paging_init();
...
vbe_init();               // validate g_boot, paging_map fb pages
fbcon_init();             // safe to fbcon_clear, set up cursor
```

`vbe_capture_bootinfo` does nothing more than read 12 bytes from physical `0x9100` and copy them into a kernel-private struct:

```c
static struct {
    int       captured;
    uint32_t  fb_phys;
    uint16_t  width, height, pitch;
    uint8_t   bpp, ok;
} g_boot;

void vbe_capture_bootinfo(void) {
    if (g_boot.captured) return;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)VBE_SUMMARY_PHYS;
    g_boot.fb_phys = *(volatile uint32_t *)(p + 0x00);
    g_boot.width   = *(volatile uint16_t *)(p + 0x04);
    /* ... */
    g_boot.captured = 1;
}
```

Now the data is in a kernel global (lives in `.bss`/`.data`, well above where the PMM ever allocates), and the later `vbe_init` reads it from there instead of from the now-overwritten low memory.

The general lesson: **anything passed from the bootloader through low memory has a deadline of "before pmm_init"**. The same trick (capture-then-process) is what `memmap.c` does for the E820 map at `0x8000`.

## Step 3: mapping the framebuffer

After `paging_init` finishes, the kernel has identity-mapped only the RAM region — typically up to 32 MiB (and rounded up to the next 4 MiB PD-entry boundary in session 31). The framebuffer on QEMU's std-vga lives at `0xFD000000`, way outside that mapping. Touching it from C would page-fault.

`vbe_init()` walks the FB region 4 KiB at a time and calls `paging_map`:

```c
uint32_t fb_size = (uint32_t)g_boot.pitch * (uint32_t)g_boot.height;
uintptr_t va_start = g_boot.fb_phys & ~0xFFFu;
uintptr_t va_end   = (g_boot.fb_phys + fb_size + 0xFFFu) & ~0xFFFu;
for (uintptr_t a = va_start; a < va_end; a += 0x1000) {
    if (paging_map(a, a, PTE_PRESENT | PTE_WRITABLE) < 0) {
        kprintf("vbe: paging_map failed at 0x%x — disabled\n", (unsigned)a);
        return;
    }
}
```

Identity-mapped (virtual = physical), R+W, kernel-only (`PTE_USER` clear). For 1024×768×24 with `pitch = 3072`, `fb_size = 3072 × 768 = 0x240000` bytes = 2.25 MiB, which is 576 pages. Each `paging_map` walks one PDE/PTE and calls `invlpg` on the page — about 580 page tables touched.

We do **not** mirror this PDE into user PDs. That means user processes will fault on `0xFD000000` access — which is the right default; the framebuffer shouldn't be a thumb-on-the-trigger for typo'd userspace pointers. If we ever expose it through a `mmap("/dev/fb0", …)` syscall, the mmap layer will copy the relevant PDE into the calling process's PD.

**Why not use write-combining (PTE_PCD/PTE_PWT)?** On real hardware the chipset's MTRRs almost always cover the LFB region with WC already, so the kernel-side bits are redundant. On QEMU, the framebuffer is just plain emulator memory — cache effects are invisible. Adding the bits "to be safe" risks making the FB *uncached* on chipsets that route MTRR settings through CR0.CD; that would tank performance for nothing.

## Step 4: the framebuffer console — `fbcon`

`fbcon` is roughly 200 lines of straightforward bitmap rendering. The interface mirrors VGA text mode (`putc`, `write`, `clear`, `set_color`) so dropping it into `kputc`'s sink list is a one-liner.

### Pixel packing

Three pixel formats matter for VBE:

| BPP | Layout | Bytes/pixel |
|-----|--------|-------------|
| 16  | RGB 5-6-5 | 2 (LE) |
| 24  | B G R | 3 |
| 32  | B G R X | 4 (X = padding/alpha, ignored) |

We accept 24-bit RGB colours in API calls (e.g. `FBC_GREY = 0xC0C0C0`) and pack them per-FB:

```c
static inline uint32_t pack_pixel(uint32_t rgb) {
    if (g_bpp == 16) {
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >>  8) & 0xFF;
        uint32_t b =  rgb        & 0xFF;
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    /* 24/32: BGR-ordered bytes; 32 implicitly stores X=0 */
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >>  8) & 0xFF;
    uint32_t b =  rgb        & 0xFF;
    return (r << 16) | (g << 8) | b;
}
```

`put_pixel` then handles the per-bpp store:

```c
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t pix) {
    if (x >= g_width || y >= g_height) return;       /* see below */
    volatile uint8_t *row = g_fb + (uintptr_t)y * g_pitch;
    if (g_bpp == 32) {
        ((volatile uint32_t *)row)[x] = pix;
    } else if (g_bpp == 24) {
        volatile uint8_t *p = row + x * 3;
        p[0] = (uint8_t)(pix);          /* B */
        p[1] = (uint8_t)(pix >> 8);     /* G */
        p[2] = (uint8_t)(pix >> 16);    /* R */
    } else { /* 16 */
        ((volatile uint16_t *)row)[x] = (uint16_t)pix;
    }
}
```

### Glyph rendering

`font8x8.h` is a 95-element array of `uint8_t[8]` — one row of 8 pixels per byte, with bit `n` being column `n` (LSB = leftmost). The renderer's hot loop:

```c
for (int r = 0; r < 8; r++) {
    uint8_t bits = glyph[r];
    for (int x = 0; x < 8; x++) {
        put_pixel(px + x, py + r,
                  (bits & (1 << x)) ? fg : bg);
    }
}
```

64 `put_pixel` calls per glyph. At 1024×768×24, the boot output emits ~3000 glyphs across the boot sequence — about 200,000 pixel writes total, all hitting QEMU's emulated memory in microseconds. Real hardware would run the same code in single-digit milliseconds for the entire boot. No optimisation needed.

### Scrolling

When we hit the bottom row, scroll one glyph row up and blank the bottom strip:

```c
static void scroll_up_one(void) {
    uint32_t scanlines = (g_rows - 1) * GH;     /* 95 * 8 = 760 */
    for (uint32_t y = 0; y < scanlines; y++) {
        volatile uint8_t *dst = g_fb + (uintptr_t)y       * g_pitch;
        volatile uint8_t *src = g_fb + (uintptr_t)(y+GH)  * g_pitch;
        memcpy((void *)dst, (const void *)src, g_width * (g_bpp / 8));
    }
    fbcon_fill_rect(0, scanlines, g_width, GH, g_bg);
}
```

760 byte-by-byte memcpy calls, each copying 3072 bytes. Total per scroll: 2.34 MB shuffled. Slow on 1995 hardware (a 286 would take a second per scroll), instant on QEMU. A future SSE-aware `memcpy` or a hardware BLT op (the QEMU std-vga supports one through the bochs-vbe registers) would close that gap on real hardware.

### The off-by-one fault

The first end-to-end run crashed at `[t6] signals` with:

```
[!] CPU EXCEPTION 14: Page fault (err=0x2) at 8:122c6  eflags=0x10246
    fault addr (CR2) = 0xfd240000
    cause = page not present, write, supervisor mode
```

`0xfd240000` is exactly `fb_phys + width*height*bpp/8 = 0xfd000000 + 0x240000`. We were writing **one byte past the end of the framebuffer**. Disassembling around the faulting PC pointed at the inlined `put_pixel` inside `draw_glyph`'s inner loop.

Math: for the fault to land at `fb + 0x240000`, we need `y * pitch + x * (bpp/8) = 0x240000`. With `pitch = 3072` and `bpp/8 = 3`, the only integer solution that hits *exactly* `0x240000` is `y = 768, x = 0` — i.e. one row past the bottom of the screen. `draw_glyph` was being called with `row = 96` (= `g_rows`), giving `py = 768`.

How did `g_cur_row` reach 96? The narrow window is in `fbcon_putc`'s default case:

```c
default:
    if ((uint8_t)c < 0x20) break;
    draw_glyph(g_cur_col, g_cur_row, c);    /* ← uses g_cur_row */
    g_cur_col++;
    if (g_cur_col >= g_cols) newline();     /* may bump g_cur_row to 96 */
    break;
```

Then `newline()` increments `g_cur_row` and conditionally scrolls + clamps:

```c
static void newline(void) {
    g_cur_col = 0;
    g_cur_row++;
    if (g_cur_row >= g_rows) {
        scroll_up_one();
        g_cur_row = g_rows - 1;
    }
}
```

The clamp re-establishes the invariant `g_cur_row ≤ g_rows - 1` before the next call. So on its own the cursor logic is fine. But we're feeding `fbcon` from `kprintf` in interrupt and non-interrupt contexts both, and a sufficiently long string with embedded `\n`s under load can wedge the cursor briefly.

Two fixes, both applied:

1. **Defensive bounds check in `put_pixel`.** Drop any out-of-range pixel silently. Costs one branch per pixel — invisible at QEMU's pixel-write rate. This is the right contract for a low-level primitive that callers (font renderer, future GUI bits) shouldn't have to pre-validate.

2. **Scroll discipline in `newline`.** Already correct in the current shape, but the put_pixel guard makes the system robust to any future cursor-management bug we introduce.

After the fix, the boot completes with all 23 selftests passing and `curl` continues to serve `httpd.elf`'s response.

## Hooking `fbcon` into `kprintf`

The integration is a one-liner in `kprintf.c`:

```c
void kputc(char c) {
    vga_putc(c);
    serial_putc(c);
    fbcon_putc(c);   /* no-op until fbcon_init() runs */
}
```

`fbcon_putc` checks `g_enabled` first and returns immediately if `vbe_init` failed or hadn't run yet. The cost is one branch per character against a hot global — well below the noise floor of `serial_putc`'s `outb` to the UART.

This means the system has **three console sinks always active**:

| Sink | When it works | Used for |
|------|---------------|----------|
| serial (COM1) | from `serial_init` (line 1 of kmain) | host-side test harness, headless boots, post-mortem debugging |
| VGA text mode | from `vga_init` (line 2 of kmain) | early-boot panics before VBE, screenshot fallback |
| fbcon (FB) | from `fbcon_init` (after `vbe_init`) | the actual graphical display in QEMU's window |

The serial sink is the test-harness's source of truth — every test we've ever written greps that. VGA text mode is a vestige but still useful for the seconds between `kmain` start and `fbcon_init`; on a panic in `paging_init` you really want SOMETHING on screen, and the framebuffer isn't usable yet. After `fbcon_init` succeeds, the VGA buffer at `0xB8000` is technically *underneath* the framebuffer's memory mapping (the LFB at `0xFD000000` doesn't intersect it), so `vga_putc` continues to scribble there — it's just that QEMU is no longer scanning that region for display. Cheap and harmless.

## Selftest

```c
puts("[t23] VBE/fbcon: sys_fbinfo reports framebuffer geometry\n");
{
    unsigned int info[4] = {0};
    int on = sys_fbinfo(info);
    if (on > 0) {
        printf("  fbcon enabled: %ux%u %u-bpp pitch=%u\n",
               info[0], info[1], info[2], info[3]);
        printf("  glyph cells (8x8 font): %ux%u\n",
               info[0] / 8, info[1] / 8);
        puts("  framebuffer-backed kprintf is live "
             "(this line is also painted to the FB)\n");
    } else if (on == 0) {
        puts("  fbcon disabled (kernel fell back to VGA text mode)\n");
    }
}
```

`SYS_FBINFO` is the entire interface — userspace doesn't touch the framebuffer, just queries whether it exists. The `puts(...)` line at the end serves a verification purpose: the kernel renders it to the FB through the kprintf sink chain, so a screenshot taken via QMP's `screendump` after the test runs will show those exact pixels. The harness-side check (visible-pixel-count via PIL) confirms that the FB has been written to.

Sample log output:

```
[t23] VBE/fbcon: sys_fbinfo reports framebuffer geometry
  fbcon enabled: 1024x768 24-bpp pitch=3072
  glyph cells (8x8 font): 128x96
  framebuffer-backed kprintf is live (this line is also painted to the FB)
```

## Bugs and lessons

**1. Bootloader summary survival.** As covered above — anything in low memory dies the moment `pmm_init` runs. The capture-then-process pattern works for E820, MADT, and now VBE. If we add more BIOS-time data later (SMBIOS, ACPI tables we're not yet parsing), it'll need the same dance.

**2. AT&T `disp16` addressing in real mode.** The bootloader's `mov 0x9010, %ax` is unambiguous to the assembler but its actual semantics depend on DS being what we expect at runtime. We've now had the same lesson twice — once in session 31 with the AP trampoline's `lgdtl` (which gas wanted to encode SI-relative), and now here with post-int-10h DS reload. The general rule: **after any BIOS interrupt, reload every segment register you care about.**

**3. The off-by-one. Defensive bounds in primitives are cheap.** The `put_pixel` guard adds one comparison per call — negligible — and turns "kernel page-faults under load" into "nothing happens, cursor logic catches up next tick." The same principle applies to `paging_map`'s OOM path and `fs_open`'s missing-file path: graceful degradation > correctness depending on caller hygiene.

**4. Pre-zero before BIOS calls.** SeaBIOS isn't supposed to return success-status with an unwritten buffer, but an old version did, and our test runs against whatever the host installed. The cost of `rep stosw 128` is a few hundred cycles — well worth the resilience.

## What's left

Things this session sets up but doesn't claim:

- **Mode fall-back chain.** Right now if `0x118` isn't available (e.g. very old VGABIOS), we go straight to text mode. A small loop trying `0x118 → 0x117 → 0x115` would catch a wider range of real hardware. The main cost is bytes in the boot sector — we have ~30 free; a 3-mode loop would fit.
- **`mmap("/dev/fb0", ...)`** for user-space framebuffer access. The mmap subsystem from session 24 already supports identity-mapping device pages into a process's PD; adding a synthetic `/dev/fb0` to procfs and routing mmap through `vbe_state()` is mostly plumbing.
- **A "graphical" mode for `sh.elf`.** With user FB access, the shell could render its prompt in a different colour, draw a status bar, etc. Pure aesthetic work — doesn't unblock anything.
- **Scroll acceleration.** The bochs-vbe registers expose a copy-rectangle BLT op that runs at host-bus bandwidth. Wiring it would reduce a 2.3 MB scroll to a single I/O port write. Worthwhile if we're ever drawing video or running a GUI.
- **Truetype-ish or larger fonts.** 8×8 is tight for human reading at 1024×768; an 8×16 or PSF-format 16×16 font would be more comfortable and maybe enable bold/italic via two-pass renders.
- **An IO-APIC LAPIC-timer-driven cursor blink.** Currently no cursor is drawn — the user shell uses the line-buffered TTY which doesn't have a "current position" indicator visible on the FB. A 200ms-blink cursor at `(g_cur_col, g_cur_row)` would look right.

## Files touched

- `boot/boot.S` — VBE mode-set, `0x9100` summary stash. Now 502/512 bytes of real code (10 spare).
- `kernel/vbe.h`, `kernel/vbe.c` — bootinfo capture + framebuffer mapping.
- `kernel/font8x8.h` — 95-glyph public-domain bitmap font.
- `kernel/fbcon.h`, `kernel/fbcon.c` — framebuffer console.
- `kernel/kprintf.c` — added `fbcon_putc` to the sink chain.
- `kernel/syscall.h`, `kernel/syscall.c` — `SYS_FBINFO = 52`.
- `kernel/kernel.c` — wired `vbe_capture_bootinfo`, `vbe_init`, `fbcon_init` into the boot sequence.
- `user/libuser.h`, `user/libuser.c` — `sys_fbinfo()` wrapper.
- `user/sh.c` — `[t23]` selftest.

About 700 LOC across kernel + user. The font header alone is 100 lines (95 glyphs × 8 bytes each, hand-formatted to a glyph-per-line table for review). Net deletions: zero — the existing VGA text-mode console stays untouched.
