# Session 130 — Path C phase 23: wmps process viewer

**Goal.** A WM client equivalent of the CLI `ps`: list every
process with its pid + state + name, in a window, refreshing on
its own.

Status: **done.** Smoke test (`smoke_wmps.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] header band @ y=220 (350/350)
  [OK] column header text (331)
  [OK] row 0 selection highlight
  [OK] row 0 text white pixels (287)
  [OK] footer rows=N pixels (135)
```

The 287 white pixels in row 0 — pid digits, state ("R"/"S"/etc.),
and process name ("wmd" / "wmps" / "init" / …) — render
identically to the CLI version because the underlying data source
is the same `/proc/<pid>/status` text.

---

## Same /proc API as `ps`

`user/ps.c` introduced the pattern in session ~64.  `wmps` reuses
it verbatim:

```c
int iter = 0;
char name[17];
while (n < MAX_ROWS) {
    int idx = sys_readdir("/proc", &iter, name);
    if (idx < 0) break;
    int pid = my_atoi_pos(name);
    if (pid <= 0) continue;
    char sbuf[256];
    if (read_status(pid, sbuf, sizeof(sbuf)) <= 0) continue;
    rows[n].pid = pid;
    extract(sbuf, "Name:",  rows[n].name,  sizeof(rows[n].name));
    extract(sbuf, "State:", rows[n].state, sizeof(rows[n].state));
    n++;
}
```

The kernel's procfs publishes a synthetic file system rooted at
`/proc`; every running task gets a directory whose name is its
decimal pid.  The directory contains `status` (a Linux-ish
key/value blob).  We don't need any new syscall — just
`sys_readdir`, `sys_open`, `sys_read`, `sys_close`.

`MAX_ROWS = 32` caps the table at 32 processes per snapshot.  The
existing AdventOS task table is 32 slots, so we'd hit kernel-side
caps before client-side ones.

---

## Rendering

Three-column table, monospace 8x8 font:

```
PID   STATE   NAME
1     S       reaper
3     R       init
9     R       wmd
12    S       wmps
...
```

- Light-grey column headers above a 1-px separator line.
- Each row is `LINE_H = 12` tall.
- Selected row gets a `0x405880` highlight band; selected text
  flips from `0xC0C0C0` to white.
- Header bar at top reads `wmps - processes (q=quit r=refresh)`.
- Footer shows `rows=N`.

Auto-refresh every 1 s.  Manual refresh on 'r'.  The list scrolls
when selection moves past `row0 + max_rows` — same scroll model
as `wmfiles`.

---

## Why not a "kill" command?

Adding 'k' to send `SIGKILL` to the selected pid would be ~3 lines
(`sys_kill(rows[selected].pid, SIGKILL)`).  Skipped because:

- We have no permission-checking gating clients from killing
  privileged processes; in a multi-user setup we'd want some.
- `sys_kill` works fine from the CLI today; a WM client adding
  the same power doesn't change the security story but does mean
  a misclick could nuke the kernel.

Future session can add it behind a confirmation overlay.

---

## Files touched

- `user/wmps.c` — new, ~210 lines
- `build.sh` — `wmps` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmps.elf` + man page packed
- `fs/man/wmps` — new
- `user/wmd.c` — `wmps` added to the Start-menu catalog (8 items
  now)
- `smoke_wmps.py` — new harness, 5 pixel checks
- `docs/117-pathC-wmps.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 19668 → 19668 (catalog entry size absorbed by linker
alignment slack).
wmps.bin: new, 14296 bytes.

---

## Path C status after session 130

- ✅ 107..129 — see prior session docs
- ✅ 130 — wmps process viewer
- ⏳ 131+ — kill-pid in wmps, window resize, LIBC_TABLE-aware
          gc-sections, image viewer

Eight WM client apps now live in the catalog:

| app        | purpose |
|------------|---------|
| wmhello    | interaction demo |
| wmtype     | text input |
| wmclock    | digital clock |
| wmpaint    | drag-to-draw |
| wmpair     | multi-window per client |
| wmfiles    | file manager |
| sysinfo    | live kernel diagnostics |
| wmps       | process list |

That's enough variety that a user can do real work in the WM —
file browsing, time check, drawing, watching the system — without
the shell as a fallback.
