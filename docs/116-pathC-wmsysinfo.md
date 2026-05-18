# Session 129 — Path C phase 22: wmsysinfo dashboard

**Goal.** Surface kernel diagnostics in a WM client window —
something a user can keep open in the corner of their desktop to
watch the system in motion.

Status: **done.** Smoke test (`smoke_wmsysinfo.py`, 5/5 pass):

```
=== pixel checks ===
  [OK] header band @ y=220 (330/330)
  [OK] body bg @ (481,368) = (32, 32, 32)
  [OK] label text grey pixels (280)
  [OK] value text white pixels (74)
  [OK] footer hint pixels (744)
```

The dashboard refreshes every 500 ms.  It shows:

```
┌────────────────────────────────────────────────────────────┐
│ wmsysinfo - live kernel diagnostics                        │
├────────────────────────────────────────────────────────────┤
│   pid:                  9                                  │
│   current cpu:          0                                  │
│   sys_time (epoch s):   1747494200                         │
│   wmsysinfo uptime s:   3                                  │
│   heap brk:             0x40200000                         │
│   framebuffer:          1024x768 @ 24bpp                   │
│   cpu 0:                742                                │
│   cpu 1:                0                                  │
│   cpu 2:                0                                  │
│   cpu 3:                0                                  │
│                                                            │
│ press q or Esc to quit; refresh every 500ms                │
└────────────────────────────────────────────────────────────┘
```

Two-column layout: labels at x=12 in light grey, values at x=130
in white.  Per-CPU scheduler tick counters come from
`sys_smp_stats` (session 53); they're the same numbers `ps`
shows in its `--smp` mode, and they tick up monotonically as the
scheduler dispatches each CPU.

---

## Syscalls exercised

| call               | shows |
|--------------------|-------|
| `sys_getpid`       | wmsysinfo's pid |
| `sys_getcpu`       | which logical CPU served the last syscall |
| `sys_time`         | NTP-corrected wall-clock seconds |
| `sys_brk(0)`       | heap pointer (mostly stable at the default `0x40200000` for clients that don't malloc much) |
| `sys_fb_info`      | FB geometry — width, height, bpp |
| `sys_smp_stats`    | per-CPU scheduler-tick counters (8 slots; we display 4) |

`sys_fb_info` is the same syscall wmd uses to negotiate the FB at
startup.  Reading it from a client is fine — it returns geometry
without taking ownership, so wmsysinfo + wmd coexist.

---

## What this is good for

- "Is the system alive?"  Watching the CPU counters tick up
  confirms the scheduler is making progress.
- "Did I just leak memory?"  The heap_brk pointer should stay
  stable for typical WM clients (libc's malloc grows it lazily
  but doesn't shrink it on free).
- "What's the FB doing?"  Width/height/bpp confirm the negotiated
  mode survived boot.

Future expansions tracked but not done:
- mouse cursor position (sys_mouse_poll's `(x, y, buttons)`)
- PMM stats (used / total pages — needs a new syscall)
- network info (sys_dhcp_info exists; we could show local IP)

---

## Files touched

- `user/wmsysinfo.c` — new, ~140 lines
- `build.sh` — `wmsysinfo` joins `WMCLIENT_PROGS`
- `mkfs.py` — `wmsysinfo.elf` + man page packed
- `fs/man/wmsysinfo` — new
- `user/wmd.c` — `sysinfo` added to the Start-menu catalog (label
  intentionally short — 16-char `gfx_text` budget per popup item)
- `smoke_wmsysinfo.py` — new harness, 5 pixel checks
- `docs/116-pathC-wmsysinfo.md` — this file

kernel.bin: 114864 (unchanged).
wmd.bin: 19636 → 19668 (+32B for the catalog entry).
wmsysinfo.bin: new, 12620 bytes.

---

## Path C status after session 129

- ✅ 107..128 — see prior session docs
- ✅ 129 — wmsysinfo dashboard
- ⏳ 130+ — window resize via drag handle, LIBC_TABLE-aware
          gc-sections (session 125's remaining blocker), image
          viewer, calculator…

We now have seven distinct WM client apps:
`wmhello` (interaction demo), `wmtype` (text input),
`wmclock` (clock), `wmpaint` (drawing), `wmpair` (multi-window),
`wmfiles` (file manager), `wmsysinfo` (diagnostics).  The Start
menu catalog reflects all of them and they all coexist on the
same desktop.
