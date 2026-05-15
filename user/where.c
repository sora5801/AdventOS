/*
 * where — filter JSONL records by a simple predicate expressed
 * as one argv triple: <field><op><value>.
 *
 *   ls /etc |> where name=passwd
 *   ls / |> where size>1000
 *   ps   |> where state!=zombie
 *
 * Operators (must match exactly, single argv token after tokenising
 * with quote stripping by the shell):
 *   =    equality. Numeric value if the field is JSON_NUM, else
 *        byte-for-byte string compare.
 *   !=   inverse of =.
 *   <  > <= >=   numeric only. Field must be JSON_NUM.
 *   ~    substring match (no regex). Field must be JSON_STR.
 *
 * Records whose field is missing fail the predicate (so a missing
 * field is treated as "no, doesn't match"). Records that fail to
 * parse are dropped silently — same robustness story as pluck.
 *
 * Why just one predicate? The spec keeps the transform vocabulary
 * minimal: where + pluck + count is 80% of pipelines. Multi-clause
 * filtering is composed by chaining `|> where ... |> where ...`
 * which the kernel pipe layer handles for free. AND is implicit
 * in the chain; OR isn't expressible (intentional — explicit
 * compound logic stays out of shell, agents use a JSON-RPC path
 * for richer queries).
 */
#include "libuser.h"
#include "../libjson/libjson.h"

#define LINE_MAX   2048
#define SCRATCH_SZ 4096

enum op {
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_RE
};

/* Find the first operator character in `s`. Returns the offset and
 * fills *op_out + *op_len. -1 if no operator found. */
static int find_op(const char *s, enum op *op_out, int *op_len) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '=') { *op_out = OP_EQ; *op_len = 1; return i; }
        if (c == '~') { *op_out = OP_RE; *op_len = 1; return i; }
        if (c == '!' && s[i+1] == '=') { *op_out = OP_NE; *op_len = 2; return i; }
        if (c == '<' && s[i+1] == '=') { *op_out = OP_LE; *op_len = 2; return i; }
        if (c == '>' && s[i+1] == '=') { *op_out = OP_GE; *op_len = 2; return i; }
        if (c == '<') { *op_out = OP_LT; *op_len = 1; return i; }
        if (c == '>') { *op_out = OP_GT; *op_len = 1; return i; }
    }
    return -1;
}

static long parse_long(const char *s) {
    long n = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return neg ? -n : n;
}

/* Does `hay` contain `needle` as a substring? Simple O(N*M). */
static int contains(const char *hay, int hlen, const char *needle) {
    int nlen = (int)strlen(needle);
    if (nlen == 0) return 1;
    for (int i = 0; i + nlen <= hlen; i++) {
        int j;
        for (j = 0; j < nlen; j++) if (hay[i + j] != needle[j]) break;
        if (j == nlen) return 1;
    }
    return 0;
}

static int eval_pred(const struct json_v *v, enum op op, const char *rhs) {
    if (!v) return 0;
    /* Numeric compares need a JSON_NUM. */
    if (op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE) {
        if (v->type != JSON_NUM) return 0;
        long r = parse_long(rhs);
        switch (op) {
            case OP_LT: return v->num <  r;
            case OP_GT: return v->num >  r;
            case OP_LE: return v->num <= r;
            case OP_GE: return v->num >= r;
            default:    return 0;
        }
    }
    if (op == OP_RE) {
        if (v->type != JSON_STR) return 0;
        return contains(v->str, v->str_len, rhs);
    }
    /* OP_EQ / OP_NE: try numeric if both look like numbers, else
     * string compare. */
    int match = 0;
    if (v->type == JSON_NUM) {
        long r = parse_long(rhs);
        match = (v->num == r);
    } else if (v->type == JSON_STR) {
        int rlen = (int)strlen(rhs);
        if (rlen != v->str_len) match = 0;
        else {
            match = 1;
            for (int i = 0; i < rlen; i++) {
                if (v->str[i] != rhs[i]) { match = 0; break; }
            }
        }
    } else if (v->type == JSON_BOOL) {
        match = ((v->num && strcmp(rhs, "true") == 0) ||
                 (!v->num && strcmp(rhs, "false") == 0));
    }
    return op == OP_EQ ? match : !match;
}

int main(int argc, char **argv) {
    const char *pred = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--advjson") == 0) continue;
        pred = argv[i];
    }
    if (!pred) {
        sys_write(2, "where: usage: where <field><op><value>\n", 39);
        return 2;
    }

    enum op op;
    int op_len;
    int op_at = find_op(pred, &op, &op_len);
    if (op_at <= 0) {
        sys_write(2, "where: predicate must be field<op>value\n", 40);
        return 2;
    }
    /* Split the predicate in place — argv strings are writable by
     * convention in our shell. */
    char field_buf[64];
    int  flen = op_at;
    if (flen >= (int)sizeof(field_buf)) flen = (int)sizeof(field_buf) - 1;
    for (int i = 0; i < flen; i++) field_buf[i] = pred[i];
    field_buf[flen] = 0;
    const char *rhs = pred + op_at + op_len;

    static char line   [LINE_MAX];
    static char scratch[SCRATCH_SZ];

    int n;
    while ((n = jsonl_read_line(0, line, sizeof(line))) != 0) {
        if (n < 0) continue;
        struct json_v *root = json_parse(line, n, scratch, sizeof(scratch));
        if (!root || root->type != JSON_OBJ) continue;
        const struct json_v *v = json_obj_get(root, field_buf);
        if (!eval_pred(v, op, rhs)) continue;
        /* Predicate matched — pass the record through unchanged. */
        sys_write(1, line, n);
        sys_write(1, "\n", 1);
    }
    return 0;
}
