# Session 19 — `ed` line editor, AdventFS write, and the .bss fix

**Goal:** Add a real text editor — line-oriented in the spirit of `ed`(1) — and the on-disk filesystem write support it forces. Until this session AdventFS was strictly read-only; the only writable storage was the in-RAM tmpfs from session 15, lost on every reboot. To be a useful editor `ed` has to *save* what you typed in a way the next boot can read back.

End state — the new `[t9]` selftest at boot:

```
[t9] fs write + ed editor pipeline
  sys_fs_write notes.txt -> 0  (17 bytes)
  read back 17 bytes:
alpha
beta
gamma
17                              ← ed prints byte count of loaded file
1d                              ← injected commands echo as they're "typed"
a
delta
.
w notes.txt
17                              ← ed prints byte count saved
q
[user task pid=16 exited code=0]
  ed exited code=0
  final notes.txt (17 bytes):
beta
gamma
delta
=== selftest done ===
```

The shell:
1. Wrote `alpha\nbeta\ngamma\n` to `notes.txt` via `SYS_FS_WRITE` — straight to disk, no tmpfs in the path.
2. Re-read it through `sys_open` + `sys_read` to confirm the bytes hit disk.
3. Injected ed commands into the keyboard input ring, fork+exec'd `ed.elf notes.txt`, and waited.
4. ed loaded the file (printed `17`), executed `1d` (delete alpha), `a delta .` (append "delta" after current), `w` (save 17 bytes), `q` (quit).
5. The shell re-read `notes.txt` — now `beta\ngamma\ndelta\n`.

A reboot of the same `os.img` shows `9 files (free sec 81..1024)` instead of the original 8 — `notes.txt` survived.

`httpd.elf` keeps serving curl on :80 throughout.

## What's in scope

In:
- `fs_write_all(name, data, size)` in `kernel/fs.c` — creates-or-truncates a disk file, allocates a fresh contiguous sector run from the FS area's free space, persists the updated superblock back to disk
- `g_high_water` tracking — the next free sector past all currently-allocated files
- `SYS_FS_WRITE = 31` syscall + libuser `sys_fs_write` wrapper
- `user/ed.c` — full ed-style line editor (~400 lines): doubly-linked-list buffer, `p` / `Np` / `n,mp` / `,p` print, `a` append (terminated by `.`), `Nd` / `n,md` delete, `s/old/new/` literal substitute, `w` write, `r` read, `q` / `Q` quit, `=` line-number, address forms `N` / `.` / `$` / `+N` / `-N`
- `[t9]` selftest in sh that drives ed via TTY injection (session 18) and verifies persistence
- `os.img` padded out to `(200 + 1024) * 512 = 624 KiB` so `SYS_FS_WRITE` writes past the initial mkfs payload land in the file
- `user.ld` no longer discards `.bss` — folds it into `.data` so zero-initialized statics get real bytes in the .bin

Out:
- Free-sector reclamation — every `fs_write_all` allocates fresh sectors; the old range leaks until reboot. A real OS has a free-sector bitmap.
- Partial-byte writes (the API is "replace whole file with this buffer"). The closest POSIX is `pwrite(fd, ..., 0)` after `ftruncate`.
- File deletion (`unlink`).
- Directories — AdventFS is still flat.
- Visual / full-screen editor (we chose `ed` over `nano` for the demo because line input is easier to drive headlessly via injection)
- Regex addresses in ed (`/pat/`)
- Substitute flags (`s/x/y/g`, `s/x/y/p`, `s/x/y/2`)
- Move (`m`) and copy (`t`) commands
- Mark addresses (`'a`)
- Undo
- Encoding — ed treats bytes as bytes; UTF-8 will look ok if you don't move within multibyte characters

## Architecture: AdventFS gains write

The on-disk layout is unchanged from session 8:

```
LBA 0       boot sector
LBA 1..N    kernel image
LBA 200     superblock (FS sector 0)
LBA 201+    file data, contiguous per file
```

The superblock is exactly one 512-byte sector:

```c
struct fs_super {
    char            magic[8];        /* "ADVENTFS" */
    uint32_t        file_count;
    struct fs_entry files[FS_MAX_FILES];   /* 16 entries × 24 bytes */
};
struct fs_entry {
    char     name[16];
    uint32_t start_sector;           /* relative to LBA 200 */
    uint32_t size;                   /* bytes */
};
```

