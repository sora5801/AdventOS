#include "memmap.h"

#define MAX_ENTRIES 64

static uint32_t                 g_count;
static const struct e820_entry *g_entries;

void memmap_init(void) {
    uint32_t n = *(volatile uint32_t *)E820_BUFFER_ADDR;
    if (n > MAX_ENTRIES) n = 0;          /* sanity */
    g_count   = n;
    g_entries = (const struct e820_entry *)(E820_BUFFER_ADDR + 4);
}

uint32_t memmap_count(void) {
    return g_count;
}

const struct e820_entry *memmap_entry(uint32_t i) {
    if (i >= g_count) return (const struct e820_entry *)0;
    return &g_entries[i];
}

const char *memmap_type_name(uint32_t type) {
    switch (type) {
        case E820_TYPE_USABLE:       return "usable";
        case E820_TYPE_RESERVED:     return "reserved";
        case E820_TYPE_ACPI_RECLAIM: return "ACPI reclaim";
        case E820_TYPE_ACPI_NVS:     return "ACPI NVS";
        case E820_TYPE_BAD:          return "bad";
        default:                     return "unknown";
    }
}

uint64_t memmap_total_usable(void) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_entries[i].type == E820_TYPE_USABLE) {
            total += g_entries[i].length;
        }
    }
    return total;
}

uint64_t memmap_largest_usable_above(uint64_t min_addr, uint64_t *out_base) {
    uint64_t best_size = 0;
    uint64_t best_base = 0;

    for (uint32_t i = 0; i < g_count; i++) {
        const struct e820_entry *e = &g_entries[i];
        if (e->type != E820_TYPE_USABLE) continue;

        uint64_t base = e->base;
        uint64_t end  = e->base + e->length;
        if (end <= min_addr) continue;
        if (base < min_addr) base = min_addr;

        uint64_t size = end - base;
        if (size > best_size) {
            best_size = size;
            best_base = base;
        }
    }

    if (out_base) *out_base = best_base;
    return best_size;
}
