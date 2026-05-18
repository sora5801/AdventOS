# Session 123 — Path C phase 17: --clean mode + wmpair in launcher

**Goal.** Two small quality-of-life polishes:
1. End users who want a tidy desktop (no daemon-internal demo
   chrome) can pass `--clean` to wmd to suppress the four session-
   111 stub windows.
2. The Start-menu launcher learns about `wmpair`, the new
   multi-window client.

Status: **done.** New smoke test (`smoke_wmclean.py`, 5/5 pass)
plus every prior smoke test (110–122) still green.

```
=== pixel checks ===
  [OK] CLEAN: ex-Clock area now desktop bg = (10, 24, 40)
  [OK] CLEAN: ex-Color-bars area now desktop bg = (10, 24, 40)
  [OK] POPUP: top item bg @ (100,634) = (32, 40, 48)
  [OK] POPUP: wmpair text in 5th item band (129)
  [OK] CLEAN: Start button green (32/48)
```

What it verifies:
- With `wmd 30 --clean`, the areas where the Clock and Color-bars
  demo windows USED to live now show the desktop background
  `0x0A1828` — proving the demos didn't paint.
- The launcher popup repositioned upward to fit 5 items (popup
  top y dropped from 648 to 626).
- "wmpair" text renders in the popup's fifth item band, proving
  the launcher catalog grew.
- The Start button is still painted (regression sanity).

---

## argv parsing in wmd

```c
int show_demos = 1;       /* default: backward-compatible ON */
for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] == '-') {
        if (a[2] == 'c') show_demos = 0;     /* --clean */
    } else {
        int v = my_atoi_str(a);
        if (v > 0) seconds = v;
    }
}
```

Demos default ON because every smoke test in sessions 110–122
expects them; flipping the default would break ~6 tests
simultaneously.  `--clean` is the opt-out for human use.

The flag is the only argv we recognise besides SECONDS, so the
parser is deliberately tiny.  Adding more flags (e.g. `--wallpaper
PATH`, `--theme NAME`) follows the same pattern.

---

## Launcher catalog grows by one

```c
static const struct launch_entry g_launch_items[] = {
    { "wmhello", "/wmhello.elf" },
    { "wmtype",  "/wmtype.elf"  },
    { "wmclock", "/wmclock.elf" },
    { "wmpaint", "/wmpaint.elf" },
    { "wmpair",  "/wmpair.elf"  },   /* session 122 */
};
```

`N_LAUNCH_ITEMS` macro auto-resizes from the array length, so the
popup geometry just shifts upward by one item height (22 px) to
accommodate.  No paint-position constants needed changing.

---

## Files touched

- `user/wmd.c` — argv parser with `--clean`, `wmpair` added to
  launch catalog, `if (show_demos)` guard on init_demo_windows
- `smoke_wmclean.py` — new harness, 5 pixel checks across two
  screendumps
- `docs/110-pathC-clean-mode.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 18452 → 18548 (+96 bytes for argv loop + catalog entry).

---

## Path C status after session 123

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx
- ✅ 109 — PS/2 mouse
- ✅ 110 — double-buffering
- ✅ 111 — wmd compositor
- ✅ 112 — shared-surface client protocol
- ✅ 113 — mouse-event routing
- ✅ 114 — keyboard-event routing
- ✅ 115 — wmclock + wmpaint apps
- ✅ 116 — close buttons + WM_EV_CLOSE
- ✅ 117 — hover vs focus separation
- ✅ 118 — taskbar with click-to-focus
- ✅ 119 — Start button + launcher popup
- ✅ 120 — scalable fonts (gfx_text_n)
- ✅ 121 — taskbar clock
- ✅ 122 — multi-window per client (wmpair sample)
- ✅ 123 — --clean mode + launcher catalog update
- ⏳ 124+ — user-prog `--gc-sections`, right-click context menus,
          window resize, desktop wallpaper image
