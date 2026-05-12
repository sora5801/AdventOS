/*
 * gclient — example out-of-process WM client (session 62).
 *
 * Connects to the WM on 127.0.0.1:7000, speaks the protocol from
 * include/wm_proto.h, and demonstrates the round trip:
 *
 *   1. HELLO + handshake
 *   2. CREATE_WIN — request a window; receive a wid back
 *   3. FILL_RECT / DRAW_TEXT — paint into the pixmap
 *   4. PRESENT — advisory commit
 *   5. read events back — clicks and keys land here
 *
 * Two modes via argv:
 *
 *   gclient          — interactive: connect, paint a static scene,
 *                       loop forever reading events. The user can
 *                       click into our window and see clicks logged
 *                       to stdout. Close button → exit.
 *
 *   gclient selftest — scripted: same connect + paint, but exits
 *                       after receiving N events from the WM. The
 *                       t45 selftest reads our printf output to
 *                       verify everything roundtripped.
 *
 * This client has zero direct framebuffer access. Every pixel goes
 * through a draw command sent over TCP — that's the whole point of
 * out-of-process apps: an unprivileged process can't crash the WM
 * by misusing the framebuffer; it can only ask politely. Same
 * isolation model real-world Wayland uses.
 */

#include "libuser.h"
#include "../include/wm_proto.h"

static int g_fd = -1;

/* Send a message: 4-byte header + payload bytes.  Returns 0 on
 * success, -1 if sys_write came up short. */
static int send_msg(uint8_t kind, const void *payload, int len) {
    struct wm_hdr h = { .kind = kind, .reserved = 0, .length = (uint16_t)len };
    if (sys_write(g_fd, &h, sizeof(h)) != sizeof(h)) return -1;
    if (len > 0) {
        if (sys_write(g_fd, payload, len) != len) return -1;
    }
    return 0;
}

/* Read N bytes — handles short reads. Returns 0 on success, -1 on
 * disconnect / error. */
