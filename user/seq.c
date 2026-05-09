/*
 * seq — print integers, one per line.
 *
 *   seq 5         -> 1 2 3 4 5
 *   seq 3 7       -> 3 4 5 6 7
 *   seq 10 -1 6   -> not supported (no `step` arg today)
 *
 * The 1- and 2-arg forms are enough for the demo pipelines:
 *   seq 100 | head -3
 *   seq 5 10 | sort
 *
 * Negative or zero counts produce no output (seq 0 is empty, like
 * GNU). Counts are bounded by INT_MAX which atoi doesn't check —
 * passing wildly huge values is the caller's problem.
 */

#include "libuser.h"

int main(int argc, char **argv) {
    int lo = 1, hi = 0;
    if (argc == 2) {
        hi = atoi(argv[1]);
    } else if (argc == 3) {
        lo = atoi(argv[1]);
        hi = atoi(argv[2]);
    } else {
        sys_write(2, "seq: usage: seq N   |   seq LO HI\n", 34);
        return 2;
    }

    for (int i = lo; i <= hi; i++) {
        printf("%d\n", i);
    }
    return 0;
}
