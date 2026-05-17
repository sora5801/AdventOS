# Session 85 — Usable Unix Phase 3: man pages

**Goal.** Close the discoverability gap. With sessions 83 and 84 the OS has working coreutils and a usable shell, but you only know how anything works if you've read the source. A `man` system gives every user a one-keystroke path from "what does this tool do?" to "here are the flags, here's an example."

Status: **done.** `man <topic>` reads `/man/<topic>` and dumps it; `man -k WORD` lists every page whose name contains WORD; `man` with no args lists every available topic. 26 pages installed at first cut covering every daily-use tool. ~140 lines of C for the binary; ~24 KB of text content under `/man/`.

---

## Design choices

### Flat layout, no section numbers

Real Unix segregates man pages by section: `man 1 cp` (commands), `man 3 printf` (libc), `man 5 passwd` (file formats), and so on. The section directory structure mirrors that: `/usr/share/man/man1/cp.1.gz`, etc.

For AdventOS at this scale, the sections add complexity without value. There are no name collisions today between commands and libc functions or file formats — `cp` is unambiguous, `passwd` would only ever mean the file format because there's no `passwd` binary. So this session uses a flat `/man/<topic>` layout. A section suffix can be retrofitted later if a real collision ever appears (e.g., `/man/signal.h` for the C header alongside `/man/signal` for the syscall family).

### Plain text, no markup

Real Unix `man` runs each page through `nroff`/`groff` to format it for the terminal — bold headings, indentation rules, line wrap to the current terminal width. AdventOS pages are plain ASCII text, written with the final layout already in place. Every page conforms to an 80-column hard limit and uses the same skeleton: NAME / SYNOPSIS / DESCRIPTION / EXAMPLES / EXIT STATUS / SEE ALSO / NOTES.

Trade-off: pages can't reflow when the terminal is wider or narrower than 80. The 80-col console renders pages exactly as authored, which is the only width that matters here.

### No pager

GNU `man` pipes its output through `less` so long pages don't scroll off the top. AdventOS `man` does a `cat`-style straight-through dump. Pages are deliberately kept under ~60 lines so this isn't a problem on an 80x25 console (the typical fbcon dimensions in QEMU).

If a page grows past one screen, the user can pipe to `head -25` for the top or `tail -25` for the bottom — both already work and respect the manual layout. A real pager is filed as a separate-session candidate; the right shape would be a small `less`-like binary that any caller can pipe into, not just `man`.

### `man` reads files from `/man/`

Not embedded into the binary. The pages live as plain files in the FS, which means:

- New pages drop into `fs/man/<topic>` and get added to `mkfs.py`'s `DATA_FILES` list. Build, reboot, page is live.
- Users with permission can `vi /man/sh` and edit the page directly.
- `man -k WORD` is trivially `sys_readdir("/man/")` + substring filter on the name.

The binary is ~140 lines of C, ~5.7 KB compiled. The 26 pages total ~24 KB of text. Net storage cost is dominated by the content, which is the right shape.

---

## The binary

`user/man.c` has four moving parts:

```c
build_man_path("cp", out, cap)        →  "/man/cp"
                                          refuses any '/' in the topic
                                          (path-traversal defense)

show_page(topic)                      →  open /man/<topic>, read 512-byte
                                          chunks, write to stdout, close

list_pages(filter)                    →  sys_readdir("/man") loop;
                                          if filter is non-empty, only
                                          print names containing it

main argv dispatch                    →  no args:        list_pages(NULL)
                                          one arg:        show_page(arg)
                                          two args "-k W": list_pages(W)
                                          -h:             usage
                                          anything else:  usage to stderr
```

The argv parser deliberately rejects multi-topic forms (`man cp mv`). One topic at a time, one page out — keeps the dispatch obvious and matches what a user usually wants.

Path-traversal defense is one check: `if (topic[i] == '/') return -1`. There's no need to canonicalize `..` or symlinks because the FS doesn't have any of that — every `/` in a topic name is a refusal.

---

## Pages

All 26 pages live under `fs/man/` and follow the same skeleton:

```
NAME
    <tool> - <one-line description>

SYNOPSIS
    <usage form(s)>

DESCRIPTION
    <2-4 paragraph explanation>

EXAMPLES
    <3-5 representative invocations>

EXIT STATUS
    0   <success case>
    1   <typical failure>

SEE ALSO
    <related man topics>

NOTES
    <what's deliberately not implemented; session-history pointer>
```

Pages this first cut ships:

| Category | Pages |
|---|---|
| Session-83 coreutils gap-fill | cp, mv, rm, mkdir, rmdir, chmod, touch, find |
| Pre-existing coreutils | ls, cat, grep, sort, head, tail, wc, ps, kill, date, echo, pwd, id, sleep |
| System | sh, ed, vi, man |

Size per page averages 800-1500 bytes. The longest is `sh` (~2 KB) because it documents both the shell language and the line editor; the shortest is `pwd` (~500 bytes) because pwd does literally one thing.

