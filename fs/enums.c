/*
 * enums.c — session 103 smoke test for enums.
 *
 * Demonstrates:
 *   enum TAG { ... };          tagged enum (tag ignored)
 *   enum { ... };              anonymous enum
 *   auto-increment values     A=0, B=1, C=2, ...
 *   explicit value            X = 5; values resume X+1 after
 *   enums used as constants   in expressions, switch-on-int, etc.
 */

enum color {
    RED,                   /* 0 */
    GREEN,                 /* 1 */
    BLUE,                  /* 2 */
    WHITE = 7,             /* 7 */
    OFF_WHITE              /* 8 */
};

enum {
    ERR_OK     = 0,
    ERR_BUSY   = 1,
    ERR_DENIED = 2,
    ERR_TIMEOUT = 16
};

int describe(int e) {
    if (e == ERR_OK)      { printf("ok\n");      return 0; }
    if (e == ERR_BUSY)    { printf("busy\n");    return 0; }
    if (e == ERR_DENIED)  { printf("denied\n");  return 0; }
    if (e == ERR_TIMEOUT) { printf("timeout\n"); return 0; }
    printf("unknown\n");
    return 0;
}

int main() {
    printf("RED=%d GREEN=%d BLUE=%d\n", RED, GREEN, BLUE);     /* 0 1 2 */
    printf("WHITE=%d OFF_WHITE=%d\n", WHITE, OFF_WHITE);       /* 7 8 */

    int bits;
    bits = RED | GREEN | BLUE;
    printf("RGB combined = %d\n", bits);                       /* 3 */

    describe(ERR_OK);
    describe(ERR_BUSY);
    describe(ERR_DENIED);
    describe(ERR_TIMEOUT);
    describe(99);                                              /* unknown */

    return ERR_TIMEOUT;     /* exit 16 */
}
