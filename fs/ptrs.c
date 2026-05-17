/*
 * ptrs.c — session 99 smoke test for scaled pointer arithmetic + sizeof.
 *
 * Demonstrates:
 *   sizeof(int) / sizeof(char) / sizeof(struct point) / sizeof(T*)
 *   int *p; p + 1                advances 4 bytes (was 1 before s99)
 *   p++                          advances 4 bytes for int* / 1 for char*
 *   *(p + i)                     equivalent to p[i] for int*
 */

struct point { int x; int y; };

int main() {
    /* sizeof folded at compile time → just an int literal in eax. */
    printf("sizeof(int)            = %d\n", sizeof(int));            /* 4 */
    printf("sizeof(char)           = %d\n", sizeof(char));           /* 1 */
    printf("sizeof(int *)          = %d\n", sizeof(int *));          /* 4 */
    printf("sizeof(char *)         = %d\n", sizeof(char *));         /* 4 */
    printf("sizeof(struct point)   = %d\n", sizeof(struct point));   /* 8 */
    printf("sizeof(struct point *) = %d\n", sizeof(struct point *)); /* 4 */

    /* Walk an int array via pointer ++. */
    int v[5];
    v[0] = 10; v[1] = 20; v[2] = 30; v[3] = 40; v[4] = 50;
    int *ip;
    ip = v;
    int sum;
    sum = 0;
    int i;
    i = 0;
    while (i < 5) {
        sum += *ip;
        ip++;                       /* advances sizeof(int) = 4 bytes */
        i++;
    }
    printf("int-walk sum  = %d\n", sum);                /* 150 */

    /* Walk a char string via pointer ++. */
    char s[16];
    s[0] = 'h'; s[1] = 'i'; s[2] = '!'; s[3] = 0;
    char *cp;
    cp = s;
    int n;
    n = 0;
    while (*cp != 0) {
        n++;
        cp++;                       /* advances sizeof(char) = 1 byte */
    }
    printf("char-walk len = %d\n", n);                  /* 3 */

    /* Pointer math via + N (scaled by sizeof element). */
    ip = v;
    printf("v[3] via *(ip+3) = %d\n", *(ip + 3));       /* 40 */
    printf("v[0] via *ip     = %d\n", *ip);             /* 10 */

    /* p - n (also scaled). */
    int *iq;
    iq = ip + 4;
    iq = iq - 2;
    printf("v[?] via shifted iq = %d\n", *iq);          /* v[2] = 30 */

    /* sizeof used in code paths. */
    int arr_bytes;
    arr_bytes = 5 * sizeof(int);
    printf("5 ints = %d bytes\n", arr_bytes);           /* 20 */

    return 0;
}
