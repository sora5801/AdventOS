# Session 62 — Out-of-process apps over IPC

**Goal:** turn the WM from "a single process with statically-linked callbacks for every app" into "a server that unrelated user processes can connect to, ask for a window, and paint into." Same architecture every real graphical OS uses (X11, Wayland, the macOS WindowServer, Windows GDI).

Three pieces ship in this session:

- **A "draw protocol"** (`include/wm_proto.h`): length-prefixed binary messages over a TCP loopback socket. Six client→WM commands (HELLO, CREATE_WIN, FILL_RECT, DRAW_TEXT, DRAW_PIXEL, DESTROY, PRESENT) and four WM→client events (WINDOW_ID, MOUSE_BTN, KEY, CLOSE).

- **The WM as an IPC server** (`user/gui.c`): listens on `127.0.0.1:7000`, accepts clients non-blockingly, slices messages out of a per-client recv buffer, rasterizes drawing commands into a per-window pixmap, dispatches mouse and key events back to the owning client's fd.

- **A sample client** (`user/gclient.c`): connects, paints a three-band scene with a centered text label, then loops on events from the WM. Demonstrates the full round-trip — and is what `[t45]` drives end-to-end.

The kernel addition needed to make this work without freezing the WM: **`SYS_FD_NB`** — a single new syscall (#80) that flips an `O_NONBLOCK`-style flag on any fd. `sys_accept` and `sys_read` on a non-blocking socket fd return `-1` immediately when there's nothing to do, instead of looping `task_yield` until a peer shows up. The WM runs a 60-fps event loop and can't afford to stall on any single client.

`[t45]` selftest, 6/6 PASS:

```
[t45] out-of-process apps over IPC: WM <-> gclient.elf
  PASS  WM accepted the client connection (HELLO handshake ok)
  PASS  WM allocated a window for the client (CREATE_WIN dispatch)
  PASS  client connected to WM on 127.0.0.1:7000
  PASS  client received WM_EVT_WINDOW_ID back from CREATE_WIN
  PASS  client sent FILL_RECT + DRAW_TEXT + PRESENT
  PASS  client received WM_EVT_MOUSE_BTN after scripted click
```

Selftest total: **140 PASS, 0 FAIL.**

---

## 1. The protocol

```
client → WM:                 WM → client:
+--------+--------+          +--------+--------+
| kind   | rsvd0  |          | kind   | rsvd0  |
+--------+--------+          +--------+--------+
| length (uint16) |          | length (uint16) |
+--------+--------+          +--------+--------+
| ... payload ... |          | ... payload ... |
```

Every message starts with the same 4-byte header (`struct wm_hdr`). `kind` is the command (or event); `length` is the byte count of the payload that follows. `kind` distinguishes message types: `WM_CMD_*` for client→WM, `WM_EVT_*` for WM→client.

### Why length-prefixed and not netstring / line-delimited

- Binary geometries (rects, RGB triples) trivial to encode without escaping.
- The WM can drain ONE complete message at a time off each client fd without inspecting payload content for delimiters.
- A `length > WM_MAX_PAYLOAD` short-circuits to "this client is misbehaving" before any allocation.

### Why TCP loopback and not a custom socket family

- `kernel/ip.c::try_loopback` already short-circuits packets destined for `127.0.0.0/8` or the local IP directly into the protocol's rx handler — zero NIC trips.
- The `sys_socket` / `sys_connect` / `sys_accept` surface is already widely used in this codebase (sshd, httpsd, httpsget).
- Clients can come and go independently of WM lifetime — same as on any modern Unix.

### Commands (client → WM)

| #   | Name           | Purpose                                                          |
|-----|----------------|------------------------------------------------------------------|
| 1   | `HELLO`        | Magic + version handshake. Must come first.                      |
| 2   | `CREATE_WIN`   | Spawn a window. WM replies with `WINDOW_ID`.                     |
| 3   | `FILL_RECT`    | Solid rect into a window's pixmap (content-local coords).        |
| 4   | `DRAW_TEXT`    | Render an 8x8 glyph string. Text bytes follow the struct.        |
| 5   | `DRAW_PIXEL`   | Single pixel (cheaper than a 1×1 fill rect).                     |
| 6   | `DESTROY`      | Close one of your windows.                                       |
| 7   | `PRESENT`      | Advisory "commit" — WM composites every frame anyway, so this is mostly a synchronization marker. |

### Events (WM → client)

| #   | Name           | Purpose                                                          |
|-----|----------------|------------------------------------------------------------------|
| 1   | `WINDOW_ID`    | Reply to `CREATE_WIN`: assigned wid (or -1 on failure).          |
| 2   | `MOUSE_BTN`    | Button transition over your window. lx/ly are content-local.     |
| 3   | `MOUSE_MOVE`   | Cursor moved while inside your window. (Not yet emitted by the WM — placeholder for a future drag-dispatch.) |
| 4   | `KEY`          | Keystroke routed to the focused window.                          |
| 5   | `CLOSE`        | User clicked the X. WM will tear the window down regardless.     |
| 6   | `ERROR`        | Protocol violation. Connection will be closed.                    |

### Sizing constants

```c
#define WM_MAX_PAYLOAD           512    /* per-message cap                 */
#define WM_MAX_WINS_PER_CLIENT   4
```

512 bytes is enough for a 64-character text payload plus the struct header, modest enough that a malicious client can't ask the WM to allocate megabytes per message.

---

## 2. The WM server

### Per-client state

```c
#define IPC_MAX_CLIENTS  4

struct ipc_client {
    int      in_use;
    int      fd;
    int      hello_ok;
    uint8_t  recv[sizeof(struct wm_hdr) + WM_MAX_PAYLOAD];
    int      recv_off;
};
```

`recv` is the per-client incremental parser buffer. TCP is a byte stream — a single `sys_read` can return half a message, or two and a half. The WM appends whatever arrives to `recv` and then slices out as many *complete* messages as fit. Leftover bytes shift down for the next frame.

### Pixmap pool

```c
#define MAX_CLIENT_WINS  4
#define WIN_PIX_W      300
#define WIN_PIX_H      200

static uint32_t g_client_pix[MAX_CLIENT_WINS][WIN_PIX_H * WIN_PIX_W];
static int      g_client_pix_used[MAX_CLIENT_WINS];
```

Each client window slots into one of four 32-bpp pixmaps, capped at 300×200. Total: 960 KiB — fits comfortably in the WM's USER_HEAP budget. Why a static pool instead of `malloc`-per-window? `malloc` works but locks the WM into "I might allocate at any time"; a static pool with explicit `alloc_pix_slot` / `free_pix_slot` keeps the worst-case memory footprint visible and pinned at boot.

### Per-window callbacks

`struct window` already has `draw / click / key` function pointers. For client windows the WM installs *its own* trampolines (`client_win_draw`, `client_win_click`, `client_win_key`) that:

- `client_win_draw(w, frame)` — blits the pixmap to the screen at the window's content origin.
- `client_win_click(w, lx, ly, btns)` — serializes a `WM_EVT_MOUSE_BTN` and `sys_write`s it to the client's fd.
- `client_win_key(w, key)` — same but for `WM_EVT_KEY`.

The trampoline pattern keeps the rest of the WM (compositing, focus handling, drag logic, z-order) blissfully unaware of the distinction between built-in and out-of-process windows. The same hit-test, the same `win_raise` on click, the same `desktop_draw` paints them all.

### Non-blocking dispatch

The WM's event loop, after this session, looks like:

```c
for (;;) {
    /* 1. Mouse / keyboard state (existing). */
    /* 2. IPC pump: */
    ipc_accept_new_clients();           /* non-blocking accept */
    for each client: pump_client(...);  /* non-blocking read + dispatch */
    /* 3. Handle mouse + keyboard (existing). */
    /* 4. Render windows (existing + pixmap blit). */
    sys_sleep_ms(16);
}
```

The non-blocking variants are what make the loop sound. `sys_accept` blocking forever was tolerable in sshd (one task per accept-loop, dedicated to that), but the WM can't yield its only thread on the chance someone *might* connect. Without the per-fd nonblock flag we'd have to:

- Spawn a separate "accept helper" subprocess and shovel events through a pipe, OR
- Use `select`-style multiplexing (which doesn't exist in AdventOS), OR
- Live with `sys_accept` blocking until something happens — i.e., one rendering frame every several seconds, only when a client connects.

The single-syscall addition was the cheapest fix.

### Kernel changes

Three small kernel edits, all motivated by the WM's non-blocking I/O need:

1. **`struct task_fd` gains `uint32_t flags`** (`kernel/task.h`) — bit 0 = `FD_FL_NONBLOCK`. Carried by fork (struct copy), zeroed by `release_fd` and the existing `t->fds[conn_fd].flags = 0` in `SYS_ACCEPT`.

2. **`sock_accept_avail(idx)` / `sock_read_avail(idx)`** (`kernel/sock.{c,h}`) — peek helpers that return `1` if accept would succeed / `1` if read has data or EOF. `0` means "would block." Pure reads — no state change.

3. **`SYS_FD_NB(fd, on)` syscall #80** (`kernel/syscall.{h,c}`) — toggles the flag bit. The `SYS_ACCEPT` and `SYS_READ` (for `FD_SOCK` kind) handlers check the flag and call the peek helper first; if it returns `0`, the syscall returns `-1` without ever calling the blocking `sock_accept` / `sock_read`.

The blocking-paths-in-sock.c are untouched. Sockets that don't opt in still see the old blocking behavior. Backwards-compatible.

---

## 3. The client

`user/gclient.c` is ~250 lines of "talk to the WM." It demonstrates:

```c
int main(int argc, char **argv) {
    g_fd = sys_socket();
    sys_connect(g_fd, {127,0,0,1}, WM_PORT);

    gc_hello();                                 /* HELLO + version */
    int wid = gc_create_win(720, 480, 220, 140, "OOP Client");

    /* Three-band scene + label. */
    gc_fill_rect(wid, 0, 0,   220, 40,  0xE03030);   /* red    */
    gc_fill_rect(wid, 0, 40,  220, 60,  0x103060);   /* blue   */
    gc_fill_rect(wid, 0, 100, 220, 40,  0x208030);   /* green  */
    gc_draw_text(wid, ..., "OOP CLIENT", 10, 0xFFFFFF);
    gc_present(wid);

    while (...) {
        recv_msg(&kind, payload, ...);
        switch (kind) {
            case WM_EVT_MOUSE_BTN:  /* printf "got CLICK at (lx,ly)" */
            case WM_EVT_KEY:        /* printf "got KEY ..." */
            case WM_EVT_CLOSE:      /* shutdown */
        }
    }
}
```

The client has **zero direct framebuffer access**. Every pixel goes through `sys_write(fd, ...)` — that's the whole point of out-of-process apps. An unprivileged process can't crash the WM by writing past pixmap bounds; it can only ask politely with `FILL_RECT(wid, x, y, w, h)`, and the WM clips against the window's content area before rasterizing. Same isolation model real Wayland uses.

---

## 4. The selftest choreography

```
[t45]
  fork (gui.elf selftest) → pipe-capture stdout      ← WM
  sleep 400 ms                                       ← let WM bind port 7000
  fork (gclient.elf selftest) → pipe-capture stdout   ← Client

  drain both pipes round-robin until EOF
    (mustn't starve one — would deadlock the other)

  wait() both children

  grep WM stdout:
    - "client fd=..."                  (HELLO handshake)
    - "created client window wid=..."  (CREATE_WIN dispatch)

  grep client stdout:
    - "connected to WM"                (sys_connect succeeded)
    - "window N created"               (received WINDOW_ID)
    - "scene painted"                  (sent the draw commands)
    - "got CLICK ..."                  (received MOUSE_BTN — the key witness!)
```

The "got CLICK" assertion is the proof the WM's scripted click at `(830, 559)` reached the client process. That round trip exercises every link in the chain:

```
WM script_apply → sys_mouse_inject (kernel)
   → mouse state updated
   → next WM frame: sys_mouse_state returns (830, 559, 1)
   → handle_mouse → down_edge → win_hit_test → client window
   → client_win_click → wm_send_evt → sys_write to fd
   → kernel routes via try_loopback → tcp rx callback
   → client's sock rx ring
   → client's blocking sys_read returns the bytes
   → client parses header + payload, prints "got CLICK ..."
```

If anything in that chain breaks — message framing, fd routing, hit-testing, loopback short-circuit, *anything* — the test catches it.

---

## 5. The held-click problem (already solved by session 61's `wants_drag`)

Session 61 introduced a `wants_drag` flag because the WM was firing click handlers every frame the button was held. That fix carries over to client windows: they leave `wants_drag = 0`, so a single mouse click yields exactly one `WM_EVT_MOUSE_BTN` event, not 10-or-however-many-frames'-worth. Good UX (and good for the test — only one witness line, no spam).

If a future client wants drag-to-paint over its window, it can request it via a future `WM_CMD_SET_DRAG` command and the WM would flip the bit. Out of scope today.

---

## 6. Threats this protocol design closes

| Threat                                | Mitigation                                                                 |
|----------------------------------------|----------------------------------------------------------------------------|
| Client allocates huge pixmap          | WM caps at `WIN_PIX_W × WIN_PIX_H`; client request ignored beyond limit.   |
| Client floods commands                | Per-client recv buf is `WM_MAX_PAYLOAD + sizeof(hdr)` — a malformed message larger than that is treated as a protocol violation and the client is dropped. |
| Client claims another's window        | Every command carries a `wid`; WM cross-checks `w->client_fd == cli->fd` before rasterizing. |
| Slow / dead client stalls the WM     | All reads/writes non-blocking — partial writes fail fast and the client gets torn down on the next pump. |
| Client misuses sequence (commands before HELLO) | First non-HELLO command flags `hello_ok == 0` and the client is torn down. |

None of these are robust against a determined attacker — the kernel doesn't isolate user memory between WM and clients at the system-call surface — but they cover the "buggy app" case which is the realistic threat for a hobby OS.

---

## 7. Touched files

- `include/wm_proto.h` — new. Shared message structs + constants.
- `kernel/task.h` — `struct task_fd.flags`; `FD_FL_NONBLOCK`.
- `kernel/sock.{h,c}` — `sock_accept_avail`, `sock_read_avail`.
- `kernel/syscall.{h,c}` — `SYS_FD_NB` (80); nonblock check in `SYS_ACCEPT` + `SYS_READ`; `release_fd` zeros the flags.
- `user/libuser.{c,h}` — `sys_fd_nb` wrapper + syscall number.
- `user/gui.c` — IPC server, client table, pixmap pool, draw protocol dispatch, render integration, client-window trampolines, WM script extension for client-click coverage.
- `user/gclient.c` — new. Example client app.
- `build.sh`, `mkfs.py` — ship `gclient.elf`.
- `user/sh.c` — `[t45]` selftest.

## 8. Out of scope (deferred)

- **Generic `BLIT` command** — would carry `w*h*4` bytes of pixel data per call. Current `SOCK_RX_BUF = 4096` would constrain that to ~32×32 max, and the round-trip per frame would saturate the socket. A real solution is shared memory (we don't have it) or chunked compressed blits. Out of scope.

- **Shared memory between WM and client** — would dramatically simplify high-bandwidth scenarios (video, big bitmaps). Needs a syscall family like Linux `mmap` with `MAP_SHARED` semantics on an anonymous backing fd, which our kernel doesn't have.

- **Hot client crash recovery** — when a client dies, the WM tears down its windows but doesn't notify any other clients. Multi-client coordination (focus stealing, accessibility events) is the next layer.

- **A real `WM_EVT_MOUSE_MOVE`** — currently only emitted on click edges. Hover would need the WM to track "the cursor entered/left this window" transitions and emit move events at some rate-limited cadence (every Nth frame), which we haven't wired up.

- **Server-side fonts** — currently every glyph in `DRAW_TEXT` is rasterized from the kernel's `font8x8` table by walking each bit. Bigger fonts, antialiasing, kerning are all open work.

Most realistic next session: shared memory for the BLIT path, so clients can ship bigger pixmaps without saturating the TCP loopback ring.
