# Session 14 — fork, exec, wait, and a real userspace shell

**Goal:** Add the three Unix process primitives that turn AdventOS from a "spawn ELFs from kernel" toy into something where userspace really runs userspace. Concretely:

- `fork()` — deep-copies the calling task's user PD (each PDE → its PT → every present page), inherits the fd table, snapshots the parent's stack, returns 0 in the child and the new pid in the parent.
- `exec()` — loads an ELF over the calling process's address space, swaps CR3, frees the old PD, resets argv on a fresh user stack.
- `wait()` — blocks the parent in a new `BLOCKED_ON_CHILD` state until any child exits, then harvests its exit code.

Then rewrite `user/sh.c` so it `fork()`s + `exec()`s + `wait()`s for every external command — retiring `SYS_KCMD` (the previous "shell forwards line to kernel parser" escape hatch).

End state — boot output of a self-test that runs three scenarios:

```
=== sh selftest: fork / exec / wait ===
[t1] forktest:
  child : pid=6  marker=0xbabe  (was 0xCAFE in parent)
[user task pid=6 exited code=42]
  parent: pid=5  marker=0xcafe  child_pid=6  reaped=6  exit=42
[t2] fork + exec hello.elf:
Hello from C in ring 3! This program uses libuser's printf.
  pid     : 7
  hello   : yes, I am a real C program
  hex pid : 0x7
yielding...
...woke back up. exiting.
[user task pid=7 exited code=0]
  parent waited: pid=7  exit=0
[t3] nested fork:
  child1 pid=8, about to fork()
  child2 pid=9 (parent=0)
[user task pid=9 exited code=7]
  child1 reaped grandchild, exit=7
[user task pid=8 exited code=11]
  parent reaped child1, exit=11
=== selftest done ===
```

That `0xCAFE`→`0xBABE` divergence is the visible proof of address-space isolation: the child writes to a stack slot at the same VA as the parent's, and the parent never sees it. `pid=8` forking to `pid=9` proves a fork-of-a-fork works. `httpd.elf` (still serving curl on :80) keeps running through all of it.

## What's in scope

In:
- `paging_clone_user_pd()` — deep PD copy
- `task_fork()` + `fork_child_return` asm trampoline
- `task_exec_inplace()` — kill old PD, build new one, rewrite syscall return frame
- `task_exit_current()` + `task_wait_current()` + `TASK_STATE_ZOMBIE` / `BLOCKED_ON_CHILD`
- `parent_id` / `exit_code` fields on `struct task`
- 3 new syscalls: `SYS_FORK = 18`, `SYS_EXEC = 19`, `SYS_WAIT = 20`
- libuser wrappers `sys_fork` / `sys_exec` / `sys_wait`
- Rewritten `user/sh.c` with `selftest` builtin and fork+exec+wait dispatch
- Variadic `LAUNCH(path, argv...)` macro in kmain so we can pass `"selftest"` to sh

Out:
- Copy-on-write — every fork is an honest deep copy
- `vfork` / `posix_spawn`
- `wait4` / `waitpid` with options
- Process groups, sessions, signals
- Reparenting orphans to init (we just go to DEAD)
- `setpgid`, controlling terminals
- `clone()`-style finer control
- `execve` taking envp

## Architecture: fork is "return twice"

Every other syscall in this kernel returns once: control comes back from `int $0x80` with the return value in EAX, life continues. fork is structurally weird because it has to manufacture a *second* control flow that emerges out of the same `int $0x80` instruction — running on a different page table, in a different scheduling slot, with a different return value.

We split that across three pieces:

1. **A deep clone of the user PD** so the child sees the same memory at the same virtual addresses but writes don't leak between them.
2. **A synthesized child kernel stack** laid out so the very first time the child is scheduled, control lands at a small trampoline that pops a fake iret frame and returns to ring 3 at the parent's saved EIP.
3. **An EAX value of 0 baked into the synthesized popa frame** so the iret restores EAX=0 — making the child see `sys_fork()` return 0 while the parent sees the child pid.

The synthesized frame is byte-identical to what the parent's `int $0x80` produced, with one number changed.

## paging_clone_user_pd

The PD copy is the easy half:

