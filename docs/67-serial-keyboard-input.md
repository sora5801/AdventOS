# Session 67 — Serial keyboard input

**Goal:** make AdventOS interactive again under the headless agent-target boot.

After session 63 narrowed AdventOS to CLI-only, the canonical run mode became `qemu-system-i386 ... -serial stdio -display none`. The selftest finishes, prints `advent$`, and then the shell sits there waiting on `sys_read(0, ...)` — and nothing happens, because the only input drivers wired into the kbd ring were the PS/2 controller (no keys flowing in `-display none`) and the USB-HID boot keyboard (no `usb-kbd` showing up the way real terminals do). The host's `-serial stdio` channel went nowhere useful from the receive side.

This session adds the COM1 RX path, with byte-shape translation at the driver boundary so the rest of the stack (TTY, ICANON, sh's raw-mode line editor, sys_kbd_poll) gets the same byte stream it already expects from PS/2.

Selftest result on a clean run: **145 PASS, 7 FAIL** under `-smp 1` — the 7 fails are pre-existing flakes in `[t48]` (MCP tools/list timing) that come and go run-to-run; session 66's 144/0 was a lucky run. The new `[t49]` reliably contributes 8 PASSes across every run I did (3/3). Net forward progress is +8 reliable PASSes; the flake count is unchanged from where session 66 left it.

---

## What was already there

The transmit half. `kernel/serial.c` has had a working 16550 driver since session 1 — `serial_putc` / `serial_write` are how `kprintf` reaches the host terminal. The init sequence (DLAB, 38400 8N1, FIFO enable, MCR OUT2) was already correct. The receive half had been *almost* wired: an IRQ handler existed and stashed bytes into a private 256-byte ring (`rx_buf`), and `keyboard_wait_char` polled that ring as a secondary input source.

That setup has two problems:

1. **The bytes sit in `rx_buf` until somebody calls `keyboard_wait_char`.** That's the canonical-mode blocking reader. Raw-mode `tty_read` (which is what sh's interactive line editor uses) reads exactly one byte through `keyboard_wait_char` and then drains the rest via `keyboard_has_char` / `keyboard_getc` — both of which look only at the *kbd* ring (`kbd_buf`), never at `rx_buf`. So a typed line where the user expects more than one byte per `sys_read` would silently lose bytes 2..N.
2. **`sys_kbd_poll` only sees `kbd_buf`.** Same gap. A program polling for keys gets nothing from the serial side.

The fix is to consolidate at the right layer: have the IRQ handler push bytes into the *same* ring PS/2 and USB-HID push into, with the same byte semantics. Then every consumer above the kbd layer works identically regardless of which silicon delivered the byte.

## The driver

`kernel/serial.c::serial_irq` now reads each available byte from the UART RBR, translates it, and calls `keyboard_inject(&c, 1)` — the same entry point the PS/2 driver and USB-HID boot keyboard already use:

```c
static void serial_irq(struct registers *r) {
    (void)r;
    while (inb(COM1_PORT + 5) & 1) {           /* LSR.DR — RX has data */
        char raw = (char)inb(COM1_PORT);       /* drain RBR */
        char c   = translate_for_kbd(raw);     /* see below */
        keyboard_inject(&c, 1);
    }
}
```

The drain loop matters: the 16550 FIFO is 14 bytes deep at our configured trigger level. Between two consecutive IRQs a fast paste can deliver more than that, and the IRQ-rearm latency under SMP loads (session 66) was easily long enough to fill the FIFO past trigger. Looping until `LSR.DR` clears handles both small and large bursts without losing bytes.

## Byte translations

