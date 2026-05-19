/*
 * Session 137 — stock hello world the polished tcc UX should compile.
 * Shipped as /hello.c.  No flags, no _start, no nothing — just
 * `tcc /hello.c -o /hello.elf`.
 */
#include <stdio.h>

int main(void) {
    printf("hi from tcc-compiled program\n");
    return 0;
}
