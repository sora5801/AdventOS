/*
 * fnptr.c — session 98 smoke test for function pointers in cc.
 *
 * Demonstrates:
 *   int *fp;                     a function pointer (stored as int*)
 *   fp = my_func;                bare function name decays to VA
 *   fp(args);                    indirect call via variable
 *   &my_func                     explicit address-of (same as bare name)
 *   dispatch tables via int *globals
 */

int square(int n) { return n * n; }
int cube  (int n) { return n * n * n; }
int neg   (int n) { return -n; }
int twice (int n) { return n + n; }

/* Apply a unary int function to each arg and print the result. */
int apply_print(int *fn, int x) {
    printf("  f(%d) = %d\n", x, fn(x));
    return 0;
}

/* A dispatch table — an int array holding function addresses
 * (cc treats function VAs and ints interchangeably). */
int table[4];

int main() {
    /* Direct: store a function in a local, call through it. */
    int *fp;
    fp = square;
    printf("square(7) = %d\n", fp(7));         /* 49 */
    fp = cube;
    printf("cube(3)   = %d\n", fp(3));         /* 27 */

    /* Equivalent via explicit address-of. */
    fp = &neg;
    printf("neg(11)   = %d\n", fp(11));        /* -11 */

    /* Pass a function pointer to a callback-style function. */
    apply_print(twice, 21);                    /* f(21) = 42 */
    apply_print(square, 6);                    /* f(6) = 36 */

    /* Dispatch via a global table. The parser only allows calls
     * after a NAME (not after an index expression), so we load each
     * entry into a local first. */
    table[0] = square;
    table[1] = cube;
    table[2] = neg;
    table[3] = twice;
    int i;
    int *f;
    i = 0;
    while (i < 4) {
        f = table[i];
        printf("table[%d](5) = %d\n", i, f(5));
        i = i + 1;
    }
    /* expected:
     *   table[0](5) = 25
     *   table[1](5) = 125
     *   table[2](5) = -5
     *   table[3](5) = 10
     */

    return 0;
}
