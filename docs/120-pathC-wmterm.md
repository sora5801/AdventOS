# Session 134 — Path C phase 27: wmterm terminal emulator

**Goal.** Run the shell inside a WM window so the desktop becomes
self-hosting for shell work.  No more dropping back to the
serial / VGA console to type a command.

Status: **done.** Smoke test (`smoke_wmterm.py`, 4/4 pass):

```
   title bar @ (200, 226) = (64, 64, 64)
   green text pixels in grid: 1403
   white marks in grid: 565

=== pixel checks ===
  [OK] wmterm title bar painted
  [OK] shell output rendered as text (1403)
  [OK] wmterm bg dark @ (500, 400) = (8, 8, 8)
  [OK] wmd status bar (887/924)
```

1403 light-green text pixels in the grid prove `sh.elf` started,
printed its banner + prompt through the PTY master, wmterm read
them via `sys_read`, and rendered them in the 60×24 grid.

The WM is now self-hosting: launch wmterm via the Start menu and
type freely.

---

## How the pieces fit

```
         wmterm                 kernel               sh.elf
         ------                 ------               ------
    sys_openpty(pty[])  ----->  [pty 0/1 alloc]
    pty[0] master <-                                 <- pty[1] slave
    sys_fd_nb(master, 1)
    sys_fork()
                       \-----.            .-----/
                              \          /
                               child
                                |
                                +--- sys_dup2(slave, 0)
                                +--- sys_dup2(slave, 1)
                                +--- sys_dup2(slave, 2)
                                +--- sys_close(slave)
                                +--- sys_exec("/sh.elf")
                                                          \-> shell runs
   loop {                                                     |
     WM_EV_KEY → sys_write(master, &c, 1) ---->   slave stdin |
     n = sys_read(master, buf, N)         <----   slave stdout / stderr
     for b in buf:  vt_feed(b)                    [shell writes]
     paint grid + present
   }
```

Two new things relative to existing PTY users (sshd):

1. **Non-blocking master.**  sshd does its read in a separate
   task that blocks; wmterm's main loop polls everything (mouse,
   keyboard, master fd) so it can't block on any one source.
   `sys_fd_nb(master, 1)` flips the per-fd `FD_FL_NONBLOCK` bit;
   `sys_read` then returns 0 / -1 when the master has nothing.
2. **Surface as the "terminal display".**  Output is rendered
   into a 2-D `char g_grid[ROWS][COLS]` and blitted to the libwm
   surface each frame, not piped to a serial port.

---

## VT state machine

`vt_feed(b)` is the per-byte input handler:

```c
if (g_vt_state == 1) {
    if (b == '[') { g_vt_state = 2; return; }
    g_vt_state = 0; return;          /* lone ESC — ignore */
}
if (g_vt_state == 2) {
    /* ESC [ ... <final> — final is in 0x40..0x7E.  Strip. */
    if (b >= 0x40 && b <= 0x7E) g_vt_state = 0;
    return;
}
switch (b) {
    case 27:   g_vt_state = 1; return;          /* ESC */
    case '\n': grid_newline();    return;
    case '\r': g_cur_col = 0;     return;
    case '\b': g_cur_col--; g_grid[r][c] = ' '; return;
    case '\t': /* expand to next 8-col tab stop */ return;
    default:
        if (b >= 0x20 && b <= 0x7E) grid_putc_printable(b);
        return;
}
```

CSI sequences (the shell's colour codes, `[2J` clear, cursor
positioning) are *stripped*, not interpreted.  The shell still
renders as readable text; just monochrome.

The grid is a fixed 60×24 of plain ASCII.  Putting a character
at the cursor advances col; col=COLS wraps to next row;
row=ROWS scrolls the grid up by one.

---

## Lifecycle

- **Open.**  wmterm opens a libwm window, then openpty + fork +
  exec sh.elf.  Master fd stays with wmterm; slave is closed
  after the dup2 in the child.  Both ends keep the PTY open
  while wmterm runs.
- **Close.**  When the user clicks the close-X (`WM_EV_CLOSE`),
  wmterm's main loop sets `closed = 1` and falls through to
  `sys_close(master)` + `wm_close(&win)`.  Closing the master
  fd should give the slave a SIGPIPE on next write — but the
  current kernel TTY layer doesn't wire that up, so the orphan
  sh.elf just sees blocking reads forever.  Cleanup TODO:
  forward WM_EV_CLOSE to a kill(child, SIGTERM) before exit.
- **EOF.**  If the shell exits cleanly, the master sees -1 reads
  going forward; wmterm just keeps running with an empty grid.

---

## Window layout

```
+-------------------------------------------------+
|  wmterm - sh.elf                  [_] [[] [X]  |  ← title bar (HDR_H = 20)
+-------------------------------------------------+
| AdventOS userspace shell, pid=10                |
| Type 'help' for builtins. | > & are honored.    |
|                                                 |
| advent$ _                                       |  ← blinking caret
|                                                 |
| ...                                             |
+-------------------------------------------------+
   540 px wide / 240 px tall
   60 cols × 24 rows of 8x8 glyphs (+ 1-px leading)
```

60×24 is the standard "small VT" size that fits in the available
window real-estate without scaling.  Bigger could be done via
larger font (gfx_text_n from session 120) or a wider window;
session 134 keeps the smallest viable layout.

---

## Things deliberately NOT done

- **Colour.**  CSI colour codes are stripped.  Single light-green
  on dark background.  A colour table indexed by the current
  attrs would add ~200 lines for marginal value at this stage.
- **Resize.**  The WM grip resizes the outer frame, but the grid
  stays 60×24.  Window content area smaller than the grid clips;
  larger shows empty surface.  Honouring SIGWINCH-equivalent +
  reflowing the grid is future work.
- **Scrollback.**  Lost rows on scroll are gone forever — no
  history buffer.
- **Mouse selection / copy-paste.**  The shell sees no mouse;
  cursor inside the wmterm window does nothing.
- **Signal forwarding.**  Closing wmterm doesn't kill its sh.elf
  child.  See "Close" above.

---

## Files touched

- `user/wmterm.c` — new, ~165 lines: openpty + fork + exec sh.elf;
  non-blocking master; VT state machine; 60×24 grid + caret
- `user/wmd.c` — added `{ "wmterm", "/wmterm.elf" }` to the
  launcher catalog (now 9 items)
- `build.sh` — `wmterm` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmterm.elf` + man page in the image
- `fs/man/wmterm` — new
- `smoke_wmterm.py` — new harness, 4 pixel checks
- `docs/120-pathC-wmterm.md` — this file

kernel.bin: 114864 (unchanged — pure userspace).
wmterm.bin: new 6220 bytes.
wmd.bin: 16456 → 16520 (+64 B for the launcher catalog entry).

---

## Path C status after session 134

- ✅ 107..133 — see prior docs
- ✅ 134 — wmterm: WM client wrapping sh.elf via PTY

The WM stack is now genuinely useful for the same workflows the
serial console handled.  A user can boot, click Start → wmterm,
and run shell commands — `ls`, `cat`, `ps`, even nested `sh` —
without leaving the framebuffer.

Future polish on wmterm (not session 134): colour, scrollback,
SIGWINCH on resize, signal forwarding on close.
