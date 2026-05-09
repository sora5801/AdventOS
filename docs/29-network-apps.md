# Session 29 — Userspace network apps: nc / wget / telnet / IRC

**Goal:** Stop only being able to *receive* TCP connections. The kernel's TCP stack has been listen-only since session 13 — httpd serves curl from a host, but nothing in ring 3 has ever opened an outbound connection. This session adds active-open (`tcp_connect`), refactors the TCP stack to support multiple concurrent TCBs (so an outbound client and a listener can coexist), implements IP loopback for `127.0.0.1`, refcounts sockets across `fork()`, and ships four ring-3 networking applications: `nc`, `wget`, `telnet`, plus an IRC client (`irc`) and a tiny matching server (`ircd`) to talk to.

End state — the new `[t20]` selftest output:

```
[t20] network apps: wget / nc / telnet / irc + ircd
  wget http://localhost/ -O /wget.txt:
httpd: GET / HTTP/1.0
wget: HTTP 200  body=317

  cat /wget.txt | head -3:
Hello from a USERSPACE HTTP server!

This page was served by user/httpd.c, which runs in ring 3.

  nc localhost 80 | head -1:
GET / HTTP/1.0
httpd: GET / HTTP/1.0
HTTP/1.0 200 OK

  telnet localhost 80 | head -2:
Host: localhost
httpd: Host: localhost
telnet: connected to localhost:80
HTTP/1.0 200 OK

  IRC: ircd & + irc localhost 6667 alice #demo
ircd: listening on port 6667 (one-shot)
*** ircd 001 alice :Welcome to AdventOS IRC
*** alice joined  #demo
*** ircd 353 alice = #demo : alice
*** ircd 366 alice #demo :End of /NAMES list.
<alice>  hello from selftest
<alice>  ACTION waves
=== ERROR :Closing link
irc: disconnected
  ircd reaped: pid=68 exit=0
=== selftest done ===
```

Five distinct programs:
- `wget http://localhost/` — fetches httpd's banner page over a real HTTP/1.0 exchange
- `nc localhost 80` — sends a hand-rolled HTTP request and prints httpd's reply
- `telnet localhost 80` — same, plus the telnet IAC option-negotiation stub
- `ircd 6667` — a tiny IRC server (NICK / USER / JOIN / PRIVMSG / QUIT)
- `irc localhost 6667 alice #demo` — connects, joins, sends a PRIVMSG and a CTCP ACTION, then quits

`httpd.elf` keeps serving the host's `curl` on the side throughout. Tests t1–t19 pass unchanged.

## What's in scope

In:
- **`tcp_connect`** — active open. Picks an ephemeral local port, allocates a TCB, sends SYN, transitions through SYN_SENT → ESTABLISHED via the existing tcp_rx state machine.
- **Multi-TCB pool** — `TCP_MAX_TCBS` bumped from 1 to 6. tcp_rx demultiplexes incoming segments by exact 4-tuple, falling back to a LISTEN-port match for SYNs.
- **Per-TCB callbacks + `user_data` cookie** — the listener's spawned conn TCB inherits its callbacks, so the same on_connect/on_recv/on_close functions fire for both passive and active opens.
- **`SYS_CONNECT` syscall + `sys_connect()` libuser wrapper.**
- **IP loopback** — `127.x.x.x` and our own assigned IP route into `tcp_rx`/`udp_rx` directly instead of going out the NIC. SLIRP doesn't loop guest-to-self packets, so without this `nc localhost 80` would never reach `httpd`.
- **Socket refcounting** — `fork()` bumps, `close()` decrements; tcp_close runs only when the count hits 0. Necessary because `nc`, `telnet`, and `irc` all fork for full duplex and the parent and child both hold the same socket fd.
- **`FS_MAX_FILES` 32 → 64** — five new programs pushed the entry table over its previous cap.
- **Five new ring-3 binaries** in `user/`.
- **`[t20]` selftest** that drives all four client apps end-to-end against local servers.

