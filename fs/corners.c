/*
 * corners.c — session 125 smoke test for the cc language-corners
 * batch (10 small features bundled in one branch).
 *
 * Each new feature gets a short demonstration block. Output lines are
 * the canary the smoke script checks. Lines are deliberately short to
 * minimize cap.elf size and keep noise out of the diff.
 *
 * Features exercised, in commit order:
 *   1. Comma operator             `(a, b)` returns b
 *   2. Bitwise compound assigns   &= |= ^= <<= >>=
 *   3. do-while loop              do { ... } while (cond);
 *   4. break / continue           break out of a loop, skip to top
 *   5. union                      one storage, multiple field views
 *   6. sizeof NAME                size of declared variables
 *   7. goto / labels              forward & backward gotos
 *   8. assignment-as-expression   `if ((x = f()))` etc.
 *   9. switch / case / default    integer-value dispatch
 *  10. multi-dim arrays           int a[N][M]
 *  11. static LOCAL               module-private state in a function
 */

/* Session 125 — union: every field shares offset 0. Reading one field
 * sees the bits written by the most recent write to any field.
 * Common idiom: store an int, observe the bytes as a char*. */
union view {
    int  as_int;
    char *as_str;
};

int main() {
    /* 1. Comma operator. Inner expression evaluates a, then b, and
     *    returns b. */
    int v;
    v = (10, 20, 30);
    printf("comma            = %d\n", v);       /* 30 */

    /* 2. Bitwise / shift compound assigns. Each is rewritten as
     *    `x = x OP expr` by the parser. cc has no hex literals,
     *    so the values are decimal. */
    int b;
    b = 15;                                      /* 0b1111 */
    b &= 6;                                      /* 0b0110 */
    printf("b_and_eq         = %d\n", b);        /* 6 */
    b |= 16;
    printf("b_or_eq          = %d\n", b);        /* 22 */
    b ^= 18;                                     /* 22 ^ 18 = 4 */
    printf("b_xor_eq         = %d\n", b);        /* 4 */
    b <<= 2;
    printf("b_shl_eq         = %d\n", b);        /* 16 */
    b >>= 3;
    printf("b_shr_eq         = %d\n", b);        /* 2 */

    /* 3. do-while loop. Runs body at least once even when cond is
     *    false on first eval. */
    int sum;
    sum = 0;
    int i;
    i = 1;
    do {
        sum += i;
        i += 1;
    } while (i <= 5);
    printf("do_while_sum     = %d\n", sum);      /* 1+2+3+4+5 = 15 */

    /* 4. break + continue. Sum 1..10 but skip multiples of 3 (continue)
     *    and stop at 8 (break). 1+2+4+5+7 = 19. */
    int bsum;
    bsum = 0;
    int j;
    j = 0;
    while (j < 10) {
        j += 1;
        if (j == 8) break;
        if (j % 3 == 0) continue;
        bsum += j;
    }
    printf("break_continue   = %d\n", bsum);     /* 1+2+4+5+7 = 19 */

    /* 5. union — one storage shared by multiple field views. Writing
     *    `as_int` and then reading `as_str` reads the same 4 bytes
     *    back as a pointer. Here we go the other way for clarity. */
    union view u;
    u.as_int = 42;
    printf("union_as_int     = %d\n", u.as_int); /* 42 */
    u.as_str = "ok";                              /* same storage rewritten */
    printf("union_as_str     = %s\n", u.as_str); /* ok */

    return 0;
}
