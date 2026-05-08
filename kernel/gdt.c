#include "gdt.h"
#include "tss.h"
#include "string.h"

#define GDT_NUM_ENTRIES 6

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[GDT_NUM_ENTRIES];
static struct gdt_ptr   gdtr;

extern void gdt_flush(uint32_t);

static void set_gate(int n, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    gdt[n].base_low         = (uint16_t)(base & 0xFFFF);
    gdt[n].base_mid         = (uint8_t)((base >> 16) & 0xFF);
    gdt[n].base_high        = (uint8_t)((base >> 24) & 0xFF);
    gdt[n].limit_low        = (uint16_t)(limit & 0xFFFF);
    gdt[n].flags_limit_high = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[n].access           = access;
}

void gdt_init(void) {
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint32_t)(uintptr_t)&gdt;

    set_gate(0, 0, 0, 0, 0);                           /* Null */
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xC0); /* 0x08: Kernel code, ring 0 */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC0); /* 0x10: Kernel data, ring 0 */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xC0); /* 0x18: User   code, ring 3 */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0); /* 0x20: User   data, ring 3 */

    /* 0x28: TSS — type 0x9 (available 32-bit TSS), DPL=0, S=0, P=1 */
    uint32_t tss_base  = (uint32_t)(uintptr_t)tss_get();
    uint32_t tss_limit = (uint32_t)tss_size() - 1;
    gdt[5].limit_low        = (uint16_t)(tss_limit & 0xFFFF);
    gdt[5].base_low         = (uint16_t)(tss_base & 0xFFFF);
    gdt[5].base_mid         = (uint8_t)((tss_base >> 16) & 0xFF);
    gdt[5].access           = 0x89;
    gdt[5].flags_limit_high = (uint8_t)((tss_limit >> 16) & 0x0F); /* G=0 (byte gran) */
    gdt[5].base_high        = (uint8_t)((tss_base >> 24) & 0xFF);

    gdt_flush((uint32_t)(uintptr_t)&gdtr);

    /* Load Task Register with the TSS selector. RPL must match the TSS's DPL=0. */
    __asm__ volatile ("ltr %%ax" :: "a"(0x28));
}
