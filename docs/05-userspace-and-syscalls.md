# Session 5 — TSS, ring 3, syscalls, per-process page directories

**Goal:** Run a real ring-3 program that talks to the kernel via `int 0x80`.

This is the biggest single jump in the whole project. Five things have to compose:

1. The CPU needs a TSS to know where to put the kernel stack on a ring-3 → ring-0 transition.
2. The IDT needs a syscall gate that ring-3 is allowed to invoke.
3. The page directory needs a USER bit on the right entries.
4. The user task needs a way to enter ring 3 the first time (it can't just `iret` from random code).
5. The scheduler needs to swap CR3 and TSS.ESP0 every time it touches a user task.

Get any one of these wrong and the box double-faults. We got two of them wrong.

## TSS in the GDT

The TSS struct (104 bytes) lives in BSS. Most of its fields are useless to us — they exist for hardware task switching, which nobody uses. We only care about:

```c
g_tss.ss0  = 0x10;        // kernel data selector
g_tss.esp0 = 0;           // updated per-task before resuming a user task
g_tss.iomap_base = sizeof(g_tss);   // disable I/O permission bitmap
```

GDT entry 5 (selector 0x28) describes it. Access byte 0x89 = present + DPL=0 + S=0 (system) + type 0x9 (available 32-bit TSS). After loading the GDT, `ltr $0x28` makes the CPU start using it.

```c
__asm__ volatile ("ltr %%ax" :: "a"(0x28));
```

From this point on, every `int $0x80` from ring 3 will:
1. Read TSS.SS0 and TSS.ESP0
2. Switch to that stack
3. Push old SS, ESP, EFLAGS, CS, EIP (5 dwords because of privilege change)
4. Jump to the IDT-handler code

## INT 0x80 with DPL=3

```c
idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);
```

Access byte 0xEE = 0b11101110:
- Bit 7 P = 1 (present)
- Bits 5-6 DPL = 11 (3 — ring 3 may invoke)
- Bit 4 S = 0 (system, ignored for gates)
- Bits 0-3 type = 0xE (32-bit interrupt gate)

This was the deliberate choice: interrupt gate clears IF on entry. Trap gate (0xF) would keep IF as the caller had it. We picked interrupt-gate-and-explicit-sti-in-the-handler because it makes the initial save/setup race-free even if a buggy user handler somehow forgot to handle interruptibility. (This bites us in session 7 — see below for the foreshadowing.)

## The syscall stub

Reuses the common ISR stub. `isr128` just pushes int_no=128 and jumps to common dispatch. The common stub already pushes a `struct registers *` for the C handler. The C handler reads syscall args from `r->eax/ebx/ecx/edx` and writes the return into `r->eax`. `iret` at the end of common stub propagates `r->eax` back to user as EAX.

```c
void syscall_dispatch(struct registers *r) {
    uint32_t num = r->eax, a = r->ebx, b = r->ecx, c = r->edx;
    int32_t ret = -1;

    switch (num) {
        case SYS_WRITE:     kputc((char)a); ret = 0; break;
        case SYS_GETPID:    ret = (int32_t)task_current()->id; break;
        case SYS_EXIT:      task_current()->state = TASK_STATE_DEAD;
                            schedule();
                            for (;;) hlt;
        case SYS_YIELD:     schedule(); ret = 0; break;
        case SYS_WRITE_STR: const char *p = (const char *)a;
                            for (int i = 0; i < 256 && p[i]; i++) kputc(p[i]);
                            ret = 0; break;
    }
    r->eax = (uint32_t)ret;
}
```

Note `SYS_WRITE_STR` reads a user pointer directly. We're still on the user task's CR3 (the kernel mappings are mirrored in every user PD), so `(const char *)a` dereferences correctly. A real OS would `copy_from_user` and validate the range. We don't, on the principle that the only user code we run is code we built ourselves.

## Per-process page directory

```
master kernel PD                           user PD (per process)
┌───────────┐                             ┌───────────┐
│ PDE 0..7  │  → kernel page tables  ←──── │ PDE 0..7  │   (same physical PT addresses)
│ PDE 8     │   (unused)                  │ PDE 8     │
│  ...      │                             │  ...      │
│ PDE 256   │   (unused)                  │ PDE 256   │ → user PT → user code page
│ PDE 257   │   (unused)                  │ PDE 257   │ → user PT → user stack page
└───────────┘                             └───────────┘
```

`paging_create_user_pd` allocates a new PD, zeros it, copies kernel PDEs 0..7 from the master. Those PDEs reference *shared* page tables — every user PD sees identical kernel mappings, and any future kernel page-table edit propagates everywhere automatically.

User mappings get added with `paging_map_in(pd, virt, phys, PTE_USER | PTE_WRITABLE)`. The internal `do_map_pd` propagates the USER bit:

```c
pd[pd_i] = (uint32_t)(uintptr_t)pt
        | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
```

Both the PDE and the PTE need USER for ring 3 to access the page. Forgetting to propagate is a classic "GP fault on first user instruction" bug.

## The user_entry_stub bug

The kernel-side trampoline that takes a user task into ring 3:

```c
static void user_entry_stub(void) {
    struct task *t = g_current;
    tss_set_kernel_stack(t->kernel_stack_top);
    __asm__ volatile (
        "mov $0x23, %%ax     \n\t"
        "mov %%ax, %%ds      \n\t"
        "mov %%ax, %%es      \n\t"
        "mov %%ax, %%fs      \n\t"
        "mov %%ax, %%gs      \n\t"
        "push $0x23          \n\t"   /* SS                */
        "push %0             \n\t"   /* user ESP          */
        "push $0x202         \n\t"   /* EFLAGS            */
        "push $0x1B          \n\t"   /* CS                */
        "push %1             \n\t"   /* user EIP          */
        "iret                \n\t"
        :
        : "r"(t->user_esp), "r"(t->user_eip)        // ← BUG
        : "memory"
    );
}
```

`"r"(t->user_eip)` lets GCC pick **any** general register for the input. If it picked `eax`, then the `mov $0x23, %ax` at the top of the asm clobbers the operand. `push %1` then pushes 0x23 instead of the entry point. `iret` jumps to address 0x23 in ring 3.

The bug didn't trip immediately. It surfaced *much* later in session 7, when adding more code shifted GCC's register allocation. The page fault was at CS=0x1b, EIP=0, user mode — the "iret jumped to garbage" signature.

Fix:

```c
        : "S"(t->user_esp), "D"(t->user_eip)
        : "eax", "memory"
```

Pin the operands to ESI and EDI specifically, and clobber EAX so GCC can't pick it. This is the kind of thing a clobbers list would have caught from the start; "memory" alone wasn't enough.

## Section name truncation, take 2

Wrote the user program as `__attribute__((section(".user_code"), naked))`. Linker reported "section below image base" and `.user_co` showed up in `objdump -h`. PE/COFF section names are 8 chars max; `.user_code` got chopped to `.user_co`. `objcopy -j .user_code` then matched nothing.

Renamed to `.usrcode` (8 chars exactly). Worked. Session 7 splits this into `.up1` and `.up2` for two programs.

## The user program is its own world

The user program is a **naked** function with **position-independent inline asm only**:

```c
__attribute__((section(".usrcode"), naked))
void user_program(void) {
    __asm__ volatile (
        "    call 1f                          \n\t"
        "1:  pop  %ebx                        \n\t"
        "    add  $(msg1 - 1b), %ebx          \n\t"
        "    mov  $5, %eax                    \n\t"   // SYS_WRITE_STR
        "    int  $0x80                       \n\t"
        ...
        "msg1: .ascii \"Hello from ring 3! \\0\" \n\t"
    );
}
```

`call 1f; 1: pop %ebx` is the canonical PIC trick on x86. `call` pushes the return address (= the byte after the `call` itself, which is exactly where label `1` is); we pop it and now `%ebx` holds the address of `1:`. Adding the assembler-computed constant `(msg1 - 1b)` produces the runtime address of `msg1` regardless of where the function was loaded.

Why this matters: the kernel allocates a fresh user page, *copies* the bytes of `.usrcode` into it, and maps that page at virtual 0x40000000. The label `msg1` was assembled with a specific physical address (whatever the linker placed `.usrcode` at), but the runtime address after copy+remap is completely different. PIC sidesteps this by computing addresses from current EIP.

Constraints on user code:
- `naked` — no compiler-generated prologue/epilogue
- No external function calls (no relocations to fix up)
- No globals (would be referenced absolutely)
- All strings via the call/pop trick

Anything more elaborate would need a real loader with relocation processing — which is what session 8 is for.

## Spawn flow

```c
uint32_t *user_pd = paging_create_user_pd();
void *code_page  = pmm_alloc_page();
void *stack_page = pmm_alloc_page();
memcpy(code_page, user_code_start, user_code_end - user_code_start);
paging_map_in(user_pd, 0x40000000, (uintptr_t)code_page,  PTE_WRITABLE | PTE_USER);
paging_map_in(user_pd, 0x40100000, (uintptr_t)stack_page, PTE_WRITABLE | PTE_USER);
struct task *t = task_create_user(0x40000000, 0x40101000, (uint32_t)user_pd, "userprog");
```

`task_create_user` is just `task_create` with the entry function set to `user_entry_stub` and the user-mode fields populated.

## Scheduler additions

```c
if (next->kernel_stack_top) {
    tss_set_kernel_stack(next->kernel_stack_top);
}
if (next->cr3 && next->cr3 != prev->cr3) {
    write_cr3(next->cr3);
}
task_switch(&prev->esp, next->esp);
```

TSS.ESP0 must reflect the **next** task's kernel stack so any subsequent ring-3 → ring-0 transition lands on the correct per-task kernel stack. Without this, a syscall from user task B with TSS.ESP0 still pointing at user task A's kernel stack would clobber A's saved state.

CR3 only changes when we cross address spaces. Kernel-only tasks share the master CR3; user tasks each have their own. The kernel mappings in every user PD are identical, so any kernel code (including the rest of `task_switch_asm` after the CR3 write, and the entire IRQ handler chain we just unwound from) keeps working.

## What the test showed

```
spawned ring-3 task pid=3  cr3=0x0000b000  entry=0x40000000  esp=0x40101000
user code: 192 bytes copied from kernel section to phys 0x0000c000
Hello from ring 3! (pid=3)
...woke back up after yield. exiting.
[user task pid=3 exited code=0]
```

Each line proves a different layer:
- "spawned" with cr3=0xb000 — per-process PD allocation worked.
- "Hello from ring 3!" — code mapped at 0x40000000, ring-3 CS, INT 0x80 → SYS_WRITE_STR worked.
- "(pid=3)" — SYS_GETPID returned the right value.
- "...woke back up" — SYS_YIELD switched out and back.
- "[user task pid=3 exited code=0]" — SYS_EXIT marked the task DEAD.

`tasks` afterward shows the task as DEAD with switches=2. The slot stays DEAD until session 7's reaper.

The whole transition runs concurrently with kernel demo tasks `[A]/[B]` which keep emitting throughout — proves CR3 swaps and TSS.ESP0 updates don't break kernel-mode preemption.

## Files added

| File | Role |
|---|---|
| `kernel/tss.{c,h}` | TSS struct + `tss_set_kernel_stack` |
| `kernel/syscall.{c,h}` | INT 0x80 dispatcher |
| `kernel/user_program.c` | Bundled ring-3 program (`.usrcode` section) |
| `kernel/isr_stubs.S` | New `_isr128` entry |
| `kernel/idt.c` | IDT[0x80] = 0xEE gate |
| `kernel/gdt.c` | New TSS descriptor + `ltr` |
| `kernel/task.{c,h}` | Per-task `cr3`, `user_eip`, `user_esp`, `kernel_stack_top`, `is_user`; `task_create_user`; `user_entry_stub` |
| `kernel/paging.{c,h}` | `paging_create_user_pd`, `paging_map_in`, `do_map_pd` with USER propagation |
| `kernel/shell.c` | `userprog` command |
| `linker_kernel.ld`, `build.sh`, `Makefile` | `.usrcode` section + objcopy add |

## Design decisions

**Identity-mapped low memory in every user PD.** Means user pointers and kernel pointers live in the same number space. In a real OS user is at 0x00000000–0xC0000000 and kernel is higher half. We chose lower-half kernel + identity user-VA for code/stack at 0x40000000 because the alternative (carve out a real virtual address space) is a much bigger surgery.

**Single global syscall handler.** Switch statement, no table-driven dispatch. Adding a new syscall means one case clause. At dozens of syscalls a function-pointer table starts to make sense; at five it doesn't.

**Naked function with PIC inline asm for user code.** The cheapest way to ship machine code that'll run after copy-and-remap. No relocations, no load-time fixups. Constraints (no globals, no calls, no strings outside inline asm) are real but tolerable for short programs. Session 8 lifts these by introducing a real ELF loader.

**No fork/exec.** A single `task_create_user(entry, stack, cr3, name)` is the only way to make a user task. Spawning is "build a fresh PD, map fresh pages, give it a TCB".

## Deferred

- Resource cleanup on `SYS_EXIT` (session 7 reaper)
- Multiple user programs (session 7)
- ELF parsing — currently we just `memcpy` raw bytes (session 8)
- USER pointer validation (never)
- Fork/exec (never)

## Pitfalls

1. **GCC register allocation in inline asm with `"r"` constraints.** If the asm clobbers a specific register before consuming the operand, pin it with a specific constraint (`"S"`, `"D"`, `"a"` etc.) or list the register in clobbers. `"memory"` clobber doesn't cover this.
2. **PE section names ≤ 8 chars.** Recurring lesson; bites again in session 7.
3. **Both PDE.U and PTE.U must be set** for ring-3 access. The internal `do_map_pd` ORs USER into the PDE if any PTE under it has USER. Forget this and you get GP-fault-on-first-instruction.
4. **TSS.ESP0 must be updated on every switch into a user task.** A stale value clobbers the previous user task's kernel stack on the next ring-3 → ring-0 transition.
5. **CR3 stays the same as long as you stay in kernel-only tasks.** Don't reload it gratuitously — TLB flush is expensive.
6. **The `iret` from `user_entry_stub` is when ring 3 begins.** Before that, the task is in ring 0 on its kernel stack. The kernel-mode code before the iret runs with the user PD already loaded — that's why kernel mappings have to be in every user PD before the very first iret.
