# Session 108 — Path C phase 2: libgfx (software drawing library)

**Goal.** Move the drawing primitives out of `gfx.c` into a static library so future graphics programs (the WM, sample clients, etc.) aren't reinventing pack-pixel, Bresenham, glyph blitting. Pattern match the existing `libjson` / `libcrypto` / `libagent` separate-archive scheme.

Status: **done.** Smoke test on a fresh boot:

```
$ gfx 4
gfx: 1024x768 24bpp, pitch=3072, 2304 KiB
[ test card visible: color bars, RGB gradient, centered title card,
  two diagonals, green-on-black status strip at the bottom ]
gfx: released
```

QMP screendump confirms:
- Red / yellow / white bars at the top (sample at y=10) ✓
- White pixel near origin (diagonal start) ✓
- Diagonal crosses screen center, overwriting the blue card with a white pixel ✓
- Bottom strip background pure black ✓
- 20+ green pixels in the bottom strip — text rendering works ✓

---

## What libgfx provides

```c
struct gfx_ctx {
    volatile unsigned char *fb;     /* user-mapped framebuffer base */
    unsigned int user_va, width, height, pitch, bpp, fb_size;
};

int  gfx_init    (struct gfx_ctx *ctx, unsigned int user_va);
void gfx_release (struct gfx_ctx *ctx);

unsigned int gfx_pack (const struct gfx_ctx *ctx, unsigned int rgb);

void gfx_put_pixel (struct gfx_ctx *ctx, int x, int y, unsigned int rgb);
void gfx_clear     (struct gfx_ctx *ctx, unsigned int rgb);
void gfx_fill_rect (struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb);
void gfx_line      (struct gfx_ctx *ctx, int x0, int y0, int x1, int y1, unsigned int rgb);
void gfx_rect      (struct gfx_ctx *ctx, int x, int y, int w, int h, unsigned int rgb);

#define GFX_TRANSPARENT 0xFFFFFFFFu
void gfx_glyph     (struct gfx_ctx *ctx, int x, int y, char c, unsigned int fg, unsigned int bg);
void gfx_text      (struct gfx_ctx *ctx, int x, int y, const char *s, unsigned int fg, unsigned int bg);
```

All primitives clip to `ctx->width × ctx->height`. The user passes 0xRRGGBB; libgfx packs to the negotiated bpp (16 = RGB565, 24 = BBGGRR bytes, 32 = 0xAARRGGBB dword) — same packings fbcon uses.

The 8x8 font is the same public-domain bitmap fbcon ships (`libgfx/font8x8.h` is a copy of `kernel/font8x8.h`). 96 printable ASCII glyphs. Non-printable chars render as `?`.

`gfx_line` is straight Bresenham. `gfx_rect` is four `gfx_line` calls.

---

## Build wiring

Same shape as `libjson` from session 64:

```
[5d/7] build libgfx (static, statically linked into graphics programs)
        libgfx.o = 4044 bytes
```

A new `GFX_PROGS=(gfx)` list in `build.sh` gets a link block that pulls in `libgfx/_obj/libgfx.o` alongside `libuser.o`. Future graphics programs (the WM) join the same list.

gfx.bin grew from 6 KB → 9 KB. The extra is the 8x8 font (`96 × 8 = 768 bytes`) + Bresenham + the rect-outline / text helpers.

---

## What gfx.c got rid of

Sessions 107's `gfx.c` had its own `pack`, `plot`, `fill_rect` helpers all inlined. Now it's just:

```c
struct gfx_ctx ctx;
if (gfx_init(&ctx, FB_VA) < 0) ...;
gfx_clear(&ctx, GFX_DARK_GREY);
gfx_fill_rect(&ctx, 0, 0, bar_w, 64, GFX_RED);
/* ...etc... */
gfx_line(&ctx, 0, 0, ctx.width-1, ctx.height-1, GFX_WHITE);
gfx_text(&ctx, 16, 16, "AdventOS Path C", GFX_WHITE, GFX_TRANSPARENT);
gfx_release(&ctx);
```

The new test card adds text labels and two diagonals to exercise the new primitives.

---

## What's deferred

- **No backbuffer / double-buffering yet.** Direct stores to the live FB. Animation is visibly torn. Session 110.
- **No clip rect / nested ctxs.** Drawing always clips to the framebuffer; can't restrict to a window region yet. The WM will need this.
- **No alpha blending.** GFX_TRANSPARENT is only honored as "skip the background fill" in `gfx_glyph`. No real alpha math.
- **Font is 8x8 only.** No different sizes, no antialiasing, no kerning.
- **No image loading.** No PPM/PNG/anything decoder. Future sessions if we want sprites.

Each is a 1-session add when motivated.

---

## Files touched

- `libgfx/libgfx.h` — new public API
- `libgfx/libgfx.c` — implementations (~180 lines)
- `libgfx/font8x8.h` — copy of kernel font (public domain)
- `build.sh` — new `[5d/7]` block builds libgfx; `gfx` moves from `USER_PROGS` to a new `GFX_PROGS` list with libgfx linked in
- `user/gfx.c` — refactored to use libgfx; expanded test card with text + diagonals

libgfx.o: 4044 bytes. gfx.bin: 6116 → 9156 bytes.

---

## Path C status after session 108

- ✅ 107 — FB syscalls (`SYS_FB_INFO`, `SYS_FB_MAP`, `SYS_FB_UNMAP`)
- ✅ 108 — libgfx drawing library
- ⏳ 109 — PS/2 mouse driver
- ⏳ 110 — double-buffering
- ⏳ 111 — window manager daemon
- ⏳ 112+ — widgets, fonts, event loop
