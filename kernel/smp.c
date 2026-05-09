#include "smp.h"
#include "lapic.h"
#include "madt.h"
#include "tss.h"
#include "gdt.h"
#include "task.h"
#include "kmalloc.h"
#include "paging.h"
#include "pmm.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

#define AP_BOOT_PHYS    0x8000
#define AP_STACK_SIZE   0x4000              /* 16 KiB per AP kernel stack */

/* Per-CPU table — one struct per logical CPU. cpu_local[0] is the
 * BSP, populated by smp_init before launching APs. cpu_local[i] for
 * i > 0 is an AP, populated atomically as APs report in. */
static struct cpu_local g_cpus[MAX_CPUS];
static int              g_cpu_count;

/* Symbols from the trampoline assembly file. The trampoline lives
 * in section .text.aptramp; the linker script reserves space for it
 * in the kernel image. We memcpy the bytes between _start and _end
 * to AP_BOOT_PHYS at runtime. */
extern uint8_t  ap_trampoline_start[];
extern uint8_t  ap_trampoline_end[];

/* Symbols within the trampoline at known relative offsets. The
 * trampoline source declares them globally so the linker resolves
 * the bytes — but they're at addresses inside the kernel image,
 * NOT at AP_BOOT_PHYS. We have to compute the actual physical
 * locations after the memcpy. */
extern uint32_t ap_cr3;
extern uint32_t ap_counter;
extern uint32_t ap_entry_addr;
extern uint32_t ap_stacks;       /* this is actually ap_stacks[8] */

static uint32_t trampoline_offset_of(const void *sym) {
    return (uint32_t)((const uint8_t *)sym - ap_trampoline_start);
}

/* Patch one of the trampoline's parameter cells AT THE POSITION
 * IT'LL OCCUPY POST-COPY (= AP_BOOT_PHYS + offset_within_trampoline). */
static void patch_trampoline_u32(const void *sym_in_kernel, uint32_t value) {
    uint32_t off = trampoline_offset_of(sym_in_kernel);
    *(volatile uint32_t *)(uintptr_t)(AP_BOOT_PHYS + off) = value;
}
static void patch_trampoline_stack(int idx, uint32_t value) {
    uint32_t off = trampoline_offset_of(&ap_stacks);
    *(volatile uint32_t *)(uintptr_t)(AP_BOOT_PHYS + off + (idx * 4)) = value;
}

int               smp_cpu_count(void) { return g_cpu_count; }
struct cpu_local *cpu_at(int idx) {
    if (idx < 0 || idx >= MAX_CPUS) return 0;
    return &g_cpus[idx];
}

/* Find the cpu_local entry for the calling CPU. Reads LAPIC ID and
 * scans the table — slow-ish (linear over MAX_CPUS) but the table
 * is tiny so a hash is overkill. Falls back to cpu_local[0] if no
 * match (= "we haven't finished SMP setup yet"). */
struct cpu_local *cpu_local(void) {
    uint32_t id = lapic_id();
    for (int i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].lapic_id == id) return &g_cpus[i];
    }
    return &g_cpus[0];
}

/* Spin-wait helper — BSP polls until each AP toggles its `online`
 * flag. The AP writes the flag from ap_entry; this is a tight
 * busy-wait gated by an iteration cap. The atomic_load_n is what
 * makes this work in -O2 — without it the compiler hoists the
 * load out of the loop and BSP never sees the AP's write. */
static int wait_for_ap_online(int idx, int max_iter) {
    for (int i = 0; i < max_iter; i++) {
        if (__atomic_load_n(&g_cpus[idx].online, __ATOMIC_ACQUIRE)) return 1;
        for (volatile int j = 0; j < 100000; j++) { /* spin */ }
    }
    return 0;
}

/* The C-side AP entry. The trampoline finishes by `call`-ing this
 * with the AP's logical id on the stack. We're now running with
 * paging on, kernel mappings, and our own kernel stack. */
void ap_entry(uint32_t my_id) {
    if (my_id >= MAX_CPUS) {
        for (;;) __asm__ volatile ("hlt");
    }
    struct cpu_local *me = &g_cpus[my_id];

    /* Reload kernel GDT (the trampoline's was a temporary). */
    extern void gdt_load_for_ap(void);
    gdt_load_for_ap();

    /* Load this AP's TSS so ring-3 → ring-0 transitions land on
     * the right kernel stack. */
    if (me->tss_selector) {
        __asm__ volatile ("ltr %%ax" :: "a"(me->tss_selector));
    }

    /* Load the kernel IDT — same one as BSP. */
    extern void idt_load_for_ap(void);
    idt_load_for_ap();

    /* Bring this AP's LAPIC online. */
    lapic_enable();
    me->lapic_id = lapic_id();

    /* Build the per-CPU idle TCB. Our current kernel stack (the one
     * the trampoline allocated and we're standing on) becomes the
     * idle task's stack. From here on, schedule() can switch FROM
     * us TO a runnable task; when nothing is runnable, schedule()
     * picks us back up and we return to the hlt loop. */
    extern struct task *task_init_ap_idle(uint32_t cpu_id, uint32_t kstop, void *kbase);
    struct task *idle = task_init_ap_idle(my_id, me->kernel_stack_top, me->idle_stack);
    me->idle    = idle;
    me->current = idle;

    /* Start the per-CPU LAPIC timer. The handler at IDT 0x40 calls
     * schedule() on every fire — that's how we get preempted out of
     * the idle hlt below. 10_000_000 LAPIC clocks ≈ 10ms on QEMU. */
    lapic_timer_init(10000000u);

    /* Mark online so the BSP's wait_for_ap_online polling sees us.
     * MUST come AFTER our scheduler state is set up — the BSP may
     * spawn user tasks immediately on seeing us online and we need
     * to be in a state that can accept being scheduled out. */
    __atomic_store_n(&me->online, 1, __ATOMIC_RELEASE);

    kprintf("[smp] AP%u online (apic_id=%u, lapic-timer @ vec 0x40)\n",
            (unsigned)my_id, (unsigned)me->lapic_id);

    /* Idle loop: enable interrupts and hlt. Each LAPIC-timer tick
     * preempts us into schedule(); if a real task is READY,
     * schedule() picks it and we don't return here until that task
     * yields back to idle. If nothing is READY, schedule() picks
     * idle (us) and we resume right at the next iteration — back
     * into hlt to save power. */
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}

