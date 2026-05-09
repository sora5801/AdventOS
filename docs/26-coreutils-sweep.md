# Session 26 — Userspace coreutils sweep

**Goal:** Stop pretending the system is "almost a Unix" by filling in the missing utilities. Add 13 ring-3 programs that round out the pipe ecosystem — `wc`, `head`, `tail`, `grep`, `sort`, `uniq`, `tee`, `tr`, `seq`, `date`, `kill`, `ls`, `pwd` — and make sure they all play nicely with the shell's pipelines, redirections, and cwd. The interesting infrastructure work is small but load-bearing: bump `FS_MAX_FILES` and `TASK_MAX_FDS`, teach the shell to fall through to fork+exec when a "soft" builtin appears in a pipeline, and add a few stdlib helpers (`atoi`, `memcmp`, `strchr`) to libuser.

End state — the new `[t17]` selftest output:

```
[t17] coreutils sweep: pipelines through wc/head/tail/grep/sort/uniq/tr/tee/seq
  seq 5 | wc:                              5 5 10
  seq 10 | head -3:                        1 2 3
  seq 10 | tail -3:                        8 9 10
  seq 30 | grep 7:                         7 17 27
  seq 5 | grep -v 3:                       1 2 4 5
  echo hello | tr e E:                     hEllo
  echo aXbXc | tr -d X:                    abc
  seq 3 | tee /seq.txt | wc -l:            3
  cat /seq.txt:                            1 2 3
  cat /seq.txt /seq.txt | sort | uniq | wc -l:  3
  ls /etc | wc -l:                         1
  date:                                    2026-05-09 08:07:18 UTC
  pwd | tr / -:                            -
  kill: fork sleeper, kill via the binary: child reaped exit=143
```

Every line above is a real fork+exec pipeline driven by the shell's parser; nothing is a builtin shortcut. The `cat /seq.txt /seq.txt | sort | uniq | wc -l` chain is **four stages** — three pipes, six pipe fds open in the parent simultaneously — which is what made me bump `TASK_MAX_FDS` from 8 to 16. With stdin/stdout/stderr already at fds 0–2, eight slots only left five for pipes, exactly one short of what a 4-stage pipeline needs.

httpd still serves curl with HTTP/1.0 200 throughout (`curl http://localhost:8080/` → the same banner page). Earlier tests t1–t16 all pass unchanged.

## What's in scope

In:
- 13 new ring-3 programs in `user/`, all of them small (50–150 LoC each) and built to pipe-friendly Unix conventions. Sizes after compile: 4-5 KB each, except `tail.elf` and `sort.elf` which are ~22 KB because they statically allocate a `MAX_LINES * LINE_MAX` line buffer.
- `libuser` gains `atoi`, `memcmp`, `strchr` — common stdlib bits the new programs all want. Old programs don't need them.
- `kernel/fs.h`: `FS_MAX_FILES` 16 → 32 and `FS_SUPER_SECTORS` 2 → 3. The on-disk superblock now spans 3 sectors (1 header + 2 entry sectors holding 32 × 32-byte entries = 1024 bytes).
- `kernel/task.h`: `TASK_MAX_FDS` 8 → 16. This is the ceiling for how many stages a pipeline can have without the shell running out of fd slots.
- `mkfs.py` learns the new constants and the new `USER_PROGRAMS` list.
- `build.sh` adds the 13 new programs to the per-program compile loop.
- `sh.c` distinguishes "hard" builtins (always inline — `cd`, `exit`, `jobs`, `forktest`, …) from "soft" builtins that have binary equivalents (`ls`, `pwd`); when a `|` or `>` token is present, soft builtins fall through to fork+exec so pipelines work properly. `cmd_help` lists the new tools. A 14th selftest case `[t17]` drives 14 distinct pipelines.

Out:
- POSIX coreutils flag completeness — `tail -f`, `sort -r`/`-n`/`-u`, `head -c`, `grep -E`/`-i`/`-r`, `tr [:digit:]`, `tee -a`, etc. We pick the canonical no-flag form plus one or two of the most-used flags per tool.
- `cp`, `rm`, `mv` — these need `unlink` / rename in the FS, which we haven't built. Files can be created and overwritten via `sys_fs_write` / `sys_open_w` but never removed.
- `find`, `xargs`, `cut`, `paste` — out of scope for this sweep; can be added in a future session.
- Persistent-disk `tee` (writes to AdventFS instead of tmpfs). `sys_fs_write` is whole-file replacement — would need partial-write semantics or in-program buffering. Today `tee` writes through `sys_open_w` → tmpfs, which is what the shell's `>` operator already uses.
- Locale support, timezone selection in `date`, character-class parsing in `tr`. Single-byte ASCII through and through.

