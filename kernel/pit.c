#include "pit.h"
#include "isr.h"
#include "pic.h"
#include "task.h"
#include "../include/io.h"

#define PIT_FREQ      1193182u
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

static volatile uint32_t ticks;
static uint32_t          ticks_per_sec;

static void pit_irq(struct registers *r) {
    (void)r;
    ticks++;
    /* Round-robin preemption. schedule() is a no-op until task_init
     * runs, so it's safe to invoke from boot onward. */
    schedule();
}

void pit_init(uint32_t hz) {
    ticks         = 0;
    ticks_per_sec = hz;

    uint32_t divisor = PIT_FREQ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    /* Channel 0, lobyte/hibyte, mode 3 (square wave), binary */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    isr_register_irq(0, pit_irq);
    pic_clear_mask(0);
}

uint32_t pit_ticks(void) { return ticks; }

uint32_t pit_seconds(void) {
    if (ticks_per_sec == 0) return 0;
    return ticks / ticks_per_sec;
}

void pit_sleep(uint32_t ms) {
    if (ticks_per_sec == 0) return;
    uint32_t target_ticks = ticks + (ms * ticks_per_sec) / 1000u;
    while (ticks < target_ticks) {
        __asm__ volatile ("hlt");
    }
}
