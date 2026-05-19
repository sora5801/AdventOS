/*
 * wmview.c — session 149 / Path C phase 42 image viewer.
 *
 * Usage: wmview <path.ppm> [seconds]
 *
 * Opens a 400x300 window and displays the contents of a P6 PPM
 * (binary RGB888) file centered in the body.  Images larger than
 * the window are clipped; smaller images are centred against a
 * dark background.  No scaling, no format negotiation — strict
 * "P6 + maxval 255" only.
 *
 * Keys: Ctrl-Q quits.
 *
 * Implementation notes:
 *   - One file read into a 256 KiB scratch buffer at startup.
 *   - Header parsing tolerates '#'-comment lines per PPM spec.
 *   - Pixel blit writes directly to win.pixels (the WM-mapped
 *     shared surface) so we avoid per-pixel sys_wm_event roundtrip
 *     cost — that's ~64K syscalls for a 320x200 image otherwise.
 *   - Image is blitted ONCE outside the main loop; the main loop
 *     just repaints the title bar (cheap) and yields.
 */

#include "libuser.h"
#include "../libwm/libwm.h"
#include "../libgfx/libgfx.h"

#define WIN_W            400
#define WIN_H            300
#define HDR_H             18
/* 32 KiB cap.  Userspace BSS is emitted as zero-bytes in the .bin
 * blob (USER_CFLAGS has -fno-zero-initialized-in-bss for ELF
 * loader simplicity), so the buffer size directly inflates the
 * binary on disk.  32 KiB fits a 100x100 P6 PPM (30 K of pixel
 * data + ~20 B header); the bundled sample.ppm is 64x48 = 9 KiB. */
#define MAX_PPM_BYTES (32 * 1024)

static char           g_path[64];
static unsigned char  g_buf[MAX_PPM_BYTES];

static int my_atoi_str(const char *s) { return atoi(s); }

static void make_surface_ctx(struct gfx_ctx *ctx, struct wm_window *w) {
    ctx->fb       = (volatile unsigned char *)w->pixels;
    ctx->fb_real  = ctx->fb;
    ctx->back     = 0;
    ctx->user_va  = (unsigned int)(uintptr_t)w->pixels;
    ctx->width    = w->w;
    ctx->height   = w->h;
    ctx->pitch    = w->w * 4;
    ctx->bpp      = 32;
    ctx->fb_size  = w->w * w->h * 4;
}

/* Skip ASCII whitespace + '#'-comment lines starting at *pos. */
static void skip_ws_and_comments(int *pos, int max) {
    while (*pos < max) {
        unsigned char c = g_buf[*pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            (*pos)++;
        } else if (c == '#') {
            while (*pos < max && g_buf[*pos] != '\n') (*pos)++;
            if (*pos < max) (*pos)++;
        } else {
            break;
        }
    }
}

/* Parse a non-negative decimal integer at *pos.  Returns -1 if no
 * digit was found; advances *pos past the number on success. */
static int parse_uint(int *pos, int max) {
    if (*pos >= max) return -1;
    int v = 0, got = 0;
    while (*pos < max && g_buf[*pos] >= '0' && g_buf[*pos] <= '9') {
        v = v * 10 + (g_buf[*pos] - '0');
        (*pos)++;
        got = 1;
    }
    return got ? v : -1;
}

