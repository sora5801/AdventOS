/*
 * sarr.c — session 102 smoke test for arrays of struct.
 *
 * Demonstrates:
 *   struct TAG NAME[N];          local array of struct
 *   NAME[i].field = expr;        indexed member assignment
 *   NAME[i].field                indexed member rvalue
 */

struct point { int x; int y; };
struct entry { int id; char *name; int value; };

int main() {
    /* 3-element array of struct point. */
    struct point pts[3];
    pts[0].x = 1; pts[0].y = 2;
    pts[1].x = 3; pts[1].y = 4;
    pts[2].x = 5; pts[2].y = 6;

    int sum;
    sum = 0;
    int i;
    i = 0;
    while (i < 3) {
        sum += pts[i].x + pts[i].y;
        i++;
    }
    printf("sum = %d\n", sum);          /* 1+2+3+4+5+6 = 21 */

    /* 4-element array of struct entry — multi-field structs. */
    struct entry items[4];
    items[0].id = 100; items[0].name = "alpha";   items[0].value = 1;
    items[1].id = 200; items[1].name = "beta";    items[1].value = 4;
    items[2].id = 300; items[2].name = "gamma";   items[2].value = 9;
    items[3].id = 400; items[3].name = "delta";   items[3].value = 16;

    int total_v;
    total_v = 0;
    i = 0;
    while (i < 4) {
        printf("  %d: %s = %d\n", items[i].id, items[i].name, items[i].value);
        total_v += items[i].value;
        i++;
    }
    printf("total value = %d\n", total_v);     /* 1+4+9+16 = 30 */

    return total_v;     /* exit code = 30 */
}
