#ifndef ADVENTOS_MEMMAP_H
#define ADVENTOS_MEMMAP_H

#include "../include/types.h"

/* Layout written by boot/boot.S at linear 0x8000:
 *   uint32_t count;
 *   struct e820_entry entries[count];
 */
#define E820_BUFFER_ADDR 0x8000

#define E820_TYPE_USABLE       1
#define E820_TYPE_RESERVED     2
#define E820_TYPE_ACPI_RECLAIM 3
#define E820_TYPE_ACPI_NVS     4
#define E820_TYPE_BAD          5

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attrs;
} __attribute__((packed));

void                       memmap_init(void);
uint32_t                   memmap_count(void);
const struct e820_entry   *memmap_entry(uint32_t i);
const char                *memmap_type_name(uint32_t type);

/* Sum of all entries with type==USABLE. */
uint64_t                   memmap_total_usable(void);

/* Find the largest usable region whose end is above `min_addr`. The
 * returned size only counts the portion at or above min_addr. Returns
 * 0 if no such region exists. */
uint64_t                   memmap_largest_usable_above(uint64_t min_addr,
                                                       uint64_t *out_base);

#endif
