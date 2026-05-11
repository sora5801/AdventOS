# Session 57 — GUI window manager + ptrace debugger

**Goal:** two independent features, packed in one session because both are user-space programs sitting on top of small new kernel primitives:

1. **A real window manager** — `gui.elf` becomes a compositing WM with multiple windows, drag-by-title-bar, click-to-focus, z-order, close buttons, and four built-in apps (Hello, Clock, Paint, Tasks). Replaces the session-34 single-window mouse demo.

2. **An in-OS debugger** — `dbg.elf` is a `ptrace`-based interactive debugger that runs another AdventOS binary as a child, sets software breakpoints by symbol name (resolved from `.syms` sidecars shipped in the fs), single-steps, reads + writes the tracee's registers and memory, and presents an interactive prompt or a `--auto` scripted demo.

Both ship with deterministic selftests:

```
[t39] GUI / window manager: multi-window compositing + scripted events
  PASS  WM mmap'd framebuffer and reported geometry
  PASS  Paint canvas cell @ (322,232) is RED (0xe03030) — scripted click landed
  PASS  Hello title bar @ (110,70) is FOCUS-BLUE (0x305080) — drag-and-raise worked
  PASS  Desktop bg @ (0,30) is steel-blue (0x103060) — fb_takeover'd cleanly
  PASS  WM exited normally after scripted frames

[t40] dbg: INT3 breakpoint + single-step + ptrace round-trip
  PASS  debugger loaded .syms file for dbgtest.elf
  PASS  INT3 at dbgtest's main entry delivered SIGTRAP (sig=5)
  PASS  addr-to-sym resolved EIP to <square+...>
  PASS  software breakpoint re-armed correctly across 5 calls
  PASS  dbgtest ran to completion under tracer (counter=30)
```

Full selftest count: **96 PASS, 0 FAIL**, no regressions.

---

## Part A — Window manager

### What was there

`gui.elf` since session 34 was a single-window proof-of-concept: mmap the framebuffer, poll the PS/2 mouse, paint a fixed background, draw a 12×12 cursor sprite that tracked the mouse, and a static "status bar." It had no concept of windows, focus, drag, or events past click highlighting.

### What's there now

`gui.elf` is a userspace WM. The core abstraction is a tiny window table:

```c
struct window {
    int      alive;
    int      id;
    char     title[32];
    int      x, y;             /* top-left, including title bar */
    int      w, h;             /* total, including title bar */
    int      z;                /* higher = nearer the front */
    int      focused;
    draw_fn  draw;
    click_fn click;
    key_fn   key;
    int      state[16];        /* per-app scratchpad */
};
static struct window g_wins[8];
```

The event loop is the conventional shape:

```c
for (;;) {
    poll_mouse();   /* sys_mouse_state → (x, y, btns) */
    poll_kbd();     /* sys_kbd_poll (non-blocking) */
    handle_mouse(); /* hit-test, focus, drag, dispatch click */
    handle_key();   /* dispatch to focused window */
    desktop_draw(); /* z-sort, paint each window's frame + content */
    draw_cursor();
    sys_sleep_ms(16); /* ~60 fps */
}
```

#### Three kernel additions enabled it

1. **`SYS_FB_TAKEOVER(on)`** — sets `fbcon`'s `g_enabled` flag. When `on=1`, every kprintf/sys_write that would otherwise hit the framebuffer is a no-op. Without this, kernel-side log output (which appears regularly: scheduler heartbeats, IRQ traces, etc.) would tear into the WM's rendering with random glyphs. The WM calls it on entry and clears it on exit so console output resumes naturally.

2. **`SYS_KBD_POLL()`** — non-blocking read from the kbd input ring. Returns the next byte, or 0 if the ring is empty. Bypasses the cooked TTY layer entirely because the WM wants raw keystrokes regardless of `TTY_ICANON`. The session-46 `vi` editor would normally toggle into raw mode with `SYS_TTY_SET_MODE`; for a UI doing 60 fps polling that round-trip is overkill.

3. **`SYS_MOUSE_INJECT(x, y, btns)`** — force absolute cursor state, the test-only counterpart of the existing relative-delta `mouse_inject()` used by USB HID. The session-57 selftest drives the WM with a hard-coded script of (frame, x, y, btns) tuples so pixel reads at known coords are deterministic, not racing real PS/2 packet timing.

