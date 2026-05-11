# Session 56 — PTY-driven signals, SSH rekey, ext-info

**Goal:** three thematically-related upgrades to the terminal + SSH stack:

1. **PTY line discipline (ISIG).** When the master end of a pty receives `Ctrl-C` / `Ctrl-\` / `Ctrl-Z`, intercept it in the kernel and send `SIGINT` / `SIGQUIT` / `SIGTSTP` to the slave's foreground process group — the way a real Unix tty driver does. Closes the "no ^C in an SSH pty session" gap left by session 52.

2. **SSH `EXT_INFO` / `server-sig-algs` (RFC 8308).** Advertise `ext-info-c` in our `KEXINIT` (client) and `ext-info-s` semantics (server), so the server sends `SSH_MSG_EXT_INFO` with `server-sig-algs = ssh-ed25519` right after the first `NEWKEYS`. This is the one extension that matters for real-world OpenSSH interop — clients need it to know which signature algorithm to use for publickey auth.

3. **SSH rekey (RFC 4253 §9 + 7.2).** Either side may initiate a re-key at any time during a live session. Server and client both refactor `do_kex_ecdh` / `do_newkeys` to take an `initial` flag, freeze `session_id` only on the first KEX, and propagate the post-rekey keys through a fork/respawn dance for the shell-mode TX helper.

Plus three drive-by kernel fixes that turned up as soon as the rekey path tried to kill a blocked task — laid out in **§5** because they're independently interesting.

The new `[t38]` selftest, all 4/4 PASS:

```
[t38] PTY signals + SSH ext-info + SSH rekey
  PASS  sys_openpty allocates pty for ISIG test
  PASS  master_write(0x03) returns 1 (byte consumed)
  PASS  child caught SIGINT via fg_pgrp (NOT bytes on slave_read)
  PASS  client received SSH_MSG_EXT_INFO after initial NEWKEYS
  PASS  EXT_INFO carried server-sig-algs extension
  PASS  client completed mid-session rekey (session_id preserved)
  PASS  id.elf output (uid=1000) flowed through post-rekey keys
```

Full selftest count: **86 PASS, 0 FAIL**, no regressions.

---

## 1. PTY line discipline (ISIG)

### The model

A POSIX tty driver sits between the data path and the slave-side process. When a process running on the slave end of a pty types `^C`, the driver:

- **does NOT** pass the `0x03` byte through to the slave's `read()`,
- **DOES** deliver `SIGINT` to the slave-tty's foreground process group.

The foreground process group is set by `tcsetpgrp(slave_fd, pgid)` and the conversion is gated by the `ISIG` mode bit (default ON). Same idea for `^\` → `SIGQUIT` and `^Z` → `SIGTSTP`.

Before this session, AdventOS pty was a dumb byte pipe with no line discipline — bytes flowed through verbatim. That worked for echoing but it meant Ctrl-C in an SSH-pty shell did nothing useful: the byte landed in the shell's `read_line` buffer instead of interrupting a running command.

### Implementation

`struct pty` grows two volatile fields:

```c
struct pty {
    ...
    volatile int fg_pgrp;   /* set by tcsetpgrp(slave, pgid) */
    volatile int mode;      /* bitmask, PTY_MODE_ISIG = 0x01 */
};
```

`pty_new()` defaults `mode = PTY_MODE_ISIG` — matching what's "right" for a fresh login pty. `pty_master_write` checks each incoming byte against the three control characters and, if ISIG is on and `fg_pgrp > 0`, calls `signal_send_pgrp` instead of pushing into the m→s ring:

```c
if (p->mode & PTY_MODE_ISIG) {
    int sig = 0;
    if      (b == 0x03) sig = SIGINT;    /* ^C */
    else if (b == 0x1C) sig = SIGQUIT;   /* ^\ */
    else if (b == 0x1A) sig = SIGTSTP;   /* ^Z */
    if (sig && p->fg_pgrp > 0) {
        signal_send_pgrp((uint32_t)p->fg_pgrp, sig);
        copied++;        /* count the byte as written... */
        continue;        /* ...but don't push it to the slave */
    }
}
```

The byte is "consumed" — `pty_master_write` returns `copied == n` so the caller can't tell anything was filtered. The reader (the slave-side process) just never sees that byte.

### Wiring the foreground pgrp

`SYS_TCSETPGRP` was already in the syscall table (session 47, console TTY). It used to unconditionally route to the global `tty_set_fg_pgrp`. Now it routes by fd type:

```c
case SYS_TCSETPGRP: {
    int fd = (int)a;
    uint32_t pgid = (uint32_t)b;
    struct task *t = task_current();
    if (fd >= 0 && fd < TASK_MAX_FDS &&
        t->fds[fd].kind == FD_PTY_S) {
        pty_set_fg_pgrp(t->fds[fd].obj_idx, (int)pgid);
    } else {
        tty_set_fg_pgrp(pgid);   /* console fallback */
    }
    ret = 0;
    break;
}
```

Routing by **slave fd only** is intentional: in real Unix `tcsetpgrp` is a tty operation. The master end isn't a tty, it's the byte-pump driving the tty. If the test passed the master fd we'd be clobbering the wrong fg_pgrp.

### Test

The t38 part 1 wire-up:

```
parent                       child
──────                       ─────
openpty(pty)                 inherits both via fork
                             setpgid(0,0)
                             sigaction(SIGINT, handler→exit 42)
                             sync ready byte
