# Session 13 — Sockets API and a userspace HTTP server

**Goal:** Replace the kernel-resident `http.c` with a real userspace HTTP server. Add `SYS_SOCKET / SYS_BIND / SYS_LISTEN / SYS_ACCEPT`. Make socket fds flow through the per-process fd table that landed in session 12, so `sys_read` / `sys_write` / `sys_close` Just Work on a TCP connection. Verify `curl http://localhost:8080/` still returns 200 OK — only now it's served by a ring-3 program.

End state of the milestone:

```
[boot] launched httpd.elf as pid 4
httpd: listening on port 80 (userspace)
[boot] launched sh.elf as pid 5
```

```
$ curl -sv http://localhost:8080/
> GET / HTTP/1.1
> Host: localhost:8080
< HTTP/1.0 200 OK
< Content-Type: text/plain
< Connection: close
<
Hello from a USERSPACE HTTP server!

This page was served by user/httpd.c, which runs in ring 3.
It calls socket()/bind()/listen()/accept() through INT 0x80,
and read/write/close on the returned fd flow through the same
task_fd[] table that backs file I/O — TCP sockets are just
another fd kind on the kernel side.
```

The wire behavior is identical to session 11's kernel HTTP server. What changed is who's holding the pen: the response now leaves the kernel through `sock_write`, gets handed to TCP, and goes out the NIC — but the program that called `write()` was running on a user-mode page table, with CS=0x1B.

## What's in scope

In:
- `SYS_SOCKET`, `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT` — four new syscalls, ABI-numbered 14–17
- `kernel/sock.{c,h}` — a thin sockets layer over the existing single-TCB TCP impl
- `FD_SOCK` kind in the per-process fd table; `sock_idx` field
- `SYS_READ` / `SYS_WRITE_FD` / `SYS_CLOSE` updated to dispatch on `FD_SOCK`
- `tcp_connect_cb` callback — fires when SYN_RCVD → ESTABLISHED
- `tcp_send_active` / `tcp_close_active` — let the sock layer talk to the active TCB without holding a `struct tcb *`
- `user/httpd.c` — userspace HTTP server using the new API
- `kernel/http.{c,h}` — **deleted**

Out:
- `socket(AF_INET, SOCK_STREAM, 0)` — no address family, type, or protocol parameters; `sys_socket()` takes nothing
- `struct sockaddr_in` — port is just an `int`, no IP because we only have one
- Outbound `connect()` — server-side only
- Multiple concurrent connections — single TCB still
- Blocking with proper wait queues (we still spin on `task_yield`)
- `select` / `poll` / `epoll`
- `SO_REUSEADDR`, `TCP_NODELAY`, any `setsockopt`
- `accept` returning peer address
- `shutdown(fd, SHUT_*)` — close is hard-close

## Architecture: bridging two execution contexts

This was the architectural core of the session. The two halves of a socket call live in completely different worlds:

| | TCP layer (since session 11) | User syscall (since session 5) |
|---|---|---|
| Trigger | RTL8139 IRQ → `tcp_rx` | `INT 0x80` from ring 3 |
| Context | IRQ — IF=0 on entry | Trap gate — re-enables IF |
| Stack | IRQ stack of whoever was running | Per-task kernel stack |
| Cannot do | Block, sleep, call schedule | Take long IRQ-disabled critical sections |
| Has access to | The TCB, packet bytes | The calling task's fd table |

A user calling `accept()` needs to *block* until a peer arrives. The arrival is detected in IRQ context as a SYN-then-ACK. The bridge has to be:

1. **Lock-free or near-lock-free**, because the IRQ side can't wait on a mutex held by the user side (would deadlock — a user task holding a mutex with IF=1 gets preempted, the IRQ fires on the new task, tries to take the lock, spins forever in IRQ context with IF=0).
2. **Async**, because the IRQ delivers data when it pleases, not when the user is in a syscall. Bytes have to land somewhere even if no user task is currently reading.
3. **Cheap to poll**, because the user side spins on `task_yield()` waiting for state to change. Every yield round-trips through the scheduler.

