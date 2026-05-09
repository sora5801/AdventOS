# Session 18 — TTY layer with cooked / raw input modes

**Goal:** Give user programs control over how stdin is delivered. Two modes:

- **Canonical (cooked, line-buffered)** — the kernel echoes characters as the user types, handles backspace, and only delivers a complete line when Enter is pressed. This is what `kshell_read_line` has done since session 11.
- **Raw** — every keystroke is delivered to `read()` as it arrives. No line editing, no echo (unless explicitly enabled), no waiting for Enter. This is what an editor or any curses-style program needs.

Add three syscalls to flip / observe the mode and to inject keystrokes for headless testing. Make `SYS_READ` on fd 0 honor the current mode.

End state — the new `[t8]` selftest:

```
[t8] tty: raw-mode read with injection
  current mode: 0x3  (default = 0x3)
  set raw: now 0x0
  raw sys_read(stdin, ., 15) -> 6 bytes = 'ABCxyz'
  restored: now 0x3
```

Six bytes were injected with no trailing newline. In canonical mode the read would have blocked forever waiting for Enter. In raw mode the read returns `n = 6` immediately, with the bytes as-typed.

There's also a `keys` builtin in the shell for interactive use:

```
advent$ keys
Press keys (Enter to exit). Each keystroke arrives raw,
not waiting for newline — that's the whole point of raw mode.
  'a'  0x61
  'b'  0x62
  '?'  0x1b      ← ESC
  ...
(canonical mode restored)
```

`httpd.elf` keeps serving curl on :80 throughout.

## What's in scope

In:
- `kernel/tty.{h,c}` — single-global TTY with mode bitmap + the `tty_read` entry point that dispatches between canonical and raw
- `keyboard_inject(bytes, n)` — push raw bytes into the keyboard input ring as if they had been typed
- `SYS_TTY_SET_MODE = 28`, `SYS_TTY_GET_MODE = 29`, `SYS_TTY_INJECT = 30`
- `SYS_READ` on `FD_STDIN` now goes through `tty_read` (mode-aware)
- libuser `tty_set_mode` / `tty_get_mode` / `tty_inject` wrappers + flag constants
- `[t8]` selftest exercising raw-mode round-trip
- `keys` interactive builtin in the shell
- `tty_init()` called from kmain

Out:
- Per-task / per-pty mode (we have one global TTY for one global console)
- A real `struct termios` with `c_iflag` / `c_oflag` / `c_cflag` / `c_lflag` / `c_cc[]`
- VMIN / VTIME for partial-read with timeout
- ISIG (Ctrl-C → SIGINT delivery from the tty layer)
- ONLCR / OCRNL output translation
- Special control characters (VINTR, VQUIT, VEOF, VEOL, VERASE...) — backspace handling lives in `kshell_read_line` only
- Window-size queries (TIOCGWINSZ / TIOCSWINSZ)
- pty / forkpty
- `select`/`poll` on stdin
- A `tty_write` path — output still goes straight through `kputc`
- Echo control in canonical mode — the TTY_ECHO flag is conceptually wired up but the canonical reader (kshell_read_line) always echoes today

## Architecture: a thin layer between fd 0 and the input drivers

Before:

```
SYS_READ(fd=0)  ─→  kshell_read_line()  ─→  keyboard_wait_char()  ─→  kbd_buf
                    (canonical only)         (drains serial too)
```

After:

```
SYS_READ(fd=0)  ─→  tty_read()          ┬─→ kshell_read_line()  ─→ keyboard_wait_char()
                    (mode-aware)         │   (canonical: echo, edit, line-buffer)
                                         │
                                         └─→ raw path             ─→ keyboard_wait_char()
                                             (block for ≥1 byte,      keyboard_has_char()
                                              return what's available, keyboard_getc()
                                              optional echo)
```

`SYS_READ_LINE` (the dedicated line-read syscall from session 11) bypasses `tty_read` entirely and always uses `kshell_read_line` regardless of mode — that's the syscall the shell uses for its prompt loop, where the mode shouldn't matter.