recv sync                    block in slave_read(pty[1])
tcsetpgrp(pty[1], child_pid)
sleep 20ms (child blocks)
master_write(0x03)           kernel: signal_send_pgrp →
                               sig_pending bit, signal_check
                               at iret runs handler → exit 42
wait(&code) → 42             — code 42 means "got SIGINT"
                             — code 99 would mean "read returned bytes"
```

Sentinel exit codes (42 vs 99) make the test self-disambiguating: any path where the byte leaks through to `read` is a distinct failure mode from "signal not delivered."

---

## 2. SSH `EXT_INFO` / `server-sig-algs` (RFC 8308)

### Why it matters

The transport protocol negotiates a host-key algorithm during KEX, but **userauth** sigs are a separate namespace. A client doing publickey auth has to pick a signature algorithm at the userauth layer that the server actually accepts. Real-world OpenSSH 8.2+ refuses `ssh-rsa` SHA-1 sigs but accepts `rsa-sha2-512` over the same RSA key — there's no way for the client to know that from KEX alone.

RFC 8308 solves it with `SSH_MSG_EXT_INFO`. The dance:

1. Client adds the pseudo-algorithm `ext-info-c` to its KEXINIT `kex_algorithms` name-list. This is a flag, not a real KEX — it's never selected.
2. Server (if it also supports the mechanism) sends `SSH_MSG_EXT_INFO` immediately after its first `NEWKEYS`. The body is `(uint32 nr_extensions, repeated (string name, string value))`.
3. The one extension that's actually deployed at scale is `server-sig-algs`: a comma-separated list of signature algorithm names the server will accept for publickey userauth.

### Server side

`recv_kexinit` walks the kex_algorithms name-list and sets a flag if `ext-info-c` is present:

```c
/* name-list = comma-separated; needle matches with word-boundary check */
c->client_wants_ext_info =
    name_list_contains(c->i_c + 17 + 4 /* skip type+cookie+len */,
                       kex_algos_len, "ext-info-c");