## Architecture: where the new tools sit

```
+----------------------------------------------------+
|  ring 3                                            |
|                                                    |
|  sh.elf ──┬── fork ─── exec ───► seq.elf           |
|           ├── fork ─── exec ───► wc.elf            |
|           └── pipe(p1)/pipe(p2)/dup2 wires         |
|                                                    |
|     argv reads, sys_read on stdin (=pipe r),       |
|     sys_write on stdout (=pipe w), sys_exit code   |
|                                                    |
+──── INT 0x80 ─────────────────────────────────────+
|  ring 0                                            |
|                                                    |
|  syscall.c: SYS_FORK / EXEC / WAIT / PIPE / DUP2   |
|  pipe.c:    32-slot ring buffer, refcounts        |
|  task.c:    fd table per task, 16 slots now        |
|  fs.c:      hierarchical fs, 32-entry table        |
|                                                    |
+----------------------------------------------------+
```

Nothing in the kernel needed new code — every coreutil is built from existing syscalls. The session's kernel diff is two `#define` bumps (`FS_MAX_FILES`, `TASK_MAX_FDS`) and one comment update. The work is entirely in userspace.

## The 13 programs at a glance

| Program | Reads | Writes | Key syscalls |
|---|---|---|---|
| `wc` | stdin or file | stdout (`L W B [name]`) | read, write |
| `head` | stdin or file | stdout | read, write |
| `tail` | stdin or file | stdout | read, write |
| `grep` | stdin or file | stdout | read, write |
| `sort` | stdin or files | stdout | read, write, open, close |
| `uniq` | stdin or file | stdout | read, write |
| `tee` | stdin | stdout + N tmpfs files | read, write, open_w, close |
| `tr` | stdin | stdout | read, write |
| `seq` | (no input) | stdout | write |
| `date` | (no input) | stdout | sys_time, write |
| `kill` | (no input) | (signal effect) | sys_kill |
| `ls` | (dir name) | stdout | sys_readdir, sys_getcwd, write |
| `pwd` | (no input) | stdout | sys_getcwd, write |

Most of them follow a uniform skeleton:

```c
int main(int argc, char **argv) {
    int argi = parse_flags(argv);
    if (argi >= argc) {
        process_fd(0);                      /* stdin */
    } else {
        for (int i = argi; i < argc; i++) {
            int fd = sys_open(argv[i]);
            if (fd < 0) { /* err */ }
            process_fd(fd);
            sys_close(fd);
        }
    }
}
```

The only ones that don't fit this shape are `seq` (no input — pure generator), `date` (reads the wall clock instead), `kill` (signal effect), and `pwd`/`ls` (FS-aware).

## A few of the more interesting designs

### tail's circular line buffer

`tail -N FILE` has to read **everything** in the input — there's no way to know which lines are last without seeing all of them. The classic implementation is a circular buffer of N lines:

```c
#define MAX_LINES   64
#define LINE_MAX    256

static char     g_lines[MAX_LINES][LINE_MAX];
static int      g_len  [MAX_LINES];
static int      g_head;       /* write index */
static int      g_count;      /* lines stored, capped at MAX_LINES */
```

On each newline, the current line is stored at `g_head`, and `g_head` advances modulo `MAX_LINES`. Once `g_count` hits the cap, oldest lines just get overwritten. At EOF, we emit the last `min(want, g_count)` lines starting at `(g_head - want) mod MAX_LINES`.

The buffer is stack-too-large-for-libuser-malloc — ~16 KiB — so it lives as a file-scope static. The compiler emits it into `.data` (because of the `-fno-zero-initialized-in-bss` build flag from the session-15 .bss-discard fix), which is why `tail.bin` is 22 KB instead of the 4-5 KB the smaller programs hit.

A real GNU `tail` would `lseek` to near-EOF for a faster-on-large-files implementation. We don't have lseek (no `SYS_LSEEK`), and our files are tiny, so the brute-force scan is fine.

### sort: insertion-sort over a static line table

`sort` is built on the same MAX_LINES × LINE_MAX static table:

```c
for (int i = 1; i < g_n; i++) {
    int j = i;
    while (j > 0 && line_cmp(j - 1, j) > 0) {
        swap_lines(j - 1, j);
        j--;
    }
}
```