#### Windowing details worth calling out

- **Drag is initiated by a click on the title-bar strip** (after excluding the close-button rectangle). Once the left mouse button goes up, drag ends. While dragging, every frame the window's (x, y) tracks the cursor with the original click-offset preserved.

- **Focus = highest `z`.** `win_raise(idx)` bumps `z = ++g_next_z` and sets `focused`, clearing focus on all others. Z-sort happens every redraw — O(n²) on `MAX_WINDOWS=8` is 32 compares, negligible.

- **Close button** is a 14×14 red square at the title-bar's right edge. Hit-tested before the title-bar drag so a click there flips `alive=0` instead of starting a drag.

- **Per-app callbacks** — `draw`, `click`, `key`. The Paint app uses all three: `paint_draw` renders the canvas + color swatches, `paint_click` paints a cell (or picks a swatch) when called with the body-local coords, and we exploit a "continuous drag-paint" path in `handle_mouse` to call `click` every frame the button is held.

#### The four apps

| App      | What it does                                                   |
|----------|----------------------------------------------------------------|
| **Hello** | Three lines of static text. The "hello world" of the WM.       |
| **Clock** | `HH:MM:SS` from `sys_time()`, redrawn every frame. Demonstrates animated content. |
| **Paint** | 64×48 cell canvas at 4 px/cell. Click + drag to paint, pick color from a 6-swatch palette. Demonstrates state + drag-tracking. |
| **Tasks** | Reads `/proc/<pid>/status` for every pid dir under `/proc`, prints `(pid, name)` rows. Demonstrates an OS-introspection app. |

### The selftest

[t39] in `sh.c` runs `gui.elf selftest` (which caps at 80 frames and applies a scripted ladder of `sys_mouse_inject` calls) and then greps three structured pixel-readback lines for exact hex colors. The choreography:

1. Frame 20 → cursor parks outside any window
2. Frame 25 → click at (322, 230) — *inside Paint's canvas at cell (3, 1)*. Window focuses + raises; click handler paints the cell red.
3. Frame 40 → release
4. Frame 50 → click Hello's title bar (100, 70). Hello rises to top, title goes focus-blue.
5. Frame 60 → release

Then the WM prints, before exiting:

```
paint cell @ (322,232) = 0xe03030
hello title bar @ (110,70) = 0x305080
desktop bg @ (0,30) = 0x103060
```

t39 greps for these byte-exact strings. Pixel-exact == no flakiness even on slow QEMUs. If the WM never registered, the framebuffer mmap failed, or a click missed by even one pixel, the colors won't match.

---

## Part B — ptrace debugger

### The shape

```
$ dbg --auto dbgtest.elf
dbg: loaded 98 symbol(s) for dbgtest.elf
dbg: traced dbgtest.elf pid=67
dbg-auto: entry trap, sig=5
dbg-auto: stopped at <main+0x5f>  eip=400000d7
dbg-auto: bp #1 @ 40000018 <square>
dbg-auto: square hit, sig=5
dbg-auto: stopped at <square+0x0>  eip-1=40000018
dbgtest: counter=30 (expected 30)
dbg-auto: total square hits = 5 (expected 5)
dbg-auto: dbgtest exited code=0
```

End-to-end: dbg loads symbols, forks dbgtest under `PTRACE_TRACEME`, dbgtest's main issues `int $3` for an entry stop, dbg plants `0xCC` at `square`'s address, continues, watches dbgtest's `compute_loop` call square 5 times — each time we hit the breakpoint, restore the byte, rewind EIP, single-step, re-arm the byte, and continue. dbgtest ends with `counter == 30` (`square(0)+square(1)+...+square(4) = 0+1+4+9+16 = 30`).

### Kernel mechanism

Three pieces had to come into existence.

#### 1. IDT vector 3 at DPL=3

```c
idt_set_gate(3, (uint32_t)isr3, 0x08, 0xEE);   /* was 0x8E */
```

Without DPL=3 on the `#BP` vector, a user-mode 0xCC byte raises **`#GP`** instead of trapping — the CPU enforces "you tried to invoke a kernel-only gate from CPL=3." With DPL=3 the trap lands cleanly in our `isr_handler`. The other 31 gates stay DPL=0.