What's new is **the bookkeeping**: at mount, walk the file table to find the highest-allocated sector + 1; that's `g_high_water`. Anything past it (up to a hard cap) is free space.

```c
g_high_water = 1;       /* sector 0 is the superblock */
for (uint32_t i = 0; i < g_super.file_count; i++) {
    struct fs_entry *e = &g_super.files[i];
    uint32_t end = e->start_sector + (e->size + 511) / 512;
    if (end > g_high_water) g_high_water = end;
}
```

The cap (`FS_DISK_TOTAL_SECTORS = 1024`) is a hardcoded budget for how much disk space to consider available. Anything past it: out-of-disk error. The build script pads `os.img` to that exact size so writes don't fall off the end.

## fs_write_all — the new entry point

```c
int fs_write_all(const char *name, const void *data, uint32_t size) {
    int idx = find_or_create_entry(name);   /* reuse slot or allocate new */
    if (idx < 0) return -1;

    uint32_t needed = (size + 511) / 512;
    if (needed == 0) needed = 1;            /* always ≥ 1 sector */

    if (g_high_water + needed > FS_DISK_TOTAL_SECTORS) return -1;

    uint32_t new_start = g_high_water;
    g_high_water += needed;                 /* old range leaks; we don't care */

    /* Write the data. Last sector is zero-padded. */
    const uint8_t *src = data;
    uint8_t buf[512];
    for (uint32_t s = 0; s < needed; s++) {
        uint32_t off  = s * 512;
        uint32_t take = (off + 512 <= size) ? 512 : (size - off);
        for (int i = 0; i < 512; i++) buf[i] = 0;
        if (take > 0) memcpy(buf, src + off, take);
        if (ata_write_sector(FS_DISK_OFFSET_SECTORS + new_start + s, buf) != 0)
            return -1;
    }

    /* Update the superblock entry, then push it out to disk. */
    g_super.files[idx].start_sector = new_start;
    g_super.files[idx].size         = size;
    return fs_write_super();
}
```

The "always allocate fresh sectors" choice is a deliberate simplification:

- **In-place rewrite** would require knowing the *allocated* size of each file (separate from its byte size, since a 17-byte file occupies 1 sector but could be rewritten to 250 bytes without touching anything else). The current `struct fs_entry` doesn't track allocation size; adding it would change the on-disk format.
- **Append-style** "every save grows" matches a log-structured FS and is easy to reason about. It leaks sectors — write a 17-byte file twice and you've used 2 sectors, with the first one unreachable but still allocated. For our cap of 1024 sectors that's plenty of headroom. A real FS would maintain a free-sector bitmap and reuse leaks.
- **Superblock persists every save** — `ata_write_sector(FS_DISK_OFFSET_SECTORS, packed_super)`. Without this the in-memory file table would diverge from disk and the next boot would mis-read.

`find_or_create_entry` looks up by name — same as `fs_open` — and returns either the existing slot's index or a fresh slot from the table's tail (incrementing `file_count`). Returns -1 if the table is full (16-entry hard cap).

## SYS_FS_WRITE

