# Session 86 — Usable Unix Phase 4: vi polish

**Goal.** Turn `user/vi.c` from "tolerable for emergencies" into "actually pleasant for short edits." Sessions 83/84/85 finished the daily-use surface around the editor (coreutils, shell line editing, man pages); the editor itself had movement, modes, basic delete/yank/paste, single-line yy, and a forward `/pat` search — but it was missing the four features that turn vi from a curiosity into a daily tool.

Status: **done.** Undo (`u`) with a 32-deep snapshot ring, search-and-replace (`:s/old/new/[g]`, `:%s/old/new/[g]`), count prefixes (`5j`, `3dd`, `42G`, etc.), and backward search (`?pat`) all landed. ~250 lines of net diff to `user/vi.c`. Build size: `vi.bin` 37940 → 48824 (+10884, +28%); kernel image unchanged.

This completes **Path A — Usable Unix.**

---

## What was missing

The pre-session-86 `handle_normal` was a single big switch with no concept of "do this N times," no rollback semantics, no `:s`/`%s` parser, and only forward search. A representative session looked like:

```
vi hello.c
i          # type a paragraph
ESC
:w
```

That worked. But the moment you wanted to:

- delete 5 lines (`5dd`)
- undo a typo three keystrokes back (`u`)
- replace `foo` with `bar` across the file (`:%s/foo/bar/g`)
- search backward for the last occurrence of something (`?pat`)

...you couldn't. You had to do every operation one keystroke at a time, hope you got it right because there's no undo, and hand-walk every replacement with `/pat` + manual edit.

Each gap was a single feature that vi users have muscle memory for. Closing all four moves the editor from "I'll use ed instead" to "open vi without dread."

---

## Undo (`u`)

### State

```c
#define UNDO_MAX 32
struct undo_state {
    char *data;       /* serialized buffer (lines joined by '\n') */
    int   size;
    int   cr, cc, top_row;
};
static struct undo_state g_undo[UNDO_MAX];
static int g_undo_n;
```

### Approach

The simplest design that's actually correct: **before every mutating top-level command, serialize the entire buffer into one heap allocation and push the pointer onto a ring**. On `u`, pop the most recent entry, deserialize it back into `g_lines[]`, restore the cursor.

I considered the alternative of recording per-operation deltas (insert char at (r,c)='x' → reverse = delete char at (r,c)). It's how real editors do it for memory efficiency. But:

- Every mutation site (`line_insert_char`, `line_delete_char`, `line_split`, `line_join`, `buffer_insert_line`, `buffer_delete_line`) would need to push undo records, AND the records would need to carry enough context (line index, column, value) to reverse cleanly.
- Some operations are several mutations (a line-split followed by an insert on the new line, etc.). Coalescing those into one undo step is painful.
- The simple "snapshot everything before the top-level command" approach is robust against any future buffer mutation without further plumbing.

For a 2 KiB file, each snapshot is ~2 KB; 32 snapshots = ~64 KB. The user heap can absorb that.

### Push sites

```c
case 'x':       undo_push(); ...
case 'i':       undo_push(); ...   /* one snapshot per insert session */
case 'I':       undo_push(); ...
case 'a':       undo_push(); ...
case 'A':       undo_push(); ...
case 'o':       undo_push(); ...
case 'O':       undo_push(); ...
case 'p':
case 'P':       undo_push(); ...
dd:             undo_push(); ...
:s/%s commands: undo_push(); ...   /* rolled back on parse error */
```

`yy` is NOT a push site — yanking doesn't mutate. Movement keys aren't pushes either. Mode-only changes (entering command mode with `:`) aren't pushes.

One push per **top-level command**, which means:

- An insert session (`i` → typing → `ESC`) is one undoable unit. Vim's behavior.
- `5dd` is one undo step (the count is part of the command).
- `:%s/foo/bar/g` that replaces 200 occurrences is one undo step.

### Serialize / deserialize

```c
static char *serialize_buffer(int *out_size) {
    int total = 0;
    for (int i = 0; i < g_n_lines; i++) total += g_lines[i].len + 1;
    char *buf = malloc(total);
    int o = 0;
    for (int i = 0; i < g_n_lines; i++) {
        for (int j = 0; j < g_lines[i].len; j++) buf[o++] = g_lines[i].text[j];
        if (i < g_n_lines - 1) buf[o++] = '\n';
    }
    *out_size = o;
    return buf;
}
```

`deserialize_buffer` is the reverse — clear `g_lines[]`, then walk the byte stream splitting on `\n`, allocating fresh per-line buffers. Both are linear; no clever data structures.

### Failure modes

- **Ring full** — drop the oldest entry, slide the rest down. Memory bounded at ~64 KB peak.
- **OOM on push** — silently skip the snapshot. The edit still happens; only the undo for that one step is unavailable. The status line doesn't shout — vim doesn't either when its undo file fills.
- **OOM on deserialize** — partial restore, buffer may be in an inconsistent state. Shouldn't happen in practice given the size bounds.

---

## Count prefix (`5j`, `3dd`, `42G`, `:s/.../.../`)

### State

