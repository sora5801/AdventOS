# Session 50 — SSH server (TLS-backed remote shell)

**Goal:** stand up a real remote-shell service for AdventOS. Login over the network using the same `/etc/passwd` accounts (salt + SHA-256), authenticated over a TLS 1.3 cert-flow handshake against a self-signed ECDSA-P256 cert, executing each command through the full session-49 shell (pipelines, env vars, builtins). Two new programs: `sshd.elf` (server) and `ssh.elf` (client). End-to-end loopback selftest exercising the whole stack.

End state — `[t33]` selftest, all six assertions PASS:

```
[t33] sshd: TLS-backed remote shell loopback (login, exec, exit)
sshd: TLS handshake OK; entering auth
sshd: authenticated user=guest uid=1000
  captured 358 bytes from ssh.elf
  ---- ssh.elf output ----
ssh: connecting to 127.0.0.1:2222 ...
ssh: TCP connected; starting TLS handshake
ssh: TLS 1.3 up; entering interactive session
AdventOS sshd over TLS 1.3 (ECDSA-P256)
login: password:
Welcome to AdventOS over TLS.  user=guest  shell=sh.elf -c
Type 'exit' to disconnect.

advent-ssh$ uid=1000 gid=1000 pid=50 pgid=50
advent-ssh$ bye

ssh: connection closed.
  ------------------------
  PASS  ssh.elf exited 0
  PASS  client logged TLS 1.3 handshake OK
  PASS  server sent 'login:' prompt over TLS
  PASS  server sent welcome banner after auth
  PASS  remote `id` printed uid=1000 (guest's uid)
  PASS  server acknowledged exit with 'bye'
```

## Naming caveat: not RFC-4253 SSH-2

What ships here is "SSH" in the AdventOS sense — a TLS-secured remote-shell service built from the three primitives the user asked us to combine: TLS 1.3 (sessions 39 / 43 / 45), the multi-user `/etc/passwd` flow (session 47), and the interactive shell with `sh -c` (session 49). It is **not** RFC-4253 SSH-2 — there is no `SSH-2.0` banner exchange, no `KEXINIT` / `NEWKEYS`, no algorithm negotiation, no channel multiplexing. An OpenSSH client cannot talk to it.

The motivation is honest: RFC 4250–4254 alone are dense enough to be their own multi-session project, and re-doing key exchange when we already have a battle-tested TLS 1.3 implementation would be a step sideways for AdventOS. So we pay the protocol-mismatch tax (no openssh interop) for the win of an immediate working stack.

## What's in scope

In:

- **`user/sshd.c`** — the server. Generates an ECDSA-P256 keypair from a hardcoded seed at startup, builds a self-signed X.509 cert (CN=`AdventOS sshd`), binds TCP/2222, accept-loops with fork-per-connection. Each child does the cert-mode TLS handshake, prompts the client for username + password (delimited by a `0x01` "your turn" sentinel), verifies salt+SHA-256 against `/etc/passwd`, `sys_setgid`+`sys_setuid` to the matched account, then runs a command loop: read line over TLS → fork+exec `sh.elf -c <line>` with stdout+stderr piped back → shuttle the pipe to the client → repeat. `cd` and `exit` are handled inline so cwd persists; everything else flows through the shell.
- **`user/ssh.c`** — the client. Parses `ssh <ip>[:port]`, connects, does the TLS client handshake (no validation — `-k` semantics), then shuttles bytes: read TLS bytes to stdout until the `0x01` sentinel arrives, prompt stdin, send the line back, repeat. EOF on stdin or seeing "exit\n" cleanly tears down.
- **`user/sh.c`** — gain `sh -c "command"` mode. Argv parsing learns the flag; when `-c` is present we skip the banner + interactive loop and run the single command through `execute_line` (so pipelines, env vars, redirects, all the session-49 builtins are available to remote commands).
- **`user/sshd.c` `tls_read_byte`** — userspace read buffer to work around a `tls_recv()` quirk that landed first: `libcrypto/tls.c:1050` truncates the plaintext to `max_n` bytes and **discards** the rest. Reading byte-by-byte therefore loses 5 of 6 bytes from a "guest\n" record and hangs on the next call.
- **`kernel/task.h`** — `TASK_MAX` 16 → 32. The new sshd flow forks three generations deep (sshd parent → per-conn child → `sh.elf -c` → executed command). Combined with init's resident `httpd` / `httpsd` / `sshd` daemons, the selftest shell, the inbound `ssh.elf` client, and kernel kthreads, the old 16-slot ceiling was hit at the very end of the chain — `fork() failed mid-pipeline`.
- **`build.sh`** — `sshd` and `ssh` added to `TLS_PROGS` (they need libcrypto for ECDSA-P256, X.509, and TLS 1.3).
- **`mkfs.py`** — both ELFs shipped on the image.
- **`fs/inittab`** — `once sshd.elf` line so the server starts at boot, in parallel with the existing httpd / httpsd.
- **`user/sh.c` selftest `[t33]`** — the loopback test described above.

