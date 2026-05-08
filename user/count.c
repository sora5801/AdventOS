/*
 * Demo user program — counter using SYS_SLEEP_MS + SYS_TIME.
 * Counterpart of the in-kernel asm user_program_2.
 */

#include "libuser.h"

int main(void) {
    uint32_t start = sys_time();
    printf("Counter (epoch start = %u):\n  ", start);
    for (int i = 0; i < 5; i++) {
        printf("%d ", i);
        sys_sleep_ms(200);
    }
    uint32_t end = sys_time();
    printf("\n  elapsed = %u seconds\n", end - start);
    return 0;
}
