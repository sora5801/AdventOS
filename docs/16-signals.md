# Session 16 — Full Unix signals

**Goal:** Add real, user-installed signal handlers via `signal()` / `sigaction()`. The kernel queues signals, delivers them at the next return-to-ring-3, builds a delivery frame on the user stack so the handler runs in user mode with a sane cdecl ABI, then restores the pre-signal context cleanly when the handler returns through a `sigreturn` trampoline.

End state — a self-test in `user/sh.c` that fork+kill+handler-runs:

```
[t6] signals: SIGUSR1 from child to parent
[user task pid=15 exited code=0]
  handler: caught signal in ring 3
  parent: got_sig=10  (spun 2 times)
  child reaped, exit=0
```

The "handler: caught signal in ring 3" line was emitted from inside a user-mode signal handler running on the parent's stack, after a child sent SIGUSR1 via `kill()` from a different process. The handler set a `volatile int g_got_sig` flag, returned, and the parent's main loop saw the flag flip and continued — proving the saved register frame round-tripped through the kernel and back without losing state.

## What's in scope

In:
- `kernel/signal.{h,c}` — pending bitmap, mask, handler table, delivery + sigreturn machinery
- Per-task state in `struct task`: `sig_pending`, `sig_mask`, `sig_handlers[NSIG]`, `sig_tramp`
- `SYS_KILL = 24`, `SYS_SIGACTION = 25`, `SYS_SIGRETURN = 26`
- libuser wrappers `kill()`, `signal()`, `sigaction()`, plus the asm `_sigreturn_tramp`
- Delivery hook at the tail of `syscall_dispatch` AND in `irq_handler` after PIT preemption
- POSIX-flavored fork/exec semantics: fork inherits handlers + mask, clears pending; exec resets handlers to SIG_DFL (SIG_IGN survives)
- Default action: TERMINATE for most signals, IGNORE for SIGCHLD
- Auto-mask of the delivered signal during the handler so it can't recurse on itself

