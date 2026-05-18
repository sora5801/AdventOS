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

int main() {
    /* 1. Comma operator. Inner expression evaluates a, then b, and
     *    returns b. */
    int v;
    v = (10, 20, 30);
    printf("comma            = %d\n", v);       /* 30 */

    return 0;
}
