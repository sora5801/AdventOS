/*
 * Demo user program — written in real C, no inline asm.
 * Counterpart of the in-kernel asm user_program_1.
 */

#include "libuser.h"

static const char *greet =
    "Hello from C in ring 3! This program uses libuser's printf.\n";

int main(void) {
    int pid = sys_getpid();

    puts(greet);
    printf("  pid     : %d\n", pid);
    printf("  hello   : %s\n", "yes, I am a real C program");
    printf("  hex pid : 0x%x\n", (uint32_t)pid);

    printf("yielding...\n");
    sys_yield();
    printf("...woke back up. exiting.\n");

    return 0;
}
