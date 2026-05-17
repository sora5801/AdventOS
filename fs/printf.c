/*
 * printf.c — session 94 smoke test for cc's printf intrinsic.
 *
 * Demonstrates:
 *   %d   decimal int (signed)
 *   %s   char* (NUL-terminated)
 *   %c   single byte (low 8 bits of an int)
 *   %x   lowercase hex (treats arg as unsigned)
 *   %%   literal %
 *   plain text + escapes
 */

int main() {
    /* The headline use case. */
    printf("hello, %s\n", "world");

    /* Mixed types. */
    int n;
    n = 42;
    printf("n = %d\n", n);
    printf("ten percent of %d is %d\n", 100, 10);

    /* Single character. */
    printf("first letter: %c\n", 'H');
    printf("count: %c %c %c\n", '1', '2', '3');

    /* Hex. */
    printf("decimal 255 in hex is %x\n", 255);
    printf("VA 0x%x maps the user code\n", 1073741824);   /* 0x40000000 */
    printf("-1 as hex = %x\n", -1);                       /* ffffffff */

    /* Literal %%. */
    printf("100%% done!\n");

    /* Several %d in one line. */
    printf("(%d, %d, %d) — done.\n", 1, 2, 3);

    /* Returning a meaningful exit code. */
    return 17;
}
