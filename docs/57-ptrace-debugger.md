# Session 57 — ptrace debugger

> Originally this session also introduced a windowing GUI (`gui.elf`). The WM and everything that fed it (mouse driver, fb mmap, out-of-process app protocol from sessions 61–63) were removed when AdventOS narrowed its target audience to developers and AI agents who only need a CLI. The debugger half — built on the same set of new syscalls minus the WM-specific ones — is preserved below.

**Goal:** an in-OS debugger. `dbg.elf` is a `ptrace`-based interactive debugger that runs another AdventOS binary as a child, sets software breakpoints by symbol name (resolved from `.syms` sidecars shipped in the fs), single-steps, reads + writes the tracee's registers and memory, and presents an interactive prompt or a `--auto` scripted demo.

Ships with a deterministic selftest:

```
[t40] dbg: INT3 breakpoint + single-step + ptrace round-trip
  PASS  debugger loaded .syms file for dbgtest.elf
  PASS  INT3 at dbgtest's main entry delivered SIGTRAP (sig=5)
  PASS  addr-to-sym resolved EIP to <square+...>
  PASS  software breakpoint re-armed correctly across 5 calls
  PASS  dbgtest ran to completion under tracer (counter=30)
```

---

## The shape

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

## Kernel mechanism

Three pieces had to come into existence.

### 1. IDT vector 3 at DPL=3

```c
idt_set_gate(3, (uint32_t)isr3, 0x08, 0xEE);   /* was 0x8E */
```

Without DPL=3 on the `#BP` vector, a user-mode 0xCC byte raises **`#GP`** instead of trapping — the CPU enforces "you tried to invoke a kernel-only gate from CPL=3." With DPL=3 the trap lands cleanly in our `isr_handler`. The other 31 gates stay DPL=0.

### 2. INT3 / #DB → `trap_stop_for_tracer`

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

### 3. The `SYS_PTRACE` multiplex

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

## The software-breakpoint dance

Software breakpoints are universal because they don't need hardware debug registers. The pattern is decades old:

```
set bp at A:    save A's byte; write 0xCC at A
hit:            CPU traps after executing 0xCC; EIP = A+1
continue:       restore byte at A; rewind EIP to A; STEP one instruction;
                re-write 0xCC at A; CONT
```

`continue_past_trap()` in `dbg.c` does exactly that. The crucial subtlety is **only** doing the rewind+restore dance if there's actually a planted bp at `EIP-1`. dbgtest's hand-rolled `int $3` at main is NOT a planted bp — it's a literal `0xCC` byte in the .text section. If we rewound EIP back to it, we'd loop forever. So `bp_at(addr)` returns NULL for non-bp traps and we just `PTRACE_CONT` from the post-int3 EIP.

## Symbol resolution

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

## Auto-mode vs interactive

`dbg.elf` supports both:

```
dbg <prog> [args...]            interactive prompt
dbg --auto <prog> [args...]     scripted demo (used by [t40])
```

Interactive commands: `regs`/`r`, `syms`/`s`, `break <name>`/`b`, `bps`, `delete <id>`/`d`, `cont`/`c`, `step`/`si`, `mem <addr>`/`x`, `quit`/`q`. The auto mode runs the dbgtest demo deterministically and prints structured lines the t40 selftest greps.

## A printf fix the debugger needed

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

- `kernel/syscall.h` — new syscall numbers, ptrace ABI structs. (Slots 72 / 74 originally fed the WM and have since been retired; 73 `SYS_KBD_POLL` and 75 `SYS_PTRACE` remain.)
- `kernel/syscall.c` — dispatchers for `SYS_KBD_POLL` and `SYS_PTRACE`.
- `kernel/ptrace.c` — new file. The full `ptrace_dispatch` switch + cross-PD peek/poke.
- `kernel/isr.c` — `trap_stop_for_tracer` + the INT3 / #DB → tracer hook.
- `kernel/idt.c` — vector 3 IDT entry bumped to DPL=3.
- `kernel/task.{h,c}` — `tracer_pid` / `traced_stopped` / `trap_frame` / `trap_signal` fields on `struct task`.
- `kernel/paging.{h,c}` — `paging_user_va_to_pa(pd, va)` for cross-PD walks.
- `libc/stdio.c` — `%0Nx` / `%0Nd` width-padding in printf.
- `user/libuser.{c,h}` — wrappers for the retained syscalls; `PTRACE_*` op constants and the ptrace_regs / ptrace_args structs mirrored from the kernel.
- `user/dbg.c` — new file. Interactive + scripted debugger.
- `user/dbgtest.c` — new file. Toy target with `noinline` helpers the debugger breaks on.
- `user/sh.c` — `[t40]` debugger test.
- `build.sh` — emit `.syms` sidecars from `nm`, add `dbg` / `dbgtest` to USER_PROGS.
- `mkfs.py` — ship `dbg.elf`, `dbgtest.elf`, and the two `.syms` files.

## What's still out of scope

- **The debugger doesn't disassemble.** `mem <addr>` is a hex dump; a real disassembler would need an x86 decoder. The `step` and `regs` commands cover the bulk of what people use a debugger for, but you read the instructions as hex bytes.
- **Hardware breakpoints (DR0–DR3) are not used.** All breakpoints are software (the 0xCC dance). For watchpoints on data, we'd want hardware DR registers.
- **`/proc/<pid>/mem` isn't exposed.** Tracee memory inspection runs through `PTRACE_PEEKDATA`, which is fine for the debugger, but a `tasks`-app that wanted to dump another process's heap wouldn't have a way in without becoming its tracer first.
