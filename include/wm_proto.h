/*
 * AdventOS — Window Manager IPC protocol (session 62).
 *
 * Tiny "draw protocol" speaking length-prefixed binary messages over a
 * TCP loopback socket. Clients connect to WM_PORT on 127.0.0.1; both
 * client→WM commands and WM→client events use a common 4-byte header
 * (kind, reserved, length) followed by a payload whose layout depends
 * on kind.
 *
 * Why TCP loopback (vs a custom socket family or shared memory):
 *   * It's already there — kernel/sock.c + try_loopback in ip.c
 *     short-circuit packets to 127.0.0.1 directly to the rx callback
 *     without ever touching the NIC.
 *   * It's the protocol shape X11 was built on for decades, so the
 *     mental model is well-understood.
 *   * Clients can come and go independently of the WM's lifetime.
 *
 * Why length-prefixed and not netstring or framed-by-newline:
 *   * Binary geometries (rects, RGB triples) trivial to encode.
 *   * The WM can drain ONE message at a time off each client fd
 *     without needing to look at content to know where the boundary is.
 *   * Lets the server enforce a max-message-size cap up front.
 *
 * Endianness: native little-endian (32-bit i386 only). If a different
 * arch ever wants to talk this protocol it'll need byte-order helpers.
 */
#ifndef ADVENTOS_WM_PROTO_H
#define ADVENTOS_WM_PROTO_H

#include "types.h"

#define WM_PORT          7000
#define WM_PROTO_MAGIC   0x47554931u    /* 'GUI1' */

/* Header common to every message in either direction. */
struct wm_hdr {
    uint8_t  kind;       /* CMD_* or EVT_* */
    uint8_t  reserved;   /* always 0 */
    uint16_t length;     /* payload bytes that follow this header */
};

/* ---- Client → WM commands -------------------------------------- */

#define WM_CMD_HELLO       1   /* identify + protocol-version handshake */
#define WM_CMD_CREATE_WIN  2   /* spawn a window owned by this client   */
#define WM_CMD_FILL_RECT   3   /* solid rectangle into a window pixmap  */
#define WM_CMD_DRAW_TEXT   4   /* 8x8 glyph string                      */
#define WM_CMD_DRAW_PIXEL  5   /* single pixel (cheaper than 1x1 rect)  */
#define WM_CMD_DESTROY     6   /* close a window the client owns        */
#define WM_CMD_PRESENT     7   /* "commit changes" — WM redraws on next */
                               /*  frame; advisory only because the WM  */
                               /*  composites every frame anyway        */

struct wm_cmd_hello {
    uint32_t magic;            /* must equal WM_PROTO_MAGIC */
    uint32_t version;          /* must equal 1 */
};

struct wm_cmd_create_win {
    int32_t  x, y;             /* window top-left (incl. title bar) */
    int32_t  w, h;             /* window total dimensions */
    char     title[32];        /* NUL-padded title */
};

struct wm_cmd_fill_rect {
    int32_t  wid;              /* window id (returned by EVT_WINDOW_ID) */
    int32_t  x, y, w, h;       /* coords relative to window CONTENT origin */
    uint32_t rgb;              /* 0x00RRGGBB */
};

struct wm_cmd_draw_text {
    int32_t  wid;
    int32_t  x, y;             /* baseline top-left, content-relative */
    uint32_t rgb;
    /* The text payload follows immediately after this struct: the
     * total message length minus sizeof(struct wm_cmd_draw_text) is
     * the text byte count. NOT NUL-terminated on the wire. */
};

struct wm_cmd_draw_pixel {
    int32_t  wid;
    int32_t  x, y;
    uint32_t rgb;
};

struct wm_cmd_destroy {
    int32_t  wid;
};

struct wm_cmd_present {
    int32_t  wid;
};

/* ---- WM → Client events ---------------------------------------- */

#define WM_EVT_WINDOW_ID   1   /* reply to CREATE_WIN: assigned wid       */
#define WM_EVT_MOUSE_BTN   2   /* mouse button transition over the window */
#define WM_EVT_MOUSE_MOVE  3   /* cursor moved while inside the window    */
#define WM_EVT_KEY         4   /* keystroke routed to the focused window  */
#define WM_EVT_CLOSE       5   /* WM tore down the window (X click)       */
#define WM_EVT_ERROR       6   /* protocol error — connection will close  */

struct wm_evt_window_id {
    int32_t  wid;
};

struct wm_evt_mouse_btn {
    int32_t  wid;
    int32_t  lx, ly;           /* local coords inside content area */
    uint32_t btns;             /* current button bitmask (bit 0 = L) */
    int32_t  down;             /* 1 = press edge, 0 = release edge */
};

struct wm_evt_mouse_move {
    int32_t  wid;
    int32_t  lx, ly;
    uint32_t btns;
};

struct wm_evt_key {
    int32_t  wid;
    int32_t  keycode;          /* ASCII byte from sys_kbd_poll */
};

struct wm_evt_close {
    int32_t  wid;
};

struct wm_evt_error {
    int32_t  code;
};

/* ---- Sizing constants ------------------------------------------ */

/* Max payload bytes the WM will accept in a single command — bounds
 * the per-client read buffer and lets the WM cap memory per client.
 * Generous enough for a 64-char string in DRAW_TEXT, modest enough
 * that a malicious client can't ask the WM to allocate a 4 MiB blob. */
#define WM_MAX_PAYLOAD     512

/* Per-client window cap. Most demos use 1 window per client; a few
 * (e.g. a multi-pane editor) could want more. 4 is plenty. */
#define WM_MAX_WINS_PER_CLIENT  4

#endif