Out:
- `sigprocmask` syscall (we implement the mask internally but don't expose it; deferred)
- Real-time signals / `sigqueue` (we don't queue counts, just bits)
- `sigaltstack` / signal-stack
- `siginfo_t` / `sa_sigaction` 3-arg handlers
- `SA_RESTART` — interrupted syscalls return -EINTR (we don't have any blocking syscalls that surface that today; the only one is `sys_wait` and our signals don't unblock it)
- Signal-interrupted blocking — a task in `BLOCKED_ON_CHILD` ignores incoming signals until it naturally unblocks
- `pause()` / `sigsuspend()`
- Job control signals (SIGSTOP / SIGCONT / SIGTSTP)
- Default action other than TERMINATE/IGNORE — no STOP, no CORE
- `kill(0, sig)` (process group), `kill(-1, sig)` (broadcast)
- Synchronous signals from CPU exceptions (we still kernel-panic on a #PF from ring 3 instead of delivering SIGSEGV)

## Architecture: signals are deferred until iret-to-ring-3

The single most important design decision is *when* a signal gets delivered. The classical Unix model is:

> Signals are delivered to the target process at the next moment the kernel is about to return to that process's user mode.

That moment is well-defined in our kernel:

- Tail of `syscall_dispatch()` — the user just made an INT 0x80, the dispatcher ran the case, and is about to return to `isr_common_stub` which will pop registers and `iret`.
- Tail of `irq_handler()` — a hardware IRQ preempted the user task; after the IRQ handler runs (which may have called `schedule()` and switched away and back), we're about to iret.

Both points have a `struct registers *r` in scope pointing at the saved-frame on the kernel stack. If we mutate `r->eip` and `r->useresp` before the iret, the user resumes at a different EIP with a different ESP. That's the entire mechanism.

```
                           ring 3                    ring 0
                        (user task)               (kernel)

   1.  kill(pid, SIGUSR1)  ── INT 0x80 ──→  syscall_dispatch
                                              |
                                              | t->sig_pending |= (1 << SIGUSR1)
                                              ↓
   2. (target task)                          schedule rotation
       was running, gets                       eventually picks
       preempted somewhere                     target task
                                              ↓
   3. target task runs in ring 3 again
       — eventually does a syscall (or
       a PIT IRQ fires and we return)        → tail of syscall_dispatch
                                              | signal_check_and_deliver(r)
                                              |   - read pending & ~mask
                                              |   - pick lowest sig
                                              |   - build delivery frame
                                              |     on the user stack
                                              |   - r->eip = handler
                                              |   - r->useresp = new top
                                              ↓
                                            iret
   4. handler runs in ring 3,
       does its work,
       ret's to trampoline
       — tramp does INT 0x80 SIGRETURN     → SYS_SIGRETURN
                                              |   read sigcontext from
                                              |   user stack, *r = ctx
                                              ↓
                                            iret
   5. resumes at original EIP/ESP/regs
       — as if nothing happened
```

## The delivery frame on the user stack

When the kernel delivers a signal to a user handler `void h(int sig)`, it has to set up the user stack so:

- The handler enters with `[esp+0] = return address` and `[esp+4] = sig number` (cdecl).
- When the handler returns (`ret`), control lands at a trampoline that knows how to invoke SIGRETURN.
- The trampoline then issues SYS_SIGRETURN with the user's ESP pointing at a saved-context blob the kernel can read.

So the frame, top to bottom (high VA to low VA):

```
[ user_esp_at_signal_time     ]   ← original top of user stack
[ saved_sigcontext: 64 bytes  ]   ← struct registers verbatim
[ sig_num                     ]   ← cdecl arg1 to handler
[ trampoline_addr             ]   ← cdecl ret addr to handler  ← new user ESP
```

`sizeof(struct registers) = 64` here because that's our saved-frame layout (DS + 8 GP regs + int_no/err_code + 5-dword iret frame). The whole thing gets memcpy'd onto the user stack because that's the simplest contract — sigreturn just memcpy's it back over `r`.

The C side is mechanical:

```c
uint32_t user_esp = r->useresp;

user_esp -= sizeof(struct registers);
*(struct registers *)(uintptr_t)user_esp = *r;        /* sigcontext */

user_esp -= 4;
*(uint32_t *)(uintptr_t)user_esp = (uint32_t)sig;     /* arg */

user_esp -= 4;
*(uint32_t *)(uintptr_t)user_esp = (uint32_t)t->sig_tramp;  /* ret addr */

r->useresp = user_esp;
r->eip     = (uint32_t)h;
t->sig_mask |= (1u << sig);                            /* don't recurse */
```

Writing through the user pointers works because the syscall handler runs on the user task's CR3 — same trick the kernel uses everywhere it touches user memory.

## The trampoline

The handler is `void h(int sig)`. When the handler executes its `ret`, it pops the address at `[esp+0]` (= the trampoline address we pushed) into EIP. ESP now points at `sig_num` (the arg slot). The trampoline:

```asm
.global _sigreturn_tramp
_sigreturn_tramp:
    add    $4, %esp              # skip past the sig_num that handler ret'd over
    mov    $26, %eax             # SYS_SIGRETURN
    int    $0x80
1:  hlt                          # should never reach this
    jmp    1b
```

After the `add $4, %esp`, user ESP points exactly at the `saved_sigcontext` blob the kernel placed there. SYS_SIGRETURN reads that blob and copies it back over `r`:

```c
void signal_sigreturn(struct registers *r) {
    uint32_t user_esp = r->useresp;
    struct registers *uctx = (struct registers *)(uintptr_t)user_esp;

    int delivered_sig = (int)*(uint32_t *)(uintptr_t)(user_esp - 4);

    *r = *uctx;       /* THE restore — wipes our syscall frame */

    if (delivered_sig > 0 && delivered_sig < NSIG) {
        task_current()->sig_mask &= ~(1u << delivered_sig);
    }
}
```

The trampoline lives in libuser at a known symbol. Each call to `signal()` / `sigaction()` passes its address to `SYS_SIGACTION` so the kernel knows where to push as the handler's return address. It's essentially the user-side half of the contract.

## Where the trampoline lives — and why the kernel needs to know

A common confusion: signal delivery happens in the kernel, but it has to push a *user-space* address as the handler's return address. The kernel can't synthesize one — there's no kernel-side code reachable from ring 3.

So the trampoline lives in libuser (every user program is linked against libuser, and this is one of those things), and the kernel learns its address out-of-band: the `sigaction` syscall takes a third argument (`tramp`) which the libuser wrapper fills in automatically:

```c
sighandler_t sigaction(int sig, sighandler_t handler) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_SIGACTION), "b"(sig),
                        "c"(handler), "d"(_sigreturn_tramp)
                      : "memory");
    return (sighandler_t)(uint32_t)ret;
}
```

The kernel stores `tramp` per-task (overwritten on each install — but every libuser caller passes the same address, so it doesn't matter). Each delivery uses the most recently registered trampoline.

This is exactly what Linux's `sa_restorer` does. The mechanism predates the sysv-rt ABI by decades.

## Auto-mask, fork, and exec

Three smaller bits round out the POSIX-flavored design.

**Auto-mask** — when delivery starts, the kernel sets `t->sig_mask |= (1 << sig)`. While the handler is running, the same signal can't be delivered again (it stays pending in `sig_pending` but `pending & ~mask` filters it out). When the handler returns through `sigreturn`, the kernel clears that bit. This prevents `SIGUSR1` from re-entering its own handler, which is the failure mode that turns a 4-line handler into stack overflow.

We don't save/restore the *full* mask through sigcontext — we just remember which bit we set and clear that one bit. A POSIX-correct implementation would save the entire mask in sigcontext (so a handler that mucks with sigprocmask would see its changes reverted on return). For our demo where no one calls `sigprocmask` yet, this is enough.

**Fork** — the child inherits the parent's handler table, mask, and trampoline. The pending set is *not* inherited — the child starts clean. POSIX-spec.

```c
for (int i = 0; i < NSIG; i++)
    child->sig_handlers[i] = parent->sig_handlers[i];
child->sig_mask  = parent->sig_mask;
child->sig_tramp = parent->sig_tramp;
child->sig_pending = 0;
```

**Exec** — caught handlers reset to SIG_DFL (because the handler code lived in the old address space and is gone now). SIG_IGN entries stay (they don't reference user code). The mask carries over; pending carries over but will hit fresh SIG_DFL handlers. The trampoline pointer is cleared — it pointed into the old libuser, and the new ELF will register its own.

```c
void signal_reset_on_exec(struct task *t) {
    for (int i = 0; i < NSIG; i++) {
        if (t->sig_handlers[i] != SIG_IGN)
            t->sig_handlers[i] = SIG_DFL;
    }
    t->sig_tramp = 0;
}
```

## kill is just a bit-set

```c
int signal_send(uint32_t target_pid, int sig) {
    if (sig < 1 || sig >= NSIG) return -1;
    if (target_pid == 0)        return -1;     /* don't signal kmain */

    struct task *t = NULL;
    for (uint32_t i = 0; i < 16; i++) {
        struct task *cand = task_at(i);
        if (cand && cand->id == target_pid) { t = cand; break; }
    }
    if (!t) return -1;

    t->sig_pending |= (1u << sig);
    return 0;
}
```

We don't try to wake a blocked target — they receive the signal naturally when they unblock. That's a real limitation: a parent in `sys_wait` is unkillable until its child exits. POSIX deals with this via "wait is interruptible by signals; returns -EINTR with errno set." We'd need to (a) wake the blocked task, (b) have wait check for a pending signal at the top of its loop, (c) propagate that as a syscall return code. Roughly half a session; deferred.

## SYS_SIGRETURN early-returns from the dispatcher

The dispatcher has a tail that writes the syscall return value into the saved EAX:

```c
void syscall_dispatch(struct registers *r) {
    ...
    switch (num) { ... }

    r->eax = (uint32_t)ret;
    signal_check_and_deliver(r);
}
```

But sigreturn JUST overwrote `r` with the saved context. If we let `r->eax = ret` run, we'd clobber the EAX we just restored. And `signal_check_and_deliver` would re-deliver a signal at the end of a sigreturn — also wrong (we just finished delivering one).

The SYS_SIGRETURN case has to early-return:

```c
case SYS_SIGRETURN: {
    signal_sigreturn(r);
    return;     /* skip both r->eax = ret AND signal_check_and_deliver */
}
```

That's the only special case. Every other syscall happily falls through the tail.

## Files added / modified

| File | Change |
|---|---|
| `kernel/signal.{h,c}` | New. Pending/mask bitmaps, handler table, delivery + sigreturn |
| `kernel/task.{h,c}` | Per-task signal state; `signal_init_task` in `task_create`; inherit on fork; reset on exec |
| `kernel/syscall.{h,c}` | SYS_KILL/SIGACTION/SIGRETURN; `signal_check_and_deliver(r)` at dispatcher tail; SIGRETURN early-returns |
| `kernel/isr.c` | Same delivery hook at end of `irq_handler` so PIT preemption surfaces signals too |
| `user/libuser.{h,c}` | `kill()`, `signal()`, `sigaction()`, `_sigreturn_tramp` asm, signal-number defines |
| `user/sh.c` | t6 selftest: handler install + fork + kill + yield-loop + wait |

## Test trace

Boot output (cleaned of `[A][B]` demo-task tags):

```
[t6] signals: SIGUSR1 from child to parent
[user task pid=15 exited code=0]
  handler: caught signal in ring 3
  parent: got_sig=10  (spun 2 times)
  child reaped, exit=0
```

What happened:

1. Parent (sh, pid 5) calls `signal(SIGUSR1, on_sigusr1)` — kernel stores `on_sigusr1`'s address in `g_tasks[5].sig_handlers[10]`.
2. Parent calls `sys_fork()`. Child (pid 15) inherits the handler table.
3. Child does `sys_sleep_ms(50)` → `sys_kill(parent_pid=5, SIGUSR1=10)` → `sys_exit(0)`.
4. Parent enters `while (g_got_sig == 0) sys_yield();`.
5. Yield syscall returns; dispatcher tail runs `signal_check_and_deliver(r)`.
6. Pending check: `g_tasks[5].sig_pending & ~mask` = `(1 << 10)`. Pick sig 10. Handler is `on_sigusr1`.
7. Build delivery frame on parent's user stack: 64 bytes of saved registers + sig_num=10 + tramp_addr. Set `r->eip = on_sigusr1`, `r->useresp = top - 72`. Mask sig 10.
8. iret → on_sigusr1 runs in ring 3. Prints "handler: caught signal in ring 3". Sets `g_got_sig = 10`.
9. Handler returns. ret pops trampoline_addr → trampoline runs.
10. Trampoline: `add $4, %esp; INT $0x80 SYS_SIGRETURN`.
11. Kernel SYS_SIGRETURN: read sigcontext at user_esp, `*r = *uctx`, unmask sig 10, early-return from dispatcher.
12. iret restores parent's pre-signal EIP/ESP/regs — back inside `sys_yield()`'s wrapper return path.
13. Parent's loop sees `g_got_sig == 10`, exits, prints "got_sig=10 (spun 2 times)".
14. Parent calls `sys_wait` — pid 15 is already a zombie, harvested immediately, exit=0.

`httpd.elf` keeps serving curl on :80 throughout. Confirmed:

```
$ curl -s http://localhost:8080/ -o /dev/null -w "status=%{http_code} bytes=%{size_download}\n"
status=200 bytes=317
```

## Design decisions

**Delivery happens at iret-to-ring-3, not on `kill` itself.** Standard Unix model. Means a kill from one task to another is async — the target sees it on its next syscall return / IRQ return. The alternative (synchronously hijacking the target's kernel stack from a different task's context) is much harder to make safe.

**Auto-mask the delivered signal during the handler.** Otherwise a SIGUSR1 handler that's slow could see the same signal re-enter and stack overflow. Real Unix masks the signal during delivery and restores from sigcontext on sigreturn.

**Sigcontext = `struct registers` verbatim.** 64 bytes of memcpy. The fields the iret cares about (EIP/CS/EFLAGS/ESP/SS) are at known offsets; the GP regs are saved-and-restored by the surrounding stub; DS gets restored by isr_common_stub's tail. Whole-struct memcpy is the simplest contract, and it's what sigreturn restores.

**Trampoline lives in libuser.** Linux did this through the early 90s before moving to a kernel-provided trampoline (because sa_restorer was painful for security). For an OS that ships its own libuser as the only way to talk to the kernel, the user-side trampoline is fine — and means the kernel doesn't need to map a special "vsyscall page" into every user PD.

**SYS_SIGRETURN early-returns from the dispatcher.** It just overwrote `r`; the dispatcher's tail must not write `r->eax` (would clobber the restored EAX) and must not run `signal_check_and_deliver` again (would deliver another signal at the top of every signal handler return — infinite recursion).

**Pending set is per-task, not per-thread.** We don't have threads. When we add them, we'll need per-thread pending and a "delivery target" rule (Linux: any thread that doesn't have it masked).

**Fork inherits handlers + mask, clears pending.** POSIX. The child shouldn't see signals queued for the parent at fork time.

**Exec resets caught handlers to SIG_DFL, leaves SIG_IGN.** POSIX. The handler code lived in the old address space; resurrecting it via the new ELF would jump into garbage.

**Default action is TERMINATE for everything except SIGCHLD.** POSIX has a richer table; we don't have STOP/CONT, so this is what we can do. SIGCHLD-default-IGNORE means a parent that doesn't `wait()` doesn't accumulate zombies *and* doesn't die on the SIGCHLD that exit raised. (Actually we don't yet raise SIGCHLD on exit — the wait machinery handles it via the BLOCKED_ON_CHILD/READY transition. This is a small departure from POSIX that we'll fix later.)

**Block-style signals only — no real-time queueing.** A second pending SIGUSR1 while one is already pending is silently dropped. POSIX RT signals would queue counts; classical signals (the ones we model) don't.

**No siginfo.** Handlers receive only the signal number — no `siginfo_t`, no sender pid, no sa_sigaction 3-arg form. Could be added by widening the handler argv on the user stack and giving the kernel a handler-flavor flag.

## Pitfalls

1. **The trampoline must be at a known user VA.** Forget to register it (or register a stale one after exec) and the kernel pushes a garbage return address; the handler returns into hyperspace.
2. **`SYS_SIGRETURN` must early-return from the dispatcher.** The standard tail's `r->eax = ret` would clobber the EAX we just restored from sigcontext.
3. **Auto-mask matters.** Without it, any handler slower than one PIT tick can re-enter itself indefinitely.
4. **Delivery must check `r->cs & 3 == 3`.** Otherwise a kernel-mode IRQ (e.g., PIT firing while a kernel task is hlt'ing) would try to push a delivery frame onto the kernel stack and iret with user CS — instant fault.
5. **Fork must inherit handlers, not pointers to handlers.** They're function-pointer values, so memcpy of the table is the right thing — but if we ever stored sigaction structs by reference (per-process), the child would share the parent's struct and `signal()` from the child would mutate the parent's table.
6. **Exec must clear `sig_tramp`.** The trampoline VA is in the old libuser; in the new ELF, libuser may sit at a different VA (technically it's the same in our build — both at 0x40000000-ish — but relying on that would be brittle).
7. **A blocked task doesn't see signals until it unblocks.** Documented limitation — kill doesn't wake. A `kill -9` to a task in `sys_wait` is essentially a no-op until the wait completes.
8. **The signal ABI uses `int $0x80` — same vector as syscalls.** The trampoline's `int $0x80` is just a normal syscall entry; sigreturn dispatch is regular case-handling. Means signal handlers can themselves make syscalls (each check for pending signals on return — recursion happens when a handler makes a syscall that has another signal pending; auto-mask of just the in-flight signal means a *different* signal can preempt the first handler).
9. **`kill(0, sig)` and process groups don't exist.** A handler on a process couldn't (yet) signal "everyone in my group" — the SIGINT-from-Ctrl-C use case would need that.
10. **CPU exceptions still kernel-panic.** A user-mode page fault prints "[!] CPU EXCEPTION 14" and halts the system instead of delivering SIGSEGV to the offending task. Easy to add (the exception handlers already see r->cs == 0x1B for ring-3 faults), but not done in this session.

## What might come next

The immediate next thing is making blocked syscalls signal-interruptible — kill should wake `sys_wait`, which should return -EINTR. After that, surfacing CPU exceptions as SIGSEGV/SIGFPE/SIGILL turns the kernel from "panic on ring-3 fault" into "deliver and let the user handle (or die normally)." Then `sigprocmask` for full mask control, `pause()` / `sigsuspend()` for sleep-until-signal, and SIGCHLD-on-child-exit so a parent can be event-driven instead of polling `wait`.
