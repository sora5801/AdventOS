# Session 31 — SMP: bringing up application processors

**Goal:** Stop assuming there's exactly one CPU. AdventOS has run for 30 sessions with `-smp 1` baked into every QEMU command line and a single global `g_current` task pointer. This session brings the **application processors (APs)** online via the standard Intel INIT-SIPI-SIPI handshake, gives each CPU its own kernel stack and TSS, and wires up `sys_getcpu` so userspace can see which CPU it's running on. The scheduler stays BSP-only — only the bootstrap CPU picks tasks from the run queue — but all the boot-up plumbing required to get the second CPU into kernel C code with paging, segments, and a stack of its own is in place.

End state — the new SMP-related boot lines:

```
[boot] starting SMP
madt: 2 CPU(s) found: apic_id=0, apic_id=1  ioapic@0xfec00000
lapic: BSP enabled, ID=0 version=0x14
[smp] starting AP1 (apic_id=1)
[smp] AP1 online (apic_id=1)
[smp] 2/2 CPU(s) online
```

And the new `[t22]` selftest:

```
[t22] SMP: sys_getcpu reports the running CPU's APIC ID
  shell pid=7 running on CPU apic_id=0
  child 0 (pid=8) on CPU apic_id=0
  child 1 (pid=9) on CPU apic_id=0
  child 2 (pid=10) on CPU apic_id=0
```

All four user tasks run on `apic_id=0` because the scheduler is BSP-only. AP1 is alive and running — it's just sitting in a `cli; hlt` loop waiting for work, because we haven't yet wired the run queue for SMP picking. Tests t1–t21 all pass with `-smp 2` enabled; `httpd` continues to serve `curl` on the host port. The kernel is now bi-CPU-clean — adding scheduler participation is a future-session task that builds on the foundation laid here.