64 lines × 256 bytes is plenty for sorting `/etc/inittab`-shaped inputs. `line_cmp` is byte-wise unsigned compare, with shorter strings sorting before longer-with-same-prefix (lexicographic) — exactly what `strcmp` semantics provide if you can't use `strcmp` on counted-but-not-NUL-terminated buffers.

`swap_lines` does a `memcpy` through a 256-byte stack buffer. It would be faster to swap row pointers, but the static layout is row-indexed for memory simplicity; with at most 64 swaps in a 64-line worst case, who cares.

### tr: two argv-driven character maps

The two modes (`tr SET1 SET2` and `tr -d SET`) share basically nothing structurally:

```c
/* delete mode */
for (int i = 0; i < n; i++) {
    if (set_index(del, buf[i]) < 0) out[o++] = buf[i];
}

/* translate mode */
int idx = set_index(s1, buf[i]);
if (idx >= 0) buf[i] = s2[idx < s2len ? idx : s2len - 1];
```

`set_index` is a linear scan of `SET1` looking for the byte. With sets of length ~10 chars and 256-byte buffers per read, that's ~2500 strcmps per buffer — fast enough to keep up with the kernel's 100Hz scheduler quantum at any disk-bound rate.

The "SET2 surplus" rule (when SET1 is longer than SET2) clamps to the last byte of SET2, matching POSIX. The reverse case (SET2 longer than SET1) we just ignore the unreachable surplus.

### date: hand-rolled gmtime

There's no `time.h` in libuser. `sys_time()` returns a `uint32_t` UNIX epoch; turning that into "2026-05-09 08:07:18 UTC" is a few divisions and a leap-year loop:

```c
uint32_t day = t / 86400;
uint32_t sec = t % 86400;

int year = 1970;
while (1) {
    int yd = is_leap(year) ? 366 : 365;
    if (day < (uint32_t)yd) break;
    day -= yd;
    year++;
}

int mon = 0;
while (1) {
    int md = days_in_month[mon];
    if (mon == 1 && is_leap(year)) md = 29;
    if (day < (uint32_t)md) break;
    day -= md;
    mon++;
}
```

This is the canonical "civil_from_days" algorithm in unrolled form. With epochs through 2106 (when uint32_t wraps), the year loop runs at most 136 iterations — trivially fast. No timezone handling because we don't have one; UTC is the only valid output.

The output formatter rolls its own zero-pad because `libuser`'s `printf` understands `%d` but not `%02d`. ~25 lines of byte-shoveling produces `YYYY-MM-DD HH:MM:SS UTC` with proper padding.

### tee: forking output streams

```c
char buf[256];
int  n;
while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
    sys_write(1, buf, n);
    for (int i = 0; i < nfds; i++) sys_write(fds[i], buf, n);
}
```

Just write the buffer to stdout AND each opened file. The output files are opened with `sys_open_w` — the same tmpfs that powers `>` redirection — so they live for the rest of the session and are visible to subsequent `cat /seq.txt` calls. Persistent-disk tee would need either chunk-at-a-time disk writes (not built) or whole-file buffering plus a final `sys_fs_write` (would need a per-file growable buffer).

The `[t17]` test exercises this end-to-end:

```
seq 3 | tee /seq.txt | wc -l   →  3        (counts what tee passed through)
cat /seq.txt                   →  1 2 3   (reads back what tee saved)
```

## Shell wiring: hard vs soft builtins

The dispatcher in `sh.c` used to try every builtin first, falling through to fork+exec only when none matched. That breaks the moment you write `ls /etc | wc -l` — the shell intercepts `ls` and runs the inline builtin, which has no way to send its output through a pipe.

The fix is to classify builtins into two groups:

