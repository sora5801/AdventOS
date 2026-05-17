/*
 * mmain.c — entry-point file for session 100's multi-file demo.
 *
 * Compile with:  cc /mmain.c /mlib.c -o /multi.elf
 *
 * Uses functions defined in /mlib.c (no prototypes; cc resolves
 * forward references across files via the existing call-fixup table).
 */

#include "mlib.h"

int main() {
    /* Call helpers defined in mlib.c. */
    int s;
    s = add(7, 35);
    printf("add(7, 35) = %d\n", s);                         /* 42 */
    printf("MULTIPLIER = %d\n", MULTIPLIER);                /* 100 */

    /* Use the struct type from mlib.h with a function that takes
     * a struct pointer (also from mlib.c). */
    struct point p;
    p.x = 3;
    p.y = 4;
    int d2;
    d2 = dot_self(&p);
    printf("dot_self((3,4)) = %d\n", d2);                   /* 25 */

    /* Use printf with a string defined as a const char* return. */
    char *name;
    name = banner();
    printf("banner = %s\n", name);                          /* AdventOS */

    return 0;
}
