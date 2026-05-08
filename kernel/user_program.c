/*
 * Bundled ring-3 programs. Each one lives in its own short-named
 * section (.up1 / .up2 — kept <= 8 chars to dodge PE/COFF
 * truncation) so the kernel can copy its bytes into a freshly
 * mapped user page at runtime.
 *
 * Constraints (no relocations are done at "load" time):
 *   - naked: the function IS the inline asm (no prologue/epilogue)
 *   - PIC: every memory reference is built from the current EIP
 *     via `call/pop`, so the same bytes work at any virtual address
 *   - no globals, no external symbols
 */

#include "syscall.h"
#define STR(x)  #x
#define XSTR(x) STR(x)

/* ---- user_program_1 — "Hello from ring 3!" + getpid + yield ----------- */

__attribute__((section(".up1"), naked))
void user_program_1(void) {
    __asm__ volatile (
        "    call 1f                          \n\t"
        "1:  pop  %%ebx                       \n\t"
        "    add  $(msg1 - 1b), %%ebx         \n\t"
        "    mov  $" XSTR(SYS_WRITE_STR) ", %%eax \n\t"
        "    int  $0x80                       \n\t"

        "    mov  $" XSTR(SYS_GETPID) ", %%eax    \n\t"
        "    int  $0x80                       \n\t"
        "    mov  %%eax, %%edx                 \n\t"

        "    call 2f                          \n\t"
        "2:  pop  %%ebx                       \n\t"
        "    add  $(msg2 - 2b), %%ebx         \n\t"
        "    mov  $" XSTR(SYS_WRITE_STR) ", %%eax \n\t"
        "    int  $0x80                       \n\t"

        "    mov  %%edx, %%ebx                 \n\t"
        "    add  $0x30, %%ebx                 \n\t"
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"

        "    mov  $0x29, %%ebx                 \n\t"  /* ')' */
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"
        "    mov  $0x0A, %%ebx                 \n\t"  /* '\n' */
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"

        "    mov  $" XSTR(SYS_YIELD) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"

        "    call 3f                          \n\t"
        "3:  pop  %%ebx                       \n\t"
        "    add  $(msg3 - 3b), %%ebx         \n\t"
        "    mov  $" XSTR(SYS_WRITE_STR) ", %%eax \n\t"
        "    int  $0x80                       \n\t"

        "    mov  $" XSTR(SYS_EXIT) ", %%eax  \n\t"
        "    xor  %%ebx, %%ebx                 \n\t"
        "    int  $0x80                       \n\t"

        "4:  jmp 4b                           \n\t"

        "msg1: .ascii \"Hello from ring 3! \\0\"        \n\t"
        "msg2: .ascii \"(pid=\\0\"                       \n\t"
        "msg3: .ascii \"...woke back up after yield. exiting.\\n\\0\" \n\t"
        : : : "memory"
    );
}

/* ---- user_program_2 — counter using SYS_SLEEP_MS + SYS_TIME ---------- */

__attribute__((section(".up2"), naked))
void user_program_2(void) {
    __asm__ volatile (
        /* "Counter:" */
        "    call 1f                          \n\t"
        "1:  pop  %%ebx                       \n\t"
        "    add  $(prefix - 1b), %%ebx       \n\t"
        "    mov  $" XSTR(SYS_WRITE_STR) ", %%eax \n\t"
        "    int  $0x80                       \n\t"

        /* for (edi = 0; edi < 5; edi++) { write digit; sleep 200ms; } */
        "    xor  %%edi, %%edi                 \n\t"
        "5:                                    \n\t"
        "    mov  %%edi, %%ebx                 \n\t"
        "    add  $0x30, %%ebx                 \n\t"
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"
        "    mov  $200, %%ebx                  \n\t"
        "    mov  $" XSTR(SYS_SLEEP_MS) ", %%eax \n\t"
        "    int  $0x80                       \n\t"
        "    inc  %%edi                        \n\t"
        "    cmp  $5, %%edi                    \n\t"
        "    jl   5b                           \n\t"

        /* " (epoch=" */
        "    call 2f                          \n\t"
        "2:  pop  %%ebx                       \n\t"
        "    add  $(epoch_lbl - 2b), %%ebx    \n\t"
        "    mov  $" XSTR(SYS_WRITE_STR) ", %%eax \n\t"
        "    int  $0x80                       \n\t"

        /* time = SYS_TIME(); print as 10 decimal digits */
        "    mov  $" XSTR(SYS_TIME) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"
        "    mov  %%eax, %%esi                 \n\t"  /* esi = epoch */

        /* Push 10 decimal digits onto stack (low-to-high), then pop
         * and write them in order so the result is high-to-low. */
        "    mov  $10, %%ecx                   \n\t"
        "6:  xor  %%edx, %%edx                 \n\t"
        "    mov  $10, %%edi                   \n\t"
        "    mov  %%esi, %%eax                 \n\t"
        "    div  %%edi                        \n\t"  /* eax /= 10, edx = rem */
        "    mov  %%eax, %%esi                 \n\t"  /* esi = quotient */
        "    add  $0x30, %%edx                 \n\t"
        "    push %%edx                        \n\t"
        "    loop 6b                           \n\t"

        "    mov  $10, %%ecx                   \n\t"
        "7:  pop  %%ebx                        \n\t"
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"
        "    loop 7b                           \n\t"

        /* ")\n" */
        "    mov  $0x29, %%ebx                 \n\t"
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"
        "    mov  $0x0A, %%ebx                 \n\t"
        "    mov  $" XSTR(SYS_WRITE) ", %%eax  \n\t"
        "    int  $0x80                       \n\t"

        /* exit(0) */
        "    mov  $" XSTR(SYS_EXIT) ", %%eax  \n\t"
        "    xor  %%ebx, %%ebx                 \n\t"
        "    int  $0x80                       \n\t"

        "8:  jmp 8b                            \n\t"

        "prefix:    .ascii \"Counter: \\0\"           \n\t"
        "epoch_lbl: .ascii \" (epoch=\\0\"             \n\t"
        : : : "memory"
    );
}