```

The `name_list_contains` helper is hand-rolled because libcrypto/ssh doesn't have a string utility for it. Substring match alone would false-positive on `ext-info-cccccc`; the check requires `,` or end-of-list on each side:

```c
if (i > 0 && list[i - 1] != ',') continue;       /* boundary before */
...
if (i + nlen == list_len || list[i + nlen] == ',') return 1; /* boundary after */
```

After `do_newkeys` finishes its KDF and bumps `enc_in = 1`, it sends EXT_INFO **if and only if** this is the initial KEX:

```c
if (initial && c->client_wants_ext_info) {
    uint8_t pkt[128];
    uint8_t *p = pkt;
    ssh_put_u8(&p, SSH_MSG_EXT_INFO);
    ssh_put_u32(&p, 1);
    ssh_put_cstring(&p, "server-sig-algs");
    ssh_put_cstring(&p, "ssh-ed25519");
    send_packet(c, pkt, (int)(p - pkt));
}
```

Only `ssh-ed25519` because that's the only signature algorithm AdventOS userauth knows. A future RSA / ECDSA addition would extend the value to `"ssh-ed25519,rsa-sha2-512,ecdsa-sha2-nistp256"`.

### Client side

The client advertises by tacking `,ext-info-c` onto the first algorithm in its KEXINIT:

```c
ssh_put_cstring(&p, "curve25519-sha256,ext-info-c");
```

Real OpenSSH puts it as its own entry in the list. Both forms are wire-equivalent — what matters is that the name appears with comma boundaries.

After receiving the server's NEWKEYS and switching keys, the client reads one more packet and treats it as EXT_INFO if the first byte matches:

```c
if (pn >= 1 && pkt[0] == SSH_MSG_EXT_INFO) {
    const uint8_t *p = pkt + 1, *end = pkt + pn;
    uint32_t nr = ssh_get_u32(&p);
    printf("ssh: server EXT_INFO with %u extension(s)\n", nr);
    for (uint32_t i = 0; i < nr && p < end; i++) {
        ... print "  ext: %s = %s" ...
    }
}
```

If the server didn't send EXT_INFO (peer doesn't support it), we lose one packet. A robust client would push it back into a one-element pending queue. AdventOS sshd always sends EXT_INFO when we ask, so we keep it simple.

---

## 3. SSH rekey (RFC 4253 §9)

### What rekey is for

Rekey isn't optional — it's expected behavior in any long-lived SSH session. Reasons:

- **Cryptographic hygiene.** Most modern advice is to rekey after ≤ 2³² packets or ≤ 1 GiB to avoid AEAD birthday-bound issues.
- **Forward secrecy step.** A compromise of the current keys doesn't compromise earlier or later traffic — each rekey runs fresh X25519 ECDHE.
- **Real OpenSSH** rekeys aggressively by default. A server that can't handle a mid-session `KEXINIT` from the client drops the connection.

### The state-id invariant

RFC 4253 §7.2 says **the first** exchange hash H becomes the session identifier — `session_id` — for the entire connection. Subsequent rekeys compute a new H (used as the salt for KDF), but `session_id` stays put. It's what userauth signatures hash in. If session_id changed on rekey, every cached pubkey auth attempt would silently break.

So both sides:
- compute H on every KEX (RFC's `K_S || Q_C || Q_S || mpint(K)`, plus the per-side banners and KEXINITs),
- assign `session_id = H` **only on the first** KEX.

In code, both server and client refactor `do_kex_ecdh` and `do_newkeys` to take an `initial` parameter:

```c
static int do_kex_ecdh(struct ssh_conn *c, int initial) {
    ...
    sha256_final(&sh, c->h);
    if (initial) {
        for (int i = 0; i < 32; i++) c->session_id[i] = c->h[i];
    }
    ...
}

static int do_newkeys(struct ssh_conn *c, int initial) {
    ... derive iv_*, key_* from c->h, c->session_id ...
    if (initial && c->client_wants_ext_info) { /* send EXT_INFO */ }
}
```

Note: client also re-verifies the host key on rekey — a server that pivots K_S mid-session is an active MITM attempt. Stored on initial, compared on subsequent:

```c
if (initial) {
    for (int i = 0; i < 32; i++) c->host_pk[i] = pkp[i];
} else {
    for (int i = 0; i < 32; i++) {
        if (pkp[i] != c->host_pk[i]) {
            puts("ssh: rekey host key changed — aborting\n");
            return -1;
        }
    }
}
```

### The wrinkle: the TX child

The server's shell mode (session 52) forks two helpers from the accept-child:

```
sshd accept-child   ← parent: RX loop (reads SSH packets, writes pty master)
   ├─ TX helper     ← child of parent: reads pty master, sends CHANNEL_DATA
   └─ shell.elf     ← child of parent: reads/writes pty slave