#### 2. INT3 / #DB → `trap_stop_for_tracer`

In `kernel/isr.c`:

```c
if ((n == 1 || n == 3) && (r->cs & 0x3) == 3) {
    struct task *t = task_current();
    if (t && t->tracer_pid != 0) {
        if (n == 1) r->eflags &= ~0x100u;   /* clear TF defensively */
        trap_stop_for_tracer(r, SIGTRAP);
        return;
    }
}
```

`trap_stop_for_tracer` parks the current task as `TASK_STATE_STOPPED`, stores the iret-frame pointer as `trap_frame` so the tracer can read/write registers off it, sets `traced_stopped=1` and `trap_signal=SIGTRAP`, then delivers `SIGCHLD` to the tracer and schedules away.

The BKL handoff is the same pattern session-56's signal_check used: `bkl_unlock` before schedule, re-take afterward when the tracer continues us.

#### 3. The `SYS_PTRACE` multiplex

A single syscall switched by `op`:

```c
ptrace_dispatch(op, pid, &args)
   ops: TRACEME, ATTACH, DETACH, PEEKDATA, POKEDATA,
        GETREGS, SETREGS, CONT, STEP, WAIT
```

The interesting ones:

- **PEEKDATA / POKEDATA**: cross-VA-space copy from the *tracer*'s syscall context. We walk the *tracee*'s page directory by physical address (which lives in the identity-mapped low 32 MiB) using a new `paging_user_va_to_pa(pd, va)` helper, then read/write through the identity-mapped physical address. **No CR3 swap needed**, which means the tracer's user buffer `args.buf` stays addressable for the duration of the copy.

- **GETREGS / SETREGS**: copy the saved iret-frame in/out via `struct ptrace_regs`. SETREGS masks EFLAGS so the tracer can't punch IF=0 or IOPL>0 (a tracee that woke up with interrupts disabled and IOPL=3 would be a privilege escalation).

- **STEP**: same as CONT but ORs `TF` into the saved EFLAGS first. The CPU clears TF on entry to the `#DB` handler so single-step doesn't infinitely loop.

- **WAIT**: poll until the tracee transitions. Implemented as a `task_yield`-driven loop rather than a custom BLOCKED state — keeps the kernel-side state machine simple. Eligibility check is "either we're the tracer, OR we're the tracee's fork-parent" — the OR clause closes a fork-race where the parent calls WAIT before the child has finished PTRACE_TRACEME.

### The software-breakpoint dance

Software breakpoints are universal because they don't need hardware debug registers. The pattern is decades old:

```
set bp at A:    save A's byte; write 0xCC at A
hit:            CPU traps after executing 0xCC; EIP = A+1
continue:       restore byte at A; rewind EIP to A; STEP one instruction;
                re-write 0xCC at A; CONT
```

`continue_past_trap()` in `dbg.c` does exactly that. The crucial subtlety is **only** doing the rewind+restore dance if there's actually a planted bp at `EIP-1`. dbgtest's hand-rolled `int $3` at main is NOT a planted bp — it's a literal `0xCC` byte in the .text section. If we rewound EIP back to it, we'd loop forever. So `bp_at(addr)` returns NULL for non-bp traps and we just `PTRACE_CONT` from the post-int3 EIP.

### Symbol resolution

`nm` emits a flat list of `<addr> <type> <name>`. Build.sh's awk filter keeps only `T`/`t` entries (text, both global and file-static) and writes them to `user/_obj/<prog>.syms`. mkfs.py ships those alongside the binary:

```
DATA_FILES = [
    ...
    ('dbgtest.syms', 'user/_obj/dbgtest.syms', None),
    ('dbg.syms',     'user/_obj/dbg.syms',     None),
]
```

`dbg.c::load_syms` reads the file, parses each line, strips the leading `_` (mingw underscore), and **drops section-name pseudo-symbols** (anything starting with `.` — nm emits `.text` / `.rdata` at the same address as the first real function, which would otherwise win the closest-symbol race in `addr_to_sym`).