```c
uint32_t *paging_clone_user_pd(uint32_t *parent) {
    uint32_t *child = pmm_alloc_page();
    memset(child, 0, PAGE_SIZE);

    /* Kernel PDEs (0..7, the 32 MiB identity-map) are mirrored by
     * REFERENCE — both PDs point at the same kernel page tables.
     * Kernel mappings stay shared so the kernel stack is reachable
     * across CR3 swaps. */
    for (int i = 0; i < 8; i++) child[i] = parent[i];

    /* User PDEs (8..1023) are mirrored by COPY — new PT, new pages. */
    for (uint32_t i = 8; i < 1024; i++) {
        if (!(parent[i] & PTE_PRESENT)) continue;

        uint32_t *parent_pt = (uint32_t *)(parent[i] & PAGE_MASK);
        uint32_t *child_pt  = pmm_alloc_page();
        memset(child_pt, 0, PAGE_SIZE);

        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t pte = parent_pt[j];
            if (!(pte & PTE_PRESENT)) continue;

            void *src = (void *)(pte & PAGE_MASK);
            void *dst = pmm_alloc_page();
            memcpy(dst, src, PAGE_SIZE);          /* THE actual fork copy */

            child_pt[j] = ((uint32_t)dst & PAGE_MASK) | (pte & 0xFFF);
        }
        child[i] = ((uint32_t)child_pt & PAGE_MASK) | (parent[i] & 0xFFF);
    }
    return child;
}
```