### Inter-page references

The `SEE ALSO` section uses the convention `name(N)` where N is the (notional) section number — even though AdventOS doesn't use sections. This is so that if/when sections become real, existing pages already have the right link format and don't need a sweep.

For now, `cp(1)` reads as "the cp page (in the commands section, which is the only section that exists)."

### What pages are NOT in this cut

Pages that exist as binaries but aren't documented yet:

- `tee`, `tr`, `seq` — small enough to be self-explanatory from `--help`.
- `nc`, `wget`, `telnet`, `irc`, `ircd`, `ssh`, `sshd`, `httpd`, `httpsd`, `httpsget` — network apps. Each deserves a real page; deferred to a "session 86 — network app docs" follow-up.
- `cryptotest`, `rsatest`, `usbtest`, `beep`, `dbg`, `dbgtest`, `sandbox`, `kvctl`, `agentctl`, `smp-hammer` — test/diagnostic tools. Self-documenting via their `--help` or selftest descriptions.
- `pluck`, `where`, `count` — structured-pipeline filters. Already documented at length in `docs/69-structured-pipelines.md`; the man pages for them are queued but those three are mostly used via the `|>` operator anyway.
- All the `*-selftest` binaries — internal test infrastructure; the harness doc in `docs/68-smp2-deadlock-fixes.md` and the README's selftest section cover them.

The 26 pages here cover the daily-use surface. The remaining 15-20 binaries can have pages added in a small follow-up commit — each is a 30-line text file, no new code needed.

---

## How to add a new man page

```bash
# 1. Write the page (use an existing page as template).
$EDITOR fs/man/newtool

# 2. Add it to mkfs.py's DATA_FILES list, parent='man':
#       ('newtool', 'fs/man/newtool', 'man'),

# 3. Rebuild + reboot:
bash build.sh
```

That's the whole flow. The `man` binary itself doesn't need changes — it walks `/man/` dynamically via `sys_readdir`.

---

## Mkfs hygiene

Two small mkfs.py changes:

```python
DIRECTORIES = [
    'etc',
    'mnt',
    'man',                   # session 85 — flat layout, no section dirs
    ('ssl', 'etc'),
]
```

```python
DATA_FILES = [
    ...,
    ('cp',    'fs/man/cp',    'man'),
    ('mv',    'fs/man/mv',    'man'),
    ...
]
```

The `'man'` directory becomes a top-level dir under root. Files in it use `parent='man'` so the FS table's `parent_dir` byte gets the right slot index when mkfs builds the image.

Total FS image growth from session 85: ~24 KB for the page content + 8 bytes for the directory entry + ~50 bytes for each page's `fs_entry`. Well within the AdventFS budget.

---

## Smoke test

```sh
advent$ man cp
NAME
    cp - copy a regular file

SYNOPSIS
    cp SRC DST
...

advent$ man -k file
cp
mv
rm
find
ls
cat
...

advent$ man
cat
chmod
cp
date
...

advent$ man notatopic
man: no manual entry for 'notatopic' (looked in /man/notatopic)

advent$ man -h
usage: man [TOPIC]    show the page for TOPIC
       man -k WORD    list pages whose name contains WORD
       man            list every available topic
```

`man find | head -25` exercises the pipeline path (use head to deal with longer pages until a pager lands).

---

## What's next in Path A

- **Phase 4 — vi polish.** `user/vi.c` is the largest remaining piece of the "Usable Unix" arc. Currently it has the modes (normal/insert/cmdline), basic motions, and minimal save/quit. Wanted next: search (/pattern + n), undo, search-and-replace, multi-line yank. Probably a couple sessions.

After Phase 4, "Path A — Usable Unix" is done. The natural next paths are the ones in the README's "Status & scope" section:

- **Path B — Self-hosting.** Port `tcc` so the OS can build its own programs.
- **Path C — Graphics.** Window manager on the VBE framebuffer.
- **Path D — Scripting.** Port Lua as both a system scripting language and a REPL.
- **Path E — Drivers.** virtio, AC97 consumer, more USB device classes.

---

## Files touched

```
user/man.c                          NEW — the man binary (~140 LOC)
fs/man/                             NEW directory holding 26 man pages:
    cp, mv, rm, mkdir, rmdir, chmod, touch, find
    ls, cat, grep, sort, head, tail, wc, ps, kill, date, echo
    sh, ed, vi, man, pwd, id, sleep
mkfs.py                             DIRECTORIES + DATA_FILES additions
                                    + 'man.elf' in USER_PROGRAMS
build.sh                            USER_PROGS gets 'man'
docs/72-man-pages.md                NEW — this file
README.md                           updated session pointer
```

Build size delta: `man.bin` is ~5.7 KB. FS image grew by ~24 KB of plain-text pages + the directory entry overhead. Kernel image unchanged.

No new syscalls.
