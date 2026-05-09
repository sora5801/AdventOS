#ifndef ADVENTOS_SMP_H
#define ADVENTOS_SMP_H

#include "../include/types.h"
#include "tss.h"

/*
 * SMP infrastructure (session 31).
 *
 *   smp_init() — runs on the BSP after paging + IDT + TSS are up.
 *     Parses MADT, maps the LAPIC, copies the AP trampoline to
 *     low memory, allocates a kernel stack and TSS for each AP,
 *     then sends INIT-SIPI-SIPI to wake them. Each AP runs the
 *     trampoline, comes up in pmode + paging, jumps into ap_entry.
 *
 *   ap_entry(my_id) — the C entry point each AP calls after the
 *     trampoline finishes. Records the CPU's metadata, becomes
 *     online, then dispatches to the scheduler.
 *
 *   cpu_local() — get the currently-running CPU's per-CPU struct.
 *     Reads the LAPIC ID and indexes the global table.
 *
 * The current task pointer is per-CPU (each CPU runs a different
 * task at a time). g_current is gone — task_current() now reads
 * cpu_local()->current.
 */

#define MAX_CPUS  8

struct task;     /* forward */

struct cpu_local {
    int           online;            /* 1 once ap_entry has run */
    uint32_t      cpu_id;            /* logical id (= madt index) */
    uint32_t      lapic_id;
    uint32_t      kernel_stack_top;  /* loaded into TSS.esp0 on syscall */
    void         *idle_stack;        /* bottom of kernel stack alloc */
    struct task  *current;           /* task running on THIS cpu */
    struct tss    tss;               /* per-CPU TSS, GDT entry per cpu */
    uint16_t      tss_selector;      /* GDT selector for the above */
};

void                smp_init(void);
int                 smp_cpu_count(void);
struct cpu_local   *cpu_local(void);
struct cpu_local   *cpu_at(int idx);

/* Called from the AP trampoline. Must NOT return. */
void                ap_entry(uint32_t my_id);

#endif
