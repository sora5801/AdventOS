# Session 4 — Round-robin scheduler + preemptive context switch

**Goal:** Multiple kernel threads time-sliced by the PIT.

## The shape of a context switch

Two design points fix everything else:

1. **Caller-save vs callee-save split.** A `task_switch(uint32_t *old_esp, uint32_t new_esp)` written in cdecl-callable assembly only needs to save callee-saves (EBX, ESI, EDI, EBP) plus EFLAGS. EAX/ECX/EDX are already the caller's responsibility per cdecl. Anyone calling `task_switch` from C land doesn't have to do anything special — GCC already saved what it cared about across the call.

2. **Synthetic stack frame for new tasks.** A brand-new task has no real saved context. We fabricate one so the very first switch to it just `pop`s and `ret`s into the entry function as if it had been switched out at the top of `task_switch`.

```
[high addr]
0           <- fake return addr; entry must not return
entry       <- task_switch's `ret` jumps here
EFLAGS=0x202   (reserved bit + IF=1)
EBP = 0
EBX = 0
ESI = 0
EDI = 0     <- task->esp points here
[low addr]
```

When the scheduler calls `task_switch(&prev->esp, next->esp)` for a brand-new task:

```
1. pushfl + cli + push ebp/ebx/esi/edi to PREV's stack
2. *prev->esp = current esp        (saved frame on prev's stack)
3. esp = next->esp                 (now on the new task's pre-built stack)
4. pop edi/esi/ebx/ebp             (gets 0,0,0,0)
5. popfl                           (gets 0x202 → IF=1)
6. ret                             (pops `entry` into EIP)
```

Step 6 is where execution leaves `task_switch` for the new task's entry function. The fake return-address-of-zero at the top means: if the entry function ever `ret`s, it pops 0 into EIP, which page-faults — a clean failure mode rather than executing garbage.

## The cli/pushfl atomicity

```asm
_task_switch:
    pushfl
    cli                         /* atomicity during the swap */
    push    %ebp
    push    %ebx
    push    %esi
    push    %edi
    movl    24(%esp), %eax      /* old_esp */
    movl    %esp, (%eax)
    movl    28(%esp), %esp      /* new_esp */
    pop     %edi
    pop     %esi
    pop     %ebx
    pop     %ebp
    popfl                       /* restores IF for incoming task */
    ret
```

`pushfl` saves the caller's IF state before `cli` clobbers it. The pushfl-then-cli order matters: if you `cli; pushfl` instead, you'd save IF=0 and lose the original.

`popfl` at the end restores the *new* task's saved IF — its 0x202 if brand-new, or whatever it had when it was last switched out.

## The scheduler

```c
void schedule(void) {
    if (!g_current) return;
    __asm__ volatile ("cli");

    struct task *next = g_current->next;
    while (next != g_current && next->state == TASK_STATE_DEAD)
        next = next->next;

    if (next != g_current) {
        struct task *prev = g_current;
        if (prev->state == TASK_STATE_RUNNING) prev->state = TASK_STATE_READY;
        next->state = TASK_STATE_RUNNING;
        g_current = next;
        task_switch(&prev->esp, next->esp);
    }
    __asm__ volatile ("sti");
}
```

The cli/sti around the body is what makes `g_current = next` and `task_switch` a single atomic step. Without it, an IRQ could fire between updating `g_current` and the actual stack swap and re-enter `schedule` with `g_current = next` while ESP is still the old task's. That would save garbage to `next->esp`.

The `sti` at the end runs after `task_switch` returns, which only happens when **this** task is switched back in — could be many timer ticks later. By that point `IF` was restored from `popfl` in the asm; the explicit `sti` is belt-and-suspenders for callers that came in with IF=1 and expect to leave with IF=1.

## The PIC-EOI bug

This was the real surprise of the session.

The original irq_handler:

```c
void irq_handler(struct registers *r) {
    int irq = (int)r->int_no - 32;
    if (irq_handlers[irq]) irq_handlers[irq](r);    // (1)
    pic_send_eoi(irq);                              // (2)
}
```

Plug `schedule()` into `pit_irq` and the timer handler **may not return**. When the scheduler picks a brand-new task, `task_switch`'s `ret` jumps to that task's entry function. Control never comes back to `pit_irq`, never to `irq_handler`. Step (2) is dead code from this iteration's perspective.

Effect: PIC's ISR (in-service register) bit for IRQ 0 stays latched. The PIC won't deliver any further IRQ 0s — or anything below it — until it's EOI'd. The system silently freezes (no more timer ticks → no more scheduling → first preempted task is also the last).

Fix: EOI **before** dispatching:

```c
void irq_handler(struct registers *r) {
    int irq = (int)r->int_no - 32;
    if (irq >= 0 && irq < 16) {
        pic_send_eoi(irq);
        if (irq_handlers[irq]) irq_handlers[irq](r);
    }
}
```

This is safe: by the time we send EOI, the IRQ stub has already pushed everything it needs to push. Even if a (theoretically) higher-priority IRQ arrives during the handler, the gate is an interrupt gate so IF=0 throughout — no nesting until iret.

## Demo tasks