Out:

- **No PTY pair.** A real sshd allocates a master/slave pseudo-terminal, hands the slave to the shell as `/dev/tty`, and uses select() on the TLS-master pair to shuttle bytes. AdventOS has neither ptys nor `select`/`poll`/non-blocking sockets. The compromise: run one `sh -c "<line>"` per command, drain its stdout fully, then loop. cd is the only state we preserve manually; env vars and command history evaporate between commands (use `source` if you really need them — though you can also just run a multi-line `sh -c` payload).
- **No bidirectional stdin during a command.** The forked `sh -c` inherits sshd's stdin (which is the original TTY, not the TLS socket), and we redirect its stdout/stderr to a pipe. Anything that reads stdin (`cat` with no args, `read` builtins) won't get input from the SSH client.
- **No SSH-2 wire compat.** See above. `ssh.elf` is the only thing that can talk to `sshd.elf`.
- **No host-key persistence.** The server keypair is seeded from a hardcoded 32-byte constant in `sshd.c`, so the cert fingerprint is stable across reboots — but it's not in `/etc/ssh/ssh_host_ecdsa_key` style storage. A real deployment would load from disk.
- **Password auth only.** `~/.ssh/authorized_keys` (pubkey auth, the typical real-world flow) isn't here; sessions 39/43 have the ECDSA verify primitives if we wanted to add it.

## The wire framing: 0x01 as "your turn"

The hardest design question for a TLS-shuttle approach is "how does the client know when to read stdin vs. read more from the server?" SSH-2 solves this with channel windows and explicit EOF/EXIT messages. Telnet solves it by being a half-duplex protocol where servers wait for line-terminated requests. We solve it the cheap way: a single byte.

After every block of server-side output that ends an interaction (every prompt, post-auth banner, post-command output), the server sends one `0x01` (ASCII SOH) byte. The client reads TLS bytes to stdout until it sees `0x01`, swallows that byte, and *then* reads stdin and sends the next line. The byte itself is invisible to the user — it's stripped before printout.

Server side:

```c
static int tls_send_ready(struct tls_conn *t) {
    char b = 0x01;
    return tls_send(t, &b, 1);
}
/* ... after every prompt + after every command's output: */
tls_send_str(t, "advent-ssh$ ");
tls_send_ready(t);
```

Client side:

```c
static int drain_until_ready(struct tls_conn *t) {
    char buf[1024];
    for (;;) {
        int n = tls_recv(t, buf, sizeof(buf));
        if (n <= 0) return (n == 0) ? 0 : -1;
        int sentinel = -1;
        for (int i = 0; i < n; i++) if (buf[i] == 0x01) { sentinel = i; break; }
        int print_end = (sentinel < 0) ? n : sentinel;
        if (print_end > 0) sys_write(1, buf, print_end);
        if (sentinel >= 0) {
            /* bytes after the sentinel (rare) flush to stdout for the next round */
            if (sentinel + 1 < n) sys_write(1, buf + sentinel + 1, n - sentinel - 1);
            return 1;
        }
    }
}
```

