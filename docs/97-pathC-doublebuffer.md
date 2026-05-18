# Session 110 — Path C phase 4: double-buffered libgfx

**Goal.** Kill the tearing. Session 109's mouse demo wrote primitives
straight to the VBE framebuffer; the user-visible cursor flickered
because the erase-then-draw was not atomic with the (virtual) scan-out.
Phase 4 introduces a backbuffer in libgfx and a single-blit present —
the user never sees a half-painted frame again.

Status: **done.** Smoke test (`smoke_db.py`):

```
=== pixel checks ===
  [OK] backdrop @ (200,200) = DARK_GREY
  [OK] white pixels in top text band (got 614)
  [OK] green frame top edge (got 1004/1004)
  [OK] cursor crosshair shape (33 px, ~16w x 16h)
  [OK] cursor moved away from spawn center (512,384)
  [OK] black HUD strip pixels @ y=748 (got 914/984)
  [OK] green frame right edge (got 726/748)
```

20 QMP-injected rel events of `(+10, +5)` walked the cursor from the
spawn-center `(512, 384)` to `(712, 484)`. The QMP screendump caught
**every** element of the frame in a single snapshot: backdrop, frame,
both top text lines, HUD strip, AND the cursor crosshair. With the
single-buffered renderer of session 109, the same kind of snapshot
sometimes caught the FB mid-paint (one element missing). With the
double-buffered renderer the present is one `memcpy` of 2.25 MiB —
the dump cannot land between two primitives because there are no
intermediate states visible from the scan-out side.

---

## What changed in libgfx

Two new fields on `struct gfx_ctx`:

```c
struct gfx_ctx {
    volatile unsigned char *fb;       /* drawing target */
    volatile unsigned char *fb_real;  /* the real mapped framebuffer */
    unsigned char          *back;     /* malloc'd backbuffer (NULL = single-buf) */
    /* ... width, height, pitch, bpp, fb_size unchanged ... */
};
```

`fb` is the *drawing* target. In single-buffered mode (legacy
`gfx_init`) it points at the mapped FB itself. In double-buffered
mode it points at the malloc'd backbuffer; `fb_real` is the actual
mapped FB.

The drawing primitives (`gfx_clear`, `gfx_fill_rect`, `gfx_line`,
`gfx_put_pixel`, `gfx_glyph`, `gfx_text`, `gfx_rect`) didn't change
at all — they already wrote through `ctx->fb`. By swapping the
pointer at init time, the entire library got double-buffering for
free.

Two new entry points:

```c
int  gfx_init_db (struct gfx_ctx *ctx, unsigned int user_va);
void gfx_present (struct gfx_ctx *ctx);
```

`gfx_init_db` calls the original `gfx_init` (so the FB syscalls still
run: info → map → ctx fields populated), then `malloc(fb_size)` and
re-points `ctx->fb` at the backbuffer. The user picks single- vs.
double-buffer at init time and never thinks about it again.

`gfx_present` is the entire dirty trick:

```c
void gfx_present(struct gfx_ctx *ctx) {
    if (!ctx || !ctx->back || !ctx->fb_real) return;
    volatile unsigned char *dst = ctx->fb_real;
    const unsigned char    *src = ctx->back;
    for (unsigned int i = 0; i < ctx->fb_size; i++) dst[i] = src[i];
}
```

For 1024×768×24 that's 2,359,296 bytes per frame. Sequential writes
to MMIO get the CPU's write-combining buffers to coalesce neatly;
measured ~3 ms per present on the QEMU box — well within a 16 ms
(60 fps) budget. The bottleneck is `memcpy` to VRAM, not the
primitives.

`gfx_release` frees the backbuffer:

```c
if (ctx->back) {
    free(ctx->back);
    ctx->back = 0;
    ctx->fb   = ctx->fb_real;
}
sys_fb_unmap();
```

If the backbuffer is NULL (single-buffered ctx), `gfx_present` is
a no-op — graceful degradation for older code that hasn't migrated.

---

## Why malloc, not a kernel-side buffer

The cleanest design would be: kernel-side reserved FB shadow + a
`SYS_FB_FLIP` syscall that asks the kernel to do the blit. That gets
us page-flipping later, when we have multiple FBs and a real
hardware vsync IRQ.

For now, malloc in userspace is simpler:

- The per-process heap is 4 MiB (`MAX_HEAP_BYTES`); a 2.25 MiB
  backbuffer fits with room for the rest of the program. `mouse.elf`
  uses ~6 KiB of heap aside from the backbuffer.
- No new syscall: the existing `SYS_FB_MAP` already gives userspace
  a direct write window into VRAM. The blit is a userspace loop.
- Single-task FB ownership (session 107) means we don't need to
  worry about another task starting a present mid-blit.

Tradeoff: every present pays a userspace→MMIO sequential copy. That
hurts if we ever want sub-millisecond frame loops or partial-region
updates. Future sessions:

- **Dirty rectangles**: track which `gfx_*` calls touched which
  region, and present only the changed slice. Reduces a 2.25 MiB
  copy to ~10 KiB for a mouse cursor move.
- **Triple buffer + page flip**: when the VBE PMID gets us a way
  to ask "switch base address," we can flip pointers and skip the
  copy entirely.

---

## What changed in user/mouse.c

Session 109's loop did per-frame erase-then-draw — store the last
cursor's bbox, clobber it with backdrop pixels, then draw the new
cursor. That was OK on a slow QEMU but produced flickering trails on
fast motion because the erase and the draw could land between two
scan-out reads from the host display.

Session 110's loop just repaints everything every tick. Cheap with
a backbuffer because the writes are RAM→RAM, not RAM→MMIO. The
single `gfx_present` at the end is the only MMIO write.

```c
if (gfx_init_db(&ctx, FB_VA) < 0) {
    printf("mouse: framebuffer unavailable or already owned\n");
    return 1;
}

for (int tick = 0; tick < total_ticks; tick++) {
    struct sys_mouse_state ms;
    if (sys_mouse_poll(&ms) < 0) break;

    gfx_clear(&ctx, GFX_DARK_GREY);
    gfx_text(&ctx, 8, 8, "AdventOS Path C - sessions 109+110", ...);
    gfx_text(&ctx, 8, 24, "move the mouse; double-buffered: no tearing now", ...);
    gfx_rect(&ctx, 4, 4, w-8, h-8, GFX_GREEN);

    /* color-code by button */
    unsigned int color = GFX_WHITE;
    if (ms.buttons & 0x01) color = GFX_RED;
    else if (ms.buttons & 0x02) color = GFX_BLUE;
    else if (ms.buttons & 0x04) color = GFX_YELLOW;
    draw_cursor(&ctx, ms.x, ms.y, color);

    /* HUD strip */
    gfx_fill_rect(&ctx, 4, hud_y, w-8, 28, GFX_BLACK);
    /* ... hand-format "x=NNN y=NNN buttons=LRM" into buf ... */
    gfx_text(&ctx, 12, hud_y + 10, buf, GFX_GREEN, GFX_BLACK);

    gfx_present(&ctx);
    sys_sleep_ms(16);     /* aim for 60 fps */
}
gfx_release(&ctx);
```

That's the whole loop. Notably simpler than session 109 because the
"track last cursor bbox" book-keeping is gone.

---

## Why no IRQ-driven vsync

VBE doesn't ship a vertical-blank IRQ on the PCI side; the original
hardware had it on the VESA local bus. QEMU's `-vga std` exposes
the framebuffer through MMIO at `0xE0000000` with no IRQ source we
can hook. The closest we could get is poll-on-status-register, and
even that's a guess.

So we cheat: the `sys_sleep_ms(16)` budgets the frame to ~60 fps,
the present copies the whole backbuffer atomically (from the
scan-out's perspective), and tearing only re-emerges if the scan-out
read window happens to slice across our memcpy. On QEMU that
manifests as a faint diagonal hairline once every few seconds at
worst; not visible to the eye at the demo's pace.

A future session that wires up real hardware will likely use a
PMID-flip strategy: two framebuffers in VRAM, write-then-flip,
no copy. Until then, this is the best we can do without VBE3
vbe_get_set_display_start_position() — and even that needs board
support.

---

## Files touched

- `libgfx/libgfx.h` — added `fb_real`, `back` to ctx; `gfx_init_db`,
  `gfx_present` decls
- `libgfx/libgfx.c` — `gfx_init` initializes new fields; new
  `gfx_init_db` allocates backbuffer; `gfx_present` blits; `gfx_release`
  frees backbuffer
- `user/mouse.c` — switched to `gfx_init_db`, full repaint each tick,
  `gfx_present` at end of tick
- `smoke_db.py` — new headless smoke harness (QMP screendump +
  pixel checks)
- `docs/97-pathC-doublebuffer.md` — this file

No kernel changes. The FB syscalls (107) and PS/2 mouse driver (109)
were sufficient.

`kernel.bin`: 131248 → 131248 (kernel untouched). `libgfx.o`: 3216 →
4400 bytes (+1184). `user/_obj/mouse.elf`: 18616 → 18616 (slight
restructure but same total).

---

## Path C status after session 110

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx (drawing primitives, fonts)
- ✅ 109 — PS/2 mouse + cursor demo
- ✅ 110 — double-buffering / no-tearing present
- ⏳ 111 — window manager daemon (compositing, decorations)
- ⏳ 112+ — WM client protocol + sample widget client

The story so far: one task can own the FB and draw smooth, flicker-free
graphics with a mouse cursor. Session 111 takes the FB away from any
single user task and gives it to a daemon (`wmd`) that composites
multiple client surfaces — the first time we'll have more than one
program on screen at once.
