# Session 161 — wmterm interactive polish (backspace + PS/2 PgUp + CSI K)

Path C phase 54.  Bug-fix follow-up to sessions 157-160.

## The user reports

After session 160's kbd-grab fix, typing into wmterm finally
worked — but three follow-on issues surfaced as soon as the user
actually tried to *use* it:

1. **Backspace did nothing visually.**  You'd type `abc`, hit
   backspace, and the `c` would stay on screen.  Subsequent
   typing piled new characters on top.
2. **Only one command per session.**  After `ls` finished, the
   `advent$` prompt didn't reappear.  Or rather — it did, the
   shell was perfectly happy to accept another command, but
   the user couldn't see it.
3. **PgUp didn't scroll.**  On a real keyboard (PS/2), the
   session-159 scrollback ring was unreachable.

Three independent bugs, one underlying theme: the syscall
boundary between sh.elf and the kernel TTY was leaking
non-PTY-aware behaviour into a PTY-hosted shell.

## Bug 1 — backspace lost in the redraw_line clear-EOL

`sh.elf`'s `read_line_interactive` calls `redraw_line` after any
mid-line edit (backspace, Ctrl-U, history nav).  The pre-161
version:

```c
static void redraw_line(const char *buf, int len) {
    putchar('\r');
    puts(current_prompt());
    for (int i = 0; i < len; i++) putchar(buf[i]);
    sys_tty_clear_eol();          /* <-- only works on the kernel console */
}
```

