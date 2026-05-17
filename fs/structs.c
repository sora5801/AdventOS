/*
 * structs.c — session 97 smoke test for struct support in cc.
 *
 * Demonstrates:
 *   struct TAG { int field; ... };       file-scope definition
 *   struct TAG name;                     local struct value
 *   struct TAG *name;                    local struct pointer
 *   var.field = expr;                    member assign on struct value
 *   ptr->field = expr;                   member assign through pointer
 *   var.field, ptr->field                member reads (rvalue)
 *   struct TAG *next;                    self-referential field (forward decl)
 *   &var                                 address-of a struct, fed to ptr arg
 */

struct point {
    int x;
    int y;
};

struct node {
    int          val;
    struct node *next;
};

/* A function that takes a pointer-to-struct and reads from it. */
int dot(struct point *a, struct point *b) {
    return a->x * b->x + a->y * b->y;
}

/* Modify a struct via its pointer. */
int swap_xy(struct point *p) {
    int t;
    t = p->x;
    p->x = p->y;
    p->y = t;
    return 0;
}

int main() {
    /* Plain struct value + member access. */
    struct point p;
    p.x = 3;
    p.y = 4;
    printf("p = (%d, %d)\n", p.x, p.y);          /* (3, 4) */

    struct point q;
    q.x = 5;
    q.y = 12;
    printf("dot(p, q) = %d\n", dot(&p, &q));     /* 3*5 + 4*12 = 63 */

    swap_xy(&p);
    printf("after swap: p = (%d, %d)\n", p.x, p.y);  /* (4, 3) */

    /* Pointer-to-struct stored in a local. */
    struct point *r;
    r = &q;
    r->x = 100;
    r->y = 200;
    printf("q via r = (%d, %d)\n", q.x, q.y);    /* (100, 200) */

    /* Self-referential struct — simulate a tiny linked list on
     * the stack. */
    struct node a;
    struct node b;
    struct node c;
    a.val = 1; a.next = &b;
    b.val = 2; b.next = &c;
    c.val = 3; c.next = 0;        /* NULL */

    struct node *cur;
    int total;
    total = 0;
    cur = &a;
    while (cur != 0) {
        total += cur->val;
        cur = cur->next;
    }
    printf("linked-list sum = %d\n", total);     /* 1+2+3 = 6 */

    return total;
}