void smp_init(void) {
    /* Step 0: bring up the LAPIC + parse the MP table on the BSP. */
    madt_init();
    lapic_init();

    /* Populate cpu_local[0] (BSP). */
    g_cpu_count = 1;
    g_cpus[0].cpu_id           = 0;
    g_cpus[0].lapic_id         = lapic_id();
    g_cpus[0].online           = 1;
    g_cpus[0].kernel_stack_top = 0;     /* BSP uses the boot stack */
    g_cpus[0].tss_selector     = 0x28;  /* set up by gdt_init */

    int total_cpus = madt_cpu_count();
    if (total_cpus <= 1) {
        kprintf("[smp] uniprocessor — only BSP detected\n");
        return;
    }

    /* Step 1: allocate per-AP kernel stack + TSS, set up GDT entry. */
    for (int i = 1; i < total_cpus && i < MAX_CPUS; i++) {
        const struct madt_cpu *mc = madt_cpu(i);
        if (!mc || !mc->enabled) continue;

        struct cpu_local *c = &g_cpus[i];
        memset(c, 0, sizeof(*c));
        c->cpu_id   = i;
        c->lapic_id = mc->apic_id;

        c->idle_stack = kmalloc(AP_STACK_SIZE);
        if (!c->idle_stack) {
            kprintf("[smp] AP%d: stack alloc failed; skipping\n", i);
            continue;
        }
        c->kernel_stack_top =
            (uint32_t)(uintptr_t)c->idle_stack + AP_STACK_SIZE;

        /* Set up this AP's TSS. */
        memset(&c->tss, 0, sizeof(c->tss));
        c->tss.ss0        = 0x10;
        c->tss.esp0       = c->kernel_stack_top;
        c->tss.iomap_base = (uint16_t)sizeof(c->tss);

        /* Add a GDT entry for this AP's TSS, get back a selector. */
        c->tss_selector = gdt_add_tss(&c->tss);

        g_cpu_count++;
    }

    if (g_cpu_count <= 1) {
        kprintf("[smp] no APs successfully prepared\n");
        return;
    }

    /* Step 2: copy the trampoline to AP_BOOT_PHYS. The bytes are
     * position-independent within the trampoline; absolute jumps
     * within use AP_BOOT_PHYS as the base. */
    uint32_t tramp_size = (uint32_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy((void *)(uintptr_t)AP_BOOT_PHYS, ap_trampoline_start, tramp_size);

    /* Patch parameter cells in the live trampoline copy. */
    patch_trampoline_u32(&ap_cr3, paging_pd_addr());
    patch_trampoline_u32(&ap_counter, 1);              /* AP ids start at 1 */
    patch_trampoline_u32(&ap_entry_addr, (uint32_t)(uintptr_t)&ap_entry);
    /* Stack table — slot[i] = top of AP[i]'s stack. Slot 0 unused
     * (BSP has its own stack); slot 1+ for APs. The trampoline's
     * lock-xadd starts at 1, so the first AP picks slot 1. */
    patch_trampoline_stack(0, 0);
    for (int i = 1; i < g_cpu_count; i++) {
        patch_trampoline_stack(i, g_cpus[i].kernel_stack_top);
    }

    /* Step 3: INIT-SIPI-SIPI the APs. Per Intel SDM, send INIT to
     * each, then wait 10ms, then send SIPI twice with a short
     * delay — APs accept the first SIPI after INIT and ignore the
     * second, but on some chipsets the first one is missed. */
    for (int i = 1; i < g_cpu_count; i++) {
        kprintf("[smp] starting AP%d (apic_id=%u)\n",
                i, (unsigned)g_cpus[i].lapic_id);
        lapic_send_init(g_cpus[i].lapic_id);
    }

    pit_sleep(10);     /* per spec: wait 10ms after INIT */

    for (int i = 1; i < g_cpu_count; i++) {
        lapic_send_sipi(g_cpus[i].lapic_id, AP_BOOT_PHYS);
    }

    /* Per spec, send SIPI again 200µs later. We don't have µs-scale
     * timers; use a coarse delay. */
    pit_sleep(1);

    for (int i = 1; i < g_cpu_count; i++) {
        if (!g_cpus[i].online) {
            lapic_send_sipi(g_cpus[i].lapic_id, AP_BOOT_PHYS);
        }
    }

    /* Step 4: wait for each AP to set its online flag. */
    int up = 1;
    for (int i = 1; i < g_cpu_count; i++) {
        if (wait_for_ap_online(i, 5000)) {
            up++;
        } else {
            /* Diagnostic: did the trampoline even run? It stamps
             * 0xCAFE into 0x8200 as its first action. */
            uint16_t marker = *(volatile uint16_t *)(uintptr_t)(AP_BOOT_PHYS + 0x200);
            kprintf("[smp] AP%d failed to come up — SIPI marker = 0x%x %s\n",
                    i, (unsigned)marker,
                    marker == 0xCAFE ? "(trampoline ran but later step hung)"
                                     : "(SIPI didn't deliver)");
        }
    }

    kprintf("[smp] %d/%d CPU(s) online\n", up, g_cpu_count);
}
