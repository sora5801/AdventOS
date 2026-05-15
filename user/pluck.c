/*
 * pluck — project named fields out of each JSONL record on stdin.
 *
 *   ls /etc |> pluck name           # bare scalar per line
 *   ls /etc |> pluck name size      # smaller record {name, size}
 *
 * Semantics (session 81):
 *   - One field arg: emit the field's bare scalar value as a text
 *     line. Strings come out unquoted; numbers as digits. Records
 *     missing the field emit a blank line. The result is a normal
 *     text stream, useful for feeding into grep/awk-style consumers
 *     OR into argv via $(pluck name).
 *   - Two or more field args: emit a smaller JSON object on each
 *     line that contains only the named fields, preserving the
 *     order requested. Missing fields are simply absent from the
 *     output record (not nulled). Still JSONL — chains into where
 *     / count / further pluck.
 *
 * Input must be JSONL (one record per line). Lines that fail to
 * parse are skipped silently — the rest of the stream continues.
 * This mirrors how the standard text tools tolerate ragged input.
 *
 * Why split scalar vs multi-field output? Single-field pluck is
 * the structured equivalent of `awk '{print $N}'`, and the result
 * needs to be plain text so downstream tools that haven't been
 * JSONL-aware'd yet still consume it cleanly. Multi-field is the
 * structured equivalent of SELECT, which needs to remain a record
 * so that further `where` / `pluck` stages keep operating on a
 * predictable schema.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

#define LINE_MAX   2048
#define SCRATCH_SZ 4096
#define FIELDS_MAX 8

/* Write a JSON value as a plain text scalar (no quotes / object
 * braces). Used for single-field pluck. */
static void emit_scalar(const struct json_v *v) {
    if (!v) { sys_write(1, "\n", 1); return; }
    if (v->type == JSON_STR) {
        sys_write(1, v->str, v->str_len);
        sys_write(1, "\n", 1);
        return;
    }
    if (v->type == JSON_NUM) {
        /* Negative numbers OK. We use a tiny stack buffer. */
        long n = v->num;
        char buf[16]; int o = 0;
        int neg = 0;
        if (n < 0) { neg = 1; n = -n; }
        if (n == 0) buf[o++] = '0';
        while (n > 0) { buf[o++] = (char)('0' + n % 10); n /= 10; }
        if (neg) buf[o++] = '-';
        /* Reverse in place. */
        for (int i = 0, j = o - 1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
        buf[o++] = '\n';
        sys_write(1, buf, o);
        return;
    }
    if (v->type == JSON_BOOL) {
        sys_write(1, v->num ? "true\n" : "false\n", v->num ? 5 : 6);
        return;
    }
    if (v->type == JSON_NULL) {
        sys_write(1, "null\n", 5);
        return;
    }
    /* Composite (object/array) — emit empty line. pluck on a nested
     * field doesn't try to flatten; the agent who wants this should
     * just include the parent in the multi-field form. */
    sys_write(1, "\n", 1);
}

/* Re-encode a parsed JSON value into the writer w. Used by multi-
 * field pluck to reconstruct each projected field. Only handles
 * the scalar + nested-object cases the schemas in docs/69 actually
 * produce — no array handling because the spec says no inline arrays
 * of records, and the only nested object that exists today is ps's
 * `limits`. */
static void rewrite_value(struct json_w *w, const struct json_v *v) {
    if (!v) { json_null(w); return; }
    switch (v->type) {
        case JSON_STR:  json_str_n(w, v->str, v->str_len); break;
        case JSON_NUM:  json_int(w, (int)v->num); break;
        case JSON_BOOL: json_bool(w, (int)v->num); break;
        case JSON_NULL: json_null(w); break;
        case JSON_OBJ: {
            json_obj_begin(w);
            for (const struct json_v *k = v->child; k && k->next; k = k->next->next) {
                json_key(w, k->str);
                rewrite_value(w, k->next);
            }
            json_obj_end(w);
            break;
        }
        case JSON_ARR: {
            json_arr_begin(w);
            for (const struct json_v *e = v->child; e; e = e->next)
                rewrite_value(w, e);
            json_arr_end(w);
            break;
        }
        default: json_null(w); break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "pluck: usage: pluck <field>...\n", 31);
        return 2;
    }
    int nf = 0;
    const char *fields[FIELDS_MAX];
    for (int i = 1; i < argc && nf < FIELDS_MAX; i++) {
        /* Ignore --advjson if the shell injected it — pluck is
         * always JSONL-aware on stdin, --advjson is a no-op for us. */
        if (strcmp(argv[i], "--advjson") == 0) continue;
        fields[nf++] = argv[i];
    }
    if (nf == 0) {
        sys_write(2, "pluck: at least one field required\n", 35);
        return 2;
    }

    static char line   [LINE_MAX];
    static char scratch[SCRATCH_SZ];
    static char outbuf [LINE_MAX];

    int n;
    while ((n = jsonl_read_line(0, line, sizeof(line))) != 0) {
        if (n < 0) continue;
        struct json_v *root = json_parse(line, n, scratch, sizeof(scratch));
        if (!root || root->type != JSON_OBJ) continue;

        if (nf == 1) {
            const struct json_v *v = json_obj_get(root, fields[0]);
            emit_scalar(v);
        } else {
            struct json_w w;
            json_w_init(&w, outbuf, sizeof(outbuf));
            json_obj_begin(&w);
            for (int i = 0; i < nf; i++) {
                const struct json_v *v = json_obj_get(root, fields[i]);
                if (!v) continue;
                json_key(&w, fields[i]);
                rewrite_value(&w, v);
            }
            json_obj_end(&w);
            if (!json_w_ok(&w)) continue;
            json_emit_line(&w, 1);
        }
    }
    return 0;
}
