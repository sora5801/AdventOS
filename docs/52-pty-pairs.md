# Session 52 — Pseudo-terminal pairs

**Goal:** add pty pairs to the kernel and wire them into sshd / ssh.elf so SSH can offer a real interactive shell — not the `sh.elf -c "<cmd>"` one-shot from session 51. The shell stays alive across commands; bytes flow bidirectionally between the SSH channel and the shell over pty master/slave fds.

End state — `[t34]` selftest, all five direct-pty-mechanics assertions PASS:

```
[t34] pty pairs: kernel master/slave rings for SSH bidi shuttle
  PASS  sys_openpty returns 0
  PASS  write master 18 bytes
  PASS  slave reads back what master wrote (m_to_s ring)
  PASS  master reads back what slave wrote (s_to_m ring)
  PASS  master read returns 0 (EOF) after slave close
```

Plus the SSH-2 protocol side: sshd accepts `pty-req` (returns `CHANNEL_SUCCESS` instead of session-51's `CHANNEL_FAILURE`), `shell` requests allocate a real pty and fork sh.elf interactively, and the bidirectional shuttle architecture is in place — see the **Known gap** section for what still needs work end-to-end.

## What's in scope

In:

- **`kernel/pty.h` + `kernel/pty.c`** — new module. `struct pty` holds two independent SPSC byte rings: `m_to_s` (master writes / slave reads) and `s_to_m` (slave writes / master reads). Each end has its own refcount, so dup2/fork bump and close decrements correctly. Read blocks via `task_yield` until bytes arrive OR the other side has fully closed. Mirrors `kernel/pipe.c`'s pattern exactly, with the byte stream simply duplicated for the two directions.
- **`kernel/task.h`** — adds `FD_PTY_M` and `FD_PTY_S` to the fd-kind enum.
- **`kernel/syscall.h` + `kernel/syscall.c`** — wires:
  - `SYS_OPENPTY = 71` — returns `fds[0] = master`, `fds[1] = slave` (mirrors `SYS_PIPE`).
  - `FD_PTY_M` / `FD_PTY_S` cases in `SYS_READ` / `SYS_WRITE_FD` / `SYS_CLOSE` / `SYS_DUP2`.
  - The fork-time fd-table inheritance loop in `kernel/task.c` bumps `pty_inc_master` / `pty_inc_slave` per inherited PTY fd.
- **`user/libuser.h` + `user/libuser.c`** — declares + defines `int sys_openpty(int fds[2])`, the userspace wrapper around the new syscall (just an `int $0x80` shim).
- **`user/sshd.c`** — the channel-request handler returns `CHANNEL_SUCCESS` for `pty-req` (was `CHANNEL_FAILURE` in session 51). The new `run_shell` opens a pty, forks two helpers, then execs `sh.elf` with the slave end as fd 0/1/2 — see the architecture below.
- **`user/ssh.c`** — argv parsing: 4 args (no command) selects shell mode. `do_pty_request` + `do_shell_request` send the protocol bits; `run_shell_shuttle` does the client side of the bidirectional shuttle.
- **`user/sh.c`** — `[t34]` selftest covering the kernel pty mechanics in isolation.

Out (deferred — see the gap section):

- **Per-pty line discipline** (icanon/echo). The pty is always raw passthrough; user-space sh.elf does the line editing.
- **Window-size ioctls** (TIOCGWINSZ/TIOCSWINSZ). pty-req's payload includes width/height but we ignore it.
- **Controlling-tty semantics for signals** (SIGINT on Ctrl-C in the pty). Our signal stack isn't wired through here yet.

## The two-ring pty model

A pty is two byte streams glued together. We don't have a single shared ring with two ends — we have **two independent rings**:

```
   master end                                     slave end
   ───────────                                    ──────────
   master_write ──► m_to_s ring ──► slave_read       (shell stdin)
   master_read  ◄── s_to_m ring ◄── slave_write      (shell stdout)
```

Each ring has its own `head` / `tail` and is producer/consumer-paired:

```c
struct pty {
    int                in_use;
    volatile int       master_refs;
    volatile int       slave_refs;

    volatile uint32_t  m_to_s_head, m_to_s_tail;
    uint8_t            m_to_s[PTY_BUF_SZ];

    volatile uint32_t  s_to_m_head, s_to_m_tail;
    uint8_t            s_to_m[PTY_BUF_SZ];
};
```

`master_read` drains `s_to_m`; `slave_read` drains `m_to_s`. Symmetric writes. The block-wait condition for either reader is "ring empty AND other side still open" — the other side closing while the ring is empty translates to a 0 return (POSIX EOF).

Each end has its own refcount so dup2 and fork can hand out additional fds. When `master_refs` drops to zero, the slave's writes start returning -1 (POSIX EPIPE-ish); when `slave_refs` hits zero, the master's reads return 0 EOF. Both at zero → `in_use = 0` and the pty slot goes back to the free list.

The pattern is a direct port of `kernel/pipe.c`. The only addition is the second ring; the refcount + try_free logic is identical.

## How sshd uses the pty

`run_shell` (the body of CHANNEL_REQUEST "shell") is a 3-way fork:

```
sshd accept-child (after auth + setuid to the matched user)
  │
  ├─ sys_openpty(pty)              master=fd3, slave=fd5
  │
  ├─ fork TX helper  (FIRST — see fork-order note below)
  │    │
  │    └─ loop: read(master) → CHANNEL_DATA → loop
  │
  ├─ fork shell
  │    │
  │    ├─ dup2(slave, 0); dup2(slave, 1); dup2(slave, 2)
  │    ├─ close(master); close(slave)
  │    └─ exec sh.elf  (interactive: read_line_interactive runs)
  │
  ├─ close(slave)                  parent doesn't hold slave anymore
  │
  └─ RX loop (parent itself)
       └─ loop: recv_packet → CHANNEL_DATA → write(master) → loop
```

Why **three** processes (not two): without `select`/`poll`/non-blocking I/O, a single process can't wait on both the TCP socket and the pty master at the same time. Splitting into TX (master → SSH) and RX (SSH → master) lets each side do one blocking read at a time. TX uses `key_s2c` + `iv_s2c`; RX uses `key_c2s` + `iv_c2s` — they each only touch their own AEAD direction, so after fork they share no live state.

A **fork-ordering quirk** showed up while wiring this: when the shell forks *first* and the TX helper forks *second*, the TX child stalls after its first syscall — the task is created (we see `[exec]` for the shell) but TX never makes progress past one `sys_write`. Forking TX *first* and then the shell works correctly: the TX child runs to completion in standalone tests, and the kernel pty traces confirm `master_read` is called and returns the shell's 31-byte banner. The cause is likely something the shell's `task_exec_inplace → paging_destroy_user_pd` path leaves in a transient state that affects the next fork on the same parent. Filed as a follow-up; the workaround (TX first) is in place.

## How ssh.elf uses the pty

The client side does the symmetric split. `run_shell_shuttle` forks **TX** (reads local stdin, sends as CHANNEL_DATA) and **parent becomes RX** (drains incoming CHANNEL_DATA to stdout). At session end the parent kills TX with `SIGTERM` to break it out of its blocking `sys_read` on stdin.

Client argv:

```
ssh <ip>[:port] <user> <password>            ← shell mode (pty)
ssh <ip>[:port] <user> <password> <command>  ← exec mode (no pty, session 51 path)
```

The exec path is unchanged from session 51 — for a single one-shot command, the pty machinery is unnecessary overhead, so `do_exec_request` + `drain_session` is still what runs.

## The protocol bits

Server side, in `do_channel_requests`:

```c
if (tl == 7 && t[0]=='p' && /* "pty-req" */ ) {
    /* Accept. Terminal modes are in the payload but we ignore them
     * — our pty is always raw and sh.elf handles line editing in
     * userspace. */
    handled_ok = 1;
}
```

That's the only line that changed for `pty-req`: was `handled_ok = 0` (CHANNEL_FAILURE), now `handled_ok = 1` (CHANNEL_SUCCESS). The shell request handler also dispatches to the new pty-backed `run_shell` instead of the session-51 placeholder.

Client side, `do_pty_request` sends a minimal but spec-shaped pty-req:

```
byte    SSH_MSG_CHANNEL_REQUEST
uint32  server_chan
string  "pty-req"
boolean want_reply (true)
string  TERM ("xterm-256color")
uint32  width  (80)
uint32  height (24)
uint32  width_px  (0)
uint32  height_px (0)
string  modes (empty — server ignores)
```

OpenSSH clients send a similar shape but with non-empty termios modes (lots of cc[ic] settings). Our server ignores them and treats every pty as raw, so the empty modes string is fine for our own client. Real OpenSSH still works because we accept its pty-req regardless of mode bytes.

## What [t34] tests, and what it doesn't

`[t34]` exercises the **kernel** pty mechanics in isolation: open a pty, write master / read slave (one direction), write slave / read master (other direction), close one end and verify the other sees EOF. All five assertions PASS reliably.

What [t34] **doesn't** test, and the reason: an in-OS loopback of the *full* SSH-2 + pty + interactive-shell flow needs both sshd and ssh.elf to do their multi-process shuttle simultaneously. With our scheduler + fork model, this end-to-end flow has timing issues that the simpler `[t33]` (SSH-2 exec mode) doesn't hit — see the gap section.

Manual testing of the SSH-2 + pty integration still works through openssh's `-T`-style flows (`ssh user@host '<cmd>'` continues to work via session 51's exec path, unchanged). The interactive `ssh user@host` (which requests a pty) reaches "pty allocated", forks the shell, the shell's banner gets into the s_to_m ring — captured by kernel pty traces — but the byte stream stalls before reaching the client end. The root cause appears to be in the multi-process shuttle on top of `task_fork`-after-exec rather than in the pty itself; addressed in the gap section below.