The mode itself is a single bitmap of two flags:

```c
#define TTY_ICANON  0x01    /* canonical (line-buffered) */
#define TTY_ECHO    0x02    /* echo input back to console */

#define TTY_DEFAULT (TTY_ICANON | TTY_ECHO)   /* boot default — cooked */
#define TTY_RAW     0u                         /* raw, no echo         */
```

`TTY_DEFAULT` is the boot state and what the shell expects for line-edited input. A program that wants raw-mode keystrokes does:

```c
uint32_t prev = tty_get_mode();
tty_set_mode(TTY_RAW);
/* ... raw reads ... */
tty_set_mode(prev);   /* always restore — POSIX-style "save and put back" */
```

## tty_read — the dispatch

```c
int tty_read(char *buf, int cap) {
    if (cap <= 0) return 0;

    if (g_mode & TTY_ICANON) {
        /* Canonical: defer to the line-edited reader. It echoes,
         * handles backspace, returns when Enter is pressed. */
        return kshell_read_line(buf, cap);
    }

    /* Raw: block for at least one byte, then drain whatever else
     * is already available. Return immediately. */
    int n = 0;
    char c = keyboard_wait_char();
    buf[n++] = c;
    while (n < cap && keyboard_has_char()) {
        buf[n++] = keyboard_getc();
    }

    /* Echo if the program asked for it (default OFF in raw mode). */
    if (g_mode & TTY_ECHO) {
        for (int i = 0; i < n; i++) kputc(buf[i]);
    }
    return n;
}
```

Three things make this work:

1. **`keyboard_wait_char` always blocks for at least one byte.** That's our minimum-block-until-something-arrives semantic — equivalent to POSIX `VMIN = 1, VTIME = 0`. A POSIX `VMIN = 0` mode would need a non-blocking variant; we don't have one.
2. **`keyboard_has_char` is non-blocking.** After the first byte we drain the ring without blocking. If the user typed five keys in quick succession before the program got scheduled, all five come back from one `read`. If they typed one slowly, only that one comes back; the next read picks up the next.
3. **The same code path works whether the bytes came from PS/2 keyboard, COM1 serial, or `tty_inject`.** `keyboard_wait_char` and `keyboard_inject` both feed the same `kbd_buf` ring — the producer is hidden behind a single consumer interface.

## tty_inject — testing raw mode without a real keyboard

The headless boot writes serial to a file (`-serial file:serial.log`) — there's no input source. Without some way to feed bytes into the kernel from kernel itself, the raw-mode test would have nothing to read.

`SYS_TTY_INJECT` exists for exactly this:

```c
case SYS_TTY_INJECT: {
    const char *p = (const char *)a;
    int len = (int)b;
    if (len < 0)   { ret = -1; break; }
    if (len > 256)  len = 256;
    char buf[256];
    for (int i = 0; i < len; i++) buf[i] = p[i];
    ret = tty_inject(buf, len);
}
```

`tty_inject` calls `keyboard_inject(bytes, n)` which calls the static `buf_push(c)` already used by `kbd_irq` — the bytes appear to subsequent reads exactly as if a key had been pressed.