The `square` function had a non-obvious caveat: **`-O2` inlined it** so it disappeared from the symbol table entirely. `__attribute__((noinline))` plus making it non-static (so its address is "exposed" enough that the optimizer can't eliminate the standalone function) kept it as a real symbol at `0x40000018`.

### Auto-mode vs interactive

`dbg.elf` supports both:

```
dbg <prog> [args...]            interactive prompt
dbg --auto <prog> [args...]     scripted demo (used by [t40])
```

Interactive commands: `regs`/`r`, `syms`/`s`, `break <name>`/`b`, `bps`, `delete <id>`/`d`, `cont`/`c`, `step`/`si`, `mem <addr>`/`x`, `quit`/`q`. The auto mode runs the dbgtest demo deterministically and prints structured lines the t40 selftest greps.

### A printf fix the debugger needed

The libc printf parser literally dumped `%08x` as text because there was no width-spec parser. Added a minimal `%0N<x|d>` path:

```c
if (*fmt == '0') {
    fmt++;
    while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt - '0'); fmt++;
    }
}
```

Now `printf("%08x", 0x1234)` produces `00001234` instead of `%08x`. The debugger uses this for pretty register dumps; every other AdventOS program inherits it.

---

## Touched files

- `kernel/syscall.h` — 4 new syscall numbers (72–75), ptrace ABI structs.
- `kernel/syscall.c` — dispatchers for SYS_FB_TAKEOVER / SYS_KBD_POLL / SYS_MOUSE_INJECT / SYS_PTRACE.
- `kernel/ptrace.c` — new file. The full ptrace_dispatch switch + cross-PD peek/poke.
- `kernel/isr.c` — `trap_stop_for_tracer` + the INT3 / #DB → tracer hook.
- `kernel/idt.c` — vector 3 IDT entry bumped to DPL=3.
- `kernel/task.{h,c}` — `tracer_pid` / `traced_stopped` / `trap_frame` / `trap_signal` fields on `struct task`.
- `kernel/paging.{h,c}` — `paging_user_va_to_pa(pd, va)` for cross-PD walks.
- `kernel/fbcon.{h,c}` — `fbcon_set_enabled(on)` so the WM can pause kernel console writes.
- `kernel/mouse.{h,c}` — `mouse_set_state(x, y, btns)` for the WM selftest.
- `libc/stdio.c` — `%0Nx` / `%0Nd` width-padding in printf.
- `user/libuser.{c,h}` — wrappers for the 4 new syscalls; `PTRACE_*` op constants and the ptrace_regs / ptrace_args structs mirrored from the kernel.
- `user/gui.c` — complete rewrite as window manager with 4 built-in apps. Selftest mode applies a scripted mouse-event ladder.
- `user/dbg.c` — new file. Interactive + scripted debugger.
- `user/dbgtest.c` — new file. Toy target with `noinline` helpers the debugger breaks on.
- `user/sh.c` — `[t39]` GUI test, `[t40]` debugger test, t24 updated to use `gui.elf selftest`.
- `build.sh` — emit `.syms` sidecars from `nm`, add `dbg` / `dbgtest` to USER_PROGS.
- `mkfs.py` — ship `dbg.elf`, `dbgtest.elf`, and the two `.syms` files.

## What's still out of scope

- **The WM is single-process.** Apps are callback tables linked into `gui.elf`. A "real" model would have separate apps talking to the WM over a socket — out of scope for what was a "GUI in a session" lift. The framework is in place.
- **No double-buffering.** Every frame is rendered directly to the framebuffer. At 1024×768/32bpp QEMU eats the ~3 MiB blit without noticeable tearing in our scripted scenarios, but a real display would want either a back buffer or per-window damage rects.
- **The debugger doesn't disassemble.** `mem <addr>` is a hex dump; a real disassembler would need an x86 decoder. The `step` and `regs` commands cover the bulk of what people use a debugger for, but you read the instructions as hex bytes.
- **Hardware breakpoints (DR0–DR3) are not used.** All breakpoints are software (the 0xCC dance). For watchpoints on data, we'd want hardware DR registers — a session 58+ topic if needed.
- **`/proc/<pid>/mem` isn't exposed.** Tracee memory inspection runs through `PTRACE_PEEKDATA`, which is fine for the debugger, but a `tasks`-app that wanted to dump another process's heap wouldn't have a way in without becoming its tracer first.