Out:
- `shutdown(2)` — half-close (sender done, receiver still draining). Without it, the irc client's parent+child both hold the conn and a single close decrement doesn't send FIN; we work around it by having `ircd` actively close on QUIT.
- TCP retransmission, RTO, congestion control, fast retransmit. Loopback is loss-free; the wire is QEMU SLIRP which is also loss-free in practice.
- TLS / HTTPS — `wget`'s URL parser only knows `http://`.
- IPv6.
- Real `getaddrinfo` — DNS resolution is via our session-21 `sys_dns_resolve` plus a hardcoded `localhost → 127.0.0.1` shortcut in each client.
- `tcp_listen()` returning multiple sockets (no `listen` queue beyond a single pending conn).
- `nc -l` listen mode, `wget -c` resume, `telnet`'s line-mode commands, `/list` / `/names` / multi-channel IRC.

## Architecture: the multi-TCB refactor

Before this session, `tcp.c` had a single global `g_tcb`. Every TCP segment routed there. The state machine looked like this:

```
[CLOSED] --tcp_listen()--> [LISTEN]
                              ↓ SYN arrives
                         [SYN_RCVD]
                              ↓ ACK arrives
                        [ESTABLISHED]
                              ↓ peer FIN
                       [CLOSE_WAIT]
                              ↓ tcp_close
                       [LAST_ACK] → [CLOSED] → re-arm to LISTEN
```

The "re-arm to LISTEN" path (in `back_to_listen`) was the only way to handle multiple sequential connections to the same server.

After this session:

```c
#define TCP_MAX_TCBS 6
static struct tcb g_tcbs[TCP_MAX_TCBS];

struct tcb {
    int            in_use;
    int            state;
    int            is_listener;
    uint16_t       local_port;
    struct ip_addr remote_ip;
    uint16_t       remote_port;
    /* sequence-space, callbacks, user_data, tx_buf */
};
```

`tcp_rx` now does explicit 4-tuple matching, with LISTEN as a fallback for SYNs:

```c
static struct tcb *find_tcb_for_seg(...) {
    /* Exact match first (excludes LISTEN). */
    for (int i = 0; i < TCP_MAX_TCBS; i++) {
        if (... t->local_port == dst_port &&
              t->remote_port == src_port &&
              memcmp(t->remote_ip, src_ip, 4) == 0) return t;
    }
    /* LISTEN fallback for SYNs. */
    if (flags & TCP_SYN) {
        for (...) if (state == LISTEN && local_port == dst_port) return t;
    }
    return 0;
}
```

When a SYN lands on a listener, `spawn_conn_from_listener` allocates a *new* TCB carrying the listener's callbacks plus the (remote_ip, remote_port) tuple, and sends SYN-ACK from that new TCB. The listener stays in LISTEN — it can spawn arbitrarily many concurrent conns up to the pool limit.

```
LISTEN TCB (port 80)              ←── stays in LISTEN forever
   │
   │ SYN arrives
   │
   ↓
SPAWN conn-TCB (port 80, peer=10.0.2.2:54321) → SYN_RCVD → ESTABLISHED → ... → CLOSED → tcb_release
```

For active opens:

```
tcp_connect() → alloc TCB → SYN_SENT → send SYN
                                          ↓
                                      tcp_rx(SYN-ACK) → ACK → ESTABLISHED → on_connect fires
```

`SYN_SENT` is a brand-new state in this session.

## tcp_connect: the active-open path

The interesting work:

```c
struct tcb *tcp_connect(const struct ip_addr *remote_ip,
                        uint16_t              remote_port, ...) {
    uint16_t local_port = alloc_ephemeral_port();    /* 49152..65535 */
    struct tcb *t = tcb_alloc();

    t->state       = TCP_SYN_SENT;
    t->local_port  = local_port;
    t->remote_ip   = *remote_ip;
    t->remote_port = remote_port;
    t->snd_isn     = (uint32_t)pit_ticks() * 2654435761u;
    t->snd_nxt     = t->snd_isn;
    t->snd_una     = t->snd_isn;

    /* Pre-advance snd_nxt so a loopback ACK can match. SYN
     * consumes one sequence number. */
    uint32_t syn_seq = t->snd_nxt;
    t->snd_nxt += 1;
    if (tcp_send_seg_seq(t, TCP_SYN, NULL, 0, syn_seq) < 0) {
        tcb_release(t);
        return 0;
    }
    return t;
}
```