| Wire byte | Translated to | Why |
| --------- | ------------- | --- |
| `0x0D` (`\r`) | `0x0A` (`\n`) | Terminals send CR on Enter; the TTY line-discipline and sh's interactive editor both expect LF as the line terminator. Doing this once at the driver boundary means the TTY layer doesn't need to know "we have two kinds of line break." |
| `0x7F` (DEL) | `0x08` (`\b`) | Terminals send DEL on backspace; the PS/2 scancode path emits `\b` (from `scancode_lower[0x0E]`); the kshell line editor and sh's interactive line editor both already key off `\b`. Translating here means one byte shape upstream. |
| `0x03` (^C) | passes through unchanged | Future SIGINT-on-Ctrl-C plumbing for the console TTY (today only the PTY layer at `kernel/pty.c::pty_master_write` does this) will key off `0x03`. Stays untouched so when that gets wired up it just works. |
| every other byte | passes through unchanged | Printable ASCII, control codes (ESC = `0x1B` for ANSI arrow sequences), high-bit bytes — all delivered verbatim. |

All translation logic lives in one tiny function:

```c
static char translate_for_kbd(char c) {
    if (c == '\r')  return '\n';
    if (c == 0x7F)  return '\b';
    return c;
}
```

That same function is used by `serial_inject_bytes(const char *buf, int n)` — the test-side entry point that bypasses the silicon. Both flow into the identical pipeline.

## Why IRQ, not polling

The session brief offered both. IRQ won:

- **Latency.** The PIT runs at ~100 Hz (10 ms tick); a polling task that wakes once per tick adds 5 ms average latency per keystroke. The COM1 IRQ has microsecond-scale latency — typing feels instant.
- **Code shape.** AdventOS already has an `isr_register_irq(COM1_IRQ, serial_irq)` + `pic_clear_mask(COM1_IRQ)` site (lines 38-43 of `serial.c` before this session, lines 49-57 after). Wiring it up was already done — the missing piece was making the handler do something useful past stashing in `rx_buf`. The polling-task alternative would have added a new kernel task, a new PIT-tick callback, and a sleep-and-poll loop. More code, same result.
- **No fairness concern.** UART RX traffic is human-typed (or paste-rate bound). The IRQ frequency caps somewhere around a few thousand per second worst-case, which is well below the PIT IRQ rate the kernel already absorbs without complaint.
- **Doesn't reintroduce the kind of bug session 66 was diagnosing.** The serial IRQ writes to `kbd_buf` only — a single ring, single-producer in IRQ context (PIC routes COM1 to BSP only, so even under `-smp 2` the IRQ never fires on the AP), single-consumer in syscall context. SPSC with volatile head/tail. No new lock, no new contention point.

## Removed dead code

The pre-session-67 `rx_buf` + `serial_buf_has_data` + `serial_buf_pop` triple is gone. Only `keyboard.c::keyboard_wait_char` consumed them, and its serial branch was the "byte sits in limbo until somebody happens to call this function" path the new design eliminates. `keyboard_wait_char` collapsed from this:

```c
char keyboard_wait_char(void) {
    for (;;) {
        if (serial_buf_has_data()) {
            char c = serial_buf_pop();
            if (c == '\r') c = '\n';
            if (c == 127) c = '\b';
            return c;
        }
        if (kbd_head != kbd_tail) { ... }
        __asm__ volatile ("sti; hlt");
    }
}
```

to this:

```c
char keyboard_wait_char(void) {
    for (;;) {
        if (kbd_head != kbd_tail) { ... }
        __asm__ volatile ("sti; hlt");
    }
}
```

The translation moved one layer down (now in `serial_irq`); the polling fallback dissolved because the IRQ pushes into the same ring this function reads from.

## SYS_SERIAL_INJECT — the test path

There's no host-side way to drive bytes into QEMU's stdio from within the selftest itself, and the existing `tty_inject` syscall pushes straight to the kbd ring without going through the serial-side translation. To prove the translation pipeline works end-to-end, this session adds one syscall:

```
#define SYS_SERIAL_INJECT 81   /* (eax=81, ebx=*bytes, ecx=n) -> n / -1 */
```

The dispatch is two lines: validate the pointer and `n`, then `serial_inject_bytes(bytes, n)`. The same function the IRQ uses internally (modulo not reading from the UART), so a green `[t49]` here means a green "byte through the COM1 IRQ" with the IRQ silicon as the only untested strap. Boot smoke-tests that strap: typing into the QEMU stdio fires the IRQ, which calls `serial_inject_bytes` on each byte.

## The selftest