```

The parent and TX child share an SSH socket fd, but **each has its own COW copy of `struct ssh_conn`** with its own `iv_s2c` / `key_s2c`. They use disjoint directions (parent reads `iv_c2s`, TX writes `iv_s2c`), so divergence at fork time is fine — but it means **rekey can't update the TX child's copy**. If we rekey in the parent and let TX keep encrypting under the old key, the very next CHANNEL_DATA packet would arrive at the client with a stale-key tag and the client would disconnect.

Solution: kill the TX, do the rekey in the (now single-threaded) parent, then respawn TX. The new TX inherits the post-rekey keys at fork time.

### The flow

`run_shell` extended to handle `SSH_MSG_KEXINIT` in the RX loop:

```c
if (m == SSH_MSG_KEXINIT) {
    if (tx_pid > 0) {
        sys_kill(tx_pid, SIGTERM);
        int code; sys_wait(&code);
    }
    /* Stash the just-received KEXINIT into c->i_c so the H-hash
     * recompute inside do_kex_ecdh sees the right bytes. */
    for (int i = 0; i < n && i < sizeof(c->i_c); i++) c->i_c[i] = rxbuf[i];
    c->i_c_len = n;
    /* re-scan ext-info advertisement (defense in depth — we don't
     * resend EXT_INFO on rekey anyway). */
    ...
    if (do_rekey(c) < 0) break;
    tx_pid = spawn_tx(c, ch, master);   /* fresh fork → new keys */
    continue;
}
```

`do_rekey` is just the three calls in order, with `initial=0`:

```c
static int do_rekey(struct ssh_conn *c) {
    if (send_kexinit(c) < 0)      return -1;
    if (do_kex_ecdh(c, 0) < 0)    return -1;
    if (do_newkeys (c, 0) < 0)    return -1;
    return 0;
}
```

The client side mirrors it: a `@rekey;<cmd>` placeholder in `ssh.elf`'s command argument triggers shell-mode (so the server enters `run_shell`), drives rekey single-threaded before forking the shuttle, then pipes `<cmd>\nexit\n` as the remote shell's stdin.

### Why kill-then-respawn TX works

Three things have to hold for the TX child to disappear cleanly:

1. **`pty_master_read` is interruptible.** Previously it was an unconditional yield loop — a blocked TX never noticed SIGTERM. Now it checks `t->sig_pending & ~t->sig_mask` and returns `-1` if any signal is deliverable. Same fix in `pty_slave_read` for symmetry.

2. **`close_all_fds` drops pty refcounts.** Previously the switch in `close_all_fds` (called from `task_exit_current`) handled `FD_SOCK / FD_PIPE_R / FD_PIPE_W / FD_TMPFS` but **fell through default for `FD_PTY_M / FD_PTY_S`**. A SIGTERM'd TX left its master ref dangling, so the new TX's `master_read` never saw EOF when the shell exited — silent hang. See §5.

3. **`signal_check_and_deliver` releases BKL across `schedule()`.** The terminating task's syscall path holds the BKL; without an explicit handoff, the next task on this CPU spins forever trying to re-acquire. See §5.

All three of these are independent bugs masked by the "happy path" not exercising them. The rekey work surfaced all three within a few minutes of integration testing.

---

## 4. Verifying it works

End-to-end through t38 — client connects, KEXs, gets `EXT_INFO`, requests pty+shell, triggers rekey, pipes `id.elf\nexit\n` through the rotated keys, gets `uid=1000` back out:

```
ssh: server banner: SSH-2.0-AdventOS_1.0
ssh: KEX done, host key verified
ssh: server EXT_INFO with 1 extension(s)
  ext: server-sig-algs = ssh-ed25519
ssh: transport encrypted (aes128-gcm)
ssh: authenticated (password)
ssh: rekey-mode: shell up, initiating mid-session rekey
ssh: rekey complete, new transport keys live

AdventOS userspace shell, pid=60
Type 'help' for builtins. | > & are honored.

advent$ id.elf
uid=1000 gid=1000 pid=64 pgid=64
advent$ exit
bye
```

On the server side, the rekey is two log lines:

```
sshd: rekey requested by client
sshd: rekey complete, new keys live
```

between which the TX child (pid 59) is killed and a fresh one (pid 63) is forked with the new keys.

---

## 5. Three kernel fixes — independently interesting

These all turned up debugging the rekey path. Each was a latent bug masked by the original "happy path" being the only path that ran. Documenting because each is a single-line fix with a non-obvious failure mode.

### 5.1 `signal_send_pgrp` capped at the old TASK_MAX

`signal_send_pgrp` was loop-bound by a literal `16`, written when `TASK_MAX == 16`. Session 50 bumped `TASK_MAX` to `32` (sshd accept-children + their forks need more slots). The literal silently kept iterating only the first 16 task slots — so any process whose pid landed in slot 16+ couldn't receive a pgrp-targeted signal at all.

```c
for (uint32_t i = 0; i < TASK_MAX; i++) {   /* was hardcoded 16 */
    struct task *t = task_at(i);
    ...
}
```

This is exactly the kind of "we changed the constant but missed one literal" bug that no test catches until something needs to deliver a pgrp signal to a high-slot task. The PTY ISIG test would have caught it on a busy enough kernel.

### 5.2 `close_all_fds` ignored pty fds

When `task_exit_current` calls `close_all_fds`, it walks all open fds and decrements the per-kind refcount. The switch had cases for `FD_SOCK`, `FD_PIPE_R`, `FD_PIPE_W`, `FD_TMPFS` — and nothing for `FD_PTY_M` / `FD_PTY_S`. That meant a task with a pty fd that exited (cleanly OR via signal) left the pty's master/slave refs dangling forever.

Symptom: kill an sshd TX child mid-stream → its master ref never drops → new TX's `master_read` checks `slave_refs > 0` and blocks forever (slave is dropped because shell exits cleanly, but the OLD TX's slave ref was never released, so slave_refs stays > 0 forever). The new TX never sees EOF, the connection hangs.

Fix is two lines:

```c
case FD_PTY_M:   pty_close_master (e->obj_idx); break;
case FD_PTY_S:   pty_close_slave  (e->obj_idx); break;
```

The reason this stayed hidden so long: every PRIOR pty user (the single TX child of session 52's shell mode) only ever exited via natural EOF, and natural EOF goes through user-side `close()` syscalls that already decrement refs. SIGTERM'd-and-then-forgotten-about wasn't a path anyone exercised.

### 5.3 BKL held across `schedule()` in signal delivery

`signal_check_and_deliver` runs at the tail of `syscall_dispatch`, between `r->eax = ret` and the bottom-of-function `bkl_unlock()`. For most signals (handler installation, SIG_IGN, default-action that returns) this is fine — control flows back out and the trailing `bkl_unlock` runs.

For `ACT_TERM` (and `ACT_STOP`), control does NOT flow back: `task_exit_current` marks us a zombie and `schedule()` switches to another task. **The syscall-tail `bkl_unlock` never runs, so the BKL leaks**. The next task on this CPU spins in `bkl_lock` forever — kernel deadlock.

This was the deadlock that hung the t38 server log between "rekey: killing TX" and "rekey: TX reaped." The TX took SIGTERM correctly, got terminated correctly, but BKL stayed pinned to the dead TX's old CPU.

Fix: drop the BKL before `schedule` in both terminal paths. For `ACT_TERM` we don't reacquire (we're never coming back); for `ACT_STOP` we conditionally re-take on wakeup so the syscall-tail unlock is balanced:

```c
/* ACT_TERM */
task_exit_current(128 + sig);
if (bkl_held()) bkl_unlock();   /* hand off before context switch */
schedule();
for (;;) __asm__ volatile ("hlt");