This deep dive walks through the whole AP boot-up path: how we tell the BIOS we want SMP (we don't — we read its tables), how we switch a 16-bit real-mode CPU into 32-bit paged kernel C code without touching the BSP's state, the four bugs that cost an evening of debugging, and what's left for the scheduler integration session.

## What's in scope

In:
- **`kernel/lapic.{h,c}`** — Local APIC MMIO mapping, enable, IPI sending (INIT, SIPI, fixed-vector), EOI, ID register read.
- **`kernel/madt.{h,c}`** — ACPI MADT parsing. Walks RSDP → RSDT → MADT, harvests `Local APIC` and `IO APIC` entries.
- **`kernel/ap_trampoline.S`** — 304-byte real-mode stub that switches an AP from its post-SIPI 16-bit state into 32-bit paged kernel C with its own stack.
- **`kernel/smp.{h,c}`** — per-CPU `cpu_local` table, AP startup orchestration, `ap_entry()` C-side init.
- **`kernel/gdt.{h,c}`** — extended for per-CPU TSS slots; `gdt_add_tss` returns a fresh selector for each AP.
- **`kernel/idt.c`** — exposes `idt_descriptor_addr` so APs can `lidt` the same IDT.
- **`kernel/paging.c`** — identity map rounded up to PD-entry boundary (covers BIOS-reserved tail pages); user PDs mirror "high kernel" PDEs (LAPIC) by reference; destroy-PD walks skip mirrored entries.
- **`SYS_GETCPU = 51`** + libuser `sys_getcpu()` wrapper.
- **`build.sh`** — runs QEMU with `-smp 2`.
- **`[t22]` selftest** — fork three children and read `sys_getcpu` from each.

Out:
- **AP scheduling.** APs run an idle hlt loop. The run queue is unlocked and BSP-only.
- **Spinlocks across CPUs.** `kernel/spinlock.h` already uses `xchg`-based locking (so it's SMP-safe), but the kernel doesn't yet have ANY SMP-shared data structures requiring it — the scheduler, fd tables, etc. are all single-CPU because all user tasks run on the BSP.
- **TLB shootdowns.** Without AP scheduling there's nothing on the AP to invalidate.
- **Per-CPU `current task` semantics.** `task_current()` still reads a global. Future SMP scheduling will need `cpu_local()->current`.
- **Multiple APs.** The table is sized for `MAX_CPUS = 8` but we only test with 2.
- **TPR / interrupt routing across CPUs.** Currently every IRQ goes to BSP via the legacy 8259 PIC.
- **CPUID-based feature detection.** We assume CPUID, LAPIC, and ACPI work; QEMU's `qemu32` model has all three.

## Architecture: the boot flow

```
                BSP (CPU 0)                                  AP (CPU 1)
                ─────────────                                ─────────────
   kmain                                                     <reset / wait-for-SIPI>
   ↓                                                         (CR0=0, CS=0xF000, etc.)
   gdt/idt/tss/paging/...
   ↓
   smp_init():
     parse MADT  →  list of LAPIC IDs
     map LAPIC   →  0xFEE00000 in kernel PD
     mirror LAPIC PDE into user PDs
     copy ap_trampoline → 0x8000
     allocate kernel stacks + TSS for each AP
     patch trampoline cells with cr3, entry_addr, stack_table
     ↓
     for each AP:
        lapic_send_init(apic_id)        ─ INIT IPI ─►       <state reset>
     ↓
     pit_sleep(10)                                          <waiting>
     ↓
     for each AP:
        lapic_send_sipi(apic_id, 0x8000) ─ SIPI ─►          CS:IP = 0x800:0
                                                            ↓
                                                            real-mode entry
                                                            ↓
                                                            lgdt cs:0x40    (mini-GDT)
                                                            CR0.PE = 1
                                                            ljmp 0x08:0x8060
                                                            ↓
                                                            32-bit pmode
                                                            ↓
                                                            reload data segs
                                                            CR3 = kernel PD
                                                            CR0.PG = 1
                                                            ↓
                                                            lock xadd → my_id
                                                            esp = ap_stacks[my_id]
                                                            call ap_entry(my_id)
                                                            ↓
                                                            ap_entry() in C:
                                                              load kernel GDT
                                                              ltr per-CPU TSS
                                                              load IDT
                                                              enable LAPIC
                                                              online = 1
                                                              cli; hlt loop
     poll AP online flag                              ──◄─ online write
     report "2/2 CPU(s) online"
```

The interesting transitions are:

1. **INIT-SIPI-SIPI**. The Intel-specified handshake to wake an AP from its post-reset-power-on hold state.
2. **Real mode → protected mode → paging**. Three CPU-state transitions in 30 instructions of inline assembly.
3. **Per-CPU TSS**. Each CPU needs its OWN TSS so ring-3-to-ring-0 transitions land on the right kernel stack.

## Reading the MADT

ACPI's Multiple APIC Description Table lives somewhere in the BIOS-reserved memory region. To find it we walk:

```
RSDP — Root System Description Pointer
   ↓ (signature: "RSD PTR ", in low memory at 16-byte boundaries)
RSDT — Root System Description Table
   ↓ (signature: "RSDT", lists pointers to other ACPI tables)
MADT — Multiple APIC Description Table
       (signature: "APIC", contains entries for each LAPIC + IOAPIC)
```

Our parser is ~150 lines (`madt.c`). For every Local APIC entry with the `enabled` flag set, we record the `apic_id`. For IO APIC entries we save the MMIO base for future IRQ routing work.

```c
struct madt_entry_lapic {
    uint8_t  type;          /* 0 = Local APIC */
    uint8_t  length;        /* 8 */
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;         /* bit 0 = enabled */
} __attribute__((packed));
```

Output for `qemu-system-i386 -smp 2`:

```
madt: 2 CPU(s) found: apic_id=0, apic_id=1  ioapic@0xfec00000
```

We hit one tripwire here. Our null-pointer guard leaves page 0 unmapped, so reading the BIOS data area at physical `0x40E` (the EBDA segment pointer) page-faults. We just skip the EBDA search — SeaBIOS always plants the RSDP in the `0xE0000-0xFFFFF` ROM scan window anyway.

## The Local APIC

The LAPIC is the per-CPU interrupt controller, present on every x86 CPU since the Pentium. It lives at physical `0xFEE00000` (the IA32_APIC_BASE MSR can relocate this; we use the default). Its MMIO is **per-CPU silicon** even though the address is shared — each CPU's own LAPIC chip decodes the access. We map the page once into the kernel PD; both BSP and APs see the right LAPIC through the same VA.

```c
#define LAPIC_REG_ID         0x020   /* this CPU's APIC ID, top 8 bits */
#define LAPIC_REG_EOI        0x0B0   /* end-of-interrupt: write any value */
#define LAPIC_REG_SVR        0x0F0   /* spurious vector + enable */
#define LAPIC_REG_ICR_LOW    0x300   /* interrupt command low */
#define LAPIC_REG_ICR_HIGH   0x310   /* interrupt command high (target ID) */
```

`lapic_init()` runs on the BSP only, maps the page, and configures the spurious vector + enable bit:

```c
lapic_write(LAPIC_REG_LVT_TIMER, 0x10000u);   /* mask LAPIC timer */
lapic_write(LAPIC_REG_LVT_ERROR, 0x10000u);   /* mask error */
lapic_write(LAPIC_REG_TPR, 0);
lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | 0xFF);
```

Critically, we do **not** touch LINT0 / LINT1. SeaBIOS configures them at boot for legacy 8259 PIC routing — LINT0 in ExtINT mode forwards 8259 IRQs to the CPU. Earlier in this session I masked them and broke `pit_sleep`: the PIT timer fires at PIC IRQ 0 → 8259 INTR → BSP's LAPIC LINT0 → if masked, the LAPIC blocks it. Without timer interrupts, `pit_sleep` waits forever. Fix: leave LINT0/LINT1 alone.

`lapic_enable()` runs on every CPU. It's idempotent — APs call it from `ap_entry()` to bring their own LAPIC online.

## INIT-SIPI-SIPI

Per Intel SDM Vol. 3A §8.4, the BSP wakes APs with a three-IPI sequence:

```c
lapic_send_init(apic_id);    /* "Reset to a known state" */
pit_sleep(10);                /* spec mandates 10ms wait */
lapic_send_sipi(apic_id, 0x8000);   /* "Start at vector 8" */
pit_sleep(1);                 /* short delay */
lapic_send_sipi(apic_id, 0x8000);   /* second SIPI; ignored if first took */
```

The Interrupt Command Register (ICR) is the LAPIC register that emits IPIs:

```
ICR_LOW (0x300):
  bits 0-7    vector
  bits 8-10   delivery mode (000=fixed, 100=NMI, 101=INIT, 110=Startup)
  bit 11      destination mode (0=physical, 1=logical)
  bit 12      delivery status (1=pending, read-only)
  bit 14      level (1=assert, used for INIT)
  bit 15      trigger mode (1=level for INIT, 0=edge for Startup)

ICR_HIGH (0x310):
  bits 24-27  destination apic_id (xAPIC mode)
```

`lapic_send_init`:

```c
void lapic_send_init(uint32_t dest_apic_id) {
    /* Assert phase. */
    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_INIT |     /* 0x500 */
                LAPIC_ICR_TRIGGER_LEVEL |     /* 0x8000 */
                LAPIC_ICR_LEVEL_ASSERT);      /* 0x4000 */
    lapic_ipi_wait();
    /* De-assert phase. Most modern boxes don't strictly need this
     * but the spec asks for it. */
    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_INIT |
                LAPIC_ICR_TRIGGER_LEVEL);
    lapic_ipi_wait();
}
```

After INIT, the AP is in a "wait-for-SIPI" hold state. Its CS:IP isn't yet meaningful.

`lapic_send_sipi`:

```c
void lapic_send_sipi(uint32_t dest_apic_id, uint32_t entry_phys) {
    uint32_t vector = (entry_phys >> 12) & 0xFFu;  /* SIPI vector = phys >> 12 */
    lapic_write(LAPIC_REG_ICR_HIGH, dest_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,
                LAPIC_ICR_DELIVERY_STARTUP |
                LAPIC_ICR_LEVEL_ASSERT |
                vector);
    lapic_ipi_wait();
}
```

The "vector" field of a SIPI is `entry_phys >> 12`. If the trampoline lives at 0x8000 (= 8 × 0x1000), the SIPI vector is 0x08, and the AP starts execution at `CS:IP = 0x0800:0x0000` (= physical 0x8000) in real mode.

`lapic_ipi_wait()` polls the PENDING bit (bit 12 of ICR_LOW). It clears once the previous IPI has been delivered. Bounded to 1M iterations so a stuck LAPIC doesn't deadlock the boot.

## The trampoline: 16-bit → 32-bit → paged

The AP wakes in 16-bit real mode at `CS:IP = 0x800:0x000`. Its job is to switch to 32-bit pmode with paging on, give itself a kernel stack, and call into `ap_entry()` in C.

The trampoline is in its own section, `.text.aptramp`, embedded in the kernel image. At runtime the BSP `memcpy`s the bytes (304 bytes) to physical `0x8000` and then sends the SIPI. The AP starts there.

The layout is hand-rolled with `.org` directives so the assembler doesn't need to compute label differences across `.code16`/`.code32` boundaries — PE/COFF gas chokes on that:

```
0x000   real-mode entry (cli, cld, set ds=cs, mark "I got the SIPI",
        set up tiny stack, lgdt cs:0x40, CR0.PE=1, ljmp 0x08:0x8060)
0x040   gdtr (limit + linear base)
0x048   3 GDT entries (null, 0x08 = code32, 0x10 = data32)
0x060   pmode entry (reload data segs, CR3 = kernel PD, CR0.PG=1,
        atomic xadd to grab my_id, esp = stack table[my_id],
        call ap_entry)
0x100   parameter cells (cr3, counter, entry_addr, stack table)
```

### The first painful bug: PE/COFF can't relocate label-difference

My initial trampoline used `(label - _ap_trampoline_start)` to compute self-relative offsets. PE/COFF gas refused to assemble those:

```
ap_trampoline.S:46: Error: can't resolve .text.aptramp - _ap_trampoline_start
```

This happens because PE/COFF's relocation model doesn't support arbitrary label-difference within a section if the section's start is itself a symbol. ELF would have just done it. Workaround: use `.org` directives to lock each piece at a fixed offset, then refer to absolute physical addresses (`AP_BOOT_PHYS + 0x40`) in the code. The trampoline knows it'll be copied to `0x8000` so it just references that directly.

### The second painful bug: `lgdtl 0x40(%si)` with uninitialized `%si`

After SIPI, only CS:IP is set deterministically. SI may be anything. My initial real-mode `lgdtl 0x40(%si)` assembled to SI-relative addressing — and SI was garbage, so the lgdt loaded a random GDT, and the next instruction (which depended on the GDT being correct) faulted.

Fix: hand-encode the lgdt with direct disp16 addressing (no register dependency):

```
.byte 0x66, 0x2E, 0x0F, 0x01, 0x16, 0x40, 0x00    /* lgdtl cs:0x40 */
```

Bytes broken down:
- `0x66` — operand-size override (32-bit operand → 32-bit base in the lgdtl operand)
- `0x2E` — CS segment override
- `0x0F 0x01` — LGDT/LIDT/SGDT/SIDT family opcode
- `0x16` — ModR/M byte. mod=00, reg=010 (/2 = LGDT), rm=110 (direct disp16)
- `0x40 0x00` — disp16 = 0x0040

In real mode, the CPU reads the 6-byte gdtr operand from cs:[0x40] = physical 0x8040. The first 2 bytes are the limit, the next 4 are the 32-bit linear base.

### The third painful bug: gdtr base off-by-2

The gdtr operand at `0x40` is a 6-byte structure (2 + 4 = 6 bytes). I claimed the GDT itself was at offset `0x48`. But 0x40 + 6 = 0x46, not 0x48. So the lgdt pointed at a base 2 bytes too late, loading garbage descriptors.

Fix: pad the gdtr to 8 bytes:

```
.org 0x40
    .word  0x0017                 /* limit */
    .long  AP_BOOT_PHYS + 0x48    /* base — 0x48, NOT 0x46 */
    .word  0                       /* 2 bytes of padding to 0x48 */
.org 0x48
    .quad  0x0000000000000000     /* null entry */
    .quad  0x00CF9A000000FFFF     /* code32, base=0, limit=4G, ring 0 */
    .quad  0x00CF92000000FFFF     /* data32 */
```

### The fourth painful bug: real-mode-cached DS in pmode

Right after the `ljmp 0x08:0x8060`, CS is reloaded to the flat code32 descriptor (base=0). But DS, ES, SS still hold their **real-mode cached descriptors** (base = old_seg × 16 = 0x8000 for our setup), because those segment registers haven't been reloaded.

If pmode entry tries to write to absolute address `(AP_BOOT_PHYS + 0x202)` = `0x8202`, the assembler emits a DS-relative operand. The CPU computes `DS_base + 0x8202` = `0x8000 + 0x8202` = `0x10202`. The write goes to the wrong place.

This was particularly insidious because the AP didn't crash — it just wrote markers to the WRONG memory and the BSP polled the right address and saw nothing.

Fix: reload DS BEFORE any absolute-address memory operation in pmode:

```
.org 0x60
    /* CS is now flat (base=0) but ds/es/ss still have their real-
     * mode cached descriptors (base = old_seg * 16). Reload data
     * segments FIRST. */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* NOW absolute addresses work correctly. */
    mov     (AP_BOOT_PHYS + 0x100), %eax
    mov     %eax, %cr3
```

### The fifth painful bug: BSP doesn't see AP's "online" write

After all the above worked and AP1 was clearly running ap_entry (it was printing `[smp] AP1 online`), the BSP's wait loop still timed out:

```
[smp] starting AP1 (apic_id=1)
[smp] AP1 online (apic_id=1)        ← AP wrote online=1 + kprintf
[smp] AP1 failed to come up         ← BSP gave up before seeing it
```

Both messages! Memory ordering. The AP wrote `online=1` with `__atomic_store_n` + RELEASE. But the BSP's read was a plain `if (g_cpus[idx].online) ...` in a loop. With `-O2`, GCC hoisted the load OUT of the loop into a register before the AP had a chance to write.

Fix: explicit acquire load on the BSP side too:

```c
while (... ) {
    if (__atomic_load_n(&g_cpus[idx].online, __ATOMIC_ACQUIRE)) return 1;
    ...
}
```

After this fix the boot output is clean: `[smp] 2/2 CPU(s) online`.

## Per-CPU TSS and GDT

Each CPU needs its own TSS so a ring-3 → ring-0 transition (syscall, IRQ, exception) lands on a kernel stack belonging to that CPU. Currently we only had a single global TSS at GDT slot 5 (selector `0x28`).

`gdt.c` was extended to reserve `MAX_CPUS = 8` TSS slots after the fixed 5 entries:

```
0x00  null
0x08  kernel code (ring 0)
0x10  kernel data (ring 0)
0x18  user code (ring 3)
0x20  user data (ring 3)
0x28  BSP TSS                      (slot 5, set up by gdt_init)
0x30  AP1 TSS                      (slot 6, set up by smp_init via gdt_add_tss)
0x38  AP2 TSS                      (slot 7, ...)
...
```

`gdt_add_tss(struct tss *tss)` returns the next available selector and fills the descriptor. AP1 calls `ltr` with its returned selector inside `ap_entry()`:

```c
if (me->tss_selector) {
    __asm__ volatile ("ltr %%ax" :: "a"(me->tss_selector));
}
```

This is independent across CPUs — they each have their own TR register pointing at their own TSS GDT entry. Future ring-3 work on AP1 will trap to the kernel using AP1's TSS.esp0.

## The LAPIC-PDE-mirroring fix

User PDs are created with `paging_create_user_pd()`, which copies kernel PDEs 0..7 (covering the low 32 MiB identity map). After enabling SMP, the LAPIC mapping at virtual `0xFEE00000` lives in PDE 1019 (= `0xFEE00000 / 4 MiB`). User PDs created BEFORE `smp_init()` ran wouldn't have it; even those created AFTER didn't, because the copy loop only ran 0..7.

`sys_getcpu` calls `lapic_id()`, which reads the LAPIC MMIO. Inside a syscall handler running on a user PD that lacks the LAPIC mapping → page fault.

Fix: extend `paging_create_user_pd` to ALSO mirror any present PDE in the high range:

```c
for (int i = 256; i < 1024; i++) {
    if (g_pd[i] & PTE_PRESENT) {
        pd[i] = g_pd[i];        /* mirror by reference */
    }
}
```

Mirror by reference means user PDs share the SAME page table as the kernel. Three new edge cases:

1. **`paging_destroy_user_pd`** must skip these mirrored PDEs — freeing them would deallocate the kernel's page table, causing kernel-side mappings to vanish. Detection: `pd[i] == g_pd[i]` means "mirrored, don't free".

2. **`paging_clone_user_pd`** (fork) currently deep-copies all PDEs from 8..1023. For mirrored kernel PDEs, this allocates a new PT and copies PTEs. Functionally correct but wastes a page per fork. Fixing properly would involve tracking which PDEs are mirrored vs. user-owned. Left as future work.

3. **The PTEs inside the mirrored PT** point to MMIO frames (LAPIC at physical `0xFEE00000`). `pmm_free_page` on those bails out because the address is outside the PMM's tracked range — so no corruption, just wasted attempts.

## SYS_GETCPU = 51

A trivial syscall:

```c
case SYS_GETCPU: {
    ret = (int32_t)lapic_id();
    break;
}
```

`lapic_id()` reads bits 24-31 of the LAPIC ID register. Each CPU's LAPIC reports the same 8 bits its `apic_id` was assigned at boot. From userspace:

```c
int sys_getcpu(void) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_GETCPU)
                      : "memory");
    return ret;
}
```

In `[t22]`, the test forks 3 children and each prints `sys_getpid()` + `sys_getcpu()`. Today they all return CPU 0 because the scheduler is BSP-only. Once AP scheduling lands, the same test will report a mix of 0 and 1 across the children.

## What's not yet there: AP scheduling

The big missing piece is letting APs participate in the scheduler. The shape of the work, in roughly ascending order of difficulty:

1. **Per-CPU `current` pointer.** Replace `g_current` with `cpu_local()->current`. Every reference to `g_current` becomes `cpu_local()->current`. Need to fix `task_current()`, `schedule()`, `task_yield()`.

2. **Run-queue spinlock.** The single global ready list needs `xchg`-based locking around insert/remove. We have `spinlock_t` already (session 6); just plumb it through.

3. **AP idle task + scheduler entry.** The AP's `for(;;) cli; hlt` loop becomes `for(;;) schedule(); hlt`. Each AP picks a runnable task off the queue, runs it, and re-enters the scheduler when the task yields/blocks/exits.

4. **TLB shootdowns.** When a page is unmapped, the AP's TLB still has the stale entry. We need an IPI mechanism to broadcast "invlpg this VA" to other CPUs. `lapic_send_fixed` is already in place; need an IPI vector + handler.

5. **Per-CPU FD tables, signal queues, etc.** Most of these are already per-task, so they don't need per-CPU duplicates — but locking has to make sense.

6. **Proper SMP-aware boot ordering.** Today APs come up and go straight to hlt. With scheduling, APs need to wait until the scheduler is fully set up before joining.

Each of these is straightforward in isolation. The combination is one or two more sessions of careful work.

## File-by-file diff

```
boot/boot.S                    no change
linker_kernel.ld               KEEP(*(.text.aptramp)) inside .text

kernel/lapic.{h,c}             NEW  ~150 lines: LAPIC enable, IPI, EOI,
                                    ID register read, MMIO map
kernel/madt.{h,c}              NEW  ~140 lines: RSDP search, RSDT walk,
                                    MADT entry parsing
kernel/smp.{h,c}               NEW  ~200 lines: per-CPU table, AP startup
                                    via INIT-SIPI-SIPI, ap_entry C-side
kernel/ap_trampoline.S         NEW  ~145 lines: real-mode → pmode →
                                    paging → C entry stub

kernel/gdt.{h,c}               extended: GDT now reserves 8 TSS slots,
                                    gdt_add_tss + gdt_load_for_ap helpers
kernel/idt.c                   add idt_descriptor_addr helper for AP lidt
kernel/paging.c                identity map rounded up to 4 MiB boundary;
                                    user PDs mirror present PDEs from
                                    256..1023; destroy skips mirrored

kernel/syscall.h               SYS_GETCPU = 51
kernel/syscall.c               SYS_GETCPU dispatcher case

kernel/kernel.c                #include + smp_init() after task_init()

user/libuser.h                 SYS_GETCPU constant + sys_getcpu prototype
user/libuser.c                 sys_getcpu wrapper

user/sh.c                      [t22] selftest fork-and-getcpu

build.sh                       suggested QEMU command line: -smp 2
```

Net diff: about 700 new lines (kernel-side), 30 lines of glue for syscalls and the selftest.

## Boot sequence with SMP

```
[boot] serial + VGA up
[boot] booted from drive 0x80
[boot] installing TSS... ok
[boot] installing GDT... ok (TR loaded)
[boot] installing IDT... ok
[boot] remapping PIC to 0x20/0x28... ok
[boot] starting PIT @ 100Hz... ok
[boot] starting keyboard... ok
[boot] enabling serial RX IRQ... ok
[boot] reading BIOS E820 map... 6 entries
[boot] initializing PMM... 7965/8160 pages free (31860 KB)
[boot] reserving heap from PMM... 0x100000..0x500000 (4096 KB)
[boot] enabling paging... PD@0x1000, 8/1024 PDEs in use
[boot] initializing ATA driver... ok
[boot] initializing block cache... ok (32 slots, 16 KiB)
[boot] mounting AdventFS... fs: AdventFS mounted, 29 entries, 617/1024 sectors free
[boot] mounting VFS... vfs: mounted 'rootfs' at /
vfs: mounted 'procfs' at /proc
[boot] RTC: 2026-5-9 17:33:9 UTC
[boot] initializing task system... ok
[boot] enabling interrupts
[boot] starting SMP
madt: 2 CPU(s) found: apic_id=0, apic_id=1  ioapic@0xfec00000
lapic: BSP enabled, ID=0 version=0x14
[smp] starting AP1 (apic_id=1)
[smp] AP1 online (apic_id=1)
[smp] 2/2 CPU(s) online
...
[t22] SMP: sys_getcpu reports the running CPU's APIC ID
  shell pid=7 running on CPU apic_id=0
  child 0 (pid=8) on CPU apic_id=0
  child 1 (pid=9) on CPU apic_id=0
  child 2 (pid=10) on CPU apic_id=0
```

## Closing thoughts on hand-encoded assembly

The single most-painful aspect of this session was the trampoline. Five distinct bugs, each one taking ~30 minutes to bisect, in 145 lines of code. The pattern across them is: **the assembler's job and your mental model can drift apart** when you mix `.code16`/`.code32`/`.org`/PE-COFF/segment overrides/operand-size prefixes.

The trampoline is mostly hand-encoded bytes (`lgdtl`, `ljmpl`) precisely because gas's text-form encoding kept producing instructions that disassembled to the right symbol but had subtly wrong addressing modes (SI-relative, real-mode-cached DS, etc.). When in doubt, write the bytes you want, then verify with `objdump -s`.

If we ever need to extend this trampoline (long mode, x2APIC, multi-AP staging), the right move is probably to write it in a separate self-hosting source tree and incbin the resulting flat binary — no PE/COFF section magic, no .org tricks, just bytes.

For now, two CPUs are alive. The kernel didn't get much harder to reason about. Hopefully the AP-scheduling session that follows will be the rewarding "now everything actually goes faster" payoff.
