/*
 * typedefs.c — session 104 smoke test for typedef.
 *
 * Demonstrates:
 *   typedef int word;             scalar alias
 *   typedef char *string;         pointer alias
 *   typedef struct point Pt;      struct value alias
 *   typedef struct point *PtP;    struct pointer alias
 *
 * The aliases work in local decls, params, return types, and globals.
 */

struct point { int x; int y; };

typedef int   word;
typedef char *string;
typedef struct point Pt;
typedef struct point *PtP;

/* Function returning a typedef name, taking typedef args. */
word add_words(word a, word b) {
    return a + b;
}

/* Function taking a struct-pointer typedef. */
int dot(PtP a, PtP b) {
    return a->x * b->x + a->y * b->y;
}

/* Function returning a string (char *). */
string banner() {
    return "AdventOS";
}

word counter;     /* global with typedef */

int main() {
    word a;
    word b;
    a = 7;
    b = 35;
    printf("add_words(%d, %d) = %d\n", a, b, add_words(a, b));   /* 42 */

    string greeting;
    greeting = banner();
    printf("greeting = %s\n", greeting);                          /* AdventOS */

    Pt p;
    p.x = 3; p.y = 4;
    Pt q;
    q.x = 5; q.y = 12;
    printf("dot(p, q) = %d\n", dot(&p, &q));                      /* 63 */

    counter = 100;
    counter = counter + add_words(1, 2);
    printf("counter = %d\n", counter);                            /* 103 */

    return 0;
}
