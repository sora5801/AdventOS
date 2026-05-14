#include "pit.h"
#include "isr.h"
#include "pic.h"
#include "task.h"
#include "serial.h"
#include "signal.h"
#include "../include/io.h"

#define PIT_FREQ      1193182u
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

static volatile uint32_t ticks;
static uint32_t          ticks_per_sec;

static void pit_irq(struct registers *r) {
    (void)r;
    ticks++;

    /* Session 71: per-task CPU + wall-clock accounting.
     *
     * Charges this tick to whichever user task was on-CPU when the
     * IRQ fired. Kernel tasks (is_user == 0) are exempt — we don't
     * want to terminate the idle thread for "exceeding" its CPU
     * cap. Overrun queues SIGKILL into sig_pending; the existing
     * signal_check_and_deliver tail of irq_handler picks it up on
     * the iret-to-ring-3 path. We can't task_exit from IRQ context
     * directly — the BKL discipline and reaper interactions are
     * subtle — so the pending-signal route is the right shape.
     *
     * SIGKILL's default action is uncatchable terminate; the task
     * dies on the next return to ring 3, which happens immediately
     * after this handler since pit_irq runs in the preempted
     * context's stack. */
    struct task *cur = task_current();
    if (cur && cur->is_user) {
        if (cur->max_cpu_ticks) {
            cur->cur_cpu_ticks++;
            if (cur->cur_cpu_ticks >= cur->max_cpu_ticks) {
                cur->sig_pending |= (1u << SIGKILL);
            }
        }
        if (cur->wall_deadline_ticks &&
            ticks >= cur->wall_deadline_ticks) {
            cur->sig_pending |= (1u << SIGKILL);
        }
    }

    /* Session 67 wired the COM1 RX path to be IRQ-driven, but in
     * practice IRQ 4 never fires under our QEMU config (`-display
     * none -serial stdio` + the i440fx i8259 model). Bytes typed at
     * the host terminal land in the UART RBR but the i8259 doesn't
     * assert the line back to the CPU. Rather than fight QEMU's
     * device model, fall back to PIT-tick polling: 100 Hz means
     * worst-case 10 ms input latency, imperceptible for human typing
     * and a no-op for the 99% of ticks when the RBR is empty (one
     * `inb` of LSR). The IRQ handler stays installed so if we ever
     * migrate to a kernel/QEMU combo where it works, we get the
     * faster path for free. */
    serial_poll_once();
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
    /* Drop the BKL across the wait if we're inside a syscall. The
     * hlt itself doesn't acquire any kernel locks, but the BKL
     * blocks every other CPU from doing kernel work — bad if we
     * sleep multiple ticks. */
    extern int  bkl_held(void);
    extern void bkl_lock(void);
    extern void bkl_unlock(void);
    int held = bkl_held();
    if (held) bkl_unlock();
    while (ticks < target_ticks) {
        __asm__ volatile ("hlt");
    }
    if (held) bkl_lock();
}
