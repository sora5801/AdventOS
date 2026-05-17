/* mlib.h — session 100 sample shared header. */
#ifndef MLIB_H
#define MLIB_H

#define MULTIPLIER 100

struct point {
    int x;
    int y;
};

/* No function prototypes — cc doesn't have them. Each file calls
 * functions defined in the other file directly; codegen patches the
 * cross-file `call rel32` via the same fixup machinery used for
 * intra-file forward references. */

#endif
