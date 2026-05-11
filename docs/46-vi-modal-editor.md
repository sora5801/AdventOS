# Session 46 — vi, a modal text editor

**Goal:** add a screen-oriented modal editor — the screen-oriented counterpart to `user/ed.c`'s line-oriented one. The existing `ed` is genuinely useful for scripted `s/foo/bar/` edits but painful for "open a file and type around in it"; `vi` is the response. Modal, two-keystroke editing, full-screen redraw, in-place cursor — recognizably vi.

End state — `[t9b]` selftest running:

```
[t9b] vi: modal editor round-trip (open, edit, :wq, verify)
  ...vi runs, drives its own screen, exits...
  vi exited code=0
  final vitest.txt (31 bytes):
alpha
beta
gamma
delta from vi
```

The injected keystrokes — `G` then `o` then `delta from vi` then ESC then `:wq` — drove the editor through *go-to-last-line / open-new-line-below-and-enter-insert-mode / type / leave-insert / write-and-quit*. The on-disk file persists with the expected content, *delta from vi* appended after the three pre-existing lines.

## What's in scope

In:

- **`kernel/vga.{c,h}`** — adds `vga_clear_to_eol()`. The existing `vga_set_cursor()` was already there.
- **`kernel/fbcon.{c,h}`** — adds `fbcon_set_cursor(row, col)` + `fbcon_clear_to_eol()` plus tiny getters (`fbcon_cols/rows/cur_row/cur_col`). Without these, the framebuffer console couldn't do in-place updates and we'd be limited to scrolling.
- **`kernel/syscall.{c,h}`** — three new syscalls: `SYS_TTY_CURSOR` (place cursor at row,col), `SYS_TTY_CLEAR` (clear screen + home), `SYS_TTY_CLEAR_EOL` (erase from cursor to end of line). Each dispatches to BOTH `vga_*` and `fbcon_*` mirroring `kputc`'s pattern, so the cursor + clears apply on whichever console is live.
- **`user/libuser.{c,h}`** — matching `sys_tty_cursor / sys_tty_clear / sys_tty_clear_eol` wrappers.
- **`user/vi.c`** — the editor itself (~550 LOC). Modal state machine, buffer model as an array of heap-allocated line strings, full-screen redraw on every keystroke, sys_fs_write atomic save.
- **`build.sh` / `mkfs.py`** — adds `vi` to `USER_PROGS` and `vi.elf` to the FS image.
- **`user/sh.c`** — new `[t9b]` selftest section that seeds a file, injects vi commands via `tty_inject`, spawns `vi.elf`, then re-reads and prints the file to confirm the edit persisted.
- **`kernel/fs.c` + `build.sh`** — bumps the on-disk FS bitmap from 1024 → 2048 sectors. Adding vi.elf to the user-program set pushed the built fs.img past 1024 sectors, and the smaller bitmap was failing to allocate sectors for *new* files like `notes.txt`. Pads the os.img up to (fs_lba + 2048) sectors to match.

Out:

- **Arrow keys**. AdventOS's keyboard driver doesn't emit ANSI escape sequences for arrow / function keys, so navigation is hjkl-only. Vi-spirited, but if you wanted "arrow keys work too" the keyboard driver would need to grow an escape-sequence emitter.
- **Undo / redo**. Real vi has `u` and Ctrl-R; this one doesn't. Adding it needs an undo journal, which is a bookkeeping exercise that fits in but didn't make this session's slice.
- **Visual mode** (`v` / `V`) — character / line selection. Same shape as the buffer-cursor pair we already track; would need to remember the anchor and apply `d` / `y` / `c` over the range.
- **Configurable buffer sizes**. `MAX_LINES = 2048` and `MAX_LINE = 1024` are hardcoded. Big enough for the demos; a real editor would grow dynamically.
- **No regex search**. `/pat` is a literal-substring search, not POSIX BRE. `g`, `c` flags aren't there either.
- **No `:%s/x/y/g` global substitute**. The colon-command parser handles `:w`, `:q`, `:q!`, `:wq`, `:NN`. Substitute is `s/x/y/[g]` territory and out of scope.

## Architecture

