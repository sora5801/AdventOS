#include "tss.h"
#include "string.h"
#include "smp.h"

/* Per-CPU TSS storage — APs allocate their own via smp_init's
 * gdt_add_tss path; the BSP gets g_tss here for the GDT entry that
 * gdt_init installs at selector 0x28. tss_set_kernel_stack reads
 * cpu_local() to find the right TSS for the calling CPU.
 *
 * Hardware uses TR (loaded once at boot) to know which TSS to read
 * for ring-3-to-ring-0 transitions. Each CPU's TR is loaded with
 * its own TSS selector (BSP: 0x28; APs: dynamically-allocated slots
 * gdt_add_tss returns). The kernel's job is to keep the TSS that
 * each CPU's TR points at up to date with the current task's
 * kernel_stack_top — that's what tss_set_kernel_stack does. */
static struct tss g_tss;

void tss_init(void) {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ss0        = 0x10;          /* kernel data selector */
    g_tss.esp0       = 0;             /* set per-task before resuming a ring-3 task */
    /* iomap_base past the end disables the I/O permission bitmap entirely */
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);
}

/* Update the running CPU's TSS.esp0 so the next ring-3 -> ring-0
 * entry on this CPU lands on the supplied kernel stack.
 *
 * cpu_local() reads LAPIC MMIO. Pre-SMP that's unmapped, so we'd
 * page-fault. The task layer exposes g_smp_ready via task_smp_ready;
 * before that flag is set, we know we're on the BSP and should
 * write g_tss directly. After SMP is up we read cpu_local() and
 * dispatch to BSP-vs-AP storage. */
extern volatile int g_smp_ready;
void tss_set_kernel_stack(uint32_t esp) {
    if (!g_smp_ready) {
        g_tss.ss0  = 0x10;
        g_tss.esp0 = esp;
        return;
    }
    struct cpu_local *cpu = cpu_local();
    if (cpu->cpu_id == 0) {
        g_tss.ss0  = 0x10;
        g_tss.esp0 = esp;
    } else {
        cpu->tss.ss0  = 0x10;
        cpu->tss.esp0 = esp;
    }
}

struct tss *tss_get(void) { return &g_tss; }
size_t      tss_size(void) { return sizeof(g_tss); }
