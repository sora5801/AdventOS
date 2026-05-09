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
    /*
     * Stack on entry from the kernel:
     *   [esp+0]            argc
     *   [esp+4]            argv[0]   (pointer to NUL-terminated string)
     *   [esp+8]            argv[1]
     *   ...
     *   [esp+4*(argc+1)]   NULL terminator
     *   ...               (argv strings further up)
     *
     * We convert that layout into a cdecl call:  main(argc, argv).
     */
    movl    (%esp), %eax            /* argc            */
    leal    4(%esp), %ebx           /* argv (= &esp[1]) */
    pushl   %ebx                    /* arg 2: argv     */
    pushl   %eax                    /* arg 1: argc     */
    call    _main

    /* main returned; call sys_exit(main's return value). EAX has it. */
    pushl   %eax
    call    _sys_exit

    /* sys_exit is __attribute__((noreturn)); guard anyway */
1:  hlt
    jmp     1b
