# Session 151 — Path C phase 44: Alt+P screenshot

**Goal.** Global screenshot shortcut.  Press **Alt+P** anywhere
in wmd → the current framebuffer is dumped to `/tmp/screen.ppm`
as a binary P6 PPM.  Then `wmview /tmp/screen.ppm` shows the
snapshot.  Pairs with session 150's wmpaint Ctrl-S save —
same pattern, but for the whole desktop.

Status: **done.**  Smoke `smoke_screenshot.py` (3/3):

```
=== checks ===
  [OK] screenshot toast visible (5223 slate px)
  [OK] wmview shows screenshot content (10530 px)
  [OK] wmd status bar alive (842/924)
```

Boot wmd + wmedit, press Alt+P — a toast appears in the bottom-
right confirming the save.  Open `wmview /tmp/screen.ppm` and
the wmview window shows the captured desktop (clipped to its
400×~278 content area since the screenshot is 1024×768).

---

## Plumbing

The shape is identical to session 147's workspace channel + 150's
wmpaint save: a kernel-side one-shot flag, an Alt-key intercept
in usb_hid, a per-frame poll in wmd, and a streaming `sys_write`
to `/tmp/screen.ppm`.

### Kernel — screenshot trigger channel

```c
/* kernel/wm.c */
static volatile int g_screenshot_pending = 0;

void wm_post_screenshot(void) {
    g_screenshot_pending = 1;
}

int wm_poll_screenshot(struct task *caller) {
    if (g_wm_owner != caller) return 0;
    if (!g_screenshot_pending) return 0;
    g_screenshot_pending = 0;
    return 1;
}
```

Exposed via `SYS_WM_POLL_SCREENSHOT = 108`.

### USB-HID — Alt+P intercept

HID Usage Tables: 'p' = 0x13 (just past 'o' = 0x12 in the
`'a'..'z'` block).

```c
/* kernel/usb_hid.c, after the Alt+Tab and Alt+1..4 intercepts */
if (alt && usage == 0x13) {
    extern void wm_post_screenshot(void);
    wm_post_screenshot();
    return;
}
```

`return` bypasses the keyboard ring — Alt+P never reaches
clients as a key event.

### wmd — serialize ctx.fb to PPM

`save_screenshot(struct gfx_ctx *ctx)` walks the framebuffer
row-by-row and writes a P6 PPM via streaming `sys_write`:

```c
int fd = sys_open_w("/tmp/screen.ppm");
/* header: "P6\n<w> <h>\n255\n" */
sys_write(fd, hdr, hn);

unsigned char row[1280 * 3];
int bytes_per_px = ctx->bpp / 8;
for (int y = 0; y < h; y++) {
    unsigned char *fb_row = (unsigned char *)ctx->fb_real + y * ctx->pitch;
    for (int x = 0; x < w; x++) {
        unsigned char *p = fb_row + x * bytes_per_px;
        row[x*3 + 0] = p[2];   /* R (FB byte 2) */
        row[x*3 + 1] = p[1];   /* G (FB byte 1) */
        row[x*3 + 2] = p[0];   /* B (FB byte 0) */
    }
    sys_write(fd, row, w * 3);
}
sys_close(fd);
```

The FB byte order is **BGR** for both 24-bpp and 32-bpp paths in
AdventOS (see `libgfx/libgfx.c:put_packed`).  PPM expects RGB, so
the byte order swaps in the inner loop.

### Toast confirmation

After save, wmd posts a notification ("screenshot:
/tmp/screen.ppm (2359306 B)") via `sys_wm_notify` so the user
has visible confirmation the save fired.

---

## Why poll BEFORE the rest of the frame

`save_screenshot` reads from `ctx->fb_real` — the actual visible
FB.  If we polled AFTER wmd's per-frame paint, the saved image
would include this frame's in-flight repaint (toast painted on
top of wallpaper that was just cleared, etc.).  Polling at the
TOP of the loop means we capture the previous frame's fully-
composited state, which is what the user sees when they pressed
Alt+P.

The newly-fired toast appears starting NEXT frame, so it isn't
in the screenshot — exactly what you'd want.

---

## What stays out of scope

- **Region screenshots.**  Full FB only.  No drag-rect, no
  current-window-only mode.
- **Multiple screenshots.**  Each Alt+P overwrites
  `/tmp/screen.ppm`.  Save copies with `cp` from a shell.
- **Format choice.**  P6 PPM only, no PNG.
- **System-wide hotkey.**  Alt+P works only when wmd is the
  active WM.  No equivalent for the serial console.

---

## Files touched

- `kernel/wm.c`, `kernel/wm.h` — `g_screenshot_pending` +
  `wm_post_screenshot` / `wm_poll_screenshot`
- `kernel/syscall.h`, `kernel/syscall.c` —
  `SYS_WM_POLL_SCREENSHOT = 108`
- `user/libuser.c`, `user/libuser.h` — `sys_wm_poll_screenshot`
- `kernel/usb_hid.c` — Alt+P (HID usage 0x13) intercept
- `user/wmd.c`:
  - `save_screenshot()` helper that streams FB → PPM
  - Per-frame `sys_wm_poll_screenshot()` at top of main loop
- `smoke_screenshot.py` — new harness, 3 pixel checks
- `docs/137-pathC-screenshot.md` — this file

Sizes:
- kernel.bin: 164016 (unchanged net — small post/poll funcs
  absorbed by --gc-sections)
- wmd.bin: 19456 → 20592 (+1136 B for the screenshot helper +
  poll site)

---

## Path C status after session 151

- ✅ 107..150 — see prior docs
- ✅ 151 — Alt+P screenshot → /tmp/screen.ppm
- ⚠️  wmterm input + close — still deferred

The save-and-view loop covers both creative (wmpaint Ctrl-S) and
documentation (Alt+P screenshot) workflows.  Either one writes a
PPM that wmview opens.
