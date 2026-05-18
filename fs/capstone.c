/*
 * capstone.c — Session 121 smoke test for Path B Phase 4.
 *
 * Exercises three things in one program:
 *
 * 1. STRUCT-BY-VALUE RETURNS (`struct point f(...)`)
 *    Hidden-first-arg cdecl ABI. The caller pushes &dest as the first
 *    arg; the callee writes the result through that pointer.
 *
 * 2. STATIC / EXTERN at top level.
 *    `extern int foo(int);`    function prototype, no body
 *    `static int bar(...)`     module-private function (no-op modifier
 *                              in cc since every symbol is already
 *                              translation-unit-private)
 *    `static int x;`           module-private global (same: no-op)
 *
 * 3. FUNCTION-POINTER TYPEDEF syntax.
 *    `typedef int (*op_t)(int, int);`     a function-ptr alias
 *    `op_t op = my_func;`                 used as a local
 *    `op(a, b)`                            indirect call through it
 */

struct point { int x; int y; };

/* Prototype declared BEFORE the definition. cc resolves forward calls
 * anyway, but extern makes the signature explicit + visible to callers
 * earlier than the definition would. */
extern int squared(int);

/* Function-pointer typedef. The alias resolves to LK_INT_PTR internally;
 * cc doesn't typecheck indirect calls so the args can be anything. */
typedef int (*op_t)(int, int);

static int s_module_counter;     /* `static` global — no-op modifier */

/* `static` on a function — also a no-op. */
static int s_add(int a, int b) { return a + b; }
static int s_mul(int a, int b) { return a * b; }

/* Struct-by-value RETURN. The hidden first arg is the dest pointer;
 * inside the callee we write fields through it via `return LOCAL;`
 * which expands to a rep-movsd copy + return. */
struct point make_point(int x, int y) {
    struct point p;
    p.x = x;
    p.y = y;
    return p;
}

/* Another SBV return; uses make_point inside. */
struct point shift(struct point in, int dx, int dy) {
    struct point out;
    out.x = in.x + dx;
    out.y = in.y + dy;
    return out;
}

/* Tail-forward of a struct return: `return shift(...)` — cc reuses
 * OUR hidden dest pointer for the inner call, no intermediate copy. */
struct point shift_x_only(struct point in, int dx) {
    return shift(in, dx, 0);
}

int main() {
    /* 1. STRUCT-BY-VALUE RETURNS */
    struct point p;
    p = make_point(3, 4);
    printf("make_point(3,4) = (%d, %d)\n", p.x, p.y);     /* (3, 4) */

    struct point q;
    q = shift(p, 10, 20);
    printf("shift(p,10,20)  = (%d, %d)\n", q.x, q.y);     /* (13, 24) */
    printf("p untouched     = (%d, %d)\n", p.x, p.y);     /* (3, 4) */

    /* Tail-forward chain. */
    struct point r;
    r = shift_x_only(p, 100);
    printf("shift_x_only    = (%d, %d)\n", r.x, r.y);     /* (103, 4) */

    /* 2. STATIC / EXTERN */
    s_module_counter = 0;
    s_module_counter = s_module_counter + s_add(7, 8);
    s_module_counter = s_module_counter + s_mul(s_module_counter, 2);
    printf("s_module_counter = %d\n", s_module_counter);  /* (0+15) + (15*2) = 45 */
    printf("squared(7)       = %d\n", squared(7));        /* 49 */

    /* 3. FUNCTION-POINTER TYPEDEF */
    op_t op;
    op = s_add;
    printf("op=s_add: op(3,5) = %d\n", op(3, 5));         /* 8 */
    op = s_mul;
    printf("op=s_mul: op(3,5) = %d\n", op(3, 5));         /* 15 */

    return 0;
}

/* Definition follows the `extern` prototype. */
int squared(int n) { return n * n; }