The pre-advance of `snd_nxt` is the load-bearing detail. With **loopback**, `tcp_send_seg_seq` calls `ip_send` which (because the destination is local) calls `tcp_rx` recursively on the listener-side TCB, which sends SYN-ACK, which calls `tcp_rx` recursively on the client-side TCB... which checks `if (ack != t->snd_nxt) return`. If we hadn't pre-incremented, the peer's ack-of-our-SYN (= `snd_isn + 1`) wouldn't match our still-`snd_isn` `snd_nxt`, and the SYN-ACK would be silently dropped.

That same "pre-advance, then send" pattern shows up in `tcp_send` (data) and `tcp_close` (FIN) too — anywhere a segment consumes sequence space.

## IP loopback

```c
static int try_loopback(const struct ip_addr *dst, uint8_t proto,
                        const void *payload, uint32_t len) {
    int is_lo = (dst->b[0] == 127);
    int is_us = we_have_ip() && memcmp(dst, &g_my_ip, 4) == 0;
    if (!is_lo && !is_us) return 0;

    struct ip_hdr lhdr = {
        .ver_ihl = 0x45, .total_len = htons(sizeof(lhdr) + len),
        .ttl = 64, .proto = proto,
        .src = *dst, .dst = *dst,
    };

    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
    switch (proto) {
        case IP_PROTO_TCP: tcp_rx(&lhdr, payload, (int)len); break;
        case IP_PROTO_UDP: udp_rx(&lhdr, payload, (int)len); break;
    }
    __asm__ volatile ("pushl %0; popfl" :: "r"(flags) : "cc");
    return 1;
}
```

Two important details:

1. **Recursion-bounded `cli`/`sti`.** A loopback TCP handshake is at most 4 levels of recursion deep (SYN → SYN-ACK → ACK → optional data ACK). Each frame is small. We hold interrupts off the whole time so a NIC packet IRQ can't race the in-flight tcp_rx on `g_tcbs[]`. Real OSes use proper locks; we're cooperative single-CPU and the scheduling latency hit is microseconds.

2. **`src = dst`.** The synthesized IP header's source IP equals the destination — both ends of the loopback connection observe the same address (127.0.0.1 → 127.0.0.1). This makes the 4-tuple matching symmetric: the listener-spawned conn TCB's `remote_ip = 127.0.0.1`, and packets going back the other way also have `src = 127.0.0.1`, so they match.

Without loopback, `nc localhost 80` couldn't talk to httpd: SLIRP doesn't loopback packets sent to the guest's own assigned IP, and `127.0.0.1` would route to the gateway (which doesn't know about it).

## Socket refcounting

The four client apps all use the same fork-for-duplex pattern:

```c
int sk = sys_socket();
sys_connect(sk, ip, port);
int pid = sys_fork();
if (pid == 0) {
    /* child: stdin -> socket */
    while (...) sys_write(sk, ...);
    sys_close(sk);
    sys_exit(0);
}
/* parent: socket -> stdout */
while (...) sys_write(1, ...);
sys_close(sk);
sys_wait(...);
```

After fork, parent and child both hold fd 3 = `sk`. Both can read and write. When the child closes its copy, the parent must still have a working socket.

Without refcounting, the very first `sys_close` from the child triggers `tcp_close` on the underlying TCB, which sends FIN, and the parent's next read returns EOF immediately. The connection dies prematurely.

The fix is per-`struct sock` refcounting:

```c
struct sock {
    int               state;
    int               refcount;     /* fork bumps, close decrements */
    /* ... */
};

void sock_inc_ref(int idx) {
    g_socks[idx].refcount++;
}

int sock_close(int idx) {
    if (s->refcount > 1) {
        s->refcount--;
        return 0;             /* still referenced by another fd */
    }
    if (s->tcb) tcp_close(s->tcb);
    s->state = SOCK_FREE;
    /* ... */
}
```

`task_fork`'s fd-inheritance loop now bumps the count for FD_SOCK (parallel to existing pipes/tmpfs):