The "mirror by reference vs mirror by copy" split is exactly the kernel/user boundary in the address space. Anything kernel-mapped stays shared (so a kernel-stack fault in the child wouldn't see different page tables than the parent). Anything user-mapped becomes its own physical pages with the same VA.

This works at all because pages are identity-mapped in the kernel master PD. We can `memcpy(dst_phys, src_phys, PAGE_SIZE)` from inside any user PD because PDEs 0..7 mirror the identity map — both `dst` and `src` are addresses in [0, 32 MiB), reachable directly. No temporary mappings, no kmap, no fancy tricks.

The COW alternative the prompt called out would replace the inner `pmm_alloc_page() + memcpy` with "share the page R/O, mark both PTEs CoW, page-fault to copy on first write." Mechanically smaller hot path, much larger machinery (page-fault handler that reads PTEs, finds CoW bit, copies, retries). For the demo, deep copy wins.

Cost analysis: a fresh user task has 2 user pages (1 code, 1 stack), so each fork copies 8 KiB of user memory + allocates 1 PD + 1 PT + 2 pages = 4 page allocations. Cheap. A bigger program scales linearly with mapped memory.

## fork_child_return — the kernel-stack synthesis

This is the heart of the trick. When a fork-child is scheduled for the first time, it has to look like it just returned from a syscall — same ring 3, same EIP, same ESP as the parent — but with EAX = 0 instead of the child's pid.

`task_switch` pops 5 dwords (callee-saved + EFLAGS) off the new task's kernel stack and `ret`s. Whatever address is at `[esp+0]` after those pops is what `ret` jumps to. So we synthesize the child's stack such that:

- The 5 dwords for `task_switch` are all zeros (clean callee-saved) plus EFLAGS = 0x202 (IF=1).
- The dword above them is the address of `fork_child_return`.
- Above THAT sits a complete `isr_common_stub` exit frame: saved DS, popa block, int_no, err_code, then iret frame (ss/useresp/eflags/cs/eip).

The popa block has EAX = 0; the iret frame's EIP/CS/EFLAGS/ESP/SS are copied from the parent's. `fork_child_return` is just the tail of `isr_common_stub`:

```asm
.global _fork_child_return
_fork_child_return:
    pop     %eax                /* saved DS                          */
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    popa                        /* restores all GP regs (eax = 0)    */
    add     $8, %esp            /* skip int_no + err_code            */
    iret                        /* back to ring 3 at parent's EIP    */
```

Annotated stack diagram (high address at the top, low address — where `task->esp` points — at the bottom):

```
high addr ──┬──────────────────────────────────────┐
            │ ss            ── iret pops           │
            │ useresp       ── iret pops           │
            │ eflags        ── iret pops           │
            │ cs            ── iret pops           │
            │ eip           ── iret pops           │
            │ err_code      ── add $8, %esp        │
            │ int_no        ──     skips           │
            │ eax = 0       ── popa: child fork()=0│
            │ ecx (parent's)                        │
            │ edx (parent's)                        │
            │ ebx (parent's)                        │
            │ esp_orig      ── popa skips           │
            │ ebp (parent's)                        │
            │ esi (parent's)                        │
            │ edi (parent's)                        │
            │ saved_ds      ── pop %eax             │
            │ &fork_child_return ── task_switch ret │
            │ saved_eflags = 0x202 ── popfl         │
            │ saved_ebp = 0 ── pop %ebp             │
            │ saved_ebx = 0 ── pop %ebx             │
            │ saved_esi = 0 ── pop %esi             │
low addr  ──┤ saved_edi = 0 ── pop %edi  ←task->esp │
            └──────────────────────────────────────┘
```

The C builder is straightforward — write 21 dwords from high to low and return the bottom address as the new task's saved ESP:

```c
static uint32_t synth_fork_child_stack(void *stack_base,
                                       const struct registers *parent) {
    uint8_t *top = (uint8_t *)stack_base + TASK_STACK_SZ;

    top -= 4; *(uint32_t *)top = parent->ss;
    top -= 4; *(uint32_t *)top = parent->useresp;
    top -= 4; *(uint32_t *)top = parent->eflags;
    top -= 4; *(uint32_t *)top = parent->cs;
    top -= 4; *(uint32_t *)top = parent->eip;
    top -= 4; *(uint32_t *)top = parent->err_code;
    top -= 4; *(uint32_t *)top = parent->int_no;
    top -= 4; *(uint32_t *)top = 0;                 /* eax = 0 */
    top -= 4; *(uint32_t *)top = parent->ecx;
    top -= 4; *(uint32_t *)top = parent->edx;
    top -= 4; *(uint32_t *)top = parent->ebx;
    top -= 4; *(uint32_t *)top = parent->esp_orig;
    top -= 4; *(uint32_t *)top = parent->ebp;
    top -= 4; *(uint32_t *)top = parent->esi;
    top -= 4; *(uint32_t *)top = parent->edi;
    top -= 4; *(uint32_t *)top = parent->ds;
    top -= 4; *(uint32_t *)top = (uint32_t)&fork_child_return;
    top -= 4; *(uint32_t *)top = 0x202;             /* EFLAGS w/ IF=1 */
    top -= 4; *(uint32_t *)top = 0;                 /* EBP */
    top -= 4; *(uint32_t *)top = 0;                 /* EBX */
    top -= 4; *(uint32_t *)top = 0;                 /* ESI */
    top -= 4; *(uint32_t *)top = 0;                 /* EDI */

    return (uint32_t)(uintptr_t)top;
}
```

The popa-block ordering must match what `pusha` would have produced in the parent: EDI at the lowest address, EAX at the highest. `popa` reads them back in the reverse-of-push order with one quirk — it skips the ESP slot rather than popping into ESP (because that would make a mess of the stack pointer mid-instruction). The `esp_orig` slot's value is therefore irrelevant; we copy the parent's value just for tidiness.

## task_fork

The kernel-side glue is short:

```c
struct task *task_fork(struct registers *parent_regs) {
    struct task *parent = g_current;
    if (!parent->is_user) return NULL;

    int slot = find_free_slot();
    if (slot < 0) return NULL;

    void *kstack = kmalloc(TASK_STACK_SZ);
    if (!kstack) return NULL;

    uint32_t *child_pd = paging_clone_user_pd((uint32_t *)parent->cr3);
    if (!child_pd) { kfree(kstack); return NULL; }

    uint32_t child_esp = synth_fork_child_stack(kstack, parent_regs);

    struct task *child = &g_tasks[slot];
    memset(child, 0, sizeof(*child));
    child->id               = g_next_id++;
    child->state            = TASK_STATE_READY;
    child->stack_base       = kstack;
    child->esp              = child_esp;
    child->cr3              = (uint32_t)child_pd;
    child->kernel_stack_top = (uint32_t)kstack + TASK_STACK_SZ;
    child->user_eip         = parent_regs->eip;
    child->user_esp         = parent_regs->useresp;
    child->is_user          = 1;
    child->parent_id        = parent->id;
    strncpy(child->name, parent->name, TASK_NAME_MAX - 1);

    /* Inherit the fd table verbatim. fd offsets are per-fd in our
     * model, so the child gets an independent file position. Socket
     * fds work too — a double-close becomes "second close fails"
     * rather than UB. */
    for (int i = 0; i < TASK_MAX_FDS; i++) child->fds[i] = parent->fds[i];

    /* Splice into the round-robin ring. */
    __asm__ volatile ("cli");
    child->next  = parent->next;
    parent->next = child;
    __asm__ volatile ("sti");

    return child;
}
```

The kernel-side dispatch into `SYS_FORK` is a one-liner:

```c
case SYS_FORK: {
    struct task *child = task_fork(r);
    ret = child ? (int32_t)child->id : -1;
    break;
}
```

Parent gets `ret = child_pid` written into `r->eax` by the dispatcher tail. Child gets EAX = 0 from its synthesized popa frame. Both irets emerge from the same INT 0x80 instruction site.

## task_exec_inplace

exec is destructive in a way fork isn't: it kills the calling task's address space and rebuilds it. Order matters because we're standing on the very thing we're tearing down.

```c
int task_exec_inplace(struct registers *r,
                      const char *path, int argc,
                      const char *const *argv_strs) {
    struct task *t = g_current;

    /* 1. Open + load BEFORE touching the existing PD. Failures here
     *    leave the caller's address space untouched. */
    int fd = fs_open(path);
    if (fd < 0) return -2;

    struct elf_load_result lr;
    if (elf_load(fd, &lr) != 0) return -1;

    elf_setup_args(&lr, argc, argv_strs);   /* writes via kernel id-map */

    /* 2. Commit. From here on we're past the no-return point. */
    uint32_t old_cr3 = t->cr3;
    t->cr3      = lr.cr3;
    t->user_eip = lr.entry;
    t->user_esp = lr.user_esp;
    if (path) strncpy(t->name, path, TASK_NAME_MAX - 1);

    /* Activate the new address space. The kernel stack we're standing
     * on is in the 0..32 MiB identity-mapped region, mirrored into
     * every user PD via shared kernel PDEs, so the load doesn't pull
     * the rug out. */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(lr.cr3) : "memory");

    paging_destroy_user_pd((uint32_t *)old_cr3);

    /* 3. Rewrite the iret frame so the syscall return jumps into the
     *    freshly-loaded program. */
    r->eip     = lr.entry;
    r->useresp = lr.user_esp;
    r->cs      = 0x1B;        /* user code, RPL=3            */
    r->ss      = 0x23;        /* user data, RPL=3            */
    r->ds      = 0x23;
    r->eflags  = 0x202;
    r->eax     = 0;

    return 0;
}
```

Three things had to be true for that CR3 swap mid-syscall to be safe:

1. **The kernel stack we're on is reachable from both the old and new PDs.** It is — kmalloc lives in [0x100000, 0x500000), inside the 0..32 MiB region that PDEs 0..7 mirror in every user PD.
2. **Nothing that the syscall handler still needs lives in the old user PD.** `path` and `argv_strs` are kernel-side: `path` was snapshotted from the user pointer into a local 64-byte buffer in `syscall.c`, and `argv_strs` were snapshotted into kmalloc'd kernel buffers before this call. The old user data is now safely irrelevant.
3. **The current CR3 register can be written without flushing TLB entries we still need.** `mov cr3` flushes the entire TLB (non-global entries). The kernel mappings have no global bit set in our setup, so they're flushed too — but they're re-walked from the new PD's PDEs 0..7, which point at the same kernel page tables. So the next instruction fetch re-fills the TLB from the right place.

The `argv` snapshot happens in `SYS_EXEC`'s case in `syscall.c`:

```c
const char  *upath = (const char *)a;
const char **uargv = (const char **)b;

char path[64];
for (int i = 0; ...)  path[i] = upath[i];

char *argv_kbufs[16] = {0};
int   argc = 0;
for (argc = 0; argc < 16 && uargv[argc]; argc++) {
    int len; for (len = 0; uargv[argc][len]; len++) {}
    char *kb = kmalloc(len + 1);
    memcpy(kb, uargv[argc], len + 1);
    argv_kbufs[argc] = kb;
}

int err = task_exec_inplace(r, path, argc,
                            (const char *const *)argv_kbufs);

for (int j = 0; j < argc; j++) kfree(argv_kbufs[j]);
```

`elf_setup_args` later pokes those strings into the new user stack via the kernel identity map (`r->stack_phys`), so by the time we kfree them, the user-visible argv array on the new stack already has its own copies of the strings. The kbuf is purely a transit buffer.

## wait, ZOMBIE, and the slot-reuse race

The wait machinery is conceptually small:

```
SYS_EXIT (child):                  SYS_WAIT (parent):
  task_exit_current(code):           task_wait_current(&out_code):
    save exit_code                     loop:
    find parent                          look for ZOMBIE child
    if parent BLOCKED_ON_CHILD:          if found: harvest + return pid
       parent → READY                    if no children: return -1
    self → ZOMBIE                        self → BLOCKED_ON_CHILD
    schedule()                           schedule()
```

Two new task states keep the scheduler honest:

- `TASK_STATE_BLOCKED_ON_CHILD` — added to `schedule()`'s skip list so a waiting parent isn't picked.
- `TASK_STATE_ZOMBIE` — also skipped, so the dying child stops getting CPU.

But there's a race the diagram hides, and it bit hard the first time I built this:

> **The reaper.** AdventOS already has a 200-ms-tick task that scans for `DEAD` slots, frees their kernel stack + user PD, splices them out of the round-robin ring, and marks them `UNUSED`. If wait demoted ZOMBIE→DEAD and let the reaper finish the job, there's a window where the slot is still spliced into the ring even though no task is using it. If the parent's NEXT fork during that window happens to land on the same slot...

Concretely:

1. Parent forks pid 6 → ring is `... → sh → pid6 → httpd → ...`
2. pid 6 exits → ZOMBIE, parent's wait demotes → DEAD.
3. Reaper hasn't run yet — pid 6's slot is still spliced in, `sh->next == pid6`.
4. Parent forks again. `find_free_slot` accepts DEAD slots, so it returns pid 6's old slot.
5. `task_fork` does `memset(child, 0, sizeof(*child)); child->next = parent->next;`. But `parent->next` IS the slot we just memset!

After step 5, `child->next == child` (because `parent->next` was the same address `&g_tasks[child_slot]`, and we cleared `child->next` then re-wrote it to `parent->next` which still pointed at us). Then `parent->next = child` writes the same slot to itself.

The result: a one-node ring loop. The child task is the only thing reachable from itself; the rest of the ring is unreachable from it. When the child later exits and `schedule()` walks the ring from the child, it sees only the child (= ZOMBIE), the loop terminates with `next == g_current`, and `schedule()` returns without switching. The dispatcher hlts forever — that's exactly what hung my first build.

The fix is to make wait the synchronous reaper. `reap_one_zombie_of` now does the full cleanup inline:

```c
static uint32_t reap_one_zombie_of(struct task *parent, int *out_code) {
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *c = &g_tasks[i];
        if (c->state != TASK_STATE_ZOMBIE)  continue;
        if (c->parent_id != parent->id)     continue;

        uint32_t pid = c->id;
        if (out_code) *out_code = c->exit_code;

        /* Unsplice atomically vs the scheduler. */
        __asm__ volatile ("cli");
        struct task *p = c->next;
        int safety = TASK_MAX * 2;
        while (p && p != c && p->next != c && safety--) p = p->next;
        if (p && p->next == c) p->next = c->next;
        __asm__ volatile ("sti");

        /* Free address space + kernel stack. We're on the parent's
         * stack; the zombie's CR3 isn't loaded by anyone. */
        if (c->is_user && c->cr3 && c->cr3 != g_kernel_cr3) {
            paging_destroy_user_pd((uint32_t *)c->cr3);
        }
        if (c->stack_base) kfree(c->stack_base);

        c->state = TASK_STATE_UNUSED;
        c->next  = NULL;
        /* ...zero out the rest of the lifecycle fields... */
        return pid;
    }
    return 0;
}
```

After this returns, the slot is `UNUSED`, the ring is intact, and `find_free_slot` can safely reuse it. The reaper still exists for the orphan case (kernel tasks that die without a `parent_id`, or user tasks whose parent already exited — though we don't have reparenting yet, so the latter currently never happens).

This is the classic "should reap-via-wait or reap-via-reaper" question that real Unix decided in favor of wait, for exactly this reason — the parent calling wait is the natural synchronization point.

## The userspace shell, freed from SYS_KCMD

Old `user/sh.c` had a thin path: read a line, dispatch a tiny set of builtins, otherwise forward the line to `SYS_KCMD` and let the kernel's own command parser run it. That meant every command from the userspace shell was actually run by the kernel — userspace was a teleprompter.

New `user/sh.c` does what a real shell does:

```c
int pid = sys_fork();
if (pid == 0) {
    sys_exec(path, (const char *const *)targv);
    /* exec only returns on failure */
    puts("sh: exec failed: "); puts(path); puts("\n");
    sys_exit(127);
}
int code = 0;
sys_wait(&code);
```

Builtins (`help`, `pid`, `time`, `sleep`, `forktest`, `exit`) run inline in the shell's own ring 3. Everything else goes through fork+exec+wait. There's no `kcmd` escape hatch — `SYS_KCMD = 9` is now an unused syscall number. The kernel's `cmd_help`/`cmd_meminfo`/`cmd_ifconfig`/etc. are still there in `kernel/shell.c`, but they're no longer reachable from userspace. They'd need to be ported to user-mode programs in a future session.

The selftest builtin runs three demos at boot, gated by an `argv[1] == "selftest"` check — kmain passes "selftest" to sh.elf via a variadic `LAUNCH(path, ...)` macro that replaces the old fixed-1-arg version. This means a headless boot leaves the entire fork/exec/wait loop captured in the serial log, no keyboard required.

## Files added / modified

| File | Change |
|---|---|
| `kernel/paging.{h,c}` | `paging_clone_user_pd` deep-copies PD/PT/pages |
| `kernel/task.{h,c}` | `TASK_STATE_ZOMBIE` / `BLOCKED_ON_CHILD`; `parent_id`/`exit_code`; `task_fork` / `task_exec_inplace` / `task_exit_current` / `task_wait_current` |
| `kernel/task_switch.S` | `_fork_child_return` trampoline |
| `kernel/syscall.{h,c}` | `SYS_FORK`/`SYS_EXEC`/`SYS_WAIT`; `SYS_EXIT` now wakes parent; `SYS_KCMD` retired |
| `kernel/kernel.c` | Variadic `LAUNCH(path, argv...)` macro; spawns `sh.elf` with `selftest` arg |
| `user/libuser.{h,c}` | `sys_fork` / `sys_exec` / `sys_wait`; dropped `sys_kcmd` |
| `user/sh.c` | Rewritten: builtins inline, externals via fork+exec+wait, selftest at boot |

## Test trace

```
=== sh selftest: fork / exec / wait ===
[t1] forktest:
  child : pid=6  marker=0xbabe  (was 0xCAFE in parent)
[user task pid=6 exited code=42]
  parent: pid=5  marker=0xcafe  child_pid=6  reaped=6  exit=42
[t2] fork + exec hello.elf:
Hello from C in ring 3! ...
[user task pid=7 exited code=0]
  parent waited: pid=7  exit=0
[t3] nested fork:
  child1 pid=8, about to fork()
  child2 pid=9 (parent=0)
[user task pid=9 exited code=7]
  child1 reaped grandchild, exit=7
[user task pid=8 exited code=11]
  parent reaped child1, exit=11
=== selftest done ===
```

And `curl http://localhost:8080/` still returns 200 OK / 317 bytes — the userspace HTTP server keeps serving across all of that, proving `sock_accept`'s `task_yield` loop isn't disturbed by the new states.

## Design decisions

**Deep copy, not COW.** Smaller code path, no page-fault handler changes, no extra PTE flag bookkeeping. Fork copy time is linear in mapped memory; for a typical ELF (one code page + one stack page) that's 8 KiB of memcpy. Real programs would notice; demo programs don't.

**`fork_child_return` is the tail of `isr_common_stub`.** Could have factored the tail into a shared symbol both refer to. Kept them separate so it's obvious from reading the asm what each is for; the duplication is 7 instructions.

**Synth-stack uses `task_switch`'s existing 5-dword frame format.** Means `task_fork` doesn't need to know about the rest of the scheduler's invariants — as long as the saved frame matches what task_switch produces, the child slots in cleanly with every other task type.

**`exec` rewrites the iret frame instead of using a special trampoline.** `r->eip = lr.entry; r->useresp = lr.user_esp;` uses the existing return path. Means exec returns from the syscall *normally* — the dispatcher's tail runs, the asm stub's `popa; iret` runs, and we land in the new program. No second control-flow path to maintain.

**`SYS_WAIT` does synchronous cleanup.** Documented above — solves the slot-reuse race. The reaper still exists but only handles the orphan case.

**`parent_id` is a uint32_t pid, not a `struct task *`.** Pids are stable, pointers can be invalidated when a slot is reused. Lookup is O(TASK_MAX) = O(16), trivial.

**Fork inherits the fd table by `memcpy`.** Per-fd offsets diverge between parent and child (Unix's open-file-table-vs-fd-table separation isn't here yet — both fds have their own copy of `offset`). For files this means independent read positions; for sockets, refcount issues, but our shell never forks while a socket is open.

**`exit_code` lives on the dying task.** Could have written it onto the parent's TCB, but multiple zombie children would clash. Per-task storage is the natural place — the parent looks it up when it harvests via wait.

**No reparenting.** When a parent dies before its children, the children keep their (now-invalid) `parent_id`. `task_exit_current` finds no live parent for them, so they go to DEAD instead of ZOMBIE; the reaper picks them up. Unix would reparent to init; we just don't bother yet.

**Variadic `LAUNCH(path, ...)` in kmain.** Took the same number of lines as the previous fixed-1-arg version once you count the `__VA_ARGS__` plumbing, and now a future kmain can spawn a task with arbitrary argv.

## Pitfalls

1. **Slot reuse race.** Documented above — the bug that took half the session to find. Always reap synchronously in wait, don't lean on a deferred reaper.
2. **`fork_child_return` must use the same DS reload sequence as `isr_common_stub`.** Otherwise the child enters ring 3 with a kernel DS, and the first user-mode load through DS faults.
3. **The synthesized popa block must match pusha order exactly.** If the EAX slot is at the wrong stack offset, the child gets a garbage return value or the parent's pid (which would make `if (pid == 0)` false in the child — silent test failure).
4. **`esp_orig` is the slot popa skips, not the slot it pops into ESP.** Don't put anything important there.
5. **EFLAGS in the synthesized iret frame must have IF=1.** The CPU clears IF on INT 0x80 entry; if you forget to set IF in the iret frame, the child enters ring 3 with interrupts disabled and the next PIT tick never gets delivered. Hello permanent silence.
6. **The kernel stack must be reachable from the new CR3 in exec.** True today because PDEs 0..7 (covering the kmalloc heap) are shared by reference. If a future change put kernel stacks above 32 MiB, exec would crash at the `mov cr3` instruction.
7. **The user-pointer snapshot for `argv` in SYS_EXEC must happen BEFORE exec destroys the user PD.** Forget the snapshot and you're walking freed memory inside `task_exec_inplace`'s args-packing loop.
8. **Argv strings must be kfree'd after `elf_setup_args` copies them.** Otherwise a fast fork+exec+exit cycle leaks bytes per cycle.
9. **`paging_destroy_user_pd` skips PDEs 0..7** — but only because they're shared kernel mappings. If a future change put user pages in PDE 7 territory by accident, `destroy` would silently leak them.
10. **The reaper must skip ZOMBIE.** Otherwise the reaper races wait for the same slot. `wait` does synchronous cleanup; the reaper only handles DEAD (orphans).

## What might come next

Real wait queues (so `task_wait_current` doesn't busy-yield-spin), `dup`/`pipe` (so a forked child can inherit a redirected fd), `signal`/`kill` (so the parent can interrupt the child), `setpgid` and process groups (so a fork bomb is shootable in one signal). Then the userspace shell can grow `|`, `&`, `<`, `>`, and start to feel like a real Unix shell.
