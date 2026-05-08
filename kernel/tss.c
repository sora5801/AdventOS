#include "tss.h"
#include "string.h"

static struct tss g_tss;

void tss_init(void) {
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ss0        = 0x10;          /* kernel data selector */
    g_tss.esp0       = 0;             /* set per-task before resuming a ring-3 task */
    /* iomap_base past the end disables the I/O permission bitmap entirely */
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);
}

void tss_set_kernel_stack(uint32_t esp) {
    g_tss.ss0  = 0x10;
    g_tss.esp0 = esp;
}

struct tss *tss_get(void) { return &g_tss; }
size_t      tss_size(void) { return sizeof(g_tss); }