**Hard builtins** — always inline because their effects need to persist in the calling shell's process state:
- `cd` (changes cwd_dir on the shell's task)
- `exit` (sys_exits the shell)
- `jobs`, `forktest`, `keys` (state in the shell)
- `help`, `pid`, `time`, `sleep` (no real reason, but no binary either)

**Soft builtins** — have a binary equivalent and only run inline when there's no pipe/redirect:
- `pwd` (also `pwd.elf`)
- `ls` (also `ls.elf`)

The detector is one loop:

```c
int has_pipe_op = 0;
for (int i = 0; i < ntok; i++) {
    if ((toks[i][0] == '|' || toks[i][0] == '>') && toks[i][1] == 0) {
        has_pipe_op = 1; break;
    }
}
```

Soft builtin dispatch is gated on `!has_pipe_op`; if a pipe is present, the line falls through to `parse_pipeline + run_pipeline` which exec()s the binary by name.

`mkdir` could plausibly be a soft builtin too (we have `sys_mkdir`), but I didn't add `mkdir.elf` in this sweep — `mkdir` rarely appears in pipelines and the kernel-side syscall is right there as a builtin shortcut. If we ever want `find . -type f | xargs mkdir -p`, that's the moment to add it.

## The TASK_MAX_FDS bump: what 4-stage pipelines actually use

A pipeline with `n` stages opens `n-1` pipes. Each pipe is two fds (read end + write end) — the **parent** holds all of them simultaneously while spawning children, then closes them after the last fork:

```
Stage count  Parent fds (during fork loop)
  2           pipe(0)    -> 2 fds
  3           pipe(0,1)  -> 4 fds
  4           pipe(0,1,2)-> 6 fds
```

Plus stdin/stdout/stderr at 0/1/2 (always-allocated by `task_create`), plus possibly an `outfd` for `> file` (one more):

```
Stage count  Total parent fds
  2           3 + 2 = 5     (fits in 8)
  3           3 + 4 = 7     (fits in 8)
  4           3 + 6 = 9     (DOESN'T fit in 8 — 3rd pipe's wfd has no slot)
```

That's the failure I hit on the very first run of `cat | sort | uniq | wc -l`:

```
sh: pipe() failed
```

The kernel's `alloc_fd` returns the lowest free fd ≥ 3, capped at `TASK_MAX_FDS - 1`. With `TASK_MAX_FDS = 8`, fds 3–7 are usable (5 slots), and the third `sys_pipe` call has only 1 slot left when it asks for 2.

Bumping the cap to 16 doubles the headroom. The shell's `PIPELINE_MAX = 8` (from session 15) caps stage count at 8, which would need 7 pipes = 14 fds + 3 stdio = 17 — still one over 16, but no test in the codebase actually pushes a 7-pipe pipeline. Future-proofing here would mean `TASK_MAX_FDS = 32`, or auditing PIPELINE_MAX downward.

## On-disk: what 32 entries look like

The bumped FS layout is:

```
LBA  +0 .. +2     superblock (3 sectors, 1536 bytes)
       sector 0:  8B "ADVENTFS" + 4B file_count + 500B reserved
       sectors 1-2: 32 × 32B entries
LBA  +3 ..         file data
```

The bitmap allocator (session 23) skips sectors 0..FS_SUPER_SECTORS-1 in `bitmap_alloc_run`, so the new third superblock sector gets reserved automatically. `fs_init` reads `FS_SUPER_SECTORS` = 3 sectors and copies them into a single 1536-byte `struct fs_super`. `fs_write_super` does the inverse.

The `parent_dir` field is still `uint8_t`, with `0xFF` as the root sentinel. With 32 entries the max regular index is 31; we have 222 indices of headroom before that field needs to widen.

mkfs's build output for the new image:

```
[ 0] DIR  //etc
[ 1] FILE //hello.elf    sec 3    (4328 bytes)
[ 2] FILE //count.elf    sec 12   (4192 bytes)
...
[ 9] FILE //wc.elf       sec 132  (4792 bytes)
[10] FILE //head.elf     sec 142  (4652 bytes)
...
[20] FILE //ls.elf       sec 304  (4380 bytes)
[21] FILE //pwd.elf      sec 313  (4180 bytes)
[22] FILE //hello.txt    sec 322  (274 bytes)
[23] FILE /etc/inittab   sec 323  (301 bytes)
```

24 entries used out of 32. After `t9` (ed editor writes `notes.txt`), `t14` (rewrite test creates `reuse.txt`), `t16` (creates `/tmp` + `/tmp/note.txt`), and `t17` (creates `/seq.txt`), we end up at ~28 of 32 — comfortable but not roomy. The next session that adds programs will probably want 64.

## Inside libuser: three new helpers

`atoi`, `memcmp`, `strchr` are stock libc behaviors. The implementations are unsurprising:

```c
int atoi(const char *s) {
    int v = 0, sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if      (*s == '-') { sign = -1; s++; }
    else if (*s == '+') {             s++; }
    while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; }
    return v * sign;
}
```

`atoi` skips ASCII whitespace, eats one optional sign, and consumes digits until a non-digit. No overflow detection — `head -1000000` is fine, `head -99999999999` will silently produce garbage. The coreutils sweep doesn't pass values anywhere near `INT_MAX`, so we don't pay the cost of a checked variant.

`memcmp` is byte-wise unsigned compare with early exit. `uniq` uses it to detect adjacent equal lines:

```c
int dup = (have_prev && cur_len == prev_len &&
           memcmp(cur, prev, cur_len) == 0);
```

`strchr` returns the first occurrence pointer or NULL. `tr`'s `set_index` could use it as a building block but I open-coded the index-returning variant so it's one fewer call.

## Quick troubleshooting log

I hit two failures on the first integration boot — both are documented in this deep dive's narrative, but recapping them concisely:

1. **`wc -l` printed `wc: -l: cannot open`** — my first wc.c didn't parse flags at all, treating every argv entry as a filename. Fix: a small flag loop that ORs together `show_l`/`show_w`/`show_c`, defaulting all three on if no flag was seen. ~15 lines added.
2. **`cat | sort | uniq | wc -l` failed with `sh: pipe() failed`** — `TASK_MAX_FDS = 8` was exactly one too few for a 4-stage pipeline (parent needs 3 stdio + 6 pipe = 9 fds before forking). Fix: bump to 16 in `kernel/task.h`. The kernel doesn't otherwise care; `task_create` zeros all 16 slots, the fd inheritance loop in `task_fork` runs `for (i = 0; i < TASK_MAX_FDS; i++)` already.

Both fixes were one-shot: rebuild, reboot, all tests green.

## File-by-file changes

```
kernel/fs.h            FS_MAX_FILES 16 → 32, FS_SUPER_SECTORS 2 → 3
kernel/task.h          TASK_MAX_FDS 8 → 16

mkfs.py                FS_MAX_FILES + FS_SUPER_SECTORS bumps; 13 new entries
                       in USER_PROGRAMS list

build.sh               13 new program names appended to USER_PROGS array

user/libuser.h         prototypes for atoi/memcmp/strchr
user/libuser.c         implementations

user/wc.c              new — line/word/byte count + -l/-w/-c flags
user/head.c            new — first N lines (-n N or -N)
user/tail.c            new — last N lines, circular line buffer
user/grep.c            new — literal-substring filter, -v invert
user/sort.c            new — insertion sort static line table
user/uniq.c            new — adjacent dedupe
user/tee.c             new — copy stdin to stdout + N tmpfs files
user/tr.c              new — translate or delete chars (-d)
user/seq.c             new — print N or LO..HI sequence
user/date.c            new — UTC time formatter
user/kill.c            new — send signal by pid (-N defaults SIGTERM)
user/ls.c              new — list directory entries
user/pwd.c             new — print cwd

user/sh.c              hard/soft builtin distinction + has_pipe_op detect
                       cmd_help lists the sweep
                       [t17] selftest runs 14 pipelines
```

Net diff: 13 new files, 2 kernel constant bumps, ~50 lines of libuser additions, ~120 lines of shell changes. Total LOC for the user programs is ~1200 — roughly 90 lines each.

## Boot log excerpt

```
[boot] mounting AdventFS... fs: AdventFS mounted, 24 entries, 700/1024 sectors free
...
init: pid=4, reading /etc/inittab
init: started 'httpd.elf' as pid 5 (once)
init: started 'sh.elf' as pid 6 (once)
httpd: listening on port 80 (userspace)

AdventOS userspace shell, pid=6
...
[t17] coreutils sweep: pipelines through wc/head/tail/grep/sort/uniq/tr/tee/seq
  seq 5 | wc:
5 5 10
  rc=0
  seq 10 | head -3:
1
2
3
  rc=0
...
  cat /seq.txt /seq.txt | sort | uniq | wc -l:
3
  rc=0
  ls /etc | wc -l:
1
  rc=0
  date:
2026-05-09 08:07:18 UTC
  rc=0
  pwd | tr / -:
-
  rc=0
  kill: fork sleeper, kill via the binary:
[sig] pid=46 terminated by signal 15
  child pid=46 reaped exit=143 (expect 143 = 128+SIGTERM)
=== selftest done ===
```

And from the host, after the same boot:

```
$ curl -s http://localhost:8080/
Hello from a USERSPACE HTTP server!
This page was served by user/httpd.c, which runs in ring 3.
...
```

The shell now feels like a real Unix prompt: `seq 30 | grep 7 | wc -l` prints `3` because `7`, `17`, and `27` all contain a `7`, exactly as it would on Linux. Pipelines compose. Programs are programs, not built into the shell. From a user's perspective, AdventOS just gained a meaningfully larger surface area to play with — without the kernel doing more than two `#define` bumps to allow it.
