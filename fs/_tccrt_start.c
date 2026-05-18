/*
 * AdventOS tcc runtime — _start.
 *
 * Shipped as /tcc/lib/start.c.  tcc passes it as the first source
 * file when the wrapper /tcc.elf invokes the underlying compiler.
 *
 * On entry the kernel pushes:
 *     (esp)        argc
 *     4(esp)       argv[0]
 *     ...
 *     4*(argc+1)(esp)  NULL terminator
 *
 * We convert that to a cdecl call to main(argc, argv) and forward
 * the return value to sys_exit (int 0x80, eax=3, ebx=code).
 *
 * Everything lives in one top-level inline-asm block so tcc can
 * emit it exactly as written — no compiler-generated prologue to
 * disturb (%esp).
 */

__asm__(
    ".text                            \n"
    ".global _start                   \n"
    "_start:                          \n"
    "    movl  (%esp), %eax           \n"   /* argc           */
    "    leal  4(%esp), %ebx          \n"   /* argv = &esp[1] */
    "    pushl %ebx                   \n"   /* arg 2          */
    "    pushl %eax                   \n"   /* arg 1          */
    "    call  main                   \n"
    "    movl  %eax, %ebx             \n"   /* exit code      */
    "    movl  $3,   %eax             \n"   /* SYS_EXIT       */
    "    int   $0x80                  \n"
    "1:  hlt                          \n"
    "    jmp   1b                     \n"
);
