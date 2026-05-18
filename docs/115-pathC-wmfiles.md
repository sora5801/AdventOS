# Session 128 — Path C phase 21: wmfiles file manager

**Goal.** A real-purpose WM client that does something useful
beyond demo'ing the WM protocol.  `wmfiles` is a tiny file
manager: it lists the entries in the current working directory
and lets the user navigate with arrow keys, descend into
sub-directories with Enter, and back up with Backspace.

Status: **done.** Smoke test (`smoke_wmfiles.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] A: header bar visible
  [OK] A: row 0 selected (highlight bg)
  [OK] A: text in row 0 (55 white)
  [OK] B: row 0 no longer selected
  [OK] B: some row in list is selected (1)
```

What the test verifies:
- screendump A (just after launch): wmfiles header bar is
  visible; row 0 carries the `0x405880` selection highlight;
  the first entry's text is rendered in white.
- click into wmfiles content area to give it keyboard focus
- send a QMP `down` arrow → kernel generates ESC `[` B
- screendump B: row 0 is no longer highlighted; exactly one
  other row IS highlighted (we don't pin which — the focus-click
  also selected a different row mid-list, and the arrow advanced
  by one).

This is the first WM client that:
- consumes the *full* keyboard event stream (multi-byte ANSI
  escape sequences for the arrow keys, not just printable ASCII),
- changes its rendered contents in response to filesystem state
  (re-running `sys_readdir` after `sys_chdir`),
- demonstrates a real navigation pattern (move-and-Enter) inside
  a WM-managed window.

---

## ANSI arrow-key parsing

The kernel's keyboard layer (kernel/keyboard.c) translates the
PS/2 scancodes for the four arrow keys into a 3-byte CSI sequence
exactly like a Unix terminal: `ESC` (0x1B), `[` (0x5B), `A` / `B`
/ `C` / `D`.  Three separate bytes hit the kbd ring buffer; wmd
forwards each as its own `WM_EV_KEY` event.

wmfiles keeps a tiny state machine across consecutive events:

```c
if (esc_state == 1) {
    if (k == '[') { esc_state = 2; break; }
    esc_state = 0;
    closed = 1;     /* lone ESC = quit */
    break;
}
if (esc_state == 2) {
    esc_state = 0;
    if      (k == 'A') selected = (selected - 1 + n) % n;   /* up */
    else if (k == 'B') selected = (selected + 1) % n;       /* down */
    break;
}
if (k == 27) esc_state = 1;     /* start of ESC sequence */
else if (k == '\n' || k == '\r') /* descend into selected entry */;
else if (k == 0x08)               /* backspace → cd .. */;
```

Lone ESC quits.  C and D (left/right arrows) are accepted but
unused — file-manager navigation uses up/down only.

---

## Filesystem syscalls

Three of them, all session-25-era:

```c
sys_getcwd(cwd, sizeof(cwd));            /* "/" or "/etc" or ... */
sys_readdir(cwd, &iter, name_buf);       /* iterate entries */
sys_chdir(entries[selected]);            /* descend */
sys_chdir("..");                         /* go up */
```

`sys_readdir` returns 16-byte name buffers; wmfiles copies each
into its own wider `char name[32]` slot for safety.  The reload
walks until `sys_readdir` returns -1 (or we hit the static cap
`MAX_ENTRIES = 64`).

**Implementation detail:** the kernel's readdir resolver doesn't
handle `"."` cleanly, so wmfiles passes the absolute cwd returned
by `sys_getcwd` instead.  Fast, deterministic, no relative-path
quirks.

---

## Rendering

The window is 400×300.  Layout:

```
┌────────────────────────────────────────────────┐
│ /etc                                           │  ← 20 px header
├────────────────────────────────────────────────┤
│ ▓ passwd                                       │  ← 12 px per row
│   ssl                                          │
│   inittab                                      │
│   ...                                          │
│                                                │
├────────────────────────────────────────────────┤
│ arrows + Enter to navigate; bksp = up; q = quit│  ← 14 px footer
└────────────────────────────────────────────────┘
```

Selection highlight is a 1-row fill in `0x405880` (mid-blue);
selected row text in white, others in light grey `0xC0C0C0`.
Header bg flips between focused blue and unfocused grey.

The list scrolls trivially when `selected >= row0 + max_rows` —
no per-pixel scrollbar.  Long directories just clip at the
bottom; pressing the arrow keys to move below `row0 + max_rows`
shifts `row0` so the selection stays visible.

---

## Files touched

- `user/wmfiles.c` — new, ~160 lines
- `build.sh` — `wmfiles` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmfiles.elf` + man page packed
- `fs/man/wmfiles` — new
- `user/wmd.c` — `wmfiles` added to the Start-menu catalog
- `smoke_wmfiles.py` — new harness, 5 pixel checks across two
  screendumps
- `docs/115-pathC-wmfiles.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 19604 → 19636 (+32 bytes for the launcher catalog entry).
wmfiles.bin: new, 13380 bytes.

---

## Path C status after session 128

- ✅ 107..127 — see prior session docs
- ✅ 128 — wmfiles file manager
- ⏳ 129+ — window resize via drag handle, LIBC_TABLE-aware
          gc-sections (session 125's remaining blocker), maybe a
          tiny image viewer

What we now have: a desktop with a launcher that, in two clicks,
opens a window where the user can browse the filesystem with
arrow keys.  That's the bar real desktop OSes cross when they
ship their first useful app, and we've hit it.