The answer is shared `volatile` state in `struct sock`: a few flags the IRQ side flips, plus an SPSC ring buffer for inbound data. The user side reads the flags non-atomically, yields if it doesn't like what it sees, tries again.

```
              IRQ context                       Syscall context
            (TCP rx callback)                  (user calling accept/read)
                  │                                     │
                  ▼                                     ▼
            ┌──────────┐                        ┌──────────┐
            │ on_connect│ ──flip pending_conn──▶│sock_accept│ ─yields until set
            │ on_recv   │ ──push rx_buf────────▶│sock_read  │ ─yields until ring nonempty
            │ on_close  │ ──flip peer_closed───▶│sock_read  │ ─returns 0 on drain+closed
            └──────────┘                        └──────────┘
                  │                                     ▲
                  │                                     │
            shared struct sock (volatile fields, SPSC ring)
```

## TCP changes: one new callback, three helpers

Before this session, [`kernel/tcp.h`](../kernel/tcp.h) exposed `on_recv` and `on_close` callbacks but no notion of "a connection just opened" — the old kernel `http.c` set up its tx loop inside `on_recv` based on the request bytes. Userspace `accept()` semantics need to know about *connection establishment*, not request data, so:

```c
typedef void (*tcp_connect_cb)(struct tcb *t);  /* SYN_RCVD -> ESTABLISHED */
typedef void (*tcp_recv_cb)   (struct tcb *t, const char *data, int len);
typedef void (*tcp_close_cb)  (struct tcb *t);

int tcp_listen(uint16_t port,
               tcp_connect_cb on_connect,
               tcp_recv_cb    on_recv,
               tcp_close_cb   on_close);
```

`tcp_listen`'s signature changed (4 args, not 3), but the only existing caller was `kernel/http.c` which we deleted. Net source-tree breakage: zero.

The fire site is exactly where you'd expect — at the SYN_RCVD → ESTABLISHED transition in [`kernel/tcp.c`](../kernel/tcp.c):

```c
case TCP_SYN_RCVD: {
    if (!(flags & TCP_ACK))                 return;
    if (ack != g_tcb.snd_nxt)               return;
    g_tcb.snd_una = ack;
    g_tcb.state   = TCP_ESTABLISHED;
    if (g_tcb.on_connect) g_tcb.on_connect(&g_tcb);
    /* Fall through: this segment may also have data piggybacked. */
}
```

Three helpers are added so the sock layer doesn't need a `struct tcb *`:

```c
int tcp_send_active (const void *data, int len);
int tcp_close_active(void);
int tcp_active_state(void);
```

They all forward to `g_tcb`. Since the underlying impl only supports one TCB anyway, "the active TCB" is unambiguous. When we eventually grow to multi-connection, this is the layer that has to learn about connection IDs — sock can stay shape-compatible.

## struct sock — the bridge data structure

```c
#define SOCK_MAX     8
#define SOCK_RX_BUF  2048

enum { SOCK_FREE = 0, SOCK_NEW, SOCK_LISTEN, SOCK_CONNECTED };

struct sock {
    int               state;
    uint16_t          port;

    /* For LISTEN sockets: connection-socket index that's been
     * created by on_connect but not yet handed out by accept(). */
    int               pending_conn;

    /* For CONNECTED sockets: SPSC ring. Producer = TCP rx callback
     * (IRQ ctx). Consumer = user task in sock_read. head/tail are
     * 32-bit aligned, atomic on x86 — no lock needed. */
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile int      peer_closed;
    uint8_t           rx_buf[SOCK_RX_BUF];
};
```

Fixed table of 8 — no malloc, no fragmentation, indices are stable for the lifetime of a sock. The `volatile` qualifiers are non-negotiable: without them the compiler is free to hoist `s->rx_head` out of the `while` loop in `sock_read` and we'd spin forever even after the IRQ updates head.

