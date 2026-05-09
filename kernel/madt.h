#ifndef ADVENTOS_MADT_H
#define ADVENTOS_MADT_H

#include "../include/types.h"

/*
 * ACPI MADT (Multiple APIC Description Table) parsing — gives us the
 * list of CPUs (Local APIC entries) and IO-APICs the firmware found.
 * For SMP boot we only need the CPU list: count + each CPU's APIC ID.
 *
 * We discover the MADT by walking:
 *
 *   RSDP — Root System Description Pointer
 *      ↓ (lives in low memory, identified by the "RSD PTR " signature)
 *   RSDT or XSDT — Root System Description Table (32-bit pointers)
 *      ↓
 *   list of ACPI tables, including the MADT (signature "APIC")
 *
 * Once we've got the MADT we walk its variable-length entry list,
 * picking out type=0 (Local APIC) entries with the enabled flag set.
 * We cap at MADT_MAX_CPUS regardless of what ACPI reports.
 */

#define MADT_MAX_CPUS  8

struct madt_cpu {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    int      enabled;       /* MADT flags bit 0 */
};

void                    madt_init(void);
int                     madt_cpu_count(void);
const struct madt_cpu  *madt_cpu(int idx);

/* The IO APIC base — used by future IRQ routing work. Returns 0 if
 * no IO APIC entry was found. */
uint32_t                madt_ioapic_base(void);

#endif
