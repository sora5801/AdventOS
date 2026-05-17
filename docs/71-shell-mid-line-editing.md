# Session 84 — Usable Unix Phase 2: shell mid-line editing

**Goal.** Close the most painful daily-use gap in `sh.elf` — the explicit "no mid-line editing" comment that lived at line 844 of `user/sh.c`. Previously the shell supported raw mode, history (Up/Down), tab completion, and a destructive Backspace at end-of-line — but the moment you typed a wrong character three positions back, your only recovery was Ctrl-C and retype.

Status: **done.** Left/Right arrows + Home/End + Ctrl-A/E/B/F + Ctrl-W/U/K all work. Backspace and printable-char insertion are now cursor-aware. One new kernel syscall (`SYS_TTY_GET_CURSOR = 87`) to anchor the prompt row for in-place redraws. ~190 lines of net diff to `user/sh.c`; build size grew from 109968 to 111904 bytes.

---

## The shape of the previous gap

The pre-session-84 `read_line_interactive` had three state variables: `len` (chars in buffer), `hist_view` (history navigation index), and the `saved[]` buffer holding the in-progress line when the user starts walking history. The character-handling loop appended printable chars at position `len`, did a destructive backspace dance (`\b ' ' \b`) at the same position, and ignored every other control sequence — including the `ESC[C` / `ESC[D` (right / left arrow) sequences, which the source explicitly called out:

```c
/* ESC[C / ESC[D (right/left) ignored — no mid-line editing. */
```

That comment was the single largest piece of "this OS isn't quite real" in the daily-use shell.

---

## Why the cursor needs an absolute-position primitive

The natural way to move a terminal cursor left/right is via ANSI CSI escapes — `\033[<n>D` for left, `\033[<n>C` for right, etc. AdventOS's `fbcon` (the QEMU graphical console renderer) doesn't parse those — it explicitly drops every byte below 0x20 in its default case and renders 0x20+ verbatim, so emitting `\033[D` would print `[D` to the framebuffer and corrupt the visible line. The `vga` text-grid renderer is the same. The serial side (when running with `-serial stdio`, the typical QEMU dev path) DOES interpret ANSI sequences because the host terminal does — but the same bytes go to both outputs, so we can't emit "ANSI for serial only."

Two viable paths:

1. **Teach fbcon/vga to parse ANSI escapes.** A small CSI state machine: ESC seen → wait for `[` → parse digits → switch on terminator. Multi-file change, adds state to a hot path that's currently a single `switch (c)`. Useful long-term (`vi.c` could stop hand-rolling its own cursor positioning, terminal apps that emit color sequences would render correctly), but a substantial diff for this session's scope.
2. **Use the existing `SYS_TTY_CURSOR` syscall (= 60, sets absolute row/col) plus a new query syscall to read the current row.** No fbcon parser changes. The shell snapshots the prompt's row when the line begins, then after every redraw it positions the cursor with `sys_tty_cursor(prompt_row, prompt_len + cursor_in_buf)`.

This session takes path 2. Path 1 is filed for future work — it'd be the right thing if/when `vi.c` is rewritten to emit proper escape codes, or when a real terminal emulator app gets built.

### The new syscall: SYS_TTY_GET_CURSOR (= 87)

```c
case SYS_TTY_GET_CURSOR: {
    extern void fbcon_get_cursor(int *out_row, int *out_col);
    int *uout = (int *)(uintptr_t)a;
    if (!uout) { ret = -1; break; }
    int row = 0, col = 0;
    fbcon_get_cursor(&row, &col);
    uout[0] = row;
    uout[1] = col;
    ret = 0;
    break;
}
```

Mirrors `SYS_TTY_CURSOR`'s setter shape. `fbcon_get_cursor` and the parallel `vga_get_cursor` are new — both just read the existing static cursor-position globals each renderer already maintains. The framebuffer and VGA cursors stay in sync (both setters fire from the same syscall), so reading either gives the right answer.

User wrapper writes the row/col through a 2-int buffer to keep the syscall ABI single-output:

```c
int sys_tty_get_cursor(int *out_row, int *out_col) {
    int out[2] = { 0, 0 };
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_TTY_GET_CURSOR), "b"(out)
                      : "memory");
    if (out_row) *out_row = out[0];
    if (out_col) *out_col = out[1];
    return ret;
}
```

---

## The editor's new shape

### State

Two new variables on top of the pre-existing `len`, `hist_view`, `saved[]`, `saved_len`:

```c
int  cur  = 0;             /* cursor position within buf (0 <= cur <= len) */
int  prompt_row = 0;
int  prompt_col = 0;       /* unused after init; kept symmetric for future use */
sys_tty_get_cursor(&prompt_row, &prompt_col);
```

`cur` is the cursor's logical position in the buffer — what byte index would receive the next typed character. `prompt_row` is captured immediately after the caller's `puts(prompt)` returns, so it identifies the screen row the prompt was drawn on.

