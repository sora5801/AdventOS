/*
 * ops.c — session 96 smoke test for compound operators in cc.
 *
 * Demonstrates:
 *   +=  -=  *=  /=  %=     compound assignments on locals + globals
 *   ++x   --x              prefix increment/decrement (lvalue result)
 *   x++   x--              postfix increment/decrement (old-value result)
 *   c ? a : b              ternary
 */

int counter;

int main() {
    int x;
    x = 10;

    x += 5;   printf("x += 5  -> %d\n", x);          /* 15 */
    x -= 3;   printf("x -= 3  -> %d\n", x);          /* 12 */
    x *= 2;   printf("x *= 2  -> %d\n", x);          /* 24 */
    x /= 4;   printf("x /= 4  -> %d\n", x);          /* 6 */
    x %= 4;   printf("x %% 4  -> %d\n", x);          /* 2 */

    /* Prefix and postfix. */
    int y;
    y = 10;
    printf("y    = %d\n", y);                         /* 10 */
    printf("++y  = %d\n", ++y);                       /* 11 — new value */
    printf("y    = %d\n", y);                         /* 11 */
    printf("y++  = %d\n", y++);                       /* 11 — old value */
    printf("y    = %d\n", y);                         /* 12 */
    printf("--y  = %d\n", --y);                       /* 11 */
    printf("y--  = %d\n", y--);                       /* 11 */
    printf("y    = %d\n", y);                         /* 10 */

    /* Compound on a global. */
    counter = 100;
    counter += 7;
    counter *= 2;
    printf("counter = %d\n", counter);                /* 214 */

    /* Ternary. */
    int a;
    int b;
    a = 5;
    b = 9;
    int m;
    m = a > b ? a : b;
    printf("max(%d, %d) = %d\n", a, b, m);            /* 9 */
    printf("min(%d, %d) = %d\n", a, b, a < b ? a : b);/* 5 */

    /* Ternary in a larger expression. */
    int abs_neg;
    abs_neg = -42 < 0 ? -(-42) : -42;
    printf("|-42|     = %d\n", abs_neg);              /* 42 */

    /* Combine ++/-- with other ops. */
    int n;
    n = 0;
    int total;
    total = 0;
    while (n < 5) {
        total += n++;        /* add current n, THEN advance */
    }
    printf("sum 0..4  = %d\n", total);                /* 0+1+2+3+4 = 10 */

    return 0;
}