The choice of `0x01` is just "guaranteed to never appear in normal stdout text" — terminals send 0x01 for Ctrl-A, but our session shell doesn't generate it, and our command output is normal printable ASCII. Anything 0x00–0x1F that isn't whitespace would work; pick something not even vim emits.

If a command's stdout did legitimately contain `0x01`, the client would prematurely think the prompt is up and would try to read stdin out of phase. The fix in that future would be either a length-prefix protocol or escaping. For now it's a known caveat.

## The big tls_recv data-loss footgun

The first sshd implementation was straightforward — read a line byte-by-byte:

```c
static int tls_read_line(struct tls_conn *t, char *out, int cap) {
    int len = 0;
    for (;;) {
        char c;
        int n = tls_recv(t, &c, 1);   /* read 1 byte at a time */
        if (n <= 0) return -1;
        if (c == '\n') break;
        ...
    }
}
```

The selftest hung. The serial log showed:

```
sshd: TLS handshake OK; entering auth
[B][A][A][B][A][B][A][A][B][A][B][A]...   ← endless CPU markers from a busy kernel
```

The culprit lives in `libcrypto/tls.c`:

```c
int tls_recv(struct tls_conn *c, void *out, int max_n) {
    static uint8_t buf[TLS_MAX_FRAGMENT + 16];
    for (;;) {
        int len;
        int it = recv_record(c, k, buf, &len);
        ...
        if (len > max_n) len = max_n;    /* ← silent truncation */
        for (int i = 0; i < len; i++) ((uint8_t *)out)[i] = buf[i];
        return len;
    }
}
```

`recv_record` drains one whole TLS record (6 bytes of `"guest\n"` plaintext) into `buf`, then `tls_recv` copies `min(record_len, max_n)` to the caller. With `max_n = 1`, **5 of 6 bytes are discarded.** The next `tls_recv(..., 1)` call waits for a *fresh* record, which the client isn't sending — so sshd blocks indefinitely on `recv_record`, the connection sits dead, and from the host's QEMU we see the scheduler spinning trying to find work for the AP CPUs while sshd's child is parked on `sys_read`.

The fix is two-layered:

1. **Userspace buffering** — sshd keeps its own ring (one per forked child, so no cross-connection bleed) and only invokes `tls_recv` when the buffer drains. The buffer is sized to `TLS_MAX_FRAGMENT` so one `tls_recv` of the maximum record never overflows.

   ```c
   static struct rx_state {
       char buf[TLS_MAX_FRAGMENT];
       int  pos, end;
   } g_rx;

   static int tls_read_byte(struct tls_conn *t, char *out) {
       if (g_rx.pos >= g_rx.end) {
           int n = tls_recv(t, g_rx.buf, (int)sizeof(g_rx.buf));
           if (n <= 0) return -1;
           g_rx.end = n; g_rx.pos = 0;
       }
       *out = g_rx.buf[g_rx.pos++];
       return 1;
   }
   ```

2. **Caller discipline** — drains until newline using `tls_read_byte`. No more "ask the API for 1 byte and lose 5".

The client side `drain_until_ready` already used a 1024-byte buffer + in-memory scan, so it sidesteps this — the bug only bit on the server.

The proper fix would be in `tls_recv`: either return the partial record's leftover bytes on the next call (introduce per-connection record-buffer state in `tls_conn`), or document the API as "always pass `max_n >= TLS_MAX_FRAGMENT`". I went with userspace buffering in sshd because changing `tls_recv` semantics now would have a ripple effect through every libcrypto consumer (httpsd, httpsget, login). A separate session for the proper fix is the right call.

## The fork-chain depth and TASK_MAX

The other regression that the new flow exposed: `TASK_MAX` was 16 since session 26, which had been comfortable for everything we'd done — single-stage pipelines, single forks, nothing deeper than a parent + a few children. The SSH flow blows past that.

Counting concurrent processes at the moment t33 tries to launch `id.elf`:

```
init                            (pid 1)
httpd                            (pid 2-ish, from inittab)
httpsd                           (pid ~3)
sshd parent (listener)           (pid 9)
sh.elf running selftest          (pid ~5–8)
ssh.elf (selftest's child)       (pid 47)
sshd accept child                (pid 48)   ← TLS handshake + auth + shell loop
sh.elf -c "id.elf"               (pid 49)   ← forked by sshd child
id.elf                                       ← about to be forked by sh.elf -c
```