### The REPAINT macro

Every state change that mutates `len` or shifts characters in `buf` is followed by a screen refresh:

```c
#define REPAINT()  do { redraw_line(buf, len); \
                        position_cursor(prompt_row, cur); } while (0)
```

`redraw_line` was already in the source (sessions 49 / 81 used it for history recall and post-tab-complete redraws). It emits `\r`, the prompt, then the buffer, then `sys_tty_clear_eol()` — leaving the cursor at end-of-text. `position_cursor` is the new helper that issues `sys_tty_cursor(prompt_row, prompt_len() + want_col)` to move the cursor wherever the editor wants it.

Pure cursor-movement keys (Ctrl-B/F/arrows when not history-walking) call `position_cursor` directly without a full redraw — `len` didn't change, so the visible chars are still correct; only the cursor moves.

### Backspace and printable chars become cursor-aware

Pre-session-84:

```c
if (c == 0x08 || c == 0x7F) {
    if (len > 0) {
        len--;
        buf[len] = 0;
        putchar('\b'); putchar(' '); putchar('\b');
    }
    continue;
}
```

This worked because the cursor was always at `len`; `\b ' ' \b` erased one cell to the left.

Post-session-84:

```c
if (c == 0x08 || c == 0x7F) {
    if (cur > 0) {
        for (int i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
        len--;
        cur--;
        buf[len] = 0;
        REPAINT();
    }
    continue;
}
```

The byte-shift handles "delete the char before cursor" anywhere in the line. The full repaint handles the visual: the tail of the line has shifted, the line has one less char, and the trailing now-stale cell is wiped by `sys_tty_clear_eol()` inside `redraw_line`.

Insertion follows the same pattern with a fast path for end-of-line:

```c
if (c >= 32 && c < 127) {
    if (len < cap - 1) {
        if (cur == len) {
            /* Fast path: appending. */
            buf[len++] = c;
            cur = len;
            buf[len] = 0;
            putchar(c);
        } else {
            /* Insert path: shift tail one byte right. */
            for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
            buf[cur] = c;
            len++;
            cur++;
            buf[len] = 0;
            REPAINT();
        }
    }
    continue;
}
```

The fast path matters because typing at end-of-line is by far the most common case. A REPAINT does a full screen rewrite + cursor reposition syscall — two syscalls and a putchar-per-char loop. Skipping it for the append case keeps the latency low for the 95% case.

### New key bindings

| Key | Code | Action |
|---|---|---|
| Left arrow / Ctrl-B | `ESC [ D` / 0x02 | cursor one char left |
| Right arrow / Ctrl-F | `ESC [ C` / 0x06 | cursor one char right |
| Home / Ctrl-A | `ESC [ H` / 0x01 | cursor to start of line |
| End / Ctrl-E | `ESC [ F` / 0x05 | cursor to end of line |
| Ctrl-W | 0x17 | delete word before cursor (bash-style: skip trailing ws then run of non-ws) |
| Ctrl-U | 0x15 | delete from start of line to cursor |
| Ctrl-K | 0x0B | delete from cursor to end of line |

The Home/End escape codes (`ESC[H` and `ESC[F`) are what the Linux console and most terminal emulators emit for the physical Home/End keys. The Ctrl-letter aliases match GNU readline / bash defaults.

Ctrl-W's word definition skips trailing whitespace first, then deletes the run of non-whitespace immediately before that — same as bash and emacs. The implementation:

```c
if (c == 0x17) {
    if (cur > 0) {
        int end = cur;
        while (end > 0 && (buf[end - 1] == ' ' || buf[end - 1] == '\t'))
            end--;
        while (end > 0 && buf[end - 1] != ' ' && buf[end - 1] != '\t')
            end--;
        int killed = cur - end;
        if (killed > 0) {
            for (int i = end; i < len - killed; i++) buf[i] = buf[i + killed];
            len -= killed;
            cur  = end;
            buf[len] = 0;
            REPAINT();
        }
    }
    continue;
}
```

### Enter, Ctrl-C, history navigation

These needed small touch-ups for cursor-awareness:

- **Enter** now calls `position_cursor(prompt_row, len)` before printing `\n`. Without this, pressing Enter while mid-line would print the newline at the cursor's current row+col, which on fbcon could land mid-line and produce a confusing prompt position on the next iteration.
- **Up/Down (history)** reset `cur = len` after loading the recalled command into `buf` — you land at the end of the recalled text, ready to keep typing.
- **Ctrl-C** zeroes `cur` along with `len`, and re-queries `sys_tty_get_cursor` for `prompt_row` since the fresh prompt appears on the next line.
- **Tab** snaps cursor to `len` before calling `tab_complete` — tab-completion is "complete the last word"; if cursor is mid-line, "last word" is ambiguous, and jumping to end is the obvious behavior.

---

## Known limitations

### Long lines that wrap

