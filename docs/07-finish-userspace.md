# Session 7 — Finishing the userspace path

**Goal:** Make user processes first-class. Resources clean up on exit. Multiple programs. Concurrent processes.

After session 5, every successful `userprog` invocation leaked: kernel stack (kmalloc), user PD (PMM page), user code page, user stack page, the TCB slot itself. Run `userprog` ten times and the box quietly ate ~100 KB of RAM and ate 5 of the 16 task slots. The path was technically working but operationally broken.

## What needed to become real

Five concrete pieces:

1. **`paging_destroy_user_pd`** — walk a user PD, free user PTs and the pages they map, free the PD itself. Skip kernel PDEs (shared).
2. **A reaper task** — kernel thread that periodically scans for DEAD tasks and frees their resources, so SYS_EXIT can return without us standing on the stack we want to free.
3. **PMM spinlock** — the last unprotected allocator. Concurrent user spawns + reaper free races otherwise.
4. **More syscalls** — `SYS_SLEEP_MS` so user code can do real timing-driven work, `SYS_TIME` so it can read the wall clock.
5. **A second user program** — to demonstrate "multiple programs, multiple concurrent processes". And to expose the latent bugs we hadn't tripped yet.

## paging_destroy_user_pd

Three layers to free, in dependency order:

```c
void paging_destroy_user_pd(uint32_t *pd) {
    if (!pd) return;
    /* Skip PDEs 0..7 — those reference page tables that are shared
     * with the kernel master PD. Free everything from PDE 8 upward. */
    for (uint32_t i = 8; i < 1024; i++) {
        if (!(pd[i] & PTE_PRESENT)) continue;
        uint32_t *pt = (uint32_t *)(uintptr_t)(pd[i] & PAGE_MASK);
        for (uint32_t j = 0; j < 1024; j++) {
            if (pt[j] & PTE_PRESENT) {
                pmm_free_page((void *)(uintptr_t)(pt[j] & PAGE_MASK));
            }
        }
        pmm_free_page(pt);
        pd[i] = 0;
    }
    pmm_free_page(pd);
}
```

The PDE 0..7 skip is the only subtlety. Kernel PDEs were copied from the master PD by `paging_create_user_pd` — they reference page tables we *share*, not own. Freeing them would tear down the kernel mapping for every other process.

This function runs from kernel-mode kernel context (the reaper). The user PD's physical pages are identity-mapped in the master PD, so we can dereference them as virtual pointers via the kernel's identity map.

## The reaper

The constraint: a task can't free its own kernel stack. Its registers and call frame live there. The same goes for the user PD it might still be loaded in (CR3 register holds the physical address even after we've conceptually moved on).

Solution: a dedicated kernel thread that's never the target of cleanup, doing the cleanup of others.

```c
static void task_reaper(void) {
    for (;;) {
        pit_sleep(200);
        for (int i = 1; i < TASK_MAX; i++) {
            struct task *t = &g_tasks[i];
            if (t == g_current)              continue;     /* never reap self  */
            if (t->state != TASK_STATE_DEAD) continue;
            if (t->stack_base == NULL)       continue;     /* already reaped   */

            /* Splice out of round-robin under cli */
            __asm__ volatile ("cli");
            struct task *p = t->next;
            int safety = TASK_MAX * 2;
            while (p && p != t && p->next != t && safety--) p = p->next;
            if (p && p->next == t) p->next = t->next;
            __asm__ volatile ("sti");

            /* Snapshot, then free the heavy stuff outside cli */
            uint32_t reaped_id   = t->id;
            int      reaped_user = t->is_user;
            uint32_t reaped_cr3  = t->cr3;
            void    *reaped_stk  = t->stack_base;

            /* Mark slot UNUSED first; subsequent task_create can reuse it */
            t->state = TASK_STATE_UNUSED;
            t->stack_base = NULL;
            t->cr3 = 0;
            ...

            if (reaped_user && reaped_cr3 && reaped_cr3 != g_kernel_cr3)
                paging_destroy_user_pd((uint32_t *)reaped_cr3);
            kfree(reaped_stk);
        }
    }
}
```

The cli/sti window is just for the list mutation. Outside that, the heavy frees take their own (kmalloc, PMM) spinlocks — those allow other tasks to run.

The "snapshot, then free outside cli" pattern matters: cli sections should be as short as possible. PMM free walks the bitmap; kmalloc free does coalesce traversal. Both are fine to interrupt.

By the time the reaper runs, the dead task has already called SYS_EXIT → schedule, so we've switched off its stack. CR3 has been swapped to the new task's. The dead task's resources are unreferenced.

## The SYS_SLEEP_MS deadlock

User_program_2 prints a counter with `SYS_SLEEP_MS(200)` between digits. Tested it. Got "Counter: 0" and a hang.

