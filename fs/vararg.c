/*
 * vararg.c — session 105 smoke test for user-defined variadic functions.
 *
 * Demonstrates:
 *   int foo(int n, ...);          variadic declaration via `...`
 *   va_start(ap, last_named);     initialise the arg walker
 *   va_arg(ap);                   read next int-sized arg, advance ap
 *   va_end(ap);                   no-op (matches real C signature)
 *
 * The user-level idiom: declare `int ap;` as the walker (cc has no
 * separate va_list type), then use va_start/va_arg/va_end.
 */

/* Sum a variable number of int args. The first param `n` says how
 * many follow. */
int sum_n(int n, ...) {
    int ap;
    va_start(ap, n);
    int total;
    total = 0;
    int i;
    i = 0;
    while (i < n) {
        total = total + va_arg(ap);
        i = i + 1;
    }
    va_end(ap);
    return total;
}

/* Print N integers labeled "[i]: V" one per line. */
int dump_n(int n, ...) {
    int ap;
    va_start(ap, n);
    int i;
    i = 0;
    while (i < n) {
        int v;
        v = va_arg(ap);
        printf("  [%d]: %d\n", i, v);
        i = i + 1;
    }
    va_end(ap);
    return 0;
}

int main() {
    printf("sum_n(3, 10, 20, 30) = %d\n", sum_n(3, 10, 20, 30));    /* 60 */
    printf("sum_n(5, 1, 2, 3, 4, 5) = %d\n", sum_n(5, 1, 2, 3, 4, 5));/* 15 */
    printf("sum_n(0) = %d\n", sum_n(0));                            /* 0 */

    printf("dump_n(4, 100, 200, 300, 400):\n");
    dump_n(4, 100, 200, 300, 400);
    /* expects:
     *   [0]: 100
     *   [1]: 200
     *   [2]: 300
     *   [3]: 400 */

    return sum_n(2, 11, 31);    /* exit 42 */
}
