# Session 30 — Real multi-connection TCP

**Goal:** Stop dropping SYNs when a server is busy. Sessions 13-29 built up the TCP stack to the point where multiple TCBs can coexist (session 29), but the listener-side socket layer still had only `int pending_conn` — a single slot. If two SYNs landed at the same time, the second was silently discarded. With the listener accept queue this session adds, up to 8 simultaneous SYNs queue up cleanly; userspace `httpd` pulls them off in order via `sock_accept()`. Three parallel `wget` clients in the selftest now each get their HTTP/200 instead of fighting over a single slot.

End state — the new `[t21]` selftest output:

```
[t21] multi-conn TCP: 3 parallel wgets vs queueing httpd
  spawned 3 wget clients (pids 71 72 73)
wget: HTTP 200  body=317
wget: HTTP 200  body=317
wget: HTTP 200  body=317
  reaped pid=71 exit=0
  reaped pid=73 exit=0
  reaped pid=72 exit=0
  result: 3/3 clients succeeded
```

The pids reaped reveal scheduler order: pid 73 finishes between 71 and 72 even though it was started last. All three SYNs land at httpd's listener, all three complete the 3-way handshake (concurrent in time — two SYN-ACKs go out while httpd is in the middle of reading the first request), all three sit on the accept queue waiting for `httpd` to pick them up.

httpd itself still serves requests sequentially (one at a time), but with the queue, no client gets dropped and every parallel client eventually gets a response.

Three parallel host-side `curl http://localhost:8102/` calls also each get the full banner page.

## What's in scope

In:
- **Listener accept queue** — replace `int pending_conn` with a FIFO ring of conn-sock indices, depth `SOCK_BACKLOG_MAX=8`, configurable via `listen(backlog)`.
- **`SYS_LISTEN` honors its backlog argument** — was previously ignored.
- **`SYS_WAIT_NB`** — non-blocking variant of `sys_wait` (returns 0 instead of blocking when children are still alive). Mirrors Linux `waitpid(-1, ..., WNOHANG)`.
- **Pool bumps** — `TCP_MAX_TCBS` 6 → 16 and `SOCK_MAX` 8 → 16 to fit multiple concurrent connections (each established conn = 2 TCBs over loopback: client-side + server-side).
- **`httpd` listens with `backlog=8`** — multi-client tests now exercise the queue.
- **`[t21]` selftest** — fork 3 wget children, run them in parallel against httpd, reap and verify all succeeded.

Out:
- **Fork-per-connection httpd.** Implemented and passing for single-client cases, but interacts subtly with the in-test `nc` and `telnet` tests that use internal-fork pipelines (`cat /req | nc | head -1`). Reverted to inline serving for this session; the queue alone is sufficient to demonstrate "real multi-connection TCP".
- **Connection-level pgrp / session.** Each accepted connection still runs in the listener's task; with fork-per-conn it would gain its own task identity.
- **`SO_REUSEADDR` / address-in-use semantics.** Not modeled.
- **Backpressure on the listener** — once the accept queue is full, new SYNs are silently dropped. POSIX-leaning behavior would be to send RST or just fail to ACK; we just don't queue.
- **Per-fd `O_NONBLOCK`.** `sock_read` and `sock_accept` still always block.

## Architecture: the accept queue

Before:

```c
struct sock {
    /* For LISTEN sockets: the conn-socket index that's been
     * created but not yet accepted. -1 if no pending connection. */
    int               pending_conn;
};

// on_connect (listener path):
if (l->pending_conn >= 0) return;       /* listener already has one queued */
/* ... allocate conn, set on listener ... */
l->pending_conn = conn;

// sock_accept:
while (g_socks[idx].pending_conn < 0) task_yield();
int conn = g_socks[idx].pending_conn;
g_socks[idx].pending_conn = -1;
return conn;
```