Two static globals track the singleton listener and the singleton connection:

```c
static int g_listen_idx = -1;
static int g_conn_idx   = -1;
```

`on_connect` writes `g_conn_idx` so `on_recv`/`on_close` know where to push bytes / set the closed flag. There's no per-TCB pointer back to a sock because the TCB is reused across connections — see *back_to_listen* below.

## SPSC ring without a lock

The receive ring is the most safety-critical bit:

```c
/* Producer (IRQ context): */
static void on_recv(struct tcb *t, const char *data, int len) {
    if (g_conn_idx < 0) return;
    struct sock *s = &g_socks[g_conn_idx];
    for (int i = 0; i < len; i++) {
        uint32_t next = (s->rx_head + 1) % SOCK_RX_BUF;
        if (next == s->rx_tail) break;          /* full → drop */
        s->rx_buf[s->rx_head] = (uint8_t)data[i];
        s->rx_head = next;
    }
}

/* Consumer (syscall context): */
int sock_read(int idx, void *buf, int n) {
    ...
    while (s->rx_head == s->rx_tail && !s->peer_closed) task_yield();
    int copied = 0;
    char *out = buf;
    while (copied < n && s->rx_tail != s->rx_head) {
        out[copied++] = (char)s->rx_buf[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % SOCK_RX_BUF;
    }
    return copied;     /* 0 = EOF (peer closed and ring drained) */
}
```

Why this is lock-free safe on x86:

- **`rx_head` is only ever written by the producer**, only ever read by both. Producer's write is a single 32-bit aligned store → atomic on x86. Consumer's read is a single 32-bit aligned load → atomic on x86.
- **`rx_tail` is only ever written by the consumer**, only ever read by both. Same argument.
- **The `rx_buf[]` write-by-producer / read-by-consumer race is ordered by the head update**: producer writes the byte first, *then* advances head; consumer checks head first, *then* reads the byte. As long as head's update is observed-after the byte's update on the consumer side, we're fine.

That last point needs a fence on architectures with weak ordering. On x86 with WB memory, store-store ordering is preserved, so the producer's `rx_buf[head] = byte; rx_head = next;` becomes visible to the consumer in that order without an explicit `sfence`. We rely on this.

The "drop on overflow" semantic is the cheapest possible. Real TCP would close the receive window, but our `tcp_send_segment` always announces `TCP_RX_BUF` regardless of how much has been consumed — the ring fills, on_recv silently drops, the application sees a truncated request. For our `curl` workload that's fine: one HTTP request line is ~80 bytes against a 2 KiB ring.

## Blocking via task_yield

`sock_accept` and `sock_read` both block. We don't have a real wait-queue/wake-up primitive yet (sessions 5 and 6 added mutexes with wait queues, but only for kernel sync — there's no `task_wait_for(&condition)` interface to plug into). So we do the dumb thing:

```c
while (g_socks[idx].pending_conn < 0) task_yield();
```

`task_yield()` calls `schedule()` which picks the next runnable task in the round-robin list. That task runs for one PIT tick (10 ms), preemption fires, scheduler comes back, we get a turn, check the flag, yield again if needed.

It's a busy-wait with a giant time quantum. Bad for power, awful for tail latency, but correct: the IRQ that flips `pending_conn` runs *between* schedule cycles (because IF=1 outside the cli sections in `schedule()`), so we always observe the flip on the next iteration.

What this skips that a real OS would do:
- A `wait_queue_head_t` per sock, plus `task_state = TASK_STATE_BLOCKED` while waiting, plus `wake_up(&sock->wq)` from on_connect.
- `schedule()` would skip BLOCKED tasks (which it already does, for mutex waiters), so a blocked accept consumes zero CPU until the wake-up.
- The wake-up call from IRQ context to ready a task is one of the rare things you can safely do in IRQ on x86 — it just flips a state field and re-splices into the ready ring. Both updates are O(1) under cli.