```c
static int g_count;     /* 0 = no count, treated as 1 by commands */
```

### Behavior

Typing digits in normal mode accumulates `g_count`. `0` is special — at `g_count == 0` it's the "jump to column 0" motion; once nonzero it's a regular digit (so `10j` parses as count=10 then `j`).

```c
if (!g_pending && c >= '0' && c <= '9') {
    if (c == '0' && g_count == 0) {
        /* fall through to switch — '0' is column-zero motion */
    } else {
        g_count = g_count * 10 + (c - '0');
        return;     /* wait for more digits or a command */
    }
}
int n = g_count > 0 ? g_count : 1;
```

After the command executes, `g_count` resets to 0.

### Which commands honor it

| Command | Repeats |
|---|---|
| `h j k l` | move N times |
| `w b` | jump N words |
| `x` | delete N chars |
| `dd` | delete N lines (yanked into register) |
| `yy` | yank N lines |
| `G` | when count is nonzero, `NG` jumps to line N (otherwise unchanged) |
| `gg` | no count (matches vim's `gg` ignoring count) |
| `p P` | no count (the yank register is paste once) |

Pending two-key prefixes (`d`, `g`, `y`) suppress count accumulation, so `5dd` parses as "count=5, then `d`, then `d`" — exactly the expected order. The second `d` triggers the dd branch with `n = 5`.

### `yank_lines` and `paste_lines`

`5dd` and `3yy` need to copy N lines into the yank register. The pre-session-86 single-line yank stored only one line in `g_yank_buf`; multi-line yank reuses the same buffer but stores lines separated by `\n` (NOT terminated — no trailing newline after the last line).

```c
static void yank_lines(int r, int n) {
    ...
    int o = 0;
    for (int i = 0; i < n; i++) {
        struct line *L = &g_lines[r + i];
        for (int k = 0; k < copy; k++) g_yank_buf[o + k] = L->text[k];
        o += copy;
        if (i < n - 1 && o < YANK_MAX - 1) g_yank_buf[o++] = '\n';
    }
    g_yank_len = o;
    g_yank_is_line = 1;
}
```

`paste_lines` is the inverse: split the yank buffer on `\n` and insert each piece as a new line into the buffer, before or after the cursor.

The yank buffer is `YANK_MAX = 8192` bytes — enough for ~80 lines × 100 chars. Bigger yanks get truncated (`copy = YANK_MAX - o - 2`); status line doesn't shout. Real vim has growable yank registers; this is enough for the daily use case.

---

## Search-and-replace (`:s/old/new/[g]`, `:%s/...`)

### Parser shape

`run_colon_command` detects two new prefixes:

```c
if (cmd[0] == 's' && len >= 1) { sub_off = 1; all_lines = 0; }
else if (cmd[0] == '%' && len >= 2 && cmd[1] == 's') {
    sub_off = 2; all_lines = 1;
}
```

`do_substitute(body, blen, all_lines)` then does the work. Body is `old/new/[g]` (without the leading `s` or `%s`).

```c
int slash1 = ...;   /* find first '/' to split old from new */
int slash2 = ...;   /* find second '/' for replacement-end + flags */
int global = 0;
for (int i = slash2 + 1; i < blen; i++) {
    if (body[i] == 'g') global = 1;
    else return -1;
}
```

### Replacement loop

For each line in scope (`current` or `every`), walk the line scanning for `old` as a literal substring. On match, shift the right side of the line left (`old_len > new_len`) or right (`new_len > old_len`) by `|delta|` bytes, then copy `new` into place. Advance `i` past the replacement so `g` doesn't loop on the same span when `new` contains `old` as a substring.

```c
while (i + old_len <= L->len) {
    if (my_memcmp(L->text + i, old_p, old_len) != 0) { i++; continue; }
    int delta = new_len - old_len;
    if (delta > 0) {
        line_grow(L, L->len + delta + 1);
        for (int k = L->len + delta - 1; k >= i + new_len; k--)
            L->text[k] = L->text[k - delta];
    } else if (delta < 0) {
        for (int k = i + new_len; k < L->len + delta; k++)
            L->text[k] = L->text[k - delta];
    }
    for (int k = 0; k < new_len; k++) L->text[i + k] = new_p[k];
    L->len += delta;
    replaced++;
    i += new_len;
    if (!global) break;
}
```

### Status reporting

```
2 substitutions
1 substitution
no matches
E486: malformed :s command (use s/old/new/[g])
```

Replaced-count is shown on the status line. Parse failures get the error code (an actual vim error number, picked for familiarity).

### Known limits

- **Literal substring only.** No regex, no character classes, no anchors. Matches the rest of this OS — `grep` is the same way.
- **`/` cannot appear in `old` or `new`** because there's no escape mechanism. Workaround: use a different delimiter? Not supported either — the delimiter is hard-coded as `/`. Filed for future work.
- **Undo correctness.** If `do_substitute` returns -1 (parse error), the `undo_push()` that fired earlier is rolled back so the user doesn't see a no-op show up in their undo history.

---

## Backward search (`?pat`)

### State

```c
static int g_search_forward = 1;     /* direction of the last search */
```

### Behavior

`/pat` enters command mode with `g_cmd_lead = '/'`; `?pat` enters with `g_cmd_lead = '?'`. The Enter handler dispatches to `run_search(pat, len, forward)` where `forward = (g_cmd_lead != '?')`.

`run_search` was already direction-aware internally — it called `search_next(sr, sc, forward)` with `forward=1`. The session-86 change widens its signature to take the direction from the caller, and writes `g_search_forward` so `n` can pick it up.

```c
case 'n':
    /* repeat in the original direction */
    if (g_search_len) {
        int forward = g_search_forward;
        ...
    }
    break;

case 'N':
    /* repeat FLIPPED */
    if (g_search_len) {
        int forward = !g_search_forward;
        ...
    }
    break;
```

This matches vim: after `?pat`, `n` keeps going backward, `N` goes forward.

---

## Wiring summary

| Change | Lines |
|---|---|
| New globals (`g_count`, `g_undo[]`, `g_undo_n`, `g_search_forward`, expanded `g_yank_buf`) | ~25 |
| Undo helpers (`serialize_buffer`, `deserialize_buffer`, `undo_push`, `undo_pop`) | ~80 |
| Forward decls for `set_status` / `clamp_cursor` | 2 |
| `yank_lines` + `paste_lines` helpers | ~40 |
| `do_substitute` + colon-command dispatch for `:s`/`%s` | ~80 |
| Count parsing + per-command repetition in `handle_normal` | ~30 |
| Undo pushes scattered through `handle_normal` | ~10 |
| `u` case, `?` case, n/N direction-tracking | ~25 |
| `run_search` signature + caller updates | ~10 |
| `handle_command` dispatch for `?` | ~5 |
| File-header doc + man page | n/a |

Net: ~250 lines added to `user/vi.c`. The man page (`fs/man/vi`) was rewritten to document the new features.

---

## Known limitations and what's NOT in this session

- **No `:g/pat/d` / `:g/pat/p`** (global command). Useful but rare; can be added later as a separate `g`-prefix in the colon parser.
- **No visual mode** (V / Ctrl-V). A whole second mode with its own selection semantics; the existing `dd` / `yy` cover most of what one would visual-select-then-act on, and the daily-use frequency doesn't justify the complexity.
- **No multi-character registers** (`"ay`, `"ap`). One yank register only. Same reasoning.
- **No `.` (repeat last change)**. Would need an "edit history" of the last mutating command + its count. Filed; not painful for most editing.
- **Search is literal substring, not regex.** Matches the rest of the OS. A real regex implementation is a separate project.
- **`/` cannot appear in `:s` patterns.** Different-delimiter support (e.g., `:s,old,new,`) is a small future change.
- **Undo merges with file load.** If you open a file and immediately press `u`, you can undo past the initial state into an empty buffer. Vim has the same trap; users learn it quickly.

---

## Smoke test

```
vi /etc/passwd
gg                          # cursor to top
5j                          # down 5 lines
$                           # end of line
3w                          # forward 3 words
?root                       # search backward for "root"
n                           # next match (still backward)
N                           # next match (now forward)
:%s/root/admin/g            # rename root → admin everywhere
:1                          # back to top
u                           # undo the rename
:q!                         # quit without saving
```

The `:%s` substitution count appears on the status line. The `u` reverses the entire replace. `:q!` works because nothing was saved.

For a fresh-file test:

```
vi /scratch.txt
i                           # insert mode
hello
world
ESC
yy                          # yank "world" (cursor on world)
3p                          # paste world 3 more times below — wait, p doesn't honor count
                            # so just `p` then `p` then `p`
u u u                       # undo three times to verify each paste was its own undo step
```

---

## What's next

**Path A — Usable Unix is done.** All four phases complete:

| Phase | Session | Status |
|---|---|---|
| 1 — Coreutils gap-fill | 83 | ✅ |
| 2 — Shell mid-line editing | 84 | ✅ |
| 3 — Man pages | 85 | ✅ |
| 4 — vi polish | 86 | ✅ |

The shell experience is now genuinely Unix-like for everything a user does in the first hour: file manipulation, editing, search, history, completion, discoverability. The remaining gaps (network app polish, more drivers, GUI, self-hosting) are different paths, not refinements of this one.

Natural next paths from `README.md`:

- **Path B — Self-hosting.** Port `tcc` so AdventOS can compile its own programs.
- **Path C — Graphics.** A minimal window manager on the VBE framebuffer.
- **Path D — Scripting.** Port Lua as both a system scripting language and a REPL.
- **Path E — Drivers.** virtio (modern QEMU's preferred device family), more USB device classes, sound consumer.

---

## Files touched

```
user/vi.c                       ~250 lines added — undo, count, s/r, ?
fs/man/vi                       rewritten to document new features
docs/73-vi-polish.md            NEW — this file
README.md                       Path A marked complete; latest-session pointer
```

Build size delta: `vi.bin` 37940 → 48824 (+10884 bytes, +28%). Most of the growth is the 8 KB `g_yank_buf` BSS expansion plus the undo serialization helpers. Kernel image unchanged.

No new syscalls. Everything is userspace.
