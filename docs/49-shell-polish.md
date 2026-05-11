# Session 49 — Shell polish

**Goal:** turn the cooked-line "prompt → enter → tokenize" loop in `sh.elf` into a real interactive shell. Specifically: arrow keys recall history, Tab completes filenames from the cwd, `$VAR` references expand inline, `export` / `unset` / `env` / `history` / `source` are real builtins, and `sh script.sh` runs a file line-by-line. Together this is what turns a teaching demo into something you can actually use to drive the system day-to-day without `sys_kill -9 $(jobs -p)`-ing yourself in frustration.

End state — `[t32]` selftest, with all 14 assertions PASS on a clean boot:

```
[t32] shell polish: env vars, history, tab completion, .sh scripts
  PASS  env_set/env_get FOO=hello
  PASS  $FOO expands inside a sentence
  PASS  $X expands to 'a b c' (3 args after tokenize)
  PASS  $NOPE (undefined) expands to empty
  PASS  env_unset FOO
  PASS  hist_add dedup'd consecutive duplicates
  PASS  hist[0]=ls
  PASS  hist[1]=pwd
  PASS  hist[2]=history
  PASS  run_script returned 0
  PASS  script's `export FROM_SCRIPT=ok` is visible to caller
  PASS  script created script_out.txt via redirect
  PASS  script_out.txt starts with 'hi'
  PASS  raw mode delivers ESC [ A intact to sys_read
```

The interactive bits — actual keypresses driving Up/Down/Tab — can't be tested from a headless boot (the kernel's `kbd_irq` only runs from a real hardware IRQ, and `tty_inject` bypasses scancode translation by design). Those land in the boot-time keyboard path, covered by manual testing.

## What's in scope

In:

- **`kernel/keyboard.c`** — track the 0xE0 "extended scancode" prefix. When the next byte after 0xE0 is an arrow scancode (0x48/0x50/0x4B/0x4D for up/down/left/right), inject the corresponding ANSI CSI sequence (ESC `[` A/B/C/D) into the keyboard ring instead of the bare ASCII character. Numpad arrows (same scancodes without the 0xE0 prefix) keep their old "type a digit" behavior so NumLock-on stays usable.
- **`user/sh.c`** — five additions:
  1. `g_env_buf[]` + `env_get/env_set/env_unset` — a 32-entry "NAME=value\0" table living in the shell process. No `envp` plumbed through `sys_exec`, so this is intentionally shell-local; what it powers is `$VAR` expansion and the `env` / `export` / `unset` builtins.
  2. `g_hist[][]` + `hist_add` — a 32-entry FIFO of recent commands. Empties and consecutive duplicates are skipped (bash `HISTCONTROL=ignoreboth`).
  3. `expand_vars()` — walks the line, finds `$IDENT` references, substitutes the value (or empty for undefined). Runs BEFORE tokenize, which is what gives word-splitting for free: `FOO="a b c"; echo $FOO` becomes three args, not one.
  4. `read_line_interactive()` — replaces `sys_read_line` in the main loop. Switches stdin to `TTY_RAW`, reads one byte at a time, and handles Backspace/Enter/Tab/Ctrl-C/Arrow keys inline. Restores the previous TTY mode before returning.
  5. `run_script()` — opens a file, walks it 64 bytes at a time assembling lines, drops blanks and `#` comments, calls `execute_line` on each remaining line. Same `execute_line` the interactive loop uses, so anything that works at the prompt works in a script.
- **New builtins** wired into `execute_line`: `env`, `export NAME=VALUE [...]`, `unset NAME [...]`, `history`, `source FILE` / `. FILE`.
- **`user/sh.c` `main()`** — argv parsing learns about script mode: `sh script.sh` (any non-"selftest" non-"-prefixed" arg) calls `run_script` and exits, leaving the interactive prompt for the no-arg case.

Out:

- **Left/right cursor movement, mid-line editing.** Up/Down rewrite the whole buffer, so they don't care about cursor position. Adding real cursor movement means tracking an `insert_pos` and reflecting it via `sys_tty_cursor`, plus shifting the buffer on insert/delete. Defer.
- **`envp` through `sys_exec`.** Exported vars are visible to scripts via `source` because that runs inline, but `sh -c '...'` would not inherit them. Wiring envp through `task_exec_inplace` is a future session.
- **Quoting.** No `"..."` or `'...'` or `\` escapes. Word splitting after expansion is unconditional. Anyone writing `echo "hello world"` today gets two quoted args.
- **Tab completion of commands.** Tab walks the cwd. PATH-aware completion (also of `cd <Tab>` finding only directories) would need fs metadata we don't expose yet.
- **History persistence.** `g_hist` lives in shell memory; closing the shell loses it. A `$HISTFILE`-style file would be straightforward but isn't here yet.

## The 0xE0 prefix and where ANSI escapes come from

QEMU's PS/2 keyboard sends scancodes from Set 1. The dedicated arrow cluster (the four-key diamond, separate from the numpad) emits a two-byte sequence: `0xE0` followed by the scancode that the numpad key would send. So <kbd>↑</kbd> is `0xE0 0x48`, where `0x48` alone is "numpad 8" (which, with NumLock on, types `8`).

Before this session, `kbd_irq` looked up `scancode_lower[sc & 0x7F]` unconditionally:

```c
char c = scancode_lower[sc & 0x7F];
if (!c) return;
```

The `scancode_lower[]` table maps `0x48` to `'8'`. So a real up-arrow looked like: byte `0xE0` (ignored — table maps to 0, returned), then byte `0x48` (mapped to `'8'`, pushed). Pressing Up typed `8`. The numpad worked, the arrows were broken.

Fix: latch a `e0_prefix` flag when 0xE0 arrives, then on the next byte branch:

```c
if (sc == 0xE0) { e0_prefix = 1; return; }
/* ... modifier handling ... */
if (sc & 0x80) { e0_prefix = 0; return; }   /* key release */