That's the natural follow-up. For now, the busy-yield is good enough that `curl` finishes in 30 ms.

## back_to_listen — single-TCB connection reuse

Because there's only one TCB, after each connection closes we have to put it back into LISTEN with the same callbacks armed. [`kernel/tcp.c`](../kernel/tcp.c):

```c
static void back_to_listen(struct tcb *t) {
    tcp_connect_cb ncb = t->on_connect;
    tcp_recv_cb    rcb = t->on_recv;
    tcp_close_cb   ccb = t->on_close;
    uint16_t       port = g_listen_port;
    if (ccb) ccb(t);
    if (g_listening) tcp_listen(port, ncb, rcb, ccb);
    else             t->state = TCP_CLOSED;
}
```

The `if (ccb) ccb(t)` runs **before** the listener is re-armed. That's the cue for the sock layer to flip its `peer_closed` and let the user-side `sock_read` finish draining the ring and return 0 (EOF).

`tcp_listen` then reinitializes the TCB completely (`memset(&g_tcb, 0, sizeof(g_tcb))`) and sets it back to LISTEN. The next SYN is treated as a fresh connection — and `on_connect` allocates a *fresh sock slot* for it, so the user's previous `conn_fd` keeps working until they close it. Three concurrent open conn fds for the same port would be possible if not for the single-TCB constraint dropping their data.

## fd table integration

[`kernel/task.h`](../kernel/task.h) gets one new kind and one new field:

```c
enum { FD_FREE = 0, FD_STDIN, FD_STDOUT, FD_FS, FD_SOCK };

struct task_fd {
    int      kind;
    int      fs_idx;       /* iff kind == FD_FS    */
    uint32_t offset;       /* iff kind == FD_FS    */
    int      sock_idx;     /* iff kind == FD_SOCK  */
};
```

`sock_idx` defaults to -1 in `task_create`. `SYS_SOCKET` allocates the lowest-free fd ≥ 3 the same way `SYS_OPEN` does, then stamps `kind = FD_SOCK; sock_idx = idx`.

`SYS_READ`, `SYS_WRITE_FD`, `SYS_CLOSE` already had a `kind` switch from session 12 — adding the FD_SOCK arm is one extra case each:

```c
case SYS_READ: {
    ...
    if      (kind == FD_STDIN) ret = kshell_read_line(buf, n);
    else if (kind == FD_FS)    { ... }
    else if (kind == FD_SOCK)  ret = sock_read(t->fds[fd].sock_idx, buf, n);
    else                       ret = -1;
}
case SYS_WRITE_FD: {
    if      (kind == FD_STDOUT) { for (...) kputc(buf[i]); ret = n; }
    else if (kind == FD_SOCK)   ret = sock_write(t->fds[fd].sock_idx, buf, n);
    else                        ret = -1;
}
case SYS_CLOSE: {
    if (t->fds[fd].kind == FD_SOCK) sock_close(t->fds[fd].sock_idx);
    t->fds[fd].kind     = FD_FREE;
    t->fds[fd].sock_idx = -1;
    ret = 0;
}
```

This is the payoff of session 12. The `kind`-tagged fd entry was always going to be the place where new resource types land. Session 13 just slots one in.

## The four new syscalls

```
SYS_SOCKET = 14   eax=14                               -> fd or -1
SYS_BIND   = 15   eax=15  ebx=fd  ecx=port             -> 0 or -1
SYS_LISTEN = 16   eax=16  ebx=fd  ecx=backlog (ignored)-> 0 or -1
SYS_ACCEPT = 17   eax=17  ebx=fd                       -> conn_fd or -1
```

`SYS_SOCKET` is the only one that actually mutates the fd table. `SYS_BIND` and `SYS_LISTEN` mutate the underlying sock state but don't touch the fd table. `SYS_ACCEPT` allocates a *second* fd in the calling task's table for the new connection.