Nine user tasks plus kernel kthreads (idle, bcache syncer, network worker, etc.) — comfortably above 16. The fork inside `sh.elf -c`'s `run_pipeline` returned NULL because `find_free_slot()` found no `TASK_STATE_UNUSED` entries.

Bumped to 32. The choice is conservative — could go higher — but doubles headroom while keeping `g_tasks` small enough that the linear scan in `find_free_slot()` stays trivial. The TCB itself is ~120 bytes plus the 16-KiB kernel stack, so 32 slots cost ~512 KiB of kernel memory at full utilization. We have it.

## Why `sh -c` instead of long-running sh

The natural design alternative is to fork one `sh.elf` per connection, redirect its stdin/stdout/stderr to a pair of pipes, and shuttle bytes between TLS and those pipes for the life of the session. State (cwd, env vars, history) persists naturally. That's what real Unix sshd does (with a pty pair instead of plain pipes, for line-discipline + signal forwarding).

We didn't take that path because the shuttle is a hard problem without non-blocking I/O. The sshd process would have to wait on two things simultaneously: bytes from the TLS connection (to send to shell stdin) and bytes from shell stdout (to send to TLS). Without `select`/`poll`/`O_NONBLOCK`, the only way is to fork yet another helper, and that helper would need to share the TLS keys with sshd — which lives in user memory, not shareable across processes.

`sh -c` sidesteps this: each command is a self-contained subshell run, its stdout drained fully before reading the next TLS command. The cost is "no persistent shell state between commands"; the win is "we're done in a session." `cd` is handled inline in sshd (which lives across commands), so the most common stateful builtin still works.

When AdventOS grows pty pairs and non-blocking I/O — both reasonable session projects — we'll port to the long-running-shell shuttle.

## What you actually do with it

Build, boot with the standard `qemu -m 32 -smp 2 -netdev user`, and either:

```
# Inside the AdventOS shell (after the selftest finishes)
advent$ ssh 127.0.0.1
ssh: connecting to 127.0.0.1:2222 ...
ssh: TCP connected; starting TLS handshake
ssh: TLS 1.3 up; entering interactive session
AdventOS sshd over TLS 1.3 (ECDSA-P256)
login: guest
password: guest
Welcome to AdventOS over TLS.  user=guest  shell=sh.elf -c
Type 'exit' to disconnect.

advent-ssh$ id
uid=1000 gid=1000 pid=N pgid=N
advent-ssh$ ls /etc
  inittab
  passwd
advent-ssh$ cat /etc/passwd | wc -l
2
advent-ssh$ cd /
advent-ssh$ pwd
/
advent-ssh$ exit
bye

ssh: connection closed.
```

Notice `cat /etc/passwd | wc -l` — that's a pipeline running inside `sh -c`, working unchanged because session 49's tokenizer + run_pipeline handle it natively. Same for `>` redirection, env vars, the lot.

The host side can also dial in with `openssl s_client -connect localhost:2222` (with the standard QEMU `hostfwd=tcp::2222-:2222`). The TLS handshake gets to "verify error: self-signed certificate" then fails on `tls12_check_peer_sigalg` — openssl rejects the cert's signature algorithm. Curl's Schannel fails earlier. Both are interoperability gaps with our TLS-cert flow that already exist for httpsd; for AdventOS-to-AdventOS the loopback works cleanly.

## Files touched

```
user/sshd.c                  +374 new       TLS server + login + per-conn shell loop
user/ssh.c                   +178 new       TLS client + 0x01-sentinel shuttle
user/sh.c                    +118 -6        sh -c mode; [t33] loopback selftest
kernel/task.h                +9 -1          TASK_MAX 16 → 32
build.sh                     +1 -1          sshd, ssh added to TLS_PROGS
mkfs.py                      +3             both ELFs shipped on AdventFS
fs/inittab                   +1             once sshd.elf
docs/50-ssh-server.md        +new           this file
```