The syscall is intentionally one-shot: replace the entire file with this buffer. No fd, no offset, no flags. A POSIX-correct system would have `creat(name)` returning a writable fd + `write(fd, buf, n)` calls + `close(fd)` to commit; we collapse all of that into one call because the use cases (programmatic writes, ed's `w` command) all already have the full file content in memory.

```c
case SYS_FS_WRITE: {
    const char *uname = (const char *)a;
    const void *udata = (const void *)b;
    uint32_t    n     = c;
    char name[FS_NAME_MAX + 1];
    int  i;
    for (i = 0; i < FS_NAME_MAX && uname[i]; i++) name[i] = uname[i];
    name[i] = 0;
    ret = fs_write_all(name, udata, n);
}
```

The name gets snapshotted into a kernel buffer (the user pointer becomes invalid if we ever fault during the multi-sector write). The data buffer stays user-side: we read it sector-by-sector via `fs_write_all`, all on the user task's CR3 — same trick the rest of the kernel uses for user buffers.

## ed — design

The buffer is a doubly-linked list of `struct line { prev, next, len, text }`. `text` is `malloc`'d via libuser malloc (session 17), `len` excludes the trailing `\n` (the editor adds it on write, strips it on read).

Three globals track state:
- `g_head` / `g_tail` — list endpoints
- `g_cur` — current line (the dot in ed)
- `g_dirty` — unsaved-changes flag
- `g_filename[64]` — default filename for `w` (no arg)

The command loop reads lines via `sys_read_line`, parses an optional address-or-range followed by a one-letter command, dispatches:

```
[ "1d"     ]  →  parse_addr → 1; cmd 'd'        → delete_range(1, 1)
[ "1,3p"   ]  →  parse_addr → 1; ',' parse_addr → 3; cmd 'p'  → print_range(1,3)
[ "a"      ]  →  cmd 'a'  → cmd_append: read lines until "."
[ "s/x/y/" ]  →  cmd 's'  → cmd_substitute: literal first-match on current line
[ "w foo"  ]  →  cmd 'w'  → save_file("foo")
[ "q"      ]  →  cmd 'q'  → exit unless g_dirty
```

`parse_addr` understands `N`, `.`, `$`, `+N`, `-N`. No regex addresses.

`cmd_append` reads lines until one of them is exactly `.` — the classic ed "input mode" terminator. Each non-`.` line becomes a fresh `struct line` inserted after `g_cur`, and `g_cur` advances to the new line.

`cmd_substitute` is literal-only — no regex, no flags. The delimiter is whatever character follows `s` (canonically `/`, but `s|x|y|` also works). First-match on `g_cur->text` only.

`save_file` joins all lines with `\n`, calls `sys_fs_write(name, joined, total)`, clears `g_dirty`. Mid-buffer corruption from a partial write isn't possible because `fs_write_all` is atomic at the superblock-flush boundary — either the write completes and the superblock is updated, or the superblock stays pointing at the old range.

`load_file` reads the entire file via `sys_open` + `sys_read` chunks, splits on `\n`, builds the linked list. Doubles a growing in-memory buffer until everything's read, then walks it once.

The whole thing is ~400 lines of C plus libuser. Compiles to ~8 KiB of ring-3 binary.

## Driving ed headlessly: TTY inject

The editor reads commands from stdin via canonical `sys_read_line` (which goes through `kshell_read_line` → `keyboard_wait_char` → `kbd_buf`). The headless boot has no real keyboard. Session 18's `SYS_TTY_INJECT` solves this: push bytes into the keyboard input ring as if typed.

Selftest sequence:

```c
const char *script =
    "1d\n"
    "a\n"
    "delta\n"
    ".\n"
    "w notes.txt\n"
    "q\n";
tty_inject(script, (int)strlen(script));

int pid = sys_fork();
if (pid == 0) {
    const char *argv[] = { "ed.elf", "notes.txt", 0 };
    sys_exec("ed.elf", argv);
    sys_exit(127);
}
sys_wait(&code);
```

Six commands worth ~30 bytes — well under `tty_inject`'s 256-byte cap. The bytes sit in `kbd_buf` until ed runs and starts pulling them out via `sys_read_line`.

There's no race: the injection completes before fork. The child (ed) sees the bytes in arrival order. `kshell_read_line` echoes each character to the console, so the script appears on the serial log as it gets "typed" — useful for verifying the test ran the commands in the expected order.

The same pattern would work for *any* program that reads stdin canonically: stuff a script into `tty_inject`, fork+exec the program, observe the result.

## The .bss fix that fork+ed forced

First boot of `[t9]` faulted at `0x400008af` in ed.elf — `movb $0x0, 0x0`, which is the compiled form of `g_filename[0] = 0;` (a write to address 0).

Disassembling around it confirmed `g_filename` had been linked at address 0. The reason was the same one that bit sessions 15 and 17: `user.ld` discards `.bss`, so any zero-initialized file-scope static gets resolved to address 0, and the first write to it page-faults.

Sessions 15 and 17 worked around it by giving offending statics non-zero initializers (forcing them into `.data`). ed has *several* statics that need to stay zero (`g_head`, `g_tail`, `g_cur` as NULL pointers; `g_dirty` as 0; `g_filename` as an empty string). Per-static workarounds were untenable.

The fix is in `user.ld`: fold `.bss` into the `.data` output section. The linker promotes input `.bss` (NOBITS) to PROGBITS when its output section is PROGBITS, which means the bytes get a real address AND get zero-filled in the .bin output:

```ld
.data : SUBALIGN(4) {
    *(.data*)
    *(.gnu.linkonce.d.*)

    /* Fold .bss INTO .data so zero-initialized statics get real
     * bytes in the .bin output (the linker promotes the input
     * NOBITS sections to PROGBITS as zeros when their output
     * section is PROGBITS). */
    . = ALIGN(4);
    *(.bss*)
    *(COMMON)
    *(.gnu.linkonce.b.*)
}
```

The .bin grows by the .bss size — in ed's case, ~80 bytes for the four globals. The kernel ELF loader copies those zeros into the user page like any other byte. The static now lives at a real address and writes to it work.

This retroactively makes the workarounds in sh.c (`static char buf[64] = "."` for resolve_program) and libuser.c (`static uint32_t g_brk = HEAP_START_VA`) unnecessary, but they're harmless and we leave them.

## Padding os.img — the OTHER fix that bit

Second discovery during testing: even after writing the file successfully, the read-back returned -1.

Cause: `os.img` was 142 KiB after build, ending at sector 279. `SYS_FS_WRITE` writes to LBA 200 + g_high_water = LBA 279 — exactly past the file's end. QEMU's raw-format drive treats the file size as the disk size; writes past that are silently dropped, reads from there return errors. So `ata_write_sector` "succeeded" (no error path triggered) but the bytes never made it to disk; `ata_read_sector` then errored.

Fix in `build.sh`:

```bash
# Pad up to (fs_lba + 1024) sectors so SYS_FS_WRITE can grow files
# past the initial mkfs payload.
final_size=$(( (fs_lba + 1024) * 512 ))
sz=$(stat -c%s os.img)
if [ "$sz" -lt "$final_size" ]; then
    truncate -s "$final_size" os.img
fi
```

`truncate -s N` extends with zeros (no-op if already ≥ N). The image grows from 142 KiB to 624 KiB. QEMU now sees a disk big enough for 1024 sectors of FS area.

A more bulletproof version would dynamically size based on `FS_DISK_TOTAL_SECTORS` from the kernel header, but for now the constants live in two places (`kernel/fs.c` and `build.sh`) — pitfall #6 below.

## Files added / modified

| File | Change |
|---|---|
| `kernel/fs.{h,c}` | `g_high_water`, `fs_write_all`, `fs_write_super`, `find_or_create_entry`; `FS_DISK_TOTAL_SECTORS = 1024` cap |
| `kernel/syscall.{h,c}` | `SYS_FS_WRITE = 31` |
| `user/ed.c` | New. ed-style line editor (~400 lines) |
| `user/libuser.{h,c}` | `sys_fs_write` wrapper |
| `user/user.ld` | Fold `.bss` into `.data` so zero-init statics work |
| `user/sh.c` | `[t9]` selftest |
| `build.sh` | `USER_PROGS+=(ed)`; pad os.img to `(200 + 1024) * 512` |
| `mkfs.py` | `('ed.elf', 'user/_obj/ed.bin')` in `USER_PROGRAMS` |

## Design decisions

**`ed` not `nano`.** Line-oriented input from stdin is trivially scriptable via TTY injection or pipes; full-screen editors need cursor positioning, redraw logic, and ideally a serial-as-terminal subprotocol that AdventOS doesn't have yet. `ed` proves the editor mechanics (line buffer, command parser, save/load) without the screen-management distraction. Once we have ANSI escape support in the serial output and a real raw-mode-driven main loop, `nano` would be a 2-3 session followup.

**`SYS_FS_WRITE` is "replace the whole file"**, not byte-streaming. Most callers (programmatic writes, ed's `w`) have the full content in memory; making them stream through write(fd, ...) would mean either (a) buffering everything in the kernel until close, or (b) supporting in-place modification of a sector-aligned region of the disk. Neither matches the use case. POSIX `creat()` + `write()` could be added on top of this primitive later.

**Allocate-on-write, leak-on-rewrite.** Documented above. With a free-sector bitmap we'd reuse leaks, but each save would still need to figure out whether the new size fits in the old run. The current design has zero such bookkeeping — it just bumps `g_high_water` and forgets.

**`g_high_water` recomputed at mount, not stored on disk.** Walking 16 file entries to find the max start+size is fast; keeping a separate field on disk is just one more thing to keep coherent. The cost is that any "leaked" sector (from a previous save+rewrite) stays leaked until reboot AND is implicitly reclaimed at reboot — because nothing on disk says "sector X is used by no one." The next boot's `g_high_water` only sees sectors referenced by current files.

**`os.img` pads to 624 KiB regardless of how much was actually written.** Wastes space if you never write anything; harmless. Without it, post-mkfs writes would silently fall off the end of the file.

**`.bss` fold-into-.data is the right cleanup.** Per-static workarounds (sessions 15, 17) were a stopgap. Now any user program that uses zero-init file-scope statics works. Cost: every binary's .bin grows by its .bss size. ed.bin grew from 7.6 KiB to 8.0 KiB.

**ed's substitute is literal-only.** Regex would be its own session — adding even a tiny BRE engine is a few hundred lines. Literal `s/x/y/` covers the common case of "fix this one typo" and is what the demo needs.

**ed's `a` reads until `.` on its own line.** Classic ed semantic. The terminator is exactly the byte sequence `.\n`. A line that's `.something` doesn't end input.

**The selftest's `tty_inject` is the *parent* shell injecting on behalf of the ed child.** Could have been done from inside ed itself (a `--script` flag), but inject-then-fork mirrors how a test harness would do it externally — easier to reuse the pattern for other input-driven programs.

## Pitfalls

1. **The .bss / address-zero bug is now fixed at the linker level**, but any program that linked against the OLD `user.ld` would still see the issue — rebuild everything when adopting.
2. **`os.img` write-past-file-end was silent.** `ata_write_sector` returned 0, the kernel had no way to know the bytes never made it. The fix (padding) is fragile if `FS_DISK_TOTAL_SECTORS` changes — the kernel constant and the build script's pad target need to stay in sync (they're hardcoded in both today).
3. **`fs_write_all` leaks sectors on every rewrite.** Save the same file 60 times and you've consumed 60+ sectors regardless of file size. With our 1024-sector cap and a typical demo, that's fine. With a heavily-edited file in long-running use, you'd hit "out of disk" sooner than expected.
4. **The 16-file cap on `FS_MAX_FILES` is hard.** Trying to create a 17th file returns -1. mkfs already pre-fills 9 entries on a fresh image, leaving 7 slots for runtime creation.
5. **No file deletion.** Once written, a file is in the table forever (within a boot). `unlink` would just clear the entry's name and shrink `file_count`, but we don't have it.
6. **Constants split between `fs.c` and `build.sh`.** `FS_DISK_TOTAL_SECTORS = 1024` in fs.c, `1024` literal in build.sh's truncate calculation. They must match.
7. **`fs_write_all` doesn't handle `size == 0` specially.** The "always allocate ≥ 1 sector" rule means a zero-byte file uses one sector. Acceptable; documented.
8. **The superblock write is non-atomic.** If we crash between writing data sectors and writing the superblock, the file table doesn't know about the new content (which is fine — old version stays accessible) but we've still leaked the new sectors. A real crash-safe FS would use journaling.
9. **ed's `s/x/y/` overwrites `g_cur->text` via `free` + `malloc`.** If `malloc` fails in the middle, the line is in an inconsistent state — we print "?out of memory" and leave it that way. A defensive impl would build the new buffer fully before freeing the old.
10. **Injected commands echo to the console** (because canonical `kshell_read_line` always echoes). That's actually useful for the test trace, but it means real programs running with injected input will look noisier than if a real user typed.
11. **`ed` doesn't update `g_filename` from `w name`.** If you start ed with no arg and save with `w foo`, the next bare `w` says "?no filename." Easy fix; not done.
12. **Reboot drops everything in `tmpfs` but persists `disk fs`.** Different storage classes for the two; the user has to know which one a given file lives in. An `ls` builtin that distinguishes them would help.

## What might come next

`unlink`, file rename, then a proper free-sector bitmap so rewrites don't leak. After that: fork in-place edits (open + lseek + pwrite), which would let `ed` save without rewriting the whole file. Then a regex engine for ed's `s/PATTERN/replacement/` — at that point ed is meaningfully a real editor. Then the visual editor: ANSI escape parser in tty for cursor movement, raw mode driving a screen-redraw loop in the user program. nano would need all of that.
