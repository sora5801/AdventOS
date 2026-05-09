# Session 11 — Minimum-viable TCP, an HTTP server, and the shell in ring 3

**Goal:** Make a real TCP connection from the host to the kernel and serve HTTP. Move the interactive shell out of the kernel and into userspace as a `libuser` program that becomes pid 1.

End state of the milestone:

```
$ curl http://localhost:8080/
Hello from AdventOS!
Userspace shell + minimal TCP/IP/HTTP working.
Path: served by kernel/http.c after a real 3-way handshake.

advent$ ifconfig
eth0:
    HWaddr  52:54:00:12:34:56
    inet    10.0.2.15
    ...
advent$ ping 10.0.2.2
PONG from 10.0.2.2  seq=1  time=0 ms
```

That's a real TCP 3-way handshake, real one's-complement-with-pseudo-header checksumming, and a shell process running in ring 3 that reads input via `SYS_READ_LINE` and calls into the kernel's command parser through `SYS_KCMD`.

## What's in scope (and what isn't)

In:
- TCP state machine: LISTEN → SYN_RCVD → ESTABLISHED → FIN_WAIT_1/CLOSE_WAIT → LAST_ACK/FIN_WAIT_2 → CLOSED
- TCP checksum with pseudo-header
- Single TCB (one connection at a time per port; one listening port)
- HTTP server with a canned 200 OK response
- `SYS_READ_LINE`, `SYS_KCMD` syscalls
- Userspace shell in `user/sh.c` running as pid 1
- Local user commands (`upid`/`utime`/`uhelp`/`usleep`/`uexit`); everything else is forwarded to the kernel's existing parser