```c
case SYS_ACCEPT: {
    int fd = (int)a;
    struct task *t = task_current();
    if (fd < 0 || fd >= TASK_MAX_FDS)         { ret = -1; break; }
    if (t->fds[fd].kind != FD_SOCK)           { ret = -1; break; }

    int conn_sock = sock_accept(t->fds[fd].sock_idx);    /* blocks */
    if (conn_sock < 0) { ret = -1; break; }

    int conn_fd;
    for (conn_fd = 3; conn_fd < TASK_MAX_FDS; conn_fd++) {
        if (t->fds[conn_fd].kind == FD_FREE) break;
    }
    if (conn_fd == TASK_MAX_FDS) { sock_close(conn_sock); ret = -1; break; }

    t->fds[conn_fd].kind     = FD_SOCK;
    t->fds[conn_fd].sock_idx = conn_sock;
    ret = conn_fd;
}
```

The `sock_accept` call is what blocks, via the `task_yield` loop in [`kernel/sock.c`](../kernel/sock.c). The fd-allocation logic that follows is straight cut-and-paste from `SYS_SOCKET`/`SYS_OPEN`.

## libuser wrappers

```c
int sys_socket(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret) : "a"(SYS_SOCKET) : "memory");
    return ret;
}

int sys_bind(int fd, int port) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret) : "a"(SYS_BIND), "b"(fd), "c"(port) : "memory");
    return ret;
}
```

`sys_listen` and `sys_accept` are the same shape. Nothing surprising — same INT 0x80 ABI, same EAX-numbered dispatch, same pattern as every other libuser wrapper since session 9.

## httpd.c — the whole program

```c
#include "libuser.h"

static const char *g_response =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from a USERSPACE HTTP server!\n"
    ...;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int s = sys_socket();
    if (s < 0)                  { puts("httpd: socket() failed\n"); return 1; }
    if (sys_bind(s, 80) < 0)    { puts("httpd: bind(80) failed\n"); return 1; }
    if (sys_listen(s, 1) < 0)   { puts("httpd: listen() failed\n"); return 1; }

    puts("httpd: listening on port 80 (userspace)\n");

    for (;;) {
        int conn = sys_accept(s);
        if (conn < 0) continue;

        char buf[1024];
        int  n = sys_read(conn, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            sys_write(1, "httpd: ", 7);
            int i;
            for (i = 0; i < n && buf[i] != '\r' && buf[i] != '\n'; i++) {}
            sys_write(1, buf, i);
            sys_write(1, "\n", 1);
        }
        sys_write(conn, g_response, (int)strlen(g_response));
        sys_close(conn);
    }
}
```

Compare to the kernel's old `http.c`. The dispatch was scattered across `on_recv` callbacks; here it's a flat `accept → read → write → close` loop in linear control flow. *That's* what userspace networking buys you — the I/O loop reads top-to-bottom, blocking calls block the program (not the OS), and a crash in httpd doesn't take the kernel down.

The `kmain` LAUNCH macro spawns it before the shell, so by the time you can type `httpd` is already serving:

```c
#define LAUNCH(path, name) do {                                        \
    int _fd = fs_open(path);                                           \
    if (_fd >= 0) {                                                    \
        struct elf_load_result _r;                                     \
        if (elf_load(_fd, &_r) == 0) {                                 \
            const char *_argv[] = { name };                            \
            elf_setup_args(&_r, 1, _argv);                             \
            struct task *_t = task_create_user(_r.entry, _r.user_esp,  \
                                               _r.cr3, name);          \
            if (_t) kprintf("[boot] launched %s as pid %u\n",          \
                            path, (unsigned)_t->id);                   \
        }                                                              \
    }                                                                  \
} while (0)

LAUNCH("httpd.elf", "httpd");
LAUNCH("sh.elf",    "sh");
```

Two-call use of a macro is on the edge of pulling it out into a real function, but pulling it out costs three new symbols and the function would need its own header — for a boot-time helper called twice, the macro wins.

