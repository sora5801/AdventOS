# Session 112 — Path C phase 6: WM client protocol

**Goal.** Make "external program draws into its own window" a real
thing.  Session 111 stood up the compositor with daemon-internal pixel
buffers; session 112 swaps those out for shared-memory surfaces
allocated by the kernel, so an unrelated process can register a
window with wmd and paint it.

Status: **done.** Smoke test (`smoke_wmclient.py`, 8/8 pass):

```
=== pixel checks ===
  [OK] wmd top status bar @ y=6 (887/924)
  [OK] client title bar dark @ y=369 (150/164)
  [OK] client blue band @ y=386 (169/170)
  [OK] client green bottom @ y=515 (180/180)
  [OK] client bg-gradient @ (346,408) = (16, 16, 81)
  [OK] client red square pixels @ y=433: 16
  [OK] wmd Clock content @ x=200 (80/80)
  [OK] wmd Color-bar 0 RED @ y=420 (22/33)
```

Sequence under test:

1. `wmd 30 &` — wmd takes the FB, calls `SYS_WM_BIND`, registers
   the four demo windows, starts compositing.
2. `wmhello 12` — wmhello calls `wm_open(...)` which delegates to
   `SYS_WM_CREATE`.  Kernel allocates physical pages, dual-maps them
   into wmhello at `0x60000000` and into wmd at the per-slot VA
   (`0x60000000 + slot * 1 MiB`), and queues an op=1 message.
3. wmd's next tick drains the message, allocates a `struct window`
   slot at the deterministic offset (340, 360), points
   `client_pixels` at the dual-mapped VA, and starts painting the
   surface in its compositor loop.
4. QMP screendump catches the wmhello content rendered through that
   path: blue title band, red moving square, green bottom border,
   shifting blue-tinted background — all of which are pixels
   wmhello wrote into the shared surface that wmd then composited.

---

## The shared-surface design

Three concerns: who allocates the pixels, who maps them, who frees
them.

### Allocation

`SYS_WM_CREATE` validates `(w, h)`, computes `n_pages = ceil(w*h*4 /
4096)`, and walks the PMM page by page calling `pmm_alloc_page()`.
Each newly-allocated page is zeroed (so the client doesn't see stale
RAM) and threaded into the slot's `pages[]` array (kmalloc'd at
create time, not statically reserved — see "Why kmalloc" below).

### Dual mapping

After the pages exist physically, we map them into both PDs:

```c
paging_map_in(client_pd, client_va + i * PAGE_SIZE, phys,
              PTE_USER | PTE_WRITABLE);
paging_map_in(wmd_pd,    wmd_va    + i * PAGE_SIZE, phys,
              PTE_USER | PTE_WRITABLE);
```

`client_va` is always `WM_SURFACE_VA_BASE = 0x60000000` — each
client only gets one window in session 112, and that VA is far
enough from code (0x40000000), stack (0x40100000), heap
(0x40200000–0x40600000), and the FB (0x50000000) that overlap is
impossible.

`wmd_va` is per-slot: `0x60000000 + slot_idx * WM_MAX_PAGES_PER_WIN
* PAGE_SIZE = 0x60000000 + slot_idx * 1 MiB`.  So wmd's address
space looks like:

```
0x60000000  surface 0  (1 MiB max)
0x60100000  surface 1
0x60200000  surface 2
0x60300000  surface 3
```

With WM_MAX_WINDOWS = 4 in session 112, that's 4 MiB of reserved VA.
The actual mapping density per surface depends on `n_pages`; a
220×140×4 = 30,800-byte surface needs 8 pages = 32 KiB out of the 1
MiB slot.

### Free

Pages are freed exactly once. The four lifecycle events:

| event                  | what we do                                |
|------------------------|-------------------------------------------|
| client SYS_WM_DESTROY  | unmap from client_pd, slot → DESTROY_PENDING |
| client task_exit       | (same; client_pd is about to be destroyed anyway) |
| WM picks up the destroy| unmap from wmd_pd, free physical pages, slot → EMPTY |
| WM task_exit while bound | pull wmd-side mappings (so PD destroy doesn't free) |

The `unmap_from_pd_keep_phys` helper walks the PD/PT manually and
clears the PTE without touching the physical page — needed because
the standard `paging_unmap` operates on the current PD only and
unconditionally frees the page.

---

## State machine (per slot)

Lives entirely in the kernel; queue-less.

```
EMPTY
  │  SYS_WM_CREATE
  ▼
OPEN_PENDING ──── SYS_WM_POLL (wmd reads op=1) ────► LIVE
                                                       │
                                  SYS_WM_DESTROY       │
                                  or task_exit         │
                                                       ▼
                                                DESTROY_PENDING
                                                       │
                              SYS_WM_POLL (wmd reads op=2)
                                                       │
                                                       ▼
                                                     EMPTY
```

`wm_pop_message` scans slots in order, returns the first
`OPEN_PENDING` or `DESTROY_PENDING` found, and transitions its
state.  Slots are independent — at most one transition per scan, no
mutex needed because the syscall handler holds preemption disabled.

The state-machine-on-slots model replaced an earlier in-kernel ring
buffer of messages.  The ring cost ~448 bytes of `.bss` (8 × 56) and
pushed the kernel image past the VGA-RAM ceiling at 0xA0000.  Per-
slot state adds zero extra `.bss`.

---

## Why kmalloc for the page list

`struct wm_window` originally had `uint32_t pages[256]` inline.  At
WM_MAX_WINDOWS = 4 that's 4 KiB of `.bss` — combined with everything
else in `wm.c`, it pushed the linker past the strict 0xA0000 limit
(see kernel/fs.h's history for the same dance).

Switching to `uint32_t *pages` lets the slot reserve only 4 bytes
when empty.  At create time the kernel kmallocs `sizeof(uint32_t) *
n_pages` — typically 32 bytes for an 8-page surface — and kfrees on
destroy.  Total `.bss` footprint of `kernel/wm.c`: 8 bytes
(two globals).  Everything dynamic.

---

## The kernel image diet

To fit the `wm.c` + four-syscall additions into `.bss` < 0xA0000,
session 112 also turned on `-ffunction-sections` + linker
`--gc-sections`:

- gcc emits one section per function (`.text.kmain`, `.text.wm_bind`,
  …).
- ld follows section reachability from `_kernel_entry` and KEEP()ed
  sections (`.text.entry`, the AP trampoline, `.up1`, `.up2`).
- Unreachable functions get dropped at link time.

Net result: `.text` shrank from 0x1a594 (108436 B) to 0x17814
(96276 B) — a 12 KiB recovery.  `.rdata` shrank by 8.7 KiB similarly
(string constants of unused functions).  Kernel image: 131248 →
114864 B, BSS end pushed back from 0xa077c down to 0x9b33c (19 KiB
of headroom).

`-fdata-sections` was *not* enabled — the PE/COFF backend folds
`.bss.*` sections into `.data` under `--gc-sections`, which bloats
kernel.bin (BSS doesn't take disk space; `.data` does).  Function-
section trimming alone is enough.

`.up1` and `.up2` got `KEEP()` in the linker script because they're
referenced only via `_up1_start`/`_up1_end` symbols (linker-defined
markers, not section references), which don't count in the GC graph.

---

## Pixel format and the per-pixel blit

Client surfaces are always 32-bit packed `0x00RRGGBB` regardless of
the underlying framebuffer bpp.  This is non-negotiable: the client
shouldn't know the FB layout, and the WM has all the format
machinery it needs (gfx_pack, etc.).

wmd's `paint_client` is a straight per-pixel loop:

```c
for (yy in window) {
    const unsigned int *row = w->client_pixels + yy * w->surface_w;
    for (xx in window) {
        gfx_put_pixel(ctx, dest_x + xx, dest_y + yy, row[xx]);
    }
}
```

`gfx_put_pixel` handles bpp packing (24-bit on the default QEMU FB).
For a 220×140 surface that's 30,800 calls per frame.  At 60 fps:
~1.85 M calls/s.  Measured composite cost: still under the 16 ms
frame budget on QEMU, but obvious bottleneck for any larger window.

Future sessions will inline the row blit per-bpp (one loop per
output bpp, write 24-bit triples directly).  For session 112 the
correctness gain of "WM handles all format conversion" was worth
the perf hit.

---

## libwm — the client side

Tiny:

```c
struct wm_window w;
if (wm_open(&w, "Hello", 200, 120) < 0) abort();
for (...) {
    wm_clear(&w, 0xFFFFFF);
    wm_fill_rect(&w, 10, 10, 80, 40, 0xE03030);
    wm_put_pixel(&w, 50, 50, 0x000000);
    wm_present(&w);
    sys_sleep_ms(16);
}
wm_close(&w);
```

`wm_open` calls `sys_wm_create`, captures the kernel-returned
`pixels_va`, and exposes it as a `unsigned int *pixels` for the
client to write directly.  Painting is just memory writes through
that pointer.

`wm_present` is a no-op in session 112 — the WM repaints every
frame anyway, so there's no "tell the WM to look at my surface"
signal needed.  The function exists for forward compatibility:
session 113+ will turn it into a damage-region notification.

`wm_close` calls `sys_wm_destroy(id)`.  Kernel + WM clean up via the
DESTROY_PENDING transition; the client just exits cleanly.

---

## What's missing (session 113+ targets)

- **Input routing to clients.**  Currently mouse and keyboard events
  go to wmd; clients can't read them.  Session 113 adds a per-
  client event queue (`sys_wm_event_poll`) so a focused window
  receives clicks, drags, and key events.
- **Multiple windows per client.**  Session 112 binds each client
  PID to a single window VA.  A multi-window client wants
  `wm_open` to return a separate `pixels` pointer per call —
  needs the kernel to bump-allocate per-client VAs instead of
  reusing 0x60000000.
- **Damage regions.**  The compositor repaints everything every
  frame, including unchanged 1024×768 backbuffers.  A damage-rect
  protocol would let it composite just the changed slices.
- **Larger surfaces.**  `WM_MAX_PAGES_PER_WIN = 256` caps a window
  at 1 MiB (≈ 512×512 at 32-bpp).  Big enough for widgets, not for
  a full-screen client.

---

## Files touched

### Kernel

- `kernel/wm.h`, `kernel/wm.c` — new (~250 lines)
- `kernel/syscall.h` — 4 new `SYS_WM_*` numbers + ABI structs
  (`sys_wm_create`, `sys_wm_msg`)
- `kernel/syscall.c` — 4 handler cases + `wm.h` include
- `kernel/task.c` — `wm_on_task_exit` hook in `task_exit_current`
- `kernel/fs.h` — `FS_MAX_FILES` 128 → 160, `FS_SUPER_SECTORS`
  9 → 11 (added wmhello.elf and its man page; the prior cap was
  tight)

### Build

- `build.sh` — `-ffunction-sections` for the kernel, `--gc-sections`
  for the kernel link, libwm build step, `WMCLIENT_PROGS` link rule
- `linker_kernel.ld` — `KEEP()` wrappers on `.up1` / `.up2` so
  `--gc-sections` doesn't drop the embedded init programs
- `mkfs.py` — `wmhello.elf` + `fs/man/wmhello`, `FS_MAX_FILES=160`,
  `FS_SUPER_SECTORS=11`

### Userspace

- `user/libuser.h`, `user/libuser.c` — 4 syscall numbers + ABI
  structs + 4 inline-asm wrappers
- `libwm/libwm.h`, `libwm/libwm.c` — new client library (~80 lines)
- `user/wmd.c` — `sys_wm_bind` at startup; `drain_wm_messages` each
  tick; `paint_client` for KIND_CLIENT windows; z-counter starts at
  4 so new client windows raise above the demo set
- `user/wmhello.c` — new sample client (~60 lines)
- `fs/man/wmhello` — new

### Tests + docs

- `smoke_wmclient.py` — new headless harness (QMP screendump + 8
  pixel checks); also flipped `smoke_db.py` / `smoke_wmd.py` to
  `wait=on` because the slimmer kernel now boots faster than the
  smoke-test serial-connect window
- `docs/99-pathC-wmprotocol.md` — this file

`kernel.bin`: 114864 bytes (down from 131248, thanks to
`--gc-sections`).  Net image growth from session 111: −16384 bytes
DESPITE adding the WM client protocol kernel and an ABI for 4 new
syscalls.

---

## Path C status after session 112

- ✅ 107 — FB syscalls
- ✅ 108 — libgfx (drawing primitives, fonts)
- ✅ 109 — PS/2 mouse + cursor demo
- ✅ 110 — double-buffering / no-tearing present
- ✅ 111 — wmd compositor (multi-window, mouse focus & drag)
- ✅ 112 — WM client protocol (SHM surfaces, libwm, wmhello sample)
- ⏳ 113 — input routing to focused client, multi-window per client
- ⏳ 114+ — widgets, real apps (clock, terminal, …)

The story so far: one program owns the framebuffer, draws smooth
flicker-free graphics, hosts multiple overlapping window
decorations with mouse-driven focus / drag, AND now lets external
programs allocate their own shared-memory surfaces and paint into
them.  The "many programs visible at once" milestone is now real,
not a hardcoded demo.
