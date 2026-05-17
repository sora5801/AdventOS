/*
 * sbv.c — session 106 smoke test for struct-by-value function args.
 *
 * Demonstrates:
 *   int f(struct point p);       struct value parameter (NOT a pointer)
 *   foo(my_struct);              caller copies the struct onto the stack
 *
 * Inside the callee, p.x and p.y access the caller's COPY. The caller's
 * original struct is unaffected by mutations inside the callee.
 */

struct point { int x; int y; };
struct triple { int a; int b; int c; };

/* By-value: receives a copy. */
int magnitude_sq(struct point p) {
    return p.x * p.x + p.y * p.y;
}

/* By-value with mutation — the caller's copy isn't visible. */
int reverse_p(struct point p) {
    int t;
    t = p.x;
    p.x = p.y;
    p.y = t;
    return p.x + p.y * 10;
}

/* Multi-field struct by value. */
int sum_triple(struct triple t) {
    return t.a + t.b + t.c;
}

/* Mix of int and struct args. The struct sits between int args. */
int between(int before, struct point p, int after) {
    return before + p.x + p.y + after;
}

int main() {
    struct point pp;
    pp.x = 3; pp.y = 4;

    printf("magnitude_sq(3,4) = %d\n", magnitude_sq(pp));  /* 25 */

    int r;
    r = reverse_p(pp);
    printf("reverse_p result  = %d\n", r);                 /* 4 + 3*10 = 34 */
    printf("pp untouched      = (%d, %d)\n", pp.x, pp.y);  /* (3, 4) */

    struct triple tt;
    tt.a = 100; tt.b = 200; tt.c = 300;
    printf("sum_triple        = %d\n", sum_triple(tt));    /* 600 */

    printf("between(1, pp, 2) = %d\n", between(1, pp, 2)); /* 1+3+4+2 = 10 */

    return 0;
}