A snapshot copy into a kernel-side buffer handles the case where the user pointer becomes invalid mid-call (which can't happen today, but is the kind of robustness drift that bites later when we add async signal-interrupted syscalls).

The cap of 256 bytes matches the keyboard ring size. Beyond that the inject would just overflow and silently drop, so we cut the call off at the source.

This isn't a "real" Unix syscall — POSIX has no equivalent. It's purely a test helper, kept in the kernel because doing it from outside QEMU (via the actual serial port) requires bidirectional serial, which we don't always have in CI / headless setups.

## SYS_READ_LINE vs SYS_READ on fd 0

There are now two ways to read stdin from libuser:

| Call | Underlying syscall | Mode-aware? | Returns |
|---|---|---|---|
| `sys_read_line(buf, cap)` | `SYS_READ_LINE` | No — always canonical | Length of line, NUL-terminated |
| `sys_read(0, buf, cap)` | `SYS_READ` | Yes | Bytes copied; in canonical mode = line incl. NUL like read_line |

The shell uses `sys_read_line` for its prompt loop — line-at-a-time behavior is exactly what it wants. Programs that need raw input use `sys_read(0, ...)` after `tty_set_mode(TTY_RAW)`. Real Unix has only one read syscall and the mode flag fully controls semantics; we kept the line-reader as a dedicated entry point to avoid disturbing the shell during this session, but it could be unified later.

## The interactive `keys` builtin

For demos that aren't headless:

```c
static void cmd_keys(void) {
    puts("Press keys (Enter to exit). Each keystroke arrives raw,\n");
    puts("not waiting for newline — that's the whole point of raw mode.\n");

    uint32_t prev = tty_get_mode();
    tty_set_mode(TTY_RAW);
    for (;;) {
        char c;
        int  n = sys_read(0, &c, 1);
        if (n <= 0) continue;
        if (c == '\n' || c == '\r') break;
        printf("  '%c'  0x%02x\n",
               (c >= 32 && c < 127) ? c : '?',
               (uint32_t)(unsigned char)c);
    }
    tty_set_mode(prev);
    puts("(canonical mode restored)\n");
}
```

In a real serial-attached session you can type Ctrl-A or arrow keys and watch them come back as escape sequences (e.g. `0x1b 0x5b 0x41` for up-arrow). In canonical mode all that gets eaten by the line editor; in raw mode it's all yours.

Pattern is the standard "save / set / restore" — never leave the TTY in a non-canonical mode if you can help it, otherwise the next program inherits a broken prompt.

## What the test demonstrates

```
current mode: 0x3  (default = 0x3)
```

`tty_get_mode()` returns 3 = `TTY_ICANON | TTY_ECHO` = `TTY_DEFAULT`. Boot default is canonical with echo. ✓

```
set raw: now 0x0
```

`tty_set_mode(TTY_RAW)` → 0. Both ICANON and ECHO cleared. ✓

```
raw sys_read(stdin, ., 15) -> 6 bytes = 'ABCxyz'
```

We injected 6 bytes (no newline) and asked for up to 15. Raw `tty_read` blocked for the first byte, drained the rest non-blocking, returned 6. The same call in canonical mode would have entered `kshell_read_line` and looped on `keyboard_wait_char()` waiting for `\n` — 6 bytes injected with no newline = forever-block.

```
restored: now 0x3
```

Mode is back to canonical. The shell's `sys_read_line` for the next prompt works normally.

## Files added / modified

| File | Change |
|---|---|
| `kernel/tty.{h,c}` | New. Mode bitmap, `tty_read` dispatch, `tty_inject` |
| `kernel/keyboard.{h,c}` | `keyboard_inject(bytes, n)` |
| `kernel/syscall.{h,c}` | `SYS_TTY_SET_MODE/GET_MODE/INJECT`; FD_STDIN read goes through `tty_read` |
| `kernel/kernel.c` | `tty_init()` call |
| `user/libuser.{h,c}` | Mode flag defines; `tty_set_mode/get_mode/inject` wrappers |
| `user/sh.c` | `[t8]` selftest; `keys` builtin |

## Design decisions

**Single global TTY.** One console, one keyboard, one mode. A real OS would have a per-pty struct reachable via the controlling-terminal pointer on each task. We have neither pty nor controlling-terminal yet, so making it global is honest. When pty lands, `tty_set_mode`'s implementation moves to `current->tty->mode`.

**`SYS_READ_LINE` survives untouched.** The shell calls it for prompts; making the shell go through mode-aware `sys_read(0, ...)` would mean every shell line risks being broken if some buggy builtin failed to restore canonical mode. Two entry points = one is always safe.

**`tty_inject` is a real syscall, not a debug back door.** It's needed for headless boot tests where the kernel has no input source. It's marked clearly in the deep dive (and source) as a test helper, but it's available to any user program that wants to script its own input — which is occasionally useful for self-test programs in real OSes too.

**Mode is a bitmap, not a struct.** POSIX has 4 flag fields plus a 32-element `c_cc[]`. We have 2 bits. When we need more, we'll widen — but the bitmap stays the right shape for `set_mode(prev)` save/restore patterns.

**Default at boot is `TTY_DEFAULT = ICANON|ECHO`.** Same as Linux on console. Programs that want raw must opt in.

**Echo in canonical mode is hardwired ON.** The `TTY_ECHO` flag is honored only in raw mode. Implementing ECHO/NOECHO in canonical mode means moving the `kputc(c)` calls inside `kshell_read_line` behind a flag check; trivial but not done in this session because no caller asks for canonical-no-echo today (the use case is password prompts; we don't have those).

**Raw read blocks for at least one byte.** Equivalent to `VMIN=1, VTIME=0`. Means a raw-mode `read` on an idle TTY parks until something arrives — same as canonical `read`. A `VMIN=0` non-blocking variant would need a separate path; deferred.

**Raw read drains whatever's already in the ring after the first byte.** Lets a single read pick up paste bursts efficiently without losing keystrokes to the next read.

**`keyboard_inject` pushes via the same `buf_push` the IRQ uses.** No second buffer, no race between injected and real bytes — they interleave in arrival order.

**Mode is single-byte writeable but stored as `volatile uint32_t`.** Concurrent writes from different tasks (e.g. fork → both want to set mode) just last-writer-wins. No mutex.

## Pitfalls

1. **A program that exits without restoring canonical mode breaks the next shell prompt.** The `keys` builtin saves/restores; a buggy `cat` that flipped raw mode and crashed would leave the TTY in raw — every subsequent `sys_read_line` would return one keystroke at a time. Real shells handle this by trapping signals and restoring on shell-driven exit; we don't.
2. **Injected bytes don't come from the PIT-tracked input pipeline.** No timestamps, no pacing — they all "arrive" simultaneously. A test that depends on inter-keystroke timing wouldn't work.
3. **`tty_inject`'s 256-byte cap matches the keyboard ring size.** Bigger injections silently drop. The test isn't expected to push more than a handful of bytes.
4. **The TTY_ECHO flag in canonical mode is currently a no-op.** Documented above. Don't rely on it.
5. **`tty_read`'s raw path uses `keyboard_has_char` after the first byte.** That's a `kbd_head != kbd_tail` check — fine for our single-consumer model. With multiple readers it'd need locking.
6. **Serial input still feeds the same buffer.** Bytes typed at a real serial console show up at injected reads with no way to tell them apart. That's actually what you want for a TTY — the kernel doesn't care which device originated the byte — but worth knowing.
7. **`SYS_READ` on fd 0 in raw mode returns "what's available" up to `cap`, but on a non-fd-0 descriptor it doesn't go through TTY.** A user that did `dup2(0, 5)` and then `read(5, ...)` would still get raw bytes (because fd 5's kind would still be `FD_STDIN` after the dup2-of-stdin). But a custom socket fd routed to stdin won't see TTY semantics.
8. **kshell_read_line doesn't know about the TTY_ECHO flag.** If you set canonical-no-echo and the user types into the kernel-side line editor, it will still echo. Documented limitation.

## What might come next

POSIX termios with proper c_cc[] (so VINTR / VQUIT / VEOF have meaning), then ISIG support to translate Ctrl-C → SIGINT delivery via the signal layer from session 16 (kernel sees Ctrl-C → kill(foreground_pid, SIGINT)). After that, pty/forkpty so you can run sshd-style "give the new task a fresh tty"; then `select`/`poll` on stdin so a curses program can interleave keypresses with timer ticks without busy-looping.