int main(int argc, char **argv) {
    int seconds = 600;

    if (argc < 2) {
        printf("usage: wmview <path.ppm> [seconds]\n");
        return 1;
    }
    int i = 0;
    for (; i < (int)sizeof(g_path) - 1 && argv[1][i]; i++)
        g_path[i] = argv[1][i];
    g_path[i] = 0;
    if (argc >= 3) seconds = my_atoi_str(argv[2]);
    if (seconds <= 0) seconds = 600;

    /* Load the file into our scratch buffer. */
    int fd = sys_open(g_path);
    if (fd < 0) {
        printf("wmview: cannot open %s\n", g_path);
        return 1;
    }
    int n = sys_read(fd, g_buf, MAX_PPM_BYTES);
    sys_close(fd);
    if (n < 0) n = 0;
    if (n < 11) {
        printf("wmview: %s: file too short (%d bytes)\n", g_path, n);
        return 1;
    }

    /* Header: "P6" magic, then width / height / maxval separated by
     * whitespace (with optional '#' comments), then exactly one
     * whitespace byte before the binary pixel data. */
    if (g_buf[0] != 'P' || g_buf[1] != '6') {
        printf("wmview: %s: not a P6 PPM\n", g_path);
        return 1;
    }
    int p = 2;
    skip_ws_and_comments(&p, n);
    int img_w = parse_uint(&p, n);
    skip_ws_and_comments(&p, n);
    int img_h = parse_uint(&p, n);
    skip_ws_and_comments(&p, n);
    int maxv  = parse_uint(&p, n);
    /* Spec: exactly one whitespace byte separates maxval from data. */
    if (p < n) p++;

    if (img_w <= 0 || img_h <= 0 || maxv <= 0) {
        printf("wmview: %s: bad header (w=%d h=%d maxv=%d)\n",
               g_path, img_w, img_h, maxv);
        return 1;
    }
    if (maxv != 255) {
        printf("wmview: %s: only maxval=255 supported (got %d)\n",
               g_path, maxv);
        return 1;
    }
    int data_pos = p;
    int need = img_w * img_h * 3;
    if (data_pos + need > n) {
        printf("wmview: %s: truncated (have %d need %d)\n",
               g_path, n - data_pos, need);
        /* Fall through — we'll blit what we have, clipped by need. */
    }

    /* Open the WM window. */
    struct wm_window win;
    if (wm_open(&win, "wmview", WIN_W, WIN_H) < 0) {
        printf("wmview: SYS_WM_CREATE failed; is wmd running?\n");
        return 1;
    }
    printf("wmview: id=%u path=%s img=%dx%d\n",
           win.id, g_path, img_w, img_h);

    struct gfx_ctx sctx;
    make_surface_ctx(&sctx, &win);

    /* One-time: clear background and blit the image centred in
     * the content area (below the title bar). */
    wm_clear(&win, 0x101010u);
    int content_y0 = HDR_H + 4;
    int content_h  = WIN_H - content_y0 - 4;
    int draw_w = img_w < WIN_W   ? img_w : WIN_W;
    int draw_h = img_h < content_h ? img_h : content_h;
    int ox = (WIN_W - draw_w) / 2;
    int oy = content_y0 + (content_h - draw_h) / 2;

    unsigned int *pix   = win.pixels;
    unsigned int  pitch = win.pitch_px;
    for (int y = 0; y < draw_h; y++) {
        for (int x = 0; x < draw_w; x++) {
            int idx = data_pos + (y * img_w + x) * 3;
            if (idx + 2 >= n) break;
            unsigned int r = g_buf[idx + 0];
            unsigned int g = g_buf[idx + 1];
            unsigned int b = g_buf[idx + 2];
            pix[(oy + y) * pitch + (ox + x)] = (r << 16) | (g << 8) | b;
        }
    }

    /* Main loop: only repaints title bar + footer each frame; the
     * image is static and survives across frames. */
    int has_focus  = 0;
    int closed     = 0;
    int total_ticks = seconds * 5;     /* 200 ms tick — image is static */
    for (int tick = 0; tick < total_ticks && !closed; tick++) {
        struct wm_event ev;
        while (wm_poll_event(&win, &ev)) {
            switch (ev.type) {
                case WM_EV_FOCUS:   has_focus = 1; break;
                case WM_EV_UNFOCUS: has_focus = 0; break;
                case WM_EV_CLOSE:   closed = 1; break;
                case WM_EV_KEY:
                    if (ev.keycode == 0x11) closed = 1; /* Ctrl-Q */
                    break;
                default: break;
            }
        }
        /* Title bar (re-paint every frame so focus-colour tracks). */
        wm_fill_rect(&win, 0, 0, WIN_W, HDR_H,
                     has_focus ? 0x4080E0u : 0x404040u);
        gfx_text(&sctx, 6, 5,
                 has_focus ? "wmview  Ctrl-Q quit"
                           : "wmview  (click to focus)",
                 GFX_WHITE, GFX_TRANSPARENT);
        wm_present(&win);
        sys_sleep_ms(200);
    }

    wm_close(&win);
    return 0;
}