```
                    ┌──────────────────────────────────┐
       fork()       │  vi.elf (one process per file)  │
   ◄──────────────► │   - reads file via sys_open      │
   parent shell     │   - tty_set_mode(TTY_RAW)        │
   waits on exit    │   - sys_read(0, 1 byte)          │
                    │   - state machine: NORMAL/INSERT/│
                    │     COMMAND                      │
                    │   - sys_tty_cursor / clear /     │
                    │     clear_eol for in-place draw  │
                    │   - sys_fs_write on :w           │
                    └──────────────┬───────────────────┘
                                   │
                                   │ syscall dispatch (syscall.c)
                                   ▼
                ┌────────────────────────────────────┐
                │ SYS_TTY_CURSOR  → vga_set_cursor   │
                │                + fbcon_set_cursor  │
                │ SYS_TTY_CLEAR   → vga_clear        │
                │                + fbcon_clear       │
                │ SYS_TTY_CLEAR_EOL → vga_clear_to_  │
                │                eol + fbcon_clear_  │
                │                to_eol              │
                └────────────────────────────────────┘
```

## The modal state machine

Three modes, ASCII-keystroke driven, transitions on key:

```
                  ESC                      ESC
   ┌─────────────────────────┐  ┌───────────────────────┐
   │                         ▼  │                       ▼
   │     i/a/I/A/o/O      ┌─────────────┐   :/ /     ┌───────────────┐
┌──────────┐ ──────────► │   INSERT    │            │   COMMAND     │
│  NORMAL  │              │ printable→  │            │ printable→buf │
│ hjkl x   │ ◄───────────│  insert     │            │ Enter→run     │
│ dd yy p  │      ESC     │ \n→split    │            │ Esc→cancel    │
│ : / /   │              │ \b→backspace│            └───────────────┘
└──────────┘              └─────────────┘
```

- **Normal** is the resting state. Movement and operators live here.
- **Insert** is what you'd expect — printable characters get inserted at the cursor; Enter splits the line; Backspace deletes the previous character (and joins with the previous line if at column 0).
- **Command** is the line at the bottom of the screen prefixed with `:` or `/`. Enter runs it; Esc cancels.

Two-key operators (`dd`, `yy`, `gg`) are handled by a single `g_pending` byte: when normal mode sees `d`, it stashes `'d'` in pending and waits for the next key. The next key either matches (`d`) → delete-line, or doesn't, and falls through to be reinterpreted.

## Buffer model

```c
struct line {
    char *text;    /* malloc'd, NOT NUL-terminated */
    int   len;
    int   cap;
};

static struct line g_lines[MAX_LINES];
static int         g_n_lines;
```

Each line is a separately-heap-allocated buffer. The line struct tracks current length and allocated capacity; on insert we grow doubling, capped at `MAX_LINE`. Insertion at column `c` in line `r` is a `memmove(text+c+1, text+c, len-c)` style shift — O(len) but fine at 80-character lines.

Whole-line operations (`buffer_insert_line`, `buffer_delete_line`, `line_split` on Enter, `line_join` on backspace at col 0) shift the *array* of line structs by a memcpy in C-loop form. Also O(n) per op but fine at 2048-line files.

This is intentionally not a gap buffer or piece table — those win at the scales that matter for "edit a 100 MB file in real time", which is well past anything AdventOS users will reach.

## Redraw on every keystroke

The redraw is the dumbest possible:

```c
void redraw(void) {
    for (int sr = 0; sr < BODY_ROWS; sr++) {
        sys_tty_cursor(sr, 0);
        int br = g_top_row + sr;
        if (br < g_n_lines) {
            int n = g_lines[br].len;
            if (n > SCREEN_COLS) n = SCREEN_COLS;
            if (n > 0) sys_write(1, g_lines[br].text, n);
        } else {
            out_char('~');         /* vi-style "this row has no line" marker */
        }
        sys_tty_clear_eol();        /* erase any leftover chars on this row */
    }
    /* ... status line ... */
    sys_tty_cursor(row, col);       /* place the visible cursor */
}
```

24 cursor-positioning syscalls + 24 line writes + a clear-to-EOL after each row = a few KB of bytes per redraw. Negligible at human typing speeds.

