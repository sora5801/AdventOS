#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "string.h"

/* GDT layout:
 *   0x00: null
 *   0x08: kernel code (ring 0)
 *   0x10: kernel data (ring 0)
 *   0x18: user code (ring 3)
 *   0x20: user data (ring 3)
 *   0x28: BSP TSS
 *   0x30..: AP TSS entries (one per AP, allocated by smp_init)
 *
 * Total slots: 5 fixed + MAX_CPUS for per-CPU TSS. Sized
 * generously so smp_init can append AP TSSes without reallocating. */
#define GDT_FIXED_ENTRIES  5
#define GDT_MAX_TSS        8
#define GDT_NUM_ENTRIES    (GDT_FIXED_ENTRIES + GDT_MAX_TSS)

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
static int              g_next_tss_slot = GDT_FIXED_ENTRIES;   /* slot 5 first */

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

/* Add a TSS descriptor at slot `n` pointing at `tss_base` with
 * `tss_limit`. Type is 0x9 (32-bit available TSS), DPL=0, S=0, P=1. */
static void set_tss_entry(int n, uint32_t tss_base, uint32_t tss_limit) {
    gdt[n].limit_low        = (uint16_t)(tss_limit & 0xFFFF);
    gdt[n].base_low         = (uint16_t)(tss_base & 0xFFFF);
    gdt[n].base_mid         = (uint8_t)((tss_base >> 16) & 0xFF);
    gdt[n].access           = 0x89;
    gdt[n].flags_limit_high = (uint8_t)((tss_limit >> 16) & 0x0F);
    gdt[n].base_high        = (uint8_t)((tss_base >> 24) & 0xFF);
}

void gdt_init(void) {
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint32_t)(uintptr_t)&gdt;

    /* Clear ALL slots so unused TSS entries are non-present. */
    memset(gdt, 0, sizeof(gdt));

    set_gate(0, 0, 0, 0, 0);                           /* Null */
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xC0); /* 0x08: Kernel code, ring 0 */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC0); /* 0x10: Kernel data, ring 0 */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xC0); /* 0x18: User   code, ring 3 */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0); /* 0x20: User   data, ring 3 */

    /* 0x28: BSP TSS — reserves slot 5 for the bootstrap CPU. */
    uint32_t tss_base  = (uint32_t)(uintptr_t)tss_get();
    uint32_t tss_limit = (uint32_t)tss_size() - 1;
    set_tss_entry(GDT_FIXED_ENTRIES, tss_base, tss_limit);
    g_next_tss_slot = GDT_FIXED_ENTRIES + 1;

    gdt_flush((uint32_t)(uintptr_t)&gdtr);

    /* Load Task Register with the BSP TSS selector. */
    __asm__ volatile ("ltr %%ax" :: "a"(0x28));
}

void gdt_load_for_ap(void) {
    /* The trampoline already loaded a temporary GDT to enter pmode;
     * we replace it now with the kernel's master GDT. The flush
     * helper does lgdt + a far jump to reload CS, then reloads
     * data segments. Same machinery the BSP used in gdt_init. */
    gdt_flush((uint32_t)(uintptr_t)&gdtr);
}

uint16_t gdt_add_tss(struct tss *tss) {
    if (g_next_tss_slot >= GDT_NUM_ENTRIES) return 0;
    int slot = g_next_tss_slot++;

    uint32_t base  = (uint32_t)(uintptr_t)tss;
    /* tss_size() at boot is sizeof(struct tss); same applies here. */
    uint32_t limit = (uint32_t)sizeof(struct tss) - 1;
    set_tss_entry(slot, base, limit);
    return (uint16_t)(slot * 8);
}

void idt_load_for_ap(void) {
    /* The IDT is shared between BSP and APs — same handlers, same
     * vectors. Just point the AP's IDTR at it. */
    extern void idt_install(void);     /* declared in idt.c */
    /* idt_install() sets up entries; idr is already loaded.
     * We just need to lidt the same descriptor. */
    extern uintptr_t idt_descriptor_addr(void);
    uintptr_t idtr_addr = idt_descriptor_addr();
    __asm__ volatile ("lidt (%0)" :: "r"(idtr_addr) : "memory");
}