Out:
- Retransmission, congestion control, SACK, window scaling
- Out-of-order reassembly (we drop)
- Multiple concurrent TCP connections
- TCP options beyond ignoring the inbound MSS
- TIME_WAIT (we go straight to CLOSED for simplicity)
- A real `accept()`-style sockets API
- HTTP request parsing (we ignore the actual request and return the same body)
- A separate user shell parser (we lean on the kernel's existing one for kernel-side commands)

## TCP state machine

```
                       (client SYN)
   LISTEN ────────────────────────────────► SYN_RCVD
     ▲                                          │
     │   (back_to_listen)                       │ (peer ACK of our SYN-ACK)
     │                                          ▼
     │                                     ESTABLISHED
     │                                          │
     │      ┌───── (peer FIN) ──────────────────┤
     │      ▼                                   │ (we call tcp_close)
     │  CLOSE_WAIT ──── (we close) ──► LAST_ACK │
     │      │                              │    ▼
     │      │                              │   FIN_WAIT_1
     │      │                              │    │
     │      │                              │    │ (peer ACKs our FIN)
     │      │                              │    ▼
     │      │                              │   FIN_WAIT_2
     │      │                              │    │
     │      │                              │    │ (peer FIN)
     │      │                              │    ▼
     │      │                              │   CLOSED
     │      └────── (peer ACK ours) ───────┘
     └──────────────────────────────────────────┘
```

The implementation collapses TIME_WAIT into "go back to LISTEN" — a real stack would wait `2 × MSL` to absorb stray duplicates. For SLIRP loopback there are no stray duplicates worth worrying about.

```c
enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,            /* declared but unused */
};
```

## Sequence-space accounting

Three pointers on the send side, one on the receive side:

```c
struct tcb {
    uint32_t snd_isn;   /* what we picked at SYN-ACK     */
    uint32_t snd_nxt;   /* next byte we'll send          */
    uint32_t snd_una;   /* oldest byte we've sent unACKed */
    uint32_t rcv_isn;   /* peer's initial seq            */
    uint32_t rcv_nxt;   /* next byte we expect           */
    ...
};
```

Rules I had to internalize:
- **SYN consumes one sequence number.** If we send a SYN, `snd_nxt += 1` immediately afterward.
- **FIN also consumes one sequence number.** Same accounting.
- **ACK number = next byte we expect.** When we receive a 32-byte data segment with seq = X, our reply has ack = X + 32.
- **SYN-ACK is special.** We send SYN and ACK together; both consume 1 seq, but the SYN is ours (snd_nxt += 1) and the ACK acknowledges peer's SYN (rcv_nxt = peer.seq + 1).

The first time you write a TCP, getting `+= 1` right in five different places is what you spend most of the debugging time on.

## The pseudo-header checksum, and the bug it caused

TCP's checksum is computed over a synthesized **pseudo-header** (src IP, dst IP, zero, protocol, TCP length) prepended to the segment, using the same 16-bit one's-complement-with-end-around-carry as IP and ICMP.

First version, written to mirror `ip_checksum`:

```c
static uint16_t tcp_checksum(...) {
    struct {
        struct ip_addr src;
        struct ip_addr dst;
        uint8_t        zero;
        uint8_t        proto;
        uint16_t       len;
    } __attribute__((packed)) ph;
    ph.src   = *src;
    ph.dst   = *dst;
    ph.zero  = 0;
    ph.proto = IP_PROTO_TCP;
    ph.len   = htons(seg_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&ph;
    for (uint32_t i = 0; i < sizeof(ph) / 2; i++) sum += p[i];
    p = (const uint16_t *)seg;
    uint16_t left = seg_len;
    while (left > 1) { sum += *p++; left -= 2; }
    if (left)        { sum += *(const uint8_t *)p; }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
```

Looks fine. Doesn't work. SLIRP silently dropped every SYN-ACK. After capturing a pcap and recomputing the checksum from the wire bytes in Python, the actual checksum was 0x5fd1; the correct one was 0x63ba. Off by 0x3e9 — not a transposition, not an end-around-carry mistake, just plain *wrong*.

The bug is in the layout / read pattern. Possibilities I considered:
- Struct padding leaking into `sizeof(ph)`. (`packed` should prevent it but worth ruling out.)
- Unaligned `(uint16_t *)&ph` reads doing something weird on x86. (They don't on x86, but compilers can sometimes do split loads.)
- An integer-promotion subtlety in the `while (left > 1)` loop.

Rather than narrow it down, I rewrote the whole function to use a flat byte buffer and `ip_checksum` (which we know works because IP and ICMP have been correct for a session):

```c
static uint16_t tcp_checksum(...) {
    uint8_t buf[12 + 1500];
    memcpy(buf + 0, src, 4);
    memcpy(buf + 4, dst, 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_TCP;
    buf[10] = (uint8_t)(seg_len >> 8);   /* explicit big-endian */
    buf[11] = (uint8_t)(seg_len & 0xFF);
    memcpy(buf + 12, seg, seg_len);
    return ip_checksum(buf, 12u + seg_len);
}
```

No packed struct, no half-word reads through a struct pointer, no `htons` whose endianness convention has to line up with the loop's. Pseudo-header is 12 explicit bytes; concatenate the segment; one call to a known-good function. **First boot after the rewrite, `curl http://localhost:8080/` printed the response.**

The lesson: when a "smaller" version isn't working and you can't see why, the rewrite that drops a clever packed-struct trick for plain `memcpy + flat sum` is almost always the right move. The performance difference doesn't matter at this scale.

## tcp_listen / tcp_send / tcp_close

Three operations the application uses:

```c
int tcp_listen(uint16_t port, tcp_recv_cb on_recv, tcp_close_cb on_close);
int tcp_send  (struct tcb *t, const void *data, int len);
int tcp_close (struct tcb *t);
```

`tcp_listen` parks the global TCB in LISTEN state and registers callbacks. The next inbound SYN to that port completes the 3-way handshake; the segment after is dispatched to `on_recv` once it has data. `on_close` fires after teardown either side initiates.

After teardown, the listener auto-rearms by calling `tcp_listen` again with the same callbacks/port. That's how curl-twice-in-a-row works — second curl finds the same TCB in LISTEN state.

`tcp_send` segments out a PSH+ACK with the given bytes, increments `snd_nxt`. No retransmit, no flow control, no MSS-respecting fragmentation — if the application tries to send more than fits in one segment we'll silently truncate-via-IP-MTU.

`tcp_close` issues a FIN+ACK and walks state forward (ESTABLISHED → FIN_WAIT_1, or CLOSE_WAIT → LAST_ACK depending on which side initiates).

## The HTTP server

[`kernel/http.c`](../kernel/http.c), 35 lines:

```c
static const char *g_response =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from AdventOS!\n"
    "Userspace shell + minimal TCP/IP/HTTP working.\n"
    "Path: served by kernel/http.c after a real 3-way handshake.\n";

static void http_on_recv(struct tcb *t, const char *data, int len) {
    if (g_handled_this_conn) return;
    g_handled_this_conn = 1;
    /* log the request line for the demo */
    int show = len < 60 ? len : 60;
    kputs("httpd: ");
    for (int i = 0; i < show && data[i] != '\r' && data[i] != '\n'; i++)
        kputc(data[i]);
    kputc('\n');
    tcp_send(t, g_response, strlen(g_response));
    tcp_close(t);
}
```

The server doesn't parse the request. We could split on `' '` and check the path, but for a demo "any HTTP request → canned 200" is the point. Keepalive isn't supported (we send `Connection: close`), so each request gets its own TCP connection and full handshake.

`g_handled_this_conn` matters because `tcp_close` from the server side starts the FIN dance, and during it the peer's `FIN+ACK` will come back as a zero-length recv — we shouldn't try to send the response again.

## Refactoring the kernel shell to expose two entry points

The kernel shell had `static void read_line(...)` and `static void run_command(...)`. Both still exist; both are also reachable as public functions:

```c
int  kshell_read_line(char *buf, int cap);   /* echoes typed chars, blocks */
void kshell_run_line (char *line);            /* parses verb, dispatches    */
```

The original `shell_run` is still there as a fallback if `sh.elf` isn't on the disk for some reason. It's literally:

```c
void shell_run(void) {
    char line[LINE_MAX];
    for (;;) {
        kputs("advent> ");
        read_line(line, sizeof(line));
        kshell_run_line(line);
    }
}
```

When the userspace shell is up, `kshell_read_line` is invoked from inside `SYS_READ_LINE` and `kshell_run_line` from `SYS_KCMD`. The kernel's command implementations don't change at all — they're still `cmd_help`, `cmd_meminfo`, `cmd_ping`, etc.

## SYS_READ_LINE

```c
case SYS_READ_LINE: {
    char *user_buf = (char *)(uintptr_t)a;
    int   cap      = (int)b;
    if (cap > 1024) cap = 1024;
    ret = kshell_read_line(user_buf, cap);
    break;
}
```

The user pointer is dereferenced directly because we're still on the user task's CR3 inside the syscall handler — the user PD has the user pages mapped, the kernel reads/writes them transparently. `kshell_read_line` already echoes typed characters via `kputc` (which writes to VGA + serial), so the user gets visible feedback while typing.

A real OS would `copy_to_user(buf, ..., n)` after building the line in a kernel buffer, both for safety (validate that the user buffer is in user-mapped pages) and for liveness (user could pass a pointer to NULL). We trust the user code.

## SYS_KCMD

```c
case SYS_KCMD: {
    const char *p = (const char *)(uintptr_t)a;
    char buf[256];
    int  i;
    for (i = 0; i < 255 && p[i]; i++) buf[i] = p[i];
    buf[i] = 0;
    kshell_run_line(buf);
    ret = 0;
    break;
}
```

The kernel-side parser **modifies the line** (replaces the first space with NUL to split verb from args) — so we have to copy the user's string into a kernel-owned buffer first. The buffer is a 256-byte stack allocation. Anything longer gets truncated at 256 chars, which is fine for a shell line.

Output during command execution goes through `kputs/kprintf`, which still write to VGA + serial directly. The user task isn't observably involved during command execution; from its POV `SYS_KCMD` is just a syscall that takes a while and returns 0.

## user/sh.c — the userspace shell

```c
int main(void) {
    puts("\nAdventOS userspace shell, pid="); printf("%d\n", sys_getpid());
    puts("Type 'uhelp' for shell-local commands, 'help' for kernel ones.\n\n");

    char line[256];
    for (;;) {
        puts("advent$ ");
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) continue;

        /* Local commands first. */
        if (strcmp(line, "uhelp") == 0)        { cmd_uhelp(); continue; }
        if (strcmp(line, "upid")  == 0)        { printf("user pid: %d\n", sys_getpid()); continue; }
        if (strcmp(line, "utime") == 0)        { printf("epoch: %u\n",   sys_time());   continue; }
        if (strcmp(line, "uexit") == 0)        { puts("bye\n"); sys_exit(0); }
        if (starts_with(line, "usleep "))      { cmd_usleep(line + 7); continue; }

        /* Everything else: hand to the kernel. */
        sys_kcmd(line);
    }
}
```

Local commands (`upid`, `utime`, `uhelp`, `usleep`, `uexit`) handle small things in user mode using direct syscalls. Everything else falls through to `sys_kcmd`. This isn't elegant — it's pragmatic. Re-implementing all 25+ kernel shell commands as their own user-mode implementations would mean: separate syscalls for every kernel feature (`SYS_LS`, `SYS_TASKS`, `SYS_PING`, ...), or one bigger generic-state-query syscall. Either way the work-to-demo ratio is bad. The passthrough demonstrates the boundary clearly.

The `advent$` prompt (vs the kernel shell's `advent>`) is a deliberate visual cue that you're in user mode now.

## Boot path — sh.elf as pid 1

```c
int fd = fs_open("sh.elf");
if (fd >= 0) {
    struct elf_load_result r;
    if (elf_load(fd, &r) == 0) {
        struct task *t = task_create_user(r.entry, r.user_esp, r.cr3, "sh");
        if (t) {
            kprintf("[boot] launched userspace shell (sh.elf) as pid %u\n\n", t->id);
            for (;;) __asm__ volatile ("sti; hlt");   /* kmain becomes idle */
        }
    }
}
shell_run();   /* fallback */
```

If sh.elf loads, kmain becomes the idle task — a `sti; hlt` loop forever. The user shell becomes the active interactive task. If sh.elf isn't available (FS empty, ELF malformed), kmain falls back to running the in-kernel shell.

When the user types `uexit`, the user task `sys_exit`s, the reaper frees its pages, and... nothing else happens. We don't auto-respawn. Bringing up a fresh shell on demand would be a `SYS_SPAWN_SHELL` or similar — left for later.

## Files added / modified

| File | Change |
|---|---|
| `kernel/tcp.{h,c}` | New — state machine, segment build, checksum |
| `kernel/http.{h,c}` | New — listener + canned response |
| `kernel/ip.c` | Dispatch IP_PROTO_TCP to `tcp_rx` |
| `kernel/shell.{h,c}` | Expose `kshell_read_line` / `kshell_run_line` |
| `kernel/syscall.{h,c}` | New `SYS_READ_LINE`, `SYS_KCMD` |
| `kernel/kernel.c` | `tcp_init`, `http_init`, then load `sh.elf` as pid 1; idle on success |
| `user/libuser.{h,c}` | Wrappers for the new syscalls; `strcmp`/`strncmp` |
| `user/sh.c` | New — userspace shell |
| `build.sh` | `USER_PROGS+=(sh)`; runline includes `hostfwd=tcp::8080-:80` |
| `mkfs.py` | Pack `sh.elf` into the FS image |

## Test trace

```
[boot] starting network stack
rtl8139: PCI 0:3.0  io=0xc000  irq=11
net: link up — MAC 52:54:00:12:34:56  IP 10.0.2.15  GW 10.0.2.2
[boot] starting HTTP server on port 80
[boot] launched userspace shell (sh.elf) as pid 4

AdventOS userspace shell, pid=4
Type 'uhelp' for shell-local commands, 'help' for kernel ones.

advent$ ifconfig
eth0:
    HWaddr  52:54:00:12:34:56
    inet    10.0.2.15
    netmask 255.255.255.0
    gateway 10.0.2.2
advent$ ping 10.0.2.2
PONG from 10.0.2.2  seq=1  time=0 ms
advent$ tasks
 ID  STATE  NAME             ESP         SWITCHES
 0  READY  kmain            ...
 1  READY  reaper           ...
 2  READY  demo_a           ...
 3  READY  demo_b           ...
 4  RUN    sh               ...        <-- current  (user pid 1)
advent$ uexit
bye
[user task pid=4 exited code=0]
[reaper] freed pid=4 (user task), slot 4 now UNUSED
```

And on the host side:

```
$ curl http://localhost:8080/
Hello from AdventOS!
Userspace shell + minimal TCP/IP/HTTP working.
Path: served by kernel/http.c after a real 3-way handshake.

$ curl http://localhost:8080/                  # second connection
Hello from AdventOS!
...
```

Two consecutive curls: each completes a fresh 3-way handshake, gets the response, the server closes (FIN_WAIT_1 → ... → CLOSED), and `back_to_listen` auto-rearms before the next SYN arrives.

## Design decisions

**One global TCB.** Real kernels keep a hashmap keyed on `(local_port, remote_ip, remote_port)` so multiple concurrent connections to the same listening socket don't step on each other. We accept exactly one connection at a time. For HTTP-as-demo it's fine; trying to handle two parallel curls would race.

**No retransmission.** A real TCP starts a retransmit timer for every unacked segment. We do the simplest thing: send once, hope for the best. Over SLIRP loopback there's no loss; this hasn't bitten us.

**Skip TIME_WAIT.** Real TCP holds the connection in TIME_WAIT for `2 × MSL` to absorb stray duplicates from the previous incarnation. We jump straight to LISTEN. Lets us serve two curls back-to-back without a delay; risks confusing some edge cases.

**TCP checksum via flat-buffer + `ip_checksum`.** Already documented above. Simpler is more correct.

**Canned HTTP response.** Real HTTP would parse the request line for path, route to handlers, support `Connection: keep-alive`, `Transfer-Encoding: chunked`, etc. None of that is the point — the point is "TCP and HTTP work end-to-end." Plain text body, `Connection: close`.

**Userspace shell as a thin SYS_KCMD wrapper.** The honest tradeoff. Re-implementing every kernel command in user mode would be a lot of work for a thin demo benefit. The `sh.elf` path is the visible "shell runs in ring 3" change; the *behavior* of running commands is identical to the kernel shell because the kernel still hosts the implementations.

**No retry / shell respawn.** `uexit` ends the user shell and the system is functionally idle (kmain hlts). A real init system would respawn. We accept the rough edge.

## Pitfalls

1. **Pseudo-header byte order matters.** `proto` and `len` go at offsets 9 and 10-11 respectively, in network order (big-endian for the 16-bit length). Getting the layout wrong produces a non-RST-able silent drop on the peer side.
2. **SYN and FIN each consume one sequence number.** Forgetting `snd_nxt += 1` after sending SYN-ACK means the peer's ACK will look "old" to your state machine and you'll never reach ESTABLISHED.
3. **The listener has to re-arm after the connection closes.** A TCB that's been left in CLOSED won't accept the next SYN.
4. **`tcp_send` should set the PSH flag** if you want the peer's stack to deliver data to its application immediately (i.e., not wait for more bytes). Without it, curl would hang waiting on a kernel readahead.
5. **`SYS_KCMD` must copy the user string.** `kshell_run_line` modifies the buffer (NUL-terminates the verb). Passing the user pointer directly would corrupt user memory.
6. **`SYS_READ_LINE`'s buffer is in the user PD.** We're in the syscall handler with the user's CR3 still live, so the kernel can write to the user buffer directly. If we ever introduced a kernel-thread context that wasn't using a user PD, this would need rework.
7. **`__main` stub still needed in libuser** (from session 9). Each new libuser-built program still triggers the mingw32 auto-emit; the stub absorbs it.
8. **`-fno-zero-initialized-in-bss`** on the user side keeps `filesz == memsz` in the ELF wrapper, so mkfs.py doesn't need to know about `.bss` size. Critical for the user shell's `static const char *g_prompt` etc. landing in `.data` instead of `.bss`.

## What session 12 might be

UDP (trivial — copy TCP's pseudo-header but skip the state machine). DHCP would fall out of UDP. DNS too. Or a real socket API in libuser, so user programs can open their own listeners. Or `argv`/`environ` to user main, so programs can take command-line arguments — would let `exec /bin/cat hello.txt` work the way you'd expect.