A second SYN that arrived while `pending_conn >= 0` got dropped at the `return` in `on_connect`. Worse, because TCP-level handshake had already completed (the SYN-ACK was sent and ACK'd before the on_connect callback fired), the kernel already had a conn-TCB for that connection — it just had nowhere to put it on the user-visible side.

After:

```c
#define SOCK_BACKLOG_MAX 8

struct sock {
    int  pending_conns[SOCK_BACKLOG_MAX];   /* FIFO of conn-sock indices */
    int  pending_head;                       /* enqueue at head */
    int  pending_tail;                       /* dequeue at tail */
    int  backlog;                            /* configured cap */
};
```

The FIFO is a standard ring buffer. `q_full()` checks both the structural fullness (head+1 == tail) and a soft cap against the user-configured `backlog`:

```c
static int q_full(struct sock *s) {
    int next = (s->pending_head + 1) % SOCK_BACKLOG_MAX;
    return next == s->pending_tail ||
           ((s->pending_head - s->pending_tail + SOCK_BACKLOG_MAX) % SOCK_BACKLOG_MAX
                >= s->backlog);
}
```

The `head + 1 == tail` test catches the always-one-slot-wasted ring-buffer invariant; the soft cap ensures `listen(2)` actually does what the user asked for. With `backlog=1`, the queue holds exactly one entry — the original behavior. With `backlog=8`, eight outstanding SYN handshakes can complete and wait.

`on_connect`'s listener path:

```c
if (q_full(l)) return;          /* refuse if backlog reached */

int conn = find_free_sock();
g_socks[conn].state = SOCK_CONNECTED;
g_socks[conn].tcb   = t;
t->user_data = (void *)(uintptr_t)(conn + 1);

q_push(l, conn);                /* into the listener's pending ring */
```

`sock_accept`:

```c
while (q_empty(&g_socks[idx])) task_yield();
return q_pop(&g_socks[idx]);
```

The structural change is small. The user-visible behavior change is significant: a server can now answer multiple concurrent clients without losing any.

## Pool sizing

`TCP_MAX_TCBS` was 6 (session 29). Each established connection over loopback costs **two** TCBs (one for the client-side state machine, one for the server-side). With:
- 1 LISTEN TCB for httpd
- 3 client-side TCBs (one per wget)
- 3 server-side TCBs (one per accepted conn)

we need 7. Plus headroom for in-flight handshakes, RST cleanup, and the host-side curl that may be running in parallel — bumped to 16.

`SOCK_MAX` from 8 to 16 for the same reason: each connection needs two sock entries (one per side), and we want headroom.

## SYS_WAIT_NB

```c
int task_waitpid_nb_current(int *out_code) {
    struct task *t = g_current;
    uint32_t pid = reap_one_zombie_of(t, out_code);
    if (pid != 0) return (int)pid;             /* zombie reaped */
    if (!has_any_live_or_zombie_child(t)) return -1; /* no children */
    return 0;                                   /* alive but no zombie */
}
```

Three return shapes:
- `> 0` — pid of a freshly-reaped zombie, exit code in `*out_code`
- `0` — children exist but none have exited yet (don't block)
- `-1` — no children at all

The non-blocking variant is the building block for fork-per-conn server patterns: drain zombies between accept calls without committing to a blocking wait. Fork-per-conn proper is a future-session task (see "What didn't ship" below); the syscall is in place when we get back to it.

`SYS_WAIT_NB = 50`, libuser wrapper `sys_wait_nb`. Keeps the existing `SYS_WAIT` (49) intact for code that wants to block.

## Test architecture: forking the clients, not the server

The selftest doesn't drive parallel SYN traffic by forking the SERVER — it forks the CLIENTS. From within the shell process:

```c
int n_clients = 3;
int pids[8];
for (int i = 0; i < n_clients; i++) {
    int pid = sys_fork();
    if (pid == 0) {
        const char *argv2[] = { "wget.elf", "http://localhost/", 0 };
        sys_exec("wget.elf", argv2);
        sys_exit(127);
    }
    pids[i] = pid;
}
```

Three forks back to back, three `wget.elf` execs. Each wget is its own ring-3 task; each calls `sys_socket()` + `sys_connect()` + reads + closes; each runs concurrently in our cooperative scheduler.

Crucially, `sys_connect()` issues the SYN and synchronously walks through the SYN-ACK handshake via the loopback path (session 29's IP loopback short-circuit). So by the time `sys_connect` returns, the client-side TCB is in `ESTABLISHED` and the server-side conn-TCB sits on httpd's accept queue. The kernel's TCP demultiplexing matches each segment by 4-tuple; the three connections are independent state machines.

Then each wget sends its HTTP request (`sys_write`) and reads the response (`sys_read`). httpd runs sequentially — pull from queue, read req, write resp, close, loop — so all three responses come back, just one at a time. From the shell's perspective:

```
spawned 3 wget clients (pids 71 72 73)
[wget output interleaves with httpd: GET diagnostics]
reaped pid=71 exit=0
reaped pid=73 exit=0
reaped pid=72 exit=0
result: 3/3 clients succeeded
```

The shell's main thread blocks in `sys_wait()`, picking off children as they exit. All three return code 0 — full HTTP exchange completed, body was the canned 317 bytes, status 200.

## What "real multi-connection" actually changes

Let me trace what happens for 3 parallel SYNs *without* the queue (session 29 baseline) vs. with it (session 30):

**Without queue (session 29):**

1. wget #1 SYN → tcp_rx → listener spawns conn-tcb #1 → on_connect populates `pending_conn` = sock_idx_1.
2. httpd's `sock_accept` returns sock_idx_1. httpd reads, processes.
3. wget #2 SYN arrives → tcp_rx → listener spawns conn-tcb #2 → on_connect sees `pending_conn >= 0` → **silently drops conn #2**.
4. wget #3 SYN arrives → same fate.
5. wgets #2 and #3 hang on `sock_read` waiting for data that never comes (their client-side TCB is in ESTABLISHED — the handshake completed before the drop).
6. Eventually wgets #2 and #3 time out (or hang forever — we don't have client-side timeout in `wget.c`).

**With queue (session 30):**

1. wget #1 SYN → conn-tcb #1 → enqueued at head=0.
2. httpd `sock_accept` pops sock_idx_1 from tail. tail=1.
3. While httpd is busy with #1, wgets #2 and #3 SYN → conn-tcbs #2 and #3 → enqueued at head=1, then head=2.
4. After serving #1, httpd loops back to `sock_accept` → pops sock_idx_2 from tail=1.
5. Continues until all three are served.

The accept queue is the ONLY thing that's structurally different about #1 vs #3 — the kernel TCP layer has been multi-TCB since session 29, but the *plumbing* between TCP and the user-space `accept` call only had one slot. Now it has 8.

## Why the rule of "8" for backlog

`SOCK_BACKLOG_MAX = 8` is a soft choice. Linux's default is 128 in modern kernels, was 5 in old POSIX. We pick 8 because:

- Each entry in `pending_conns[]` is a small int, so the per-listener overhead is 8 ints + a few counters: ~40 bytes.
- The cap is the same magnitude as `TCP_MAX_TCBS`. Adding more queue depth than total TCB pool would let SYNs pile up faster than they could complete.
- Real workloads with deeper backlogs typically use SO_REUSEPORT and multiple listeners, which we don't have.

## httpd's role

The userspace httpd was the test target for this work. The full diff was small:

```diff
- if (sys_listen(s, 1) < 0) ...
+ if (sys_listen(s, 8) < 0) ...

- puts("httpd: listening on port 80 (userspace)\n");
+ puts("httpd: listening on port 80 (userspace, backlog=8)\n");
```

The `accept`/`read`/`write`/`close` loop is unchanged. httpd doesn't have to know about the queue — it just calls `sock_accept` and the kernel pulls from the front of the FIFO. The structural improvement is entirely in the kernel and the syscall API.

## Verification: t21 + parallel curl

Selftest output:

```
[t21] multi-conn TCP: 3 parallel wgets vs queueing httpd
  spawned 3 wget clients (pids 71 72 73)
wget: HTTP 200  body=317
wget: HTTP 200  body=317
wget: HTTP 200  body=317
  reaped pid=71 exit=0
  reaped pid=73 exit=0
  reaped pid=72 exit=0
  result: 3/3 clients succeeded
```

Host-side parallel curl, after waiting for the system to come up:

```
$ curl -s http://localhost:8102/ &
$ curl -s http://localhost:8102/ &
$ curl -s http://localhost:8102/ &
$ wait
Hello from a USERSPACE HTTP server!
This page was served by user/httpd.c, which runs in ring 3.
It calls socket()/bind()/listen()/accept() through INT 0x80,
...
Hello from a USERSPACE HTTP server!
This page was served by user/httpd.c, which runs in ring 3.
...
Hello from a USERSPACE HTTP server!
This page was served by user/httpd.c, which runs in ring 3.
...
```

Three full responses interleaved on stdout, one per curl. Same demo from inside the guest (selftest) and outside (host).

## What didn't ship: fork-per-conn httpd

The original goal was fork-per-connection serving — each accepted client handled in its own forked child, with the httpd parent immediately returning to `accept()` to pick up the next. That's the canonical Unix server pattern.

The implementation is straightforward:

```c
for (;;) {
    reap_zombies();          /* sys_wait_nb in a loop */
    int conn = sys_accept(s);
    int pid = sys_fork();
    if (pid == 0) {
        sys_close(s);        /* child doesn't need listener */
        handle_client(conn);
        sys_exit(0);
    }
    sys_close(conn);          /* parent: drop our ref, child holds it */
}
```

It works for direct wget tests. It hangs in the existing `[t20]` test which uses `cat /req | nc localhost 80 | head -1` — a 3-stage pipeline where the middle stage (`nc`) ALSO forks internally for full duplex. The combination of pipeline-fork + nc-internal-fork + httpd-fork pushes too many concurrent processes through pipe-fd inheritance bookkeeping, and one of them hangs in `sys_read`.

The diagnostic isolated the hang to nc's child reading from `pipe1_r` after `cat` had already exited and decremented `pipe1.write_refs` to 0. Tracing through the kernel showed `write_refs` was correct, but somewhere in the dup2-then-close sequence in `run_pipeline`, an extra reference is held.

Rather than ship a fix that papered over the symptom, I reverted httpd to inline serving. The accept queue + multi-TCB + non-blocking wait are all in place; the deep refactor of dup2/pipe-inheritance is a future-session task.

What does inline serving cost? Per-connection latency goes up by a few microseconds (`fork()` would have let the next request start its handshake while we were writing the response). For a real concurrent workload, fork-per-conn would matter. For the demo, the queue alone proves the multi-connection plumbing works end-to-end.

## File-by-file changes

```
kernel/sock.h            SOCK_MAX 8 -> 16; SOCK_BACKLOG_MAX = 8;
                         pending_conns[] FIFO instead of pending_conn;
                         sock_listen takes backlog argument
kernel/sock.c            q_init / q_push / q_pop / q_full / q_empty
                         helpers; on_connect listener path queues
                         (or refuses on full); sock_accept pops

kernel/syscall.h         SYS_WAIT_NB = 50
kernel/syscall.c         SYS_LISTEN passes backlog through;
                         SYS_WAIT_NB dispatcher case

kernel/task.h            task_waitpid_nb_current prototype
kernel/task.c            task_waitpid_nb_current implementation

kernel/tcp.h             TCP_MAX_TCBS 6 -> 16

user/libuser.h           SYS_WAIT_NB constant + sys_wait_nb prototype
user/libuser.c           sys_wait_nb wrapper

user/httpd.c             listen with backlog=8 (was 1);
                         banner updated

user/sh.c                [t21] selftest: fork 3 wget clients in parallel,
                         reap, verify 3/3 success
```

Net diff: about 120 lines, mostly in `sock.c` for the queue helpers + `sock_accept`/`on_connect`. The kernel-side changes ripple through cleanly because the TCP layer (session 29) is already multi-TCB; we only had to fix the bridge between TCP and the user-visible accept API.

## Boot log highlights

```
[boot] mounting AdventFS... fs: AdventFS mounted, 29 entries, 619/1024 sectors free
[boot] mounting VFS... vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
init: pid=4, reading /etc/inittab
init: started 'httpd.elf' as pid 5 (once)
init: started 'sh.elf' as pid 6 (once)
httpd: listening on port 80 (userspace, backlog=8)

[t20] network apps: wget / nc / telnet / irc + ircd
   ... (all four pass) ...

[t21] multi-conn TCP: 3 parallel wgets vs queueing httpd
  spawned 3 wget clients (pids 71 72 73)
wget: HTTP 200  body=317
wget: HTTP 200  body=317
wget: HTTP 200  body=317
  reaped pid=71 exit=0
  reaped pid=73 exit=0
  reaped pid=72 exit=0
  result: 3/3 clients succeeded
=== selftest done ===
```

And from a host with three parallel curls running simultaneously, all three see the full banner. The same `httpd.elf` task handles all of them — sequentially, but never dropping a SYN.

## What this opens up

Fork-per-connection httpd is the obvious next step — the building blocks (`SYS_WAIT_NB`, the queue, sock refcounting from session 29) are all in place. The remaining work is the dup2/pipe-inheritance refactor that would let our existing test pipelines coexist with internal forking.

After that, useful follow-ups include:
- Per-connection task pgrps, so a long-running server could send signals to all of its in-flight client handlers as a group.
- Async accept (epoll-style readiness notification on the listener fd), letting a single-task server multiplex many connections without forking. We'd need real file-descriptor readiness state and `select`/`poll` syscalls.
- TLS over the connection, which is a session in itself.

The wire is now a pipe a real server could plausibly use; the user-visible API matches `listen(backlog)` semantics; multiple clients can talk to the same port simultaneously without anyone getting dropped. That's the "real" in "real multi-connection TCP."