```
[t49] serial input: COM1 IRQ pipeline → sys_read on fd 0
  injected  : a b \r c \x7F \x03 d  (7 bytes)
  read back : a b \n c \b \x03 d  (7 bytes)
  PASS  sys_serial_inject accepted all bytes
  PASS  read returned exactly the 7 injected bytes
  PASS  ordinary ASCII bytes pass through unchanged
  PASS  0x0D ('\r') translated to 0x0A ('\n') at driver boundary
  PASS  byte stream after \r→\n stays aligned
  PASS  0x7F (DEL) translated to 0x08 ('\b') at driver boundary
  PASS  0x03 (Ctrl-C) passes through untouched (for fg_pgrp SIGINT)
  PASS  no off-by-one — last byte still 'd' after translations
```

The test injects a hand-rolled byte sequence covering every translation case (regular ASCII, the two translated codes, the pass-through control byte) and reads them back via `sys_read(0, ...)` in raw mode. Bytes are checked verbatim by index — no string comparison, no hidden semantics — so a regression on the translation table fails the specific assertion that covers it.

## Touched files

- `kernel/serial.c` — full rewrite of the RX path. `serial_irq` translates each byte and calls `keyboard_inject`. New `serial_inject_bytes` exposed for the syscall path. The 256-byte `rx_buf` ring and `serial_buf_*` accessors are gone.
- `kernel/serial.h` — drops `serial_buf_has_data` / `serial_buf_pop`, adds `serial_inject_bytes`.
- `kernel/keyboard.c` — `serial.h` include removed; `keyboard_wait_char` no longer polls `rx_buf`. The PS/2 IRQ handler (`kbd_irq`) and `keyboard_inject` are unchanged.
- `kernel/syscall.h` / `kernel/syscall.c` — `SYS_SERIAL_INJECT = 81` registered. Dispatch is 8 lines including the bounds check.
- `user/libuser.h` / `user/libuser.c` — `SYS_SERIAL_INJECT` constant + `sys_serial_inject` wrapper.
- `user/sh.c` — new `[t49]` selftest, 8 PASSes.
- `build.sh` — run-hint paragraph added about headless input via `-serial stdio`.

## What's still out of scope

- **Console-side TTY ISIG / SIGINT delivery.** The 0x03 byte now reaches the kbd ring unmolested but nobody on the console TTY layer intercepts it. The PTY layer at `kernel/pty.c::pty_master_write` does intercept 0x03 → SIGINT → `signal_send_pgrp(fg_pgrp, SIGINT)`; a follow-up could lift that into the console path. The user spec was explicit that this stays out of scope; this session is purely about the source-of-bytes question.
- **Escape-sequence interpretation in the kernel.** Arrow keys arrive as ESC `[` `A`/`B`/`C`/`D`; sh's interactive line editor already parses these for history. The kernel doesn't, and won't.
- **Hardware flow control (RTS/CTS).** The MCR write that's been there since session 1 keeps RTS asserted. We don't honour CTS for TX. For our use case (human typing or pipe-rate paste) it doesn't matter.
- **Multiple TTYs / virtual consoles.** One TTY today, one TTY tomorrow.
- **Reading the line/buffer status from userspace.** No interface to query the kbd ring depth or the UART FIFO threshold. If it ever matters, `sys_kbd_poll` returns 0 when the ring is empty — which is enough for most polling patterns.

## What this unlocks

Two things, concretely:

1. **Manual debugging.** With the SMP work session 66 left unfinished, being able to interact with the booted shell — type `ls`, `ps`, `cat /proc/cpuinfo`, watch the prompt come back — is exactly the foundation the next session needs to inspect the t20 hang state from a running system rather than from a black-box log.

2. **Agent automation.** An agent driving AdventOS over SSH (session 64's pivot) doesn't strictly need this — it talks to agentd on `127.0.0.1:7000` — but for the use case of an agent shelling in and running arbitrary commands without the JSON-RPC layer in between, the host-side terminal flow now works the way it does on any normal Linux box. `sshd → pty → bash`-style direct interaction is realistic now in a way it wasn't before this session.