Looked at qemu's interrupt log: timer IRQs were firing into the kernel just fine. The user task wasn't being preempted because it was still inside the syscall handler.

`pit_sleep` does:

```c
while (ticks < target_ticks) __asm__ volatile ("hlt");
```

`hlt` halts the CPU until the next interrupt. With IF=0, no interrupts can fire — the CPU halts forever. The IDT gate for INT 0x80 is type 0xEE, which is an **interrupt gate**. Interrupt gates clear IF on entry. So during `syscall_dispatch`, IF=0. `pit_sleep`'s hlt waits for an interrupt that never comes. Lockup.

Fix:

```c
void syscall_dispatch(struct registers *r) {
    /* The 0xEE IDT gate cleared IF. Several syscalls below
     * (SYS_SLEEP_MS, SYS_YIELD, SYS_EXIT->schedule, long SYS_WRITE_STR)
     * need a live timer to make progress. iret restores user IF
     * from the saved EFLAGS regardless. */
    __asm__ volatile ("sti");
    /* ... dispatch ... */
}
```

Re-enabling interrupts inside the syscall handler is fine — the iret at the tail of the common ISR stub restores EFLAGS from the saved frame, which has the user's pre-syscall IF=1. So whatever we do in the handler doesn't affect the user's post-iret state.

Alternatives we considered:
- Trap gate (0xEF) instead of interrupt gate — keeps IF as caller had it. Would work, but the `cli` baked into every ISR stub still kicks in at entry. Would need either a separate stub for syscalls or removal of the cli for vector 128 specifically.
- Per-syscall sti — `case SYS_SLEEP_MS: sti; pit_sleep(...)`. Works but easy to forget on new syscalls.

A blanket `sti` at the top of dispatch is the most foolproof.

## The user_entry_stub register-allocation bug

This was a **latent** bug from session 5 that surfaced here.