The `~` glyphs on rows below the buffer's last line are pure vi homage but also genuinely useful — they make "this row is empty" visually distinct from "this row is just an empty line in the file".

## Why three new syscalls instead of ANSI escapes

The textbook approach to "make a text editor on a terminal" is to emit `ESC[H` for home, `ESC[2J` for clear, `ESC[r;cH` for cursor positioning, and let the terminal interpret. AdventOS's VGA + fbcon writers don't interpret ANSI escapes; characters go straight through `vga_putc` / `fbcon_putc` and get rendered literally. So `ESC[2J` would print as four glyphs, not clear the screen.

Two ways to fix that:

1. **Add an ANSI escape state machine** to vga.c and fbcon.c. ~100 LOC of "accumulate digits, dispatch on terminator" each. Universal compatibility — any vt100-aware program would Just Work.
2. **Expose direct primitives** as syscalls. Three lines per syscall in the dispatcher. Less general but trivial.

We took (2). It's smaller, doesn't risk subtly breaking anything that *does* expect ANSI escapes to print literally (none of our code does, but the surface area is tighter), and lets the editor be unambiguous about what it's asking the console to do.

If a later session wants to port a real terminal-aware program (`emacs -nw`, `htop`-style), adding (1) on top is additive — the new syscalls don't preclude it.

## Persistence: atomic `:w` via sys_fs_write

When the user runs `:w`, the editor concatenates all line buffers (re-adding the `\n` separators) into one heap-allocated blob, then calls `sys_fs_write(filename, buf, n)` — one syscall, no fd-based incremental writes. AdventFS's `fs_write_all` rewrites the whole file atomically (free the old sectors, allocate new ones, update the entry, write the bitmap), so we never see a half-written intermediate.

The same pattern is what `ed` uses. Both editors avoid the partial-write-after-crash failure mode that bites editors that do open-truncate-write-close.

## The 1024-sector cliff (collateral fix)

Adding `vi.elf` to the user-program list pushed `fs.img` past 1024 sectors (1063, actually). AdventFS's bitmap was sized for 1024 sectors; the kernel's `fs_init` was passing `n_sectors=1024` to the per-instance constructor. Result: files past sector 1024 in the image were still readable (the entries' `start_sector` works regardless), but *new* files like `:w`'s freshly-allocated sectors had no free slots in the (full) bitmap. `sys_fs_write` returned -1; `[t9]` ed broke.

Fix in `kernel/fs.c`: bump `FS_BITMAP_BYTES_MAX` and the `fs_init` n_sectors arg from 1024 → 2048. `build.sh` also bumps the os.img pad target from `fs_lba + 1024` → `fs_lba + 2048` so the disk image has room for files allocated past sector 1024.

Same shape as session 23's bitmap bump, just at a higher water mark. Future sessions will hit the same cliff again at some point — at which point the right fix is a dynamic bitmap rather than a recompile-time max.

## Test results

`[t9]` ed editor pipeline — PASS (after FS bump). Reads back the expected `beta / gamma / delta`.

`[t9b]` vi modal editor round-trip:

```
  vi exited code=0
  final vitest.txt (31 bytes):
alpha
beta
gamma
delta from vi
```

PASS — vi opened the file, the injected `G o delta from vi <ESC> :wq <CR>` sequence drove it through the expected state machine, and the on-disk file contains exactly the four-line result.

Everything else still green: cryptotest 27/27, the local httpsget→httpsd loop, the real-world `https://1.1.1.1/` GET, USB MSC, USB hub + HID kbd + HID mouse enumeration.

## What's next

- **Undo / redo** with a simple operation journal.
- **Visual mode** (`v`/`V`) for character/line selection over an anchor.
- **`:%s/pat/rep/g`** substitute — the most-asked-for vi feature beyond what's here.
- **Read-only / quickfix-like split** view — share screen real estate between buffer + a help line / search results list.
- **Arrow-key support** if the keyboard driver ever grows ANSI escape emission.
- **A dynamic FS bitmap** so adding the next big user program doesn't drag in another recompile-time bump.