## Known gap: end-to-end SSH+pty interactive flow

What works:
- Kernel pty pairs (test PASSes)
- Direct `sys_openpty` from userspace returns master + slave fds
- Bytes flow through both rings correctly within a single process
- EOF propagation when a side closes

What needs more work:
- **Bidirectional shuttle across multiple forked processes hits a kernel-side timing issue**: after sshd does a forked + exec'd child (the shell), a second fork (the TX helper) doesn't reliably reach its first syscall, even though the kernel `[exec]` trace shows the shell was loaded.
- Workaround in place: fork TX *before* shell, which restores TX's ability to make progress. But the full shuttle still has the master_read → send_channel_data path stalling after the first 31-byte banner is forwarded.
- The pty pairs themselves are bug-free under direct use; the issue is in the multi-process orchestration on top.

A reasonable next step is to **add a non-blocking pty master read** (a `pty_master_read_nb` that returns 0 instead of blocking) so we can collapse the 3-process shuttle into 1 process with a `yield`-driven polling loop. That avoids the fork-after-fork-after-exec sequence entirely and sidesteps the timing quirk. Out of scope for this session.

## Files touched

```
kernel/pty.h            +new      pty struct, ring-buffer API
kernel/pty.c            +new      implementation (pipe.c shape, two rings)
kernel/task.h           +2        FD_PTY_M, FD_PTY_S
kernel/task.c           +2        fork's fd-inheritance ref-bumps
kernel/syscall.h        +3        SYS_OPENPTY = 71
kernel/syscall.c        +30       FD_PTY_* cases in read/write/close/dup2
                                    + the SYS_OPENPTY entry point
kernel/kernel.c         +1        pty_init() at boot
user/libuser.h          +5        sys_openpty prototype + comment
user/libuser.c          +9        sys_openpty asm shim
user/sshd.c             ~modified pty-req accept + run_shell with shuttle
user/ssh.c              ~modified shell-mode argv + bidi shuttle
user/sh.c               +50       [t34] pty mechanics selftest
docs/52-pty-pairs.md    +new      this file
```
