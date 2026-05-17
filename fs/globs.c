/*
 * globs.c — session 93 smoke test for global variables in cc.
 *
 * Demonstrates:
 *   - int globals with initializer:  int counter = 100;
 *   - int globals defaulting to 0:   int hits;
 *   - char buffer globals:           char buf[64];
 *   - functions reading & writing globals (no params, no locals)
 *   - mixing locals and globals in one expression
 */

int counter = 100;
int hits;                /* defaults to 0 */
int neg_init = -7;

char buf[16];

int bump() {
    counter = counter + 1;
    hits    = hits + 1;
    return counter;
}

int main() {
    print_int(counter);        /* 100 */
    print_int(neg_init);       /* -7 */
    print_int(hits);           /* 0 */

    print_int(bump());          /* 101 */
    print_int(bump());          /* 102 */
    print_int(bump());          /* 103 */
    print_int(hits);            /* 3 */

    /* Write into a global char buffer, then puts it. */
    buf[0] = 'H';
    buf[1] = 'i';
    buf[2] = ',';
    buf[3] = ' ';
    buf[4] = 'g';
    buf[5] = 'l';
    buf[6] = 'o';
    buf[7] = 'b';
    buf[8] = 0;
    puts(buf);                  /* Hi, glob */

    /* Mixed expression: local + global. */
    int local;
    local = 5;
    print_int(local + counter); /* 5 + 103 = 108 */

    return counter - 100;       /* 3 */
}