That last line is a kernel syscall that operates on the **global
framebuffer console grid**, not on the file descriptor.  When
sh.elf is running on a PTY slave (fd 0/1/2 dup'd from
wmterm's slave end), the call goes through but doesn't propagate:
wmterm only sees what comes through the PTY ring, and the
clear-EOL is invisible.

Net effect: backspace shortens sh.elf's internal buffer + redraws
the new content with `\r prompt buf`, but the *trailing* old
characters in the wmterm grid never get cleared.  The user sees
the old `c` from `abc` stay on screen forever.

### The fix

Emit `ESC [ K` in addition to the syscall:

```c
/* user/sh.c */
static void redraw_line(const char *buf, int len) {
    putchar('\r');
    puts(current_prompt());
    for (int i = 0; i < len; i++) putchar(buf[i]);
    sys_tty_clear_eol();           /* still no-op when PTY, but */
    putchar(27); putchar('['); putchar('K');  /* this one isn't  */
}
```

And teach wmterm to interpret `ESC [ K` instead of stripping it:

```c
/* user/wmterm.c */
static void csi_dispatch(char final) {
    switch (final) {
        case 'K': {
            int mode = csi_get_param(0);
            if (mode == 0) {                    /* cursor → EOL */
                for (int c = g_cur_col; c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            } else if (mode == 1) {             /* BOL → cursor */
                for (int c = 0; c <= g_cur_col && c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            } else if (mode == 2) {             /* whole line */
                for (int c = 0; c < COLS; c++)
                    g_grid[g_cur_row][c] = 0;
            }
            return;
        }
        default: return;                        /* still strip the rest */
    }
}
```

The old vt_feed stripped *all* CSI sequences in one branch
(`if (b >= 0x40 && b <= 0x7E) g_vt_state = 0;`).  The new
implementation actually parses parameter bytes (0x30..0x3F) into
`g_csi_param[]`, then dispatches at the final byte.  `K` is
handled; everything else (colour codes, scroll-region setup,
cursor-position queries) is still silently dropped — wmterm
doesn't render colours and doesn't track those states.

## Bug 2 — "prompt didn't reappear"

This one turned out to be misdiagnosis.  The smoke trace shows
the second prompt does arrive:

```
wmterm: rd n=14 first=0x70 [pwd./.advent$ ]
```

The user almost certainly *did* see the prompt — it was just
visually buried at the bottom of the screen after `ls /`'s ~30
lines of output scrolled past.  The PgUp fix (bug 3) is what
actually makes that situation usable.

I confirmed this by running `pwd` twice in a row in the polish
smoke; the second `pwd` works fine, proving sh.elf had reached
the next `read_line_interactive` cleanly.

## Bug 3 — PS/2 PgUp / PgDn not wired

Session 159 added PgUp / PgDn handling to `kernel/usb_hid.c`'s
`hid_kbd_to_ascii` translator — but only there.  Real keyboard
input in QEMU's GUI default flows through PS/2, not USB-HID.
That driver lives in `kernel/keyboard.c`'s `process_scancode`
and originally only handled arrows:

```c
case 0x48: push_csi('A'); return;    /* Up */
case 0x50: push_csi('B'); return;    /* Down */
case 0x4D: push_csi('C'); return;    /* Right */
case 0x4B: push_csi('D'); return;    /* Left */
default: return;
```

PgUp's E0-prefixed scancode is `0x49`, PgDn's is `0x51`.  Both
fell to `default: return` — silently dropped.

### The fix

```c
case 0x49:                                 /* PageUp */
    buf_push(27); buf_push('[');
    buf_push('5'); buf_push('~');
    return;
case 0x51:                                 /* PageDown */
    buf_push(27); buf_push('[');
    buf_push('6'); buf_push('~');
    return;
```

`push_csi` is a 3-byte helper (ESC '[' final) for the arrow keys;
PgUp / PgDn need a 4-byte sequence (ESC '[' digit '~') so they
buf_push directly.  Same wire format USB-HID emits — wmterm's
input-side CSI parser can't tell the two driver paths apart.

## Diagnostic: wmterm verbose read preview

Helping these bugs reproducible: wmterm's `-v` mode now dumps up
to 80 chars of every PTY read as a preview string in square
brackets.  Non-printable bytes show as `.`:

```
wmterm: rd n=60 first=0xd [.advent$ ab.[K.sh: command not found: ab.[exit 127].advent$ ]
```

That single line proves bug 1 is fixed end-to-end: the `.[K` is
the CSI K wmterm received from sh.elf's `redraw_line`, the
"command not found: ab" (not "abc") proves backspace actually
deleted a character from sh.elf's input buffer.

## Smoke

`smoke_wmterm_polish.py` — seven checks, exercises all three
bugs end-to-end:

| Check                                            | Pass |
|--------------------------------------------------|------|
| focus wmterm                                     |  ✓   |
| pwd runs + prompt reappears                      |  ✓   |
| second pwd works (prompt was ready)              |  ✓   |
| backspace routed to wmterm                       |  ✓   |
| inner shell saw 'ab' after backspace             |  ✓   |
| PgUp ESC reached wmterm (PS/2 + USB path)        |  ✓   |
| scrollback view went non-zero                    |  ✓   |

2/3 runs full-pass; the pwd read-back check is occasionally
flaky depending on how the shell flushes (one read vs. split),
but the second-pwd check captures the same property without that
fragility.

Regression: sessions 157, 158, 159 smokes all still 100%.

## What changed, exhaustively

- `kernel/keyboard.c` — PS/2 E0+0x49 / E0+0x51 emit ESC[5~ / ESC[6~.
- `user/sh.c` — `redraw_line` also emits CSI K after writing the
  redrawn line, so the trailing characters on the wmterm grid
  get cleared.  No-op on the kernel console (the syscall already
  clears).
- `user/wmterm.c` — vt_feed CSI parser now buffers parameter
  bytes and dispatches at the final.  Only `K` is implemented;
  everything else still gets stripped.  Verbose-mode read line
  prints up to 80 preview bytes for debugging.
- `fs/man/wmterm` — updated.
- `smoke_wmterm_polish.py` — new.

## What this *doesn't* fix

- **Mid-line backspace + right-side characters.**  sh.elf's
  insert-mode redraw_line still uses `position_cursor` →
  `sys_tty_cursor` to put the cursor mid-line, which is a global
  console syscall.  Inside wmterm the cursor stays at end-of-line
  after redraw_line returns, so backspace in the middle of a
  word looks correct visually (CSI K clears the tail) but the
  caret blinks in the wrong place.  Fixing that needs
  position_cursor to also emit `ESC [ row ; col H`.

- **No colour support.**  Anything sh writes via colour CSI (`m`)
  is still stripped.  `ls --color=auto` would compile but the
  output would be plain green text in wmterm.  Out of scope.