## Files added / modified

| File | Change |
|---|---|
| `kernel/tcp.{h,c}` | `tcp_connect_cb` typedef, fire on SYN_RCVD→ESTABLISHED, `tcp_send/close/active_state` helpers, `tcp_listen` 4-arg signature, `back_to_listen` preserves `on_connect` |
| `kernel/sock.{h,c}` | New. Sockets layer with SPSC ring, blocking accept/read, FD_SOCK glue |
| `kernel/task.{h,c}` | `FD_SOCK` enum, `sock_idx` field, init to -1 in `task_create` |
| `kernel/syscall.{h,c}` | `SYS_SOCKET/BIND/LISTEN/ACCEPT` (14–17), FD_SOCK arms in SYS_READ/WRITE_FD/CLOSE |
| `kernel/kernel.c` | `#include "sock.h"`, `sock_init()`, `LAUNCH` macro, spawn `httpd.elf` |
| `kernel/http.{c,h}` | **Deleted.** No more in-kernel HTTP server |
| `user/libuser.{h,c}` | `sys_socket/bind/listen/accept` wrappers, syscall constants |
| `user/httpd.c` | New. Userspace HTTP/1.0 server |
| `mkfs.py` | `httpd.elf` in `USER_PROGRAMS` |
| `build.sh` | `httpd` in `USER_PROGS` |

## Test trace

Boot:

```
[boot] starting network stack
rtl8139: PCI 0:3.0  io=0xc000  irq=11
net: link up — MAC 52:54:00:12:34:56  IP 10.0.2.15  GW 10.0.2.2
[boot] spawning reaper + demo tasks A, B
[boot] launched httpd.elf as pid 4
httpd: listening on port 80 (userspace)
[boot] launched sh.elf as pid 5
```

Three `curl` requests:

```
$ curl -s http://localhost:8080/ -o /dev/null -w "req1 status=%{http_code} bytes=%{size_download}\n"
req1 status=200 bytes=317
$ curl -s http://localhost:8080/ -o /dev/null -w "req2 status=%{http_code} bytes=%{size_download}\n"
req2 status=200 bytes=317
$ curl -s http://localhost:8080/ -o /dev/null -w "req3 status=%{http_code} bytes=%{size_download}\n"
req3 status=200 bytes=317
```

Serial log shows the request line printed by httpd's `puts` after each one:

```
...[A][B][A][A]httpd: GET / HTTP/1.1
[B][A][B][A][A]...httpd: GET / HTTP/1.1
[A][A][B]httpd: GET / HTTP/1.1
```

Three connections, three accepts, three reads, three writes, three closes. `back_to_listen` re-armed the listener cleanly between each. The `[A]/[B]` interleavings prove the demo tasks kept getting scheduled while the user-mode httpd was handling requests — multitasking is alive across the syscall boundary.

## Design decisions

**Single global TCB, even at the sockets layer.** The TCP impl from session 11 has one TCB; sock keeps it. The alternative would be teaching TCP about a per-listener / per-connection TCB hash and making sock manage them. Big lift for our single-curl workload, deferred.

**`int port` instead of `struct sockaddr_in`.** Sockaddr is the layer that carries `AF_INET` + IPv4 + port for TCP; we have one local IP, one address family, one protocol. The cast-and-bind dance buys nothing here. A future multi-IP setup would have to replace the int with a real address — but the rest of the API stays.

**`backlog` ignored.** `sys_listen(s, 1)` — the 1 is dead weight. Real backlog matters when accepts can fall behind connection arrivals; we have one TCB so the second SYN during a half-handshake is silently dropped at the TCP layer regardless. Keeping the parameter for shape compatibility with POSIX.

**SPSC ring, not a kfifo.** A real kernel ring buffer would have memory barriers, `READ_ONCE`/`WRITE_ONCE` semantics, and probably a `wake_up` on the consumer side. We get away with `volatile uint32_t` because x86's TSO ordering does the heavy lifting and our consumer is busy-yielding.

