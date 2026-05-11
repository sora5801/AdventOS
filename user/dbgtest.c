/*
 * dbgtest — a tiny target program for the session-57 debugger.
 *
 * It's deliberately structured to give the debugger interesting,
 * named entry points to break in:
 *
 *   square(int x)      — leaf compute function, easy to verify EAX
 *                         on return holds x*x.
 *   add(int a, int b)  — simple two-arg leaf, EAX = a + b.
 *   compute_loop(void) — iterates 5 times, calls both leaves, mutates
 *                         a `static volatile` counter so the debugger
 *                         can PEEK the data segment too.
 *
 * Argv:
 *   dbgtest            — runs compute_loop and prints the result.
 *   dbgtest trap       — issues `int $3` BEFORE compute_loop so a
 *                         debugger that PTRACE_ATTACHed (or just had
 *                         TRACEME set in this process) gets an entry
 *                         stop without needing to plant a breakpoint
 *                         in advance. Used by the t40 selftest as the
 *                         simplest possible "did the trap path work?"
 *                         check.
 *
 * The expected output (no debugger):
 *   dbgtest: counter=30 (expected 30)
 *
 * Why 30:
 *   square(0) + square(1) + ... + square(4) = 0+1+4+9+16 = 30.
 */

#include "libuser.h"

static volatile int g_counter = 0;

/* `noinline` is critical: at -O2 the compiler would otherwise inline
 * these tiny helpers into main, leaving the debugger nothing to break
 * on by name. With noinline they survive as standalone functions in
 * the .text section and show up in the .syms sidecar as T-class
 * entries (their globalness comes from removing `static` — file-local
 * statics that are noinline can still get elided by -O2's IPO. Making
 * them external puts a hard "address taken" wall in front of -O2's
 * passes). */
__attribute__((noinline))
int square(int x) {
    return x * x;
}

__attribute__((noinline))
int add(int a, int b) {
    return a + b;
}

__attribute__((noinline))
void compute_loop(void) {
    for (int i = 0; i < 5; i++) {
        int s = square(i);
        g_counter = add(g_counter, s);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1][0] == 't') {
        /* Entry trap — synchronous handoff to the debugger. */
        __asm__ volatile ("int $0x03");
    }

    compute_loop();

    printf("dbgtest: counter=%d (expected 30)\n", g_counter);
    return g_counter == 30 ? 0 : 1;
}