```c
case FD_SOCK: sock_inc_ref(parent->fds[i].obj_idx); break;
```

This is the same pattern pipes use (`pipe_inc_read` / `pipe_inc_write`). Without it, the pipe would close on the first child exit too — that bug was fixed for pipes years ago and hadn't yet been ported to sockets.

## The listener spawn path

When the listener-side TCB receives a SYN, `spawn_conn_from_listener` carves out a new TCB:

```c
static struct tcb *spawn_conn_from_listener(struct tcb *l, ...) {
    struct tcb *c = tcb_alloc();
    if (!c) return 0;

    c->state       = TCP_SYN_RCVD;
    c->local_port  = l->local_port;
    c->remote_ip   = *src_ip;
    c->remote_port = src_port;
    c->rcv_isn     = seq;
    c->rcv_nxt     = seq + 1;
    c->snd_isn     = (uint32_t)pit_ticks() * 2654435761u;
    c->snd_nxt     = c->snd_isn;
    c->snd_una     = c->snd_isn;
    c->on_connect  = l->on_connect;
    c->on_recv     = l->on_recv;
    c->on_close    = l->on_close;
    c->user_data   = l->user_data;          /* same listener cookie */

    uint32_t syn_seq = c->snd_nxt;
    c->snd_nxt += 1;                         /* pre-advance, see above */
    tcp_send_seg_seq(c, TCP_SYN | TCP_ACK, NULL, 0, syn_seq);
    return c;
}
```

The conn TCB inherits its callbacks AND user_data from the listener. When `on_connect` later fires for the conn TCB, sock.c's handler sees `t->user_data` pointing back at the LISTENER sock — and from there spawns a fresh conn-sock for `sock_accept` to return:

```c
static void on_connect(struct tcb *t) {
    int sidx = sock_for_tcb(t);
    struct sock *s = &g_socks[sidx];

    /* Active connect path. */
    if (s->state == SOCK_CONNECTING) {
        s->state = SOCK_CONNECTED;
        s->tcb   = t;
        return;
    }

    /* Listener path: spawn a conn-sock. */
    if (s->state == SOCK_LISTEN) {
        int conn = find_free_sock();
        g_socks[conn].state = SOCK_CONNECTED;
        g_socks[conn].tcb   = t;
        t->user_data = (void *)(uintptr_t)(conn + 1);   /* re-target */
        s->pending_conn = conn;
    }
}
```

The same callback discriminates between active and passive opens by looking at the calling sock's state. The `user_data` cookie does triple duty:
- For listener TCBs, it identifies the listener sock so on_connect can spawn a conn-sock.
- For conn TCBs spawned from listeners, it gets re-targeted to point at the conn-sock so on_recv/on_close fire on the right rx ring.
- For active-connect TCBs, it identifies the connecting sock from the start.

## `cookie + 1` encoding

We use `(idx + 1)` as the user_data cookie because `0` is reserved for "no user_data attached":

```c
#define DECODE_SIDX(ud)  ((int)(uintptr_t)(ud) - 1)
```

If a TCB spawns and `user_data == 0`, `sock_for_tcb` returns -1 and the callback drops the event. This guards against partially-initialized TCBs leaking into the callback path.

## sock_connect: the synchronous client API

```c
int sock_connect(int idx, const uint8_t dst[4], uint16_t port) {
    struct ip_addr remote;
    for (int i = 0; i < 4; i++) remote.b[i] = dst[i];

    g_socks[idx].state    = SOCK_CONNECTING;
    g_socks[idx].peer_closed = 0;

    void *ud = (void *)(uintptr_t)(idx + 1);
    struct tcb *t = tcp_connect(&remote, port, on_connect, on_recv,
                                on_close, ud);
    if (!t) { ... return -1; }
    g_socks[idx].tcb = t;

    /* Block until ESTABLISHED or timeout. */
    int spins = 0;
    while (g_socks[idx].state == SOCK_CONNECTING && spins < 500) {
        task_yield();
        spins++;
        if (tcp_state_of(t) == TCP_CLOSED) break;
    }
    if (g_socks[idx].state != SOCK_CONNECTED) return -1;
    return 0;
}
```

Two things worth noting:

1. **`s->tcb` is set AFTER `tcp_connect` returns.** In `on_connect`, we can't compare `s->tcb == t` because the loopback recursion may have fired the callback BEFORE this assignment runs. State alone is the discriminator — `SOCK_CONNECTING` means "active open in flight."

2. **5-second timeout.** 500 yields × ~10ms timer tick = 5 seconds. Loopback handshakes complete in microseconds, so this is comfortable for normal use; for slow / lossy networks (which we don't have), it'd need to be longer.

## The five user programs

### `wget` — HTTP GET

```c
parse_url("http://localhost/path", host, &port, path);
resolve(host, ip);                       /* DNS or localhost shortcut */
sk = sys_socket();
sys_connect(sk, ip, port);

/* Send GET request. */
sys_write(sk, "GET / HTTP/1.0\r\nHost: ...\r\n\r\n", ...);

/* Slurp response, split at first blank line, body to stdout/file. */
while ((n = sys_read(sk, rbuf, sizeof(rbuf))) > 0) {
    if (!in_body) {
        /* scan for \r\n\r\n in hdrbuf, parse status code */
    } else {
        sys_write(sink, rbuf, n);
    }
}
```

No fork — wget is sequential request/response. ~190 lines including URL parsing and the running-window header scan.

### `nc` — netcat-style relay

```c
/* Full duplex: child copies stdin -> socket; parent copies socket
 * -> stdout. Whoever finishes first closes the socket fd. */
int pid = sys_fork();
if (pid == 0) {
    while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
        if (sys_write(sk, buf, n) <= 0) break;
    }
    sys_close(sk);
    sys_exit(0);
}
while ((n = sys_read(sk, buf, sizeof(buf))) > 0) {
    sys_write(1, buf, n);
}
sys_close(sk);
sys_wait(&code);
```

The parent+child split is the only way to do full duplex without non-blocking I/O. Both sides hold the socket; refcounting ensures neither closes prematurely.

### `telnet` — netcat plus IAC handling

Same skeleton as nc, with one extra layer: a 4-state machine over the inbound stream that strips Telnet's `IAC` (0xFF) command bytes. Recognizes:

| Sequence | Action |
|---|---|
| `IAC IAC` | Pass `0xFF` through as data |
| `IAC DO X` | Reply `IAC WONT X` (we refuse all options) |
| `IAC WILL X` | Reply `IAC DONT X` |
| `IAC SB ... IAC SE` | Swallow subnegotiation block |

The refuse-everything strategy ensures the server doesn't expect any cooperation from us. RFC 854 calls this the "minimal viable" Telnet client. ~190 lines.

### `irc` — RFC 1459 client

```c
sys_connect(sk, ip, port);
send_line(sk, "NICK alice");
send_line(sk, "USER alice 0 * :alice");
send_line(sk, "JOIN #demo");

int pid = sys_fork();
if (pid == 0) {
    /* Read stdin lines.
     * /quit  -> send QUIT, exit
     * /me X  -> send PRIVMSG with CTCP \x01ACTION X\x01
     * /msg target text -> send PRIVMSG target :text
     * other  -> PRIVMSG to the joined channel
     */
}

/* Parent: pretty-print inbound lines.
 *   :nick PRIVMSG target :text   ->  <nick> text
 *   :nick JOIN :#chan            ->  *** nick joined #chan
 *   :ircd NNN ... :text          ->  *** ircd NNN text
 *   PING :token                  ->  (silent: send PONG)
 */
```

The line-renderer recognizes the four most common IRC message shapes; everything else falls through to a verbatim `=== <line>` echo. ~250 lines.

### `ircd` — single-channel IRC server

A pared-down ircd that handles NICK / USER / JOIN / PRIVMSG / PING / QUIT just enough to round-trip the demo client. PRIVMSGs to the joined channel are echoed back to the same client (since there's only one client at a time). On QUIT, ircd sends `ERROR :Closing link` and explicitly closes the connection — which is the single trick the demo depends on, because we don't have `shutdown(2)` for half-closes.

```c
/* One-shot mode (default): handle one connection and exit.
 * "loop" mode: accept forever. */
while (how_many == 0 || handled < how_many) {
    int conn = sys_accept(srv);
    /* read line-by-line, dispatch to handle_line(conn, line) */
}
```

~170 lines.

## Two debugging stops worth remembering

### The "loopback ACK arrives mid-tcp_send" bug

First boot after wiring up loopback, `wget localhost` produced `wget: connect failed`. Tracing showed the SYN went out, the SYN-ACK came back, but our state-transition check `if (ack != t->snd_nxt) return` rejected it.

Reason: `tcp_connect` had

```c
tcp_send_segment(t, TCP_SYN, NULL, 0);   /* uses snd_nxt as seq */
t->snd_nxt += 1;                          /* THEN advance */
```

With loopback, the recursive `tcp_rx` for the SYN-ACK fires *during* the first call — before the `snd_nxt += 1`. So the ACK looked like `snd_isn + 1` against an `snd_nxt` of just `snd_isn`, and got dropped.

Fix: refactor `tcp_send_segment` to take an explicit `seq` parameter, and have callers do "claim seq, advance snd_nxt, send segment with claimed seq" in that order. Same fix needed for SYN-ACK on the listener side, FIN segments in tcp_close, and data segments in tcp_send.

### The fork-shared-socket hang

Second hit: `cat /req.txt | nc localhost 80 | head -1` hung in nc's child with no diagnostic. Analysis:

```
nc creates sk = fd 3                            sock.refcount = 1
nc forks → child inherits fd 3                  refcount = 2 (would be wrong)
child reads stdin, writes sk
child closes sk                                  refcount → 0 (premature!)
                                                 → tcp_close → FIN → server closes
parent's sys_read(sk) returns 0 immediately
```

Without refcount, the child's close terminates the connection while the parent is still trying to use it. With session-21-vintage pipes already refcounted, the question was "why aren't sockets?" — because we'd never written a fork-and-read-from-socket app before now (httpd doesn't fork after binding).

Fix: add `sock_inc_ref` and a refcount field on `struct sock`, parallel to pipes. Bump from `task_fork`. Decrement (instead of immediate close) on `sock_close`.

After the fix, a forked nc/telnet/irc properly shares the socket, and only the LAST close triggers `tcp_close`.

## The sub-bug we couldn't quite kill

There's still a known limitation in 3-stage pipelines where the middle stage forks internally (`cat /file | nc | head`):

```
shell forks 3 stages. Each stage does dup2(pipe_r, 0) / dup2(pipe_w, 1).
Pipe refcounts include shell's originals + each stage's inherited copies +
each stage's dup2'd handle.

Then nc forks internally. nc-child inherits all of nc-parent's fds:
including pipe1_r at fd 0 (used) and pipe2_w at fd 1 (also held but
not read).

After cat exits, pipe1's write_refs SHOULD drop to 0, releasing the
read-side EOF to nc-child. But somewhere in the bookkeeping it doesn't,
and nc-child's read on fd 0 blocks indefinitely.
```

Tested workaround: drive nc/telnet's stdin via `tty_inject` in the shell selftest, keeping the pipeline at 2 stages (`nc | head -1`). The 2-stage form doesn't hit the bug. The deep cause is somewhere in `dup2` + pipe inheritance + inner fork; tracking it down is one of those "not today" things.

## File-by-file changes

```
kernel/tcp.h            multi-TCB declarations (TCP_MAX_TCBS=6,
                        SYN_SENT state, user_data field, struct tcb
                        gains in_use/is_listener)
kernel/tcp.c            rewrite: 4-tuple matching in tcp_rx,
                        spawn_conn_from_listener, tcp_connect,
                        tcp_send_seg_seq + tcp_send_ack helpers,
                        ephemeral port allocator, pre-advance pattern
                        in tcp_connect/spawn/send/close

kernel/sock.h           sock gains refcount + tcb pointer; SOCK_CONNECTING
                        + SOCK_CLOSED states; sock_connect / sock_inc_ref
                        prototypes
kernel/sock.c           refcount machinery, sock_connect (active-open
                        path with 5-sec spin timeout), unified
                        on_connect that handles both listener and
                        client TCBs

kernel/ip.c             try_loopback for 127.x and our own IP, with
                        cli/sti around the recursive tcp_rx/udp_rx

kernel/syscall.h        SYS_CONNECT = 49
kernel/syscall.c        SYS_CONNECT dispatcher case

kernel/task.c           task_fork inherits FD_SOCK with sock_inc_ref

kernel/fs.h, mkfs.py    FS_MAX_FILES 32 -> 64 (4 super sectors -> 5)

user/libuser.h          sys_connect prototype + SYS_CONNECT constant
user/libuser.c          sys_connect wrapper

user/nc.c               new — full-duplex netcat client
user/wget.c             new — HTTP/1.0 downloader
user/telnet.c           new — telnet client with IAC stub
user/irc.c              new — RFC 1459 client (NICK/USER/JOIN/PRIVMSG/...)
user/ircd.c             new — one-shot single-channel IRC server

build.sh                +5 program names
mkfs.py                 +5 USER_PROGRAMS entries

user/sh.c               [t20] selftest driving wget / nc / telnet / irc
                        sequentially with kbd ring drain before IRC
```

Net diff: ~1300 LoC across 11 files. Of that, ~900 lines are the five new user programs (mostly URL/IRC/Telnet protocol code); ~250 lines is the TCP/sock refactor; ~100 lines is the loopback + refcount plumbing; the rest is build/fs/test wiring.

## Boot log highlights

```
[boot] mounting AdventFS... fs: AdventFS mounted, 29 entries, 619/1024 sectors free
[boot] mounting VFS... vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
[boot] enabling interrupts
...
init: pid=4, reading /etc/inittab
init: started 'httpd.elf' as pid 5 (once)
init: started 'sh.elf' as pid 6 (once)
httpd: listening on port 80 (userspace)

[t20] network apps: wget / nc / telnet / irc + ircd
  wget http://localhost/ -O /wget.txt:
httpd: GET / HTTP/1.0
wget: HTTP 200  body=317

  cat /wget.txt | head -3:
Hello from a USERSPACE HTTP server!

This page was served by user/httpd.c, which runs in ring 3.

  nc localhost 80 | head -1:
GET / HTTP/1.0
httpd: GET / HTTP/1.0
HTTP/1.0 200 OK

  telnet localhost 80 | head -2:
Host: localhost
httpd: Host: localhost
telnet: connected to localhost:80
HTTP/1.0 200 OK

  IRC: ircd & + irc localhost 6667 alice #demo
ircd: listening on port 6667 (one-shot)
*** ircd 001 alice :Welcome to AdventOS IRC
*** alice joined  #demo
*** ircd 353 alice = #demo : alice
*** ircd 366 alice #demo :End of /NAMES list.
<alice>  hello from selftest
<alice>  ACTION waves
=== ERROR :Closing link
irc: disconnected
  ircd reaped: pid=68 exit=0
=== selftest done ===
```

And from a host curl during the same run:

```
$ curl -s http://localhost:8090/
Hello from a USERSPACE HTTP server!

This page was served by user/httpd.c, which runs in ring 3.
```

httpd serves the host curl request through SLIRP port-forwarding while the in-guest selftest runs nc/wget/telnet/irc against itself via 127.0.0.1 loopback — six different end-to-end TCP scenarios exercising the new multi-TCB pool concurrently.

## What's not yet here, ranked

1. **`shutdown(2)`**. The cleanest way to express "I'm done writing but still want to read the response" for fork-based clients. Today we work around it by having servers actively close on protocol-level QUIT messages.
2. **A real listen queue.** `tcp_listen` accepts at most one pending conn; a second SYN before `sock_accept` fires drops on the floor.
3. **Non-blocking sockets** + `select`/`poll`. Would let nc/telnet/irc be single-task instead of forking, eliminating the refcount + half-close gymnastics.
4. **TLS.** `wget` is HTTP-only. Adding TLS would be a session in itself — needs a crypto stack, certificate handling, and BoringSSL-equivalent state machines.
5. **PTY layer.** `irc` would benefit from raw-mode editing during chat input; today it relies on the line-edited canonical mode of the shared kernel TTY.

The networking story is now coherent enough to actually use, not just demo.