Two trivial tasks emit identifying tags to serial only (not VGA, to avoid contention with the shell's cursor):

```c
static void demo_task_a(void) {
    for (;;) { serial_write("[A]"); pit_sleep(150); }
}
static void demo_task_b(void) {
    for (;;) { serial_write("[B]"); pit_sleep(250); }
}
```

`pit_sleep` is `while (ticks < target) hlt;`. The hlt waits for an interrupt — typically the next timer tick, which calls `schedule`, which preempts us out. When we eventually come back, the loop checks again. Effectively a cooperative yield with a hard floor on duration.

After running for ~10 seconds:
- A appears ~33 times, B ~19 times. Ratio ≈ 1.74 vs expected 250/150 ≈ 1.67. Close enough to confirm fair preemption.

## Tasks command

`tasks` shell command lists every live TCB. Required adding `-` flag (left-align) to `kprintf`'s `%s`:

```
 ID  STATE  NAME             ESP         SWITCHES
 0  RUN    kmain            0x0003deb0  61  <-- current
 1  READY  demo_a           0x00103f64  60
 2  READY  demo_b           0x00107f64  61
```

`switches_in` is incremented in `schedule()` every time a task gets selected. Used here as a sanity check: 61/60/61 means round-robin is fair — no task is being skipped.

The `0x00103f64` ESP for demo_a is `0x100000 + 0x4000 - 28` — the heap base, plus one 16 KiB stack (= 0x4000), minus the seven 4-byte saved entries from the synthetic frame. The math checks out.

## kmain becomes task 0

```c
void task_init(void) {
    g_tasks[0].id    = 0;
    g_tasks[0].state = TASK_STATE_RUNNING;
    g_tasks[0].next  = &g_tasks[0];        // self-loop initially
    strncpy(g_tasks[0].name, "kmain", ...);
    g_current = &g_tasks[0];
}
```

kmain's register state is already live when `task_init` runs — we don't synthesize a frame for it. Its `esp` field is left zero and gets populated on the first preemption (when `task_switch` saves callee-saves to its stack and writes `g_current->esp = current_esp`).

`task_create` splices new tasks into the ring after `g_current`:

```c
__asm__ volatile ("cli");
t->next = g_current->next;
g_current->next = t;
__asm__ volatile ("sti");
```

cli is required because `schedule` walks the ring and we don't want it to see a half-spliced state.

## Files added

| File | Role |
|---|---|
| `kernel/task.{c,h}` | TCB, `task_create`, `schedule`, `task_yield`, round-robin ring |
| `kernel/task_switch.S` | Callee-saves swap + ESP swap + iret-back |
| `kernel/pit.c` | `pit_irq` now ends with `schedule()` |
| `kernel/isr.c` | EOI moved before handler dispatch |
| `kernel/kernel.c` | Spawns demo_a and demo_b at boot |
| `kernel/shell.c` | `tasks` and `yield` commands |
| `kernel/kprintf.c` | `%-Ns` left-align width support |

## Design decisions

**Co-routine-style switch over hardware task switching.** x86 has a "TSS-based hardware task switch" instruction (`jmp` to a TSS selector) that auto-saves/restores all registers. Nobody uses it — it's slow, inflexible, and doesn't compose with everything else. Software context switch via stack swap is universal practice.

**Per-task kernel stack of 16 KiB.** Generous for kernel code. Could be 4-8 KiB. Larger ones avoid stack overflows during deep ISR + handler chains.

**Round-robin via circular linked list.** Simplest possible scheduler. No priorities, no time accounting, no dynamic adjustment. Preemption is "every PIT tick = 10 ms quantum". Fits in 30 lines of code, demonstrably fair.

**`pit_sleep` as `while < target: hlt`.** Doesn't require a sorted timer wheel or sleep queue. Doesn't really cooperate with the scheduler — just lets timer IRQs reach the scheduler. Each task ends up checking its own deadline. O(N) wakeups per tick if N tasks are sleeping, but for our workloads N is tiny.

**No `task_exit` yet.** A kernel task that "returns" hits the synthetic frame's fake-zero return address and page-faults. We rely on this as a "kernel task that exits is a bug" enforcement. Real exit (DEAD state + reaper) shows up in session 7.

## Deferred

- Sleeping on conditions (mutex wait queue → session 6)
- Reaping DEAD tasks (session 7)
- Priorities / fairness beyond strict round-robin (never)
- SMP (never — would need per-CPU current pointer + real spinlocks)

## Pitfalls

1. **EOI before any handler that might not return.** This is the lesson — easy to miss because non-switching IRQ handlers work fine with the natural-looking "do work, then EOI" order.
2. **`pushfl` before `cli`.** Reverse order saves IF=0 and loses the caller's state.
3. **Atomicity around `g_current = next` + `task_switch`.** Must be cli'd or you'll save state to the wrong TCB.
4. **The fake-zero return address** is a feature, not waste. Page-faulting on accidental task return is much better than executing whatever was at address 0.
5. **`task_switch`'s arg offsets** (24(%esp) and 28(%esp)) depend on the exact number of pushes. Adjusting the prologue means adjusting these.
