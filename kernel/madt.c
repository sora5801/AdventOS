#include "madt.h"
#include "string.h"
#include "kprintf.h"

/* Packed ACPI table headers. */
struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " — note trailing space */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ extends with length/xsdt_addr/etc — we only use rev 1. */
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;
    /* variable-length entries follow */
} __attribute__((packed));

struct madt_entry_lapic {
    uint8_t  type;          /* 0 */
    uint8_t  length;        /* 8 */
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;         /* bit 0 = enabled */
} __attribute__((packed));

struct madt_entry_ioapic {
    uint8_t  type;          /* 1 */
    uint8_t  length;        /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

/* ---- discovery -------------------------------------------------- */

static struct madt_cpu g_cpus[MADT_MAX_CPUS];
static int             g_cpu_count = 0;
static uint32_t        g_ioapic_base = 0;

/* Search the BIOS ROM area 0xE0000-0xFFFFF for "RSD PTR " on a
 * 16-byte boundary. We skip the EBDA search (which would need
 * reading the BDA pointer at physical 0x40E) because page 0 is
 * intentionally unmapped — it's our null-deref guard. QEMU/SeaBIOS
 * always plants the RSDP in the ROM range, so a single window
 * sweep is enough. */
static const struct acpi_rsdp *find_rsdp(void) {
    for (uintptr_t p = 0xE0000; p < 0x100000; p += 16) {
        if (memcmp((const void *)p, "RSD PTR ", 8) == 0) {
            return (const struct acpi_rsdp *)p;
        }
    }
    return 0;
}

static void parse_madt(const struct acpi_madt *madt) {
    g_cpu_count   = 0;
    g_ioapic_base = 0;

    const uint8_t *p   = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;

    while (p < end) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2) break;

        if (type == 0 /* Local APIC */) {
            const struct madt_entry_lapic *e = (const struct madt_entry_lapic *)p;
            int enabled = (e->flags & 1u) != 0;
            if (enabled && g_cpu_count < MADT_MAX_CPUS) {
                g_cpus[g_cpu_count].apic_id = e->apic_id;
                g_cpus[g_cpu_count].acpi_id = e->acpi_id;
                g_cpus[g_cpu_count].enabled = 1;
                g_cpu_count++;
            }
        } else if (type == 1 /* IO APIC */) {
            const struct madt_entry_ioapic *e = (const struct madt_entry_ioapic *)p;
            if (g_ioapic_base == 0) g_ioapic_base = e->address;
        }
        p += len;
    }
}

void madt_init(void) {
    const struct acpi_rsdp *rsdp = find_rsdp();
    if (!rsdp) {
        kputs("madt: RSDP not found; assuming uniprocessor\n");
        return;
    }

    const struct acpi_sdt_header *rsdt =
        (const struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_addr;
    if (memcmp(rsdt->signature, "RSDT", 4) != 0) {
        kputs("madt: RSDT signature mismatch\n");
        return;
    }

    int n = (rsdt->length - sizeof(*rsdt)) / 4;
    const uint32_t *entries = (const uint32_t *)(rsdt + 1);
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *t =
            (const struct acpi_sdt_header *)(uintptr_t)entries[i];
        if (memcmp(t->signature, "APIC", 4) == 0) {
            parse_madt((const struct acpi_madt *)t);
            break;
        }
    }

    kprintf("madt: %d CPU(s) found", g_cpu_count);
    for (int i = 0; i < g_cpu_count; i++) {
        kprintf("%s apic_id=%u",
                i == 0 ? ":" : ",",
                (unsigned)g_cpus[i].apic_id);
    }
    if (g_ioapic_base) {
        kprintf("  ioapic@0x%x", (unsigned)g_ioapic_base);
    }
    kputs("\n");
}

int madt_cpu_count(void) { return g_cpu_count; }

const struct madt_cpu *madt_cpu(int idx) {
    if (idx < 0 || idx >= g_cpu_count) return 0;
    return &g_cpus[idx];
}

uint32_t madt_ioapic_base(void) { return g_ioapic_base; }
