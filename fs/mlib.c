/*
 * mlib.c — companion to /mmain.c for session 100's multi-file demo.
 *
 * Defines functions called from /mmain.c. The shared `struct point`
 * definition comes from /mlib.h; both files include it, and the
 * header guard ensures the second include is a no-op.
 */

#include "mlib.h"

int add(int a, int b) {
    return a + b;
}

int dot_self(struct point *p) {
    return p->x * p->x + p->y * p->y;
}

/* String literal that lives in this file's pool; returned to main. */
char *banner() {
    return "AdventOS";
}
