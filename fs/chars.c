/*
 * chars.c — session 92 smoke test for cc char/pointer/array support.
 *
 * Demonstrates:
 *   - char buf[N]              local char array
 *   - buf[i] = ch              byte-store via indexing
 *   - ch = buf[i]              byte-load via indexing
 *   - char *p                  char* parameter
 *   - *p                       byte-dereference
 *   - 'A' literal              char literal lexed as T_NUM
 *   - puts(buf)                passing a char[] as char*
 */

/* strlen: count bytes until NUL. Demonstrates char* param + *p deref. */
int my_strlen(char *s) {
    int n;
    n = 0;
    while (*s != 0) {
        n = n + 1;
        s = s + 1;
    }
    return n;
}

/* strcpy: copy bytes from src to dst until (and including) the NUL.
 * Returns nothing useful — both pointer args carry the result. */
int my_strcpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] != 0) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = 0;
    return i;
}

int main() {
    /* Build "Hi!\n" byte-by-byte and print it. */
    char buf[16];
    buf[0] = 'H';
    buf[1] = 'i';
    buf[2] = '!';
    buf[3] = 0;
    puts(buf);                  /* expects: Hi! */

    /* my_strlen("hello, world") via passing a string literal. */
    print_int(my_strlen("hello, world"));    /* 12 */

    /* my_strcpy to demonstrate pointer-out args. */
    char dst[32];
    my_strcpy(dst, "copied via my_strcpy");
    puts(dst);                  /* copied via my_strcpy */

    /* Echo argv[0]-ish: use a known buffer. */
    char greet[8];
    greet[0] = 'h'; greet[1] = 'e'; greet[2] = 'y';
    greet[3] = '\n'; greet[4] = 0;
    print_str(greet);           /* hey\n  (print_str doesn't add a newline) */

    /* int array sanity. */
    int v[4];
    v[0] = 100; v[1] = 200; v[2] = 300; v[3] = 400;
    print_int(v[0] + v[3]);     /* 500 */
    print_int(v[1] * v[2]);     /* 60000 */

    return 99;
}