static int read_n(void *buf, int n) {
    char *p = (char *)buf;
    int got = 0;
    while (got < n) {
        int r = sys_read(g_fd, p + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* Receive one message into a caller-supplied buffer. *out_len is
 * set to the payload byte count. */
static int recv_msg(uint8_t *out_kind, void *payload, int cap, int *out_len) {
    struct wm_hdr h;
    if (read_n(&h, sizeof(h)) < 0) return -1;
    if (h.length > cap) return -1;
    if (h.length > 0) {
        if (read_n(payload, h.length) < 0) return -1;
    }
    *out_kind = h.kind;
    *out_len  = h.length;
    return 0;
}

/* ---- High-level wrappers --------------------------------------- */

static int gc_hello(void) {
    struct wm_cmd_hello m = { .magic = WM_PROTO_MAGIC, .version = 1 };
    return send_msg(WM_CMD_HELLO, &m, sizeof(m));
}

static int gc_create_win(int x, int y, int w, int h, const char *title) {
    struct wm_cmd_create_win m = {0};
    m.x = x; m.y = y; m.w = w; m.h = h;
    for (int i = 0; i < (int)sizeof(m.title) - 1 && title[i]; i++) {
        m.title[i] = title[i];
    }
    if (send_msg(WM_CMD_CREATE_WIN, &m, sizeof(m)) < 0) return -1;
    /* Wait for the WM_EVT_WINDOW_ID reply. */
    uint8_t kind;
    static uint8_t payload[WM_MAX_PAYLOAD];
    int len = 0;
    if (recv_msg(&kind, payload, sizeof(payload), &len) < 0) return -1;
    if (kind != WM_EVT_WINDOW_ID || len < (int)sizeof(struct wm_evt_window_id)) return -1;
    const struct wm_evt_window_id *ev = (const void *)payload;
    return ev->wid;
}

static int gc_fill_rect(int wid, int x, int y, int w, int h, uint32_t rgb) {
    struct wm_cmd_fill_rect m = { .wid = wid, .x = x, .y = y,
                                   .w = w, .h = h, .rgb = rgb };
    return send_msg(WM_CMD_FILL_RECT, &m, sizeof(m));
}

static int gc_draw_text(int wid, int x, int y, uint32_t rgb,
                         const char *s, int slen) {
    /* The payload is the struct prefix followed by `slen` raw bytes.
     * We construct it in a single buffer so send_msg ships one
     * coherent message. */
    static uint8_t buf[sizeof(struct wm_cmd_draw_text) + 64];
    if (slen < 0 || slen > 64) return -1;
    struct wm_cmd_draw_text *m = (void *)buf;
    m->wid = wid; m->x = x; m->y = y; m->rgb = rgb;
    for (int i = 0; i < slen; i++) buf[sizeof(*m) + i] = (uint8_t)s[i];
    return send_msg(WM_CMD_DRAW_TEXT, buf, (int)sizeof(*m) + slen);
}

static int gc_present(int wid) {
    struct wm_cmd_present m = { .wid = wid };
    return send_msg(WM_CMD_PRESENT, &m, sizeof(m));
}

/* ---- Draw a static scene --------------------------------------- */

/* Paint the entire window content area with a 3-region pattern:
 *
 *   top stripe: bright red 0xE03030
 *   middle:     navy blue 0x103060
 *   bottom:     dark green 0x208030
 *
 * Plus a label "OOP CLIENT" centered over the middle band. Selftest
 * t45 pixel-checks a couple of coords inside each band as proof that
 * the WM rasterized our fill_rect commands correctly. */
static void draw_scene(int wid, int cw, int ch) {
    int top    = ch / 3;
    int middle = ch - 2 * top;
    int bot_y  = top + middle;

    gc_fill_rect(wid, 0,     0,        cw, top,    0xE03030u);
    gc_fill_rect(wid, 0,     top,      cw, middle, 0x103060u);
    gc_fill_rect(wid, 0,     bot_y,    cw, top,    0x208030u);

    const char *label = "OOP CLIENT";
    int slen = 0; while (label[slen]) slen++;
    int tx = (cw - slen * 8) / 2;
    int ty = top + (middle - 8) / 2;
    gc_draw_text(wid, tx, ty, 0xFFFFFFu, label, slen);

    gc_present(wid);
}

/* ---- main ------------------------------------------------------- */

int main(int argc, char **argv) {
    int selftest = (argc >= 2 && argv[1][0] == 's');

    g_fd = sys_socket();
    if (g_fd < 0) { puts("gclient: socket() failed\n"); return 1; }
    unsigned char wm_ip[4] = { 127, 0, 0, 1 };
    if (sys_connect(g_fd, wm_ip, WM_PORT) < 0) {
        puts("gclient: connect to WM failed — is gui.elf running?\n");
        sys_close(g_fd);
        return 1;
    }
    printf("gclient: connected to WM on fd=%d\n", g_fd);

    if (gc_hello() < 0) {
        puts("gclient: HELLO send failed\n");
        sys_close(g_fd);
        return 1;
    }

    /* Spawn our window. Below the existing Tasks app so it doesn't
     * overlap with t39's pixel-check region in the upper-left. */
    int win_w = 220, win_h = 140;
    int win_x = 720, win_y = 480;
    int wid = gc_create_win(win_x, win_y, win_w, win_h, "OOP Client");
    if (wid < 0) {
        puts("gclient: CREATE_WIN failed\n");
        sys_close(g_fd);
        return 1;
    }
    printf("gclient: window %d created at %d,%d %dx%d\n",
           wid, win_x, win_y, win_w, win_h);

    /* Paint the static scene. content area is window minus title bar
     * (= w x (h - TITLE_H - 1)). Mirror what the WM cap'd it at —
     * just use the requested h minus 19 (TITLE_H+1) for safety. */
    int cw = win_w;
    int ch = win_h - 19;
    draw_scene(wid, cw, ch);
    printf("gclient: scene painted (%dx%d) and presented\n", cw, ch);

    /* Event loop. */
    int event_budget = selftest ? 8 : 1024;
    int n_events = 0;
    int got_close = 0;
    while (n_events < event_budget && !got_close) {
        uint8_t kind = 0;
        static uint8_t pl[WM_MAX_PAYLOAD];
        int len = 0;
        if (recv_msg(&kind, pl, sizeof(pl), &len) < 0) {
            puts("gclient: WM disconnected\n");
            break;
        }
        n_events++;
        switch (kind) {
            case WM_EVT_MOUSE_BTN: {
                if (len < (int)sizeof(struct wm_evt_mouse_btn)) break;
                const struct wm_evt_mouse_btn *e = (const void *)pl;
                printf("gclient: got CLICK wid=%d at (%d,%d) btns=0x%x down=%d\n",
                       e->wid, e->lx, e->ly, e->btns, e->down);
                break;
            }
            case WM_EVT_KEY: {
                if (len < (int)sizeof(struct wm_evt_key)) break;
                const struct wm_evt_key *e = (const void *)pl;
                printf("gclient: got KEY wid=%d code=0x%x\n",
                       e->wid, e->keycode);
                break;
            }
            case WM_EVT_CLOSE: {
                puts("gclient: got CLOSE — shutting down\n");
                got_close = 1;
                break;
            }
            default:
                printf("gclient: unhandled event kind=%d len=%d\n", kind, len);
                break;
        }
    }
    printf("gclient: processed %d event(s), exiting\n", n_events);
    sys_close(g_fd);
    return 0;
}