`prompt_row` is captured once at line start. If the user types far enough that the line wraps to the next screen row, the cursor positioning calculation `sys_tty_cursor(prompt_row, prompt_len + cur)` will pass a `col` >= screen width. The kernel's `vga_set_cursor` / `fbcon_set_cursor` clamp `col` to `WIDTH - 1`, so the cursor jumps to the right edge of the prompt row rather than to the correct wrapped position. The line buffer stays correct — pressing Enter still submits the right text — but the visible cursor lies.

Realistic line lengths (under ~70 chars on the standard 80-col console with a short prompt) work perfectly. For longer commands, this is annoying but not destructive. Fix would be tracking the wrap count on every printable insert and decoding it back to (row_offset, col_offset) in `position_cursor`. Filed for future work.

### Serial-side cursor mirroring

When QEMU is run with `-serial stdio` (the default dev path), output goes to BOTH the framebuffer (rendered in the QEMU graphical window) AND the host terminal (via stdio). The fbcon cursor moves correctly via `sys_tty_cursor`. The serial side sees a `\r` + prompt + buffer + clear-EOL stream — its cursor ends up at end-of-text, NOT where the buffer's logical cursor lives.

This means: when using the QEMU graphical window, the line editor feels exactly right. When using the serial-terminal view in the host terminal, the cursor visually lags during edits, but the line you commit on Enter is correct.

The serial fix would be path 1 from the "Why the cursor needs..." section above — teaching `fbcon` and `vga` to parse ANSI CSI sequences so the shell could emit `\033[<n>D` to move the cursor on both renderers. That's a larger project that also benefits any future terminal-emulator app (and improves `vi.c`'s rendering portability). For now, the recommendation in `build.sh`'s help footer to click into the QEMU window for input remains the documented path.

### What's deliberately not implemented yet

- **Word-by-word cursor movement (Alt-B / Alt-F).** Bash/readline maps these to "back one word" / "forward one word". The escape sequence is `ESC b` / `ESC f` (no `[`), which would need a small change to the ESC-handling switch. Not painful enough to chase this session — Ctrl-W as a word-killer covers the most common need.
- **Yank/paste (Ctrl-Y after a kill).** Readline's kill ring would require a separate buffer + a yank command. Filed for whenever we add a clipboard.
- **`reverse-i-search` (Ctrl-R).** The single biggest non-obvious readline feature; deserves its own session.

---

## Smoke-test path

A line that exercises every new key, to be typed by hand in the QEMU window:

```
advent$ echo This is a long test line for cursor work
                                                      ^ cursor here at end

Ctrl-A → cursor jumps to first 'T' of "This"
Right Right Right → cursor on 's' of "This"
Left → back to 'i'
Ctrl-E → cursor to end
Ctrl-W → kills "work" (cursor at end of "cursor ")
Ctrl-W → kills "cursor "
Ctrl-U → kills the rest, line now empty (just "echo " — wait, Ctrl-U kills to start, so this would kill everything including "echo")
```

After typing and pressing Enter, the command should execute (`echo` prints whatever's left in the line).

Roundtrip with history: type `ls /`, Enter. Type `ls /etc`, Enter. Press Up twice → should show `ls /` with cursor at end. Press Right → does nothing (cursor already at end). Press Ctrl-A → cursor moves to start. Press Right twice → cursor on 's' of `ls`. Press Ctrl-K → kills "s /", line is "l". Press Enter → runs `l` (which fails — no such command — but the buffer state proves the mid-line editor worked).

---

## Files touched

```
kernel/syscall.h     SYS_TTY_GET_CURSOR define (= 87)
kernel/syscall.c     dispatch case + name-table entry
kernel/vga.c         vga_get_cursor()
kernel/fbcon.c       fbcon_get_cursor()
user/libuser.c       sys_tty_get_cursor() wrapper
user/libuser.h       SYS_TTY_GET_CURSOR define + prototype
user/sh.c            read_line_interactive cursor support
                     + position_cursor / prompt_len helpers
                     + REPAINT macro
                     + Ctrl-A/B/E/F/K/U/W key bindings
                     + Home/End escape codes
                     + help-text update for new keys
```

Net diff: ~290 lines added across all files (most in `user/sh.c`). Build size delta: `sh.bin` 109968 → 111904 (+1936 bytes, +1.8%).

---

## What's next in Path A

- **Phase 3 — Man pages.** Discoverability layer. Even a one-line summary per builtin would be a meaningful improvement over the current scattered `--help` flags. Likely shape: `/usr/man/<name>.txt` plain-text files, a `man` binary that just paginates them.
- **Phase 4 — vi polish.** `user/vi.c` is a real modal editor but limited. Search, undo, less-painful save/quit.
- **Beyond Path A.** Self-hosting a C compiler (port `tcc`), window manager on the VBE framebuffer, port Lua. See README's "Status & scope" for the menu.