if (e0_prefix) {
    e0_prefix = 0;
    switch (sc) {
        case 0x48: push_csi('A'); return;   /* up arrow    */
        case 0x50: push_csi('B'); return;   /* down arrow  */
        case 0x4D: push_csi('C'); return;   /* right arrow */
        case 0x4B: push_csi('D'); return;   /* left arrow  */
        default: return;                     /* ignore other extended keys */
    }
}
```

`push_csi(final)` pushes three bytes into the ring: ESC (0x1B), `[`, then the final character. That's the standard ANSI CSI form — what every terminal emulator (xterm, gnome-terminal, the Linux console) emits when you press an arrow key, and what `readline` / `vi` / `tmux` all expect to read back. Reusing the ANSI spelling keeps us compatible with that whole ecosystem; if we ever boot AdventOS under a hardware terminal or in a serial console, the same shell code keeps working.

The numpad — which sends `0x48` without an 0xE0 prefix — falls through to the normal `scancode_lower` lookup and still types `'8'`. Switch the NumLock and... well, the kernel doesn't track NumLock yet, but that's a separate refactor.

Key releases interleave with key presses on every keystroke. A naive 0xE0 latch that didn't clear on release would persist forever after one arrow key — a Shift-up sequence (`E0 2A E0 48` or similar) would leave the flag set and break the next non-extended key. The three `e0_prefix = 0;` resets in the modifier-handling block and in the key-release short-circuit prevent that.

## The line editor: raw mode + a 3-byte ESC parser

The old loop was a one-liner:

```c
int n = sys_read_line(line, sizeof(line));
```

`sys_read_line` runs entirely in the kernel: it buffers keystrokes in canonical mode, handles Backspace by mutating the buffer + emitting `\b \b`, and only returns when Enter is pressed. Simple, but doesn't see the bytes until commit, so there's no way to grab Up/Down/Tab.

The new loop is a state machine in userspace:

```c
uint32_t prev_mode = tty_set_mode(TTY_RAW);     /* echo off, byte-at-a-time */
int  len = 0;
int  hist_view = g_hist_count;
char saved[LINE_MAX]; int saved_len = 0;

for (;;) {
    char c;
    int  n = sys_read(0, &c, 1);
    if (n <= 0) continue;

    if (c == '\r' || c == '\n') {
        putchar('\n');
        buf[len] = 0;
        tty_set_mode(prev_mode);
        return len;
    }
    if (c == 0x08 || c == 0x7F) {
        if (len > 0) {
            len--;
            buf[len] = 0;
            putchar('\b'); putchar(' '); putchar('\b');
        }
        continue;
    }
    if (c == 27) {
        char a, b;
        sys_read(0, &a, 1);
        if (a != '[') continue;
        sys_read(0, &b, 1);
        if      (b == 'A') { /* up   — load hist[--hist_view] */ }
        else if (b == 'B') { /* down — load hist[++hist_view] or saved partial */ }
        /* C / D ignored — no cursor movement yet */
        continue;
    }
    if (c == '\t') { tab_complete(buf, &len, cap); continue; }
    if (c == 0x03) { /* Ctrl-C — discard line */ }
    if (c >= 32 && c < 127) {
        buf[len++] = c; buf[len] = 0; putchar(c);
    }
}
```

A couple of design choices worth naming:

**Why echo manually instead of leaving ECHO on.** `TTY_DEFAULT` is `TTY_ICANON | TTY_ECHO`. Drop ICANON and you get byte-at-a-time reads, but the kernel still echoes each byte before you see it. Problem: when we get ESC `[` A, the kernel echoes ESC and `[` as control bytes — visually `^[[` — before our parser can swallow them. Solution: clear both flags (TTY_RAW = 0), and the shell echoes each printable byte itself with `putchar(c)`. The arrow-key bytes get dropped silently because the parser doesn't echo them — they're metadata, not text.

**Why peek for 2 bytes after ESC.** A pressed arrow key arrives as 3 contiguous bytes in the input ring. A user pressing the ESC key alone arrives as 1 byte. Either way, after seeing ESC we do two unconditional reads. If it was a bare ESC, those reads block waiting for the next keystroke — annoying but not broken. A real implementation would use a timeout or a non-blocking peek; AdventOS has neither, so this is the simplest correct thing. The shell never sends meaningful ESC sequences itself, so the user doesn't reach for ESC standalone in practice.

**`hist_view` semantics.** It's an index into `g_hist`, with the convention that `hist_view == g_hist_count` means "showing the user's current in-progress input, not a history entry." First press of Up: copy current buffer into `saved`, decrement `hist_view`, load `g_hist[hist_view]`. Down: increment. When `hist_view` ticks back up to `g_hist_count`, restore from `saved`. That last bit is what makes "type half a command, press Up to check history, press Down to come back" not lose your half-typed line.

**`redraw_line` uses `sys_tty_clear_eol`.** When Up loads a shorter history line over a longer one, the trailing characters from the previous line would still be on screen. The kernel exposes a clear-to-end-of-line syscall (session 46, for the vi editor). Use it:

```c
static void redraw_line(const char *buf, int len) {
    putchar('\r');
    puts(current_prompt());
    for (int i = 0; i < len; i++) putchar(buf[i]);
    sys_tty_clear_eol();
}
```

`\r` carriage-returns to column 0, the puts/putchar combo redraws prompt+buffer, `clear_eol` scrubs whatever stale characters remain on the rest of the line. No need to track column ourselves or compute padding-with-spaces — the kernel's TTY cursor tracking does it.

## Tab completion

`tab_complete` is a one-shot: find the prefix (everything after the last whitespace), enumerate the cwd via `sys_readdir`, collect entries whose names start with that prefix, then dispatch on count:

- **0 matches** — silent no-op.
- **1 match** — splice the missing tail into the buffer + echo it, then append a space (so `ls<Tab>foo` becomes `ls foo ` ready for the next arg).
- **2+ matches** — newline, list each on its own indented line, redraw the prompt + buffer (the user's cursor is still at the end where it was when Tab was pressed).

```c
int word_start = len;
while (word_start > 0 &&
       buf[word_start - 1] != ' ' && buf[word_start - 1] != '\t') {
    word_start--;
}
int         prefix_len = len - word_start;
const char *prefix     = &buf[word_start];

char  cwd[64];   sys_getcwd(cwd, sizeof(cwd));
int   iter = 0;
char  name[17];
char  matches[16][17];
int   n_matches = 0;
while (sys_readdir(cwd, &iter, name) >= 0) {
    name[16] = 0;
    int ok = 1;
    for (int i = 0; i < prefix_len; i++)
        if (name[i] != prefix[i]) { ok = 0; break; }
    if (ok && n_matches < 16) {
        /* copy into matches[n_matches] */
        n_matches++;
    }
}
```

The 16-entry match cap matters for `/`-cwd users who would otherwise see 30+ ELFs scroll past. Overflow is signaled with a trailing `...`.

Why filter against the cwd (not PATH)? AdventOS doesn't have a PATH concept yet — `resolve_program` just appends `.elf` and execs by name. The kernel's FS does a single-namespace lookup in the task's cwd, so completing against cwd matches actual exec resolution. When we add a PATH later, this becomes a `getenv("PATH")` walk.

The name buffer is 17 bytes because `sys_readdir`'s `name_buf` is 16 chars NUL-padded — we add a guaranteed terminator. Important that it's NOT 32 (`d_name`-style) bytes: AdventFS shipped names are bounded at 16.

## `$VAR` expansion, word splitting, and order of operations

`expand_vars` is the only stage between read-the-line and tokenize. It walks the line character by character, and when it sees `$` followed by an identifier-start character ([A-Za-z_]), pulls the longest identifier match, looks it up via `env_get`, and emits the value (or nothing for undefined).

```c
while (*in) {
    if (*in == '$' && /* in[1] is identifier-start */) {
        in++;
        char name[32]; int ni = 0;
        while (/* in is identifier-cont */) name[ni++] = *in++;
        name[ni] = 0;
        const char *v = env_get(name);
        if (v) while (*v) out[oi++] = *v++;
        continue;
    }
    out[oi++] = *in++;
}
```

The order matters. **Expansion-before-tokenize** gives word-splitting for free:

```
$ export X="a b c"
$ echo $X
```

→ `expand_vars` writes `echo a b c` into the output buffer
→ `tokenize` splits on whitespace, gives `["echo","a","b","c"]`
→ `run_pipeline` execs `echo.elf` with three arg tokens

If we'd tokenized first and expanded each token after, `$X` would have stayed one argv slot — closer to bash's `"$X"` semantics. The current behavior matches bash's bare `$X` (no quotes), which is what most scripts mean.

A bare `$` not followed by an identifier passes through as a literal. So `$1` and `$?` work as "literal $1 / $?" (degraded behavior, but doesn't break) until we add positional/special params.

## The built-in dispatch refactor

The old main loop had ~60 lines of `if (strcmp(toks[0], "X") == 0) { cmd_X(); continue; }`. Adding 5+ new builtins would have made it untenable, and we needed the same dispatch path from `run_script` too. So:

```c
static void execute_line(char *line_in) {
    char line[LINE_MAX];
    if (expand_vars(line_in, line, sizeof(line)) < 0) {
        puts("sh: variable expansion overflowed line buffer\n");
        return;
    }

    char *toks[ARG_MAX];
    int ntok = tokenize(line, toks, ARG_MAX);
    if (ntok == 0) return;

    /* hard builtins... */
    if (strcmp(toks[0], "export") == 0) { cmd_export(toks, ntok); return; }
    /* soft builtins (skipped if has_pipe_op)... */

    struct pipeline pl;
    if (parse_pipeline(toks, ntok, &pl) < 0) { puts("sh: parse error\n"); return; }
    int rc = run_pipeline(&pl);
    if (rc != 0) printf("[exit %d]\n", rc);
}
```

The function takes a mutable buffer, expands into a stack-local fresh buffer (so `tokenize`'s NUL-stamping doesn't trash the caller's copy), then dispatches. Used from two callers:

```c
/* interactive */
for (;;) {
    puts(current_prompt());
    int n = read_line_interactive(line, sizeof(line));
    if (n <= 0) continue;
    hist_add(line);
    execute_line(line);
}

/* script */
while (read_one_line_from_fd(line, ...)) {
    execute_line(line);
}
```

The new builtins fit into the existing pattern:

- `env` — print every `g_env_buf[i]` followed by `\n`.
- `export NAME=VALUE [...]` — split each arg at `=`, call `env_set`.
- `unset NAME [...]` — call `env_unset` for each.
- `history` — number each `g_hist[i]` and print.
- `source FILE` / `. FILE` — call `run_script(FILE)` inline, so the script's `export`s mutate the running shell's env.

## Script mode: `sh script.sh`

`run_script` slurps a file 64 bytes at a time and assembles lines:

```c
char line[LINE_MAX];
int  pos = 0;
char chunk[64];
for (;;) {
    int n = sys_read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
        char c = chunk[i];
        if (c == '\r') continue;                       /* Windows CRLF */
        if (c == '\n' || pos >= LINE_MAX - 1) {
            line[pos] = 0;
            /* skip leading whitespace, then `#` comments + blanks */
            int s = 0;
            while (line[s] == ' ' || line[s] == '\t') s++;
            if (line[s] && line[s] != '#') execute_line(&line[s]);
            pos = 0;
        } else {
            line[pos++] = c;
        }
    }
}
```

Three details:

1. **Chunked read, not slurp.** A 4 KB static buffer would have to live in `.data` (because user.ld DISCARDs `.bss`… no wait, it folds `.bss` into `.data` now — but it'd still bloat the ELF by 4 KB per shell). 64 bytes on the stack is fine and streams arbitrary script sizes.
2. **CR drop.** Scripts written from Windows hosts (mkfs.py running on Windows, scripts edited in Notepad) have CRLF line endings. We drop the CR silently so the same `\n` parser handles both.
3. **Blank/comment skip.** A line that's whitespace-only or starts with `#` after leading whitespace is ignored without invoking `execute_line` (which would have called it an unknown command).

`main()` learns to detect script mode:

```c
int         run_selftest = 0;
const char *script_arg   = 0;
for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "selftest") == 0) { run_selftest = 1; continue; }
    if (argv[i][0] != '-' && !script_arg) script_arg = argv[i];
}
/* ... selftest block ... */
if (script_arg && !run_selftest) {
    if (run_script(script_arg) < 0) {
        puts("sh: cannot open script: "); puts(script_arg); puts("\n");
        sys_exit(1);
    }
    sys_exit(0);
}
/* ... interactive loop ... */
```

So:

- `sh.elf selftest` → run selftest, drop to interactive (unchanged headless path).
- `sh.elf script.sh` → run script lines via `execute_line`, exit with status 0.
- `sh.elf` (no args) → straight to interactive prompt.

This is also why `source FILE` is distinct from `sh FILE`: `source` runs the lines in the CURRENT shell process (env mutations stick), `sh FILE` runs them in a fresh forked child (env mutations evaporate when the child exits). Standard Unix semantics.

## The selftest t32 picks 14 things to verify

Most of the new code is testable from inside a running shell without keystroke injection:

```c
/* (a) env round-trip */
env_set("FOO", "hello");
EXPECT(env_get("FOO") && strcmp(env_get("FOO"), "hello") == 0,
       "env_set/env_get FOO=hello");

/* (b) expansion + word-splitting */
env_set("X", "a b c");
char out[64];
EXPECT(expand_vars("echo $X", out, sizeof(out)) == 0 &&
       strcmp(out, "echo a b c") == 0,
       "$X expands to 'a b c' (3 args after tokenize)");

/* (e) history dedup */
hist_add("ls");
hist_add("ls");                  /* duplicate of last → skipped */
hist_add("pwd");
EXPECT(g_hist_count - hist_before == 2, ...);

/* (f) round-trip a .sh script through run_script */
const char *script =
    "# session 49 script test\n"
    "export FROM_SCRIPT=ok\n"
    "echo hi from script > script_out.txt\n";
sys_fs_write("test.sh", script, strlen(script));
run_script("test.sh");
EXPECT(strcmp(env_get("FROM_SCRIPT"), "ok") == 0,
       "script's `export FROM_SCRIPT=ok` is visible to caller");

/* (g) the raw-mode bridge: ESC [ A flows through TTY in TTY_RAW */
uint32_t prev = tty_set_mode(TTY_RAW);
char csi_up[3] = { 27, '[', 'A' };
tty_inject(csi_up, 3);
char rb[3] = {0};
for (int i = 0; i < 3; i++) sys_read(0, &rb[i], 1);
tty_set_mode(prev);
EXPECT(rb[0] == 27 && rb[1] == '[' && rb[2] == 'A', ...);
```

The (g) check is the one that needs a comment: `tty_inject` pushes bytes directly into the kernel's TTY-input ring as if they had already been translated from scancodes. It does NOT exercise `kbd_irq`'s 0xE0 → ESC[A path; that only fires from a real PS/2 IRQ. What (g) DOES verify is that once the kernel has ESC[A in the ring (regardless of how it got there), our `tty_set_mode(TTY_RAW)` plus byte-at-a-time `sys_read(0, &c, 1)` delivers all three bytes intact to userspace. That's the contract `read_line_interactive`'s ESC parser relies on. The keyboard → ESC[A side is covered by booting interactively and pressing Up.

## What you actually do with it now

```
advent$ export NAME=adventos
advent$ echo hello $NAME
hello adventos
advent$ history
  1  export NAME=adventos
  2  echo hello $NAME
  3  history
advent$ <Up>           ← cycles back to "history"
advent$ <Up>           ← cycles back to "echo hello $NAME"
advent$ <Enter>
hello adventos
advent$ ls
  hello.elf
  sh.elf
  cat.elf
  ...
advent$ ca<Tab>
advent$ cat hello.txt  ← completion + space appended
advent$ <Ctrl-C>       ← line discarded, fresh prompt
advent$
```

And scripts work:

```
advent$ ed test.sh           ← write a script via the line editor
*a
export GREETING=hi
echo $GREETING from script
.
w
q
advent$ sh test.sh
hi from script
advent$ source test.sh
hi from script
advent$ echo $GREETING       ← only `source` leaks the var back
hi
```

## Known gaps

- **Persistent `perm.txt` state breaks t31 across reboots.** Spotted while running t32 — t31's `perm.txt` is chmod'd to 0600 and chowned to uid 1000 mid-test, but never reset, so on the *second* boot of the same `os.img`, t31's "default mode 0644 on new file" assertion sees the stale 0600 file and fails. Flagged for follow-up; not session 49's fault.
- **No mid-line editing.** Left/Right arrow are parsed and ignored. Backspace works from the end. To edit a typo at position 5 in a 20-char line, currently: Backspace 15 times, retype.
- **ESC alone hangs the parser for one keystroke.** After `c == 27` we unconditionally read 2 more bytes; if the user really did press just ESC, those reads block until the next keypress. A `sys_read` with O_NONBLOCK / `TIOCINQ` would be the right fix.
- **No envp through `exec`.** `sh -c 'echo $PATH'` would not see exported vars (and we don't have `-c` anyway). Only inline expansion + `source` propagate the env.
- **No quoting, no escapes.** `echo "hello world"` is two args, `\n` is two characters. Real quote handling needs a stateful tokenize.
- **Tab completes from cwd only.** Not from PATH; not directory-filtered for `cd`.
- **History doesn't persist.** Reboots wipe it.

## Files touched

```
kernel/keyboard.c          +43 -3    0xE0 prefix, push_csi, e0_prefix state
user/sh.c                +431 -69    env/history/expansion/line editor/script runner
docs/49-shell-polish.md     +new     this file
```
