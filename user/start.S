/*
 * User-space startup. Linked first into every user binary so its bytes
 * land at the start of .text (= entry virtual address). The kernel
 * iret's into _start with ESP = top of the user stack page.
 *
 * We just call main() and forward its return value to sys_exit().
 *
 * The lone .startup section is what the linker script picks up first
 * (kept ≤ 8 chars to dodge PE/COFF section-name truncation).
 */

.code32
.section .startup, "ax"
.global _start

_start:
    /* mingw32 PE: C symbols get a leading underscore, so `int main(void)`
     * is `_main`, `void sys_exit(int)` is `_sys_exit`. Stack arrives at
     * user_esp = USER_STACK_VA + 4 KiB (16-byte aligned).  call → ESP%16
     * = 12 inside the callee, the i386 SysV ABI invariant. */
    call    _main

    /* push EAX (= main's return value) and call sys_exit(code) */
    push    %eax
    call    _sys_exit

    /* sys_exit is __attribute__((noreturn)); guard anyway */
1:  hlt
    jmp     1b