Adding several files of new code shifted GCC's register allocation choices. In the new layout, `"r"(t->user_eip)` ended up in `%eax`. The asm that runs before `push %1` clobbered eax with `mov $0x23, %ax` (setting up user data segment). `iret`'s saved EIP was 0x23. Ring-3 page fault at EIP=0x00000000 (since 0x23 wraps to a tiny address that's inside the unmapped null page).

Diagnostic from the page fault handler:

```
[!] CPU EXCEPTION 14: Page fault (err=0x4) at 1b:0  eflags=0x10202
    fault addr (CR2) = 0x00000000
    cause = page not present, read, user mode
```

`cs:eip = 0x1b:0x0` is the smoking gun: ring 3 (RPL=3 selector) trying to fetch instruction from address 0.

Fix: explicit register pinning + clobber list:

```c
__asm__ volatile (
    /* asm body */
    :
    : "S"(t->user_esp), "D"(t->user_eip)        /* esi, edi */
    : "eax", "memory"                            /* clobbers */
);
```

The general lesson: `"r"` constraints with asm that mutates specific registers is almost always wrong. Either pin operands to non-conflicting registers or clobber the registers you'll mutate. We didn't catch this in session 5 because the compiler happened to pick safe registers; one more file of code shifted that.

## PMM spinlock

Three writers now: PMM allocations from boot/heap/page-tables, the reaper freeing user pages, concurrent user spawn calling `pmm_alloc_page` and `pmm_alloc_contiguous`. The session-3 PMM had no locking. `kmalloc` already had a spinlock (session 6). Time for PMM:

```c
void *pmm_alloc_page(void) {
    spin_lock(&g_pmm_lock);
    /* ... bitmap walk ... */
    spin_unlock(&g_pmm_lock);
    return ...;
}
```

Same for `pmm_alloc_contiguous`, `pmm_free_page`, `pmm_free_contiguous`. Only `pmm_init` is unlocked; it's called once at boot, single-task.

## Two programs in two sections

Session-5 `.usrcode` becomes session-7 `.up1` and `.up2`. PE/COFF section names ≤ 8 chars; both fit. Linker:

```
.up1 : ALIGN(0x1000) {
    _up1_start = .;
    *(.up1*)
    _up1_end = .;
}
.up2 : ALIGN(0x1000) {
    _up2_start = .;
    *(.up2*)
    _up2_end = .;
}
```

`objcopy -j .up1 -j .up2` includes both in the kernel binary.

`user_program_1` is the existing Hello-from-ring-3 demo. `user_program_2` is new — counts 0..4 with `SYS_SLEEP_MS(200)` between digits, then calls `SYS_TIME` and prints the 10-digit epoch as decimal. Same naked-PIC-asm style.

The decimal-print uses a div-by-10 loop into the user stack:

```asm
mov  $10, %ecx
6:  xor  %edx, %edx
    mov  $10, %edi
    mov  %esi, %eax
    div  %edi              ; eax /= 10, edx = remainder
    mov  %eax, %esi
    add  $0x30, %edx
    push %edx
    loop 6b

    mov  $10, %ecx
7:  pop  %ebx
    mov  $1, %eax          ; SYS_WRITE
    int  $0x80
    loop 7b
```

10 div-by-10s, push remainders as ASCII digits onto the user stack (low-to-high = digits high-to-low, since stack grows down), then pop and write 10 times. Result: digits in correct human order.

Shell gets `cmd_userprog2`. The original `cmd_userprog` is refactored into `spawn_user_task(src, len, name)` so both can share the load-and-spawn boilerplate.

## What "concurrent user processes" looks like in the trace

```
spawned userprog1 pid=4  cr3=0x0000b000  code=192 bytes @ phys 0x0000c000
Hello from ring 3! (pid=4)
[user task pid=4 exited code=0]
[reaper] freed pid=4 (user task), slot 4 now UNUSED

spawned userprog2 pid=5  cr3=0x0000b000  code=176 bytes @ phys 0x0000c000   ← same cr3
Counter: 01234 (epoch=1778269160)
[user task pid=5 exited code=0]
[reaper] freed pid=5 (user task), slot 4 now UNUSED                          ← same slot
```

After pid=4 was reaped, its user PD page (0xb000) and code page (0xc000) went back to the PMM bitmap. The next allocation (pid=5) drew the same physical addresses because `pmm_alloc_page` returns the lowest free page and those were the lowest free pages. Same with the slot — TCB array index 4 became UNUSED, so the next `task_create` reused it.

Then a concurrency test, two `userprog` invocations in quick succession:

```
spawned userprog1 pid=6  cr3=0x0000b000  code=192 bytes @ phys 0x0000c000
spawned userprog1 pid=7  cr3=0x0000f000  code=192 bytes @ phys 0x00043000   ← different
```

pid=6 spawns first, takes 0xb000 / 0xc000. pid=7 spawns before pid=6 has exited or been reaped. PMM hands out fresh pages: 0xf000 for the PD, 0x43000 for the code. Two concurrent user tasks with completely independent address spaces.

## Files added

| File | Role |
|---|---|
| `kernel/paging.{c,h}` | `paging_destroy_user_pd` |
| `kernel/pmm.c` | spinlock around alloc/free |
| `kernel/syscall.{c,h}` | `SYS_SLEEP_MS`, `SYS_TIME` + `sti` at dispatch entry |
| `kernel/task.{c,h}` | `task_reaper` + `task_reaper_start` |
| `kernel/user_program.c` | `user_program_1` (.up1) + `user_program_2` (.up2) |
| `linker_kernel.ld`, `build.sh`, `Makefile` | `.usrcode` → `.up1`/`.up2` |
| `kernel/shell.c` | `cmd_userprog2`, refactored `spawn_user_task` |
| `kernel/kernel.c` | `task_reaper_start()` at boot |

`task.c` import the additional headers (`paging.h`, `pit.h`, `kmalloc.h`, `kprintf.h`).

## Design decisions

**Reaper as a separate kernel thread.** The alternative is reaping inline in `schedule()` — every time we switch away from a task, check if it's DEAD and clean up. Tighter coupling, but ugly: schedule is the hottest path and shouldn't take spinlocks (kmalloc/pmm). A separate thread that sleeps 200 ms + processes batch is decoupled and easy to reason about.

**`sti` at top of `syscall_dispatch` rather than per-syscall.** Foolproof. The cost (a syscall handler can be preempted mid-handle) is fine — our syscalls are safe under preemption.

**Direct ownership transfer in mutex unlock** (carry-over from session 6). This was important even before the reaper; with the reaper, it'd be fatal to break it because reaped tasks could be left holding mutexes.

**Slot reuse via UNUSED state.** `task_create`'s search for free slots checks `state == UNUSED || state == DEAD`. After reap, the slot is UNUSED and instantly available. We never compact the array.

## Deferred

- Sigals, signal handlers (never)
- exec / fork (never)
- Real ELF loading — currently we just memcpy raw bytes (next session)
- Filesystem (next session)
- User memory allocator / sbrk equivalent (never)
- Per-process resource limits (never)

## Pitfalls

1. **A task can't free its own kernel stack.** Reap from a different context.
2. **Don't free shared resources.** Kernel PDEs in user PDs reference shared kernel page tables; only the user-only PDEs (8+) are owned by the user PD.
3. **Interrupt gates clear IF.** Any syscall handler that wants to wait for an interrupt (`hlt`, `pit_sleep`, anything that calls `schedule`) must `sti`.
4. **Latent register-allocation bugs in inline asm** can hide for sessions before surfacing. Pin operands or clobber registers you mutate.
5. **PMM lock matters as soon as more than one writer exists.** Reaper + concurrent user spawns is the trigger.
6. **The list-splice in the reaper must be cli'd**, but the kfree/PMM frees should be outside the cli section so they can take their own locks without deadlock concerns.