/* ACT_STOP — same handoff, plus re-take on SIGCONT-driven resume */
int had_bkl = bkl_held();
if (had_bkl) bkl_unlock();
schedule();                      /* sleep until SIGCONT */
if (had_bkl) bkl_lock();
```

Same shape as `task_yield`'s already-existing `bkl_held()` → unlock → schedule → lock pattern, just specialized for the "we may never come back" terminate case.

---

## 6. Touched files

- `kernel/pty.{h,c}` — `fg_pgrp` + `mode` per-pty state; `PTY_MODE_ISIG` constant; signal-interruptible `pty_master_read` / `pty_slave_read`; getters/setters.
- `kernel/syscall.c` — `SYS_TCSETPGRP` / `SYS_TCGETPGRP` route by fd kind.
- `kernel/task.c` — `close_all_fds` handles `FD_PTY_M` / `FD_PTY_S`.
- `kernel/signal.c` — `TASK_MAX` constant; BKL handoff in `ACT_TERM` / `ACT_STOP`.
- `libcrypto/ssh.h` — `SSH_MSG_EXT_INFO` (7), `SSH_MSG_NEWCOMPRESS` (8) constants.
- `user/sshd.c` — `client_wants_ext_info`; `name_list_contains`; `do_kex_ecdh(c, initial)`; `do_newkeys(c, initial)` w/ EXT_INFO; `do_rekey`; `run_shell` KEXINIT handling + TX kill-respawn via factored-out `spawn_tx` / `run_shell_tx`.
- `user/ssh.c` — `ext-info-c` advertisement; refactored `do_kex_ecdh(c, initial)` / `do_newkeys(c, initial)` with EXT_INFO consumption + host-key recheck; `do_rekey`; `@rekey;<cmd>` placeholder in `main`.
- `user/sh.c` — `caught_sigint_t38` SIGINT handler; `[t38]` selftest with the seven sub-PASSes.

## 7. What's still out of scope

Everything previously documented (RSA host keys, host-key verification via known_hosts, port-forwarding, agent forwarding) is still out of scope. **New gaps from this session:**

- **Time-based rekey trigger.** A real server rekeys after N packets / M bytes / T seconds. AdventOS rekeys only when the client requests it. The mechanism is now there; we just don't have a heartbeat counter that decides "it's time."
- **Strict KEX message ordering.** RFC 4253 §7.1 mandates that once one side sends KEXINIT, only KEX-related messages are accepted until both sides finish. We rely on the client being well-behaved — a client that sends CHANNEL_DATA between KEXINIT and NEWKEYS would confuse us.
- **`name_list_contains` is byte-exact.** No case-folding, no whitespace tolerance. RFC 4250 §4.6 says name-lists are exact-match, so this is technically conformant — but real-world implementations sometimes get loose with whitespace. AdventOS-vs-AdventOS works; cross-vendor stress testing is a session 57+ topic.

Next plausible session: real-world OpenSSH `ssh` client connecting to AdventOS sshd (the rekey + ext-info work was the prerequisite for that).