**Blocking accept/read by spinning on `task_yield`.** Documented above. Real wait queues are the next obvious upgrade.

**`sock_close` on a LISTEN socket doesn't actually close the TCP listener.** `back_to_listen` keeps re-arming `g_tcb` after each connection. Closing the listener-fd in user just nulls `g_listen_idx` so future on_connects are dropped — the TCP state machine still goes through the motions. A real `close` of the listening socket would need a `tcp_unlisten()` call.

**`SYS_SOCKET` doesn't take type/protocol arguments.** No `SOCK_DGRAM`, no `SOCK_RAW`, no UDP — TCP is the only thing we have. When UDP shows up we can grow `SYS_SOCKET(int type)` without renumbering.

**`struct sock` lives in BSS, not on the heap.** 8 sockets × ~2 KiB ≈ 16 KiB of BSS, statically allocated at boot. No fragmentation, no allocator churn, indices are stable. The downside is the per-process socket cap is global — a user task that loved sockets could starve httpd of slots. For two ring-3 programs, this isn't a problem.

**`on_connect` is fired in IRQ context, but allocates a sock slot.** Doing slot allocation under IF=0 isn't great, but the slot table is a fixed array with a tiny linear scan; the scan is bounded by `SOCK_MAX = 8`, so the worst-case time-in-IRQ is dominated by the memset, not the scan. Acceptable.

## Pitfalls

1. **`tcp_listen` signature change is silently ABI-breaking** for any out-of-tree user. Inside the tree we deleted the only caller (kernel `http.c`); a real project would have to bump a major version.
2. **The `volatile` on `rx_head`/`rx_tail`/`peer_closed` is load-bearing.** Drop it and the compiler can hoist the loop check, freezing the user task in a yield-storm even after the IRQ has flipped the flag. There is no warning that reproduces this; the code just hangs.
3. **`on_connect` runs in IRQ context.** It must not call `task_yield`, take a sleeping mutex, or otherwise block. Our impl just memsets a struct + sets two ints, which is fine.
4. **The TCB is reused across connections.** That means `g_conn_idx` only points to the *current* connection. If a user holds a connection fd after `back_to_listen` has re-armed for the next client, on_recv for the new connection will route into the old socket's ring. We rely on httpd closing its conn fd before the next SYN — true for HTTP/1.0, but a longer-lived connection (websocket-style) would need per-connection sock binding inside the TCB.
5. **The receive ring is per-`g_conn_idx`, not per-listener.** If we ever support multiple listeners, the `g_conn_idx` global has to become a per-listener field, and `on_recv` needs to know which listener it belongs to (which the TCB already knows).
6. **Stale `kernel/http.o` was left behind** after deleting `kernel/http.{c,h}`. The build script globs `kernel/*.c` for sources, but `*.o` from previous builds linger. Had to `rm kernel/http.o`. A `make clean` equivalent would help; for now, deleting the .o by hand was a one-shot.
7. **First request after boot has higher latency** than subsequent ones because the demo tasks A/B are gobbling CPU between the SYN arrival and httpd waking up. The `task_yield` busy-wait makes this worse than it needs to be. Real wait queues would fix it.
8. **`sys_accept` returning `-1` doesn't tell you why.** No errno, no error code in the return value beyond pass/fail. For our trivial loop (`if (conn < 0) continue;`) this is fine; debugging a real failure mode would benefit from a separate `sys_errno()`.

## What might come next

The natural sequence: real wait queues (so accept/read sleep instead of yielding), multi-connection TCP (so a real backlog has meaning), an outbound `connect()` so user code can be a TCP *client*, then `dup`/`fork` so a single accepting parent can hand off to a per-connection child. Each of those is a few hundred lines; together they'd close the gap between "demo OS with sockets" and "OS that could plausibly run a real service."
