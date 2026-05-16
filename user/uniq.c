/*
 * uniq — collapse adjacent duplicate lines.
 *
 *   printf "a\na\nb\na\n" | uniq      -> a b a
 *   sort foo | uniq                  -> all unique lines (because
 *                                        sort makes equals adjacent)
 *
 * Strictly adjacent dedup — we don't see the lines that came earlier,
 * so `a b a` keeps both `a`s. POSIX-correct; if you want global
 * uniq, run `sort | uniq`.
 *
 * Flags: none today. (-c for count would just bloat the binary.)
 */

#include "libuser.h"
#include "../libjson/libjson.h"

#define LINE_MAX  512

/* Session 82: JSONL consumer mode.
 *
 * With --advjson and -f <field>, adjacent records with the same
 * value at that field collapse. Without -f, we fall back to
 * byte-for-byte line compare — which is functionally identical to
 * text-mode uniq on JSONL input, because two JSON records that
 * differ only in key order are different bytes (documented).
 *
 * Adjacent-only: this is POSIX uniq's defining behaviour.
 * Chain `|> sort -k <field> |> uniq -f <field>` for global dedup. */
#define JSONL_LINE_MAX     2048
#define JSONL_SCRATCH_SZ   4096

static int jsonl_uniq(const char *field) {
    char line   [JSONL_LINE_MAX];
    char scratch[JSONL_SCRATCH_SZ];
    char prev_key[256];      int prev_key_len = 0; int have_prev = 0;
    int  n;
    while ((n = jsonl_read_line(0, line, sizeof(line))) != 0) {
        if (n < 0) continue;

        const char *key = 0;
        int         key_len = 0;
        char        num_buf[24];

        if (field) {
            struct json_v *root = json_parse(line, n, scratch, sizeof(scratch));
            if (!root || root->type != JSON_OBJ) {
                /* Malformed — emit unchanged AND reset dedup state
                 * (since the "key" is meaningless). */
                sys_write(1, line, n); sys_write(1, "\n", 1);
                have_prev = 0;
                continue;
            }
            const struct json_v *v = json_obj_get(root, field);
            if (v && v->type == JSON_STR) {
                key = v->str; key_len = v->str_len;
            } else if (v && v->type == JSON_NUM) {
                long m = v->num; int o = 0; int neg = 0;
                if (m < 0) { neg = 1; m = -m; }
                if (m == 0) num_buf[o++] = '0';
                while (m > 0) { num_buf[o++] = (char)('0' + m % 10); m /= 10; }
                if (neg) num_buf[o++] = '-';
                for (int i = 0, j = o-1; i < j; i++, j--) {
                    char t = num_buf[i]; num_buf[i] = num_buf[j]; num_buf[j] = t;
                }
                key = num_buf; key_len = o;
            }
            /* Missing field: dedup against the bare "missing" sentinel
             * — all missing-field records collapse to one. Documented
             * as the agent-predictable choice (vs "always passes
             * through" which would be silently inconsistent with
             * how `where` and `grep -f` treat missing fields). */
            if (!key) { key = ""; key_len = 0; }
        } else {
            /* No -f: dedup by byte-for-byte line content. */
            key = line; key_len = n;
        }

        int dup = (have_prev &&
                   key_len == prev_key_len &&
                   memcmp(key, prev_key, key_len) == 0);
        if (!dup) {
            sys_write(1, line, n);
            sys_write(1, "\n", 1);
            int copy = key_len < (int)sizeof(prev_key) ? key_len : (int)sizeof(prev_key);
            for (int i = 0; i < copy; i++) prev_key[i] = key[i];
            prev_key_len = copy;
            have_prev = 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int         advjson = 0;
    const char *field   = 0;
    int         argi    = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--advjson") == 0) { advjson = 1; argi++; continue; }
        if (strcmp(argv[argi], "-f") == 0 && argi + 1 < argc) {
            field = argv[++argi]; argi++; continue;
        }
        break;
    }

    if (advjson) return jsonl_uniq(field);

    int fd = 0;
    if (argi < argc) {
        fd = sys_open(argv[argi]);
        if (fd < 0) {
            sys_write(2, "uniq: cannot open ", 18);
            sys_write(2, argv[argi], (int)strlen(argv[argi]));
            sys_write(2, "\n", 1);
            return 1;
        }
    }

    char prev[LINE_MAX]; int prev_len = 0; int have_prev = 0;
    char cur [LINE_MAX]; int cur_len  = 0;

    char c;
    int  n;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (cur_len < LINE_MAX) cur[cur_len++] = c;
        if (c == '\n' || cur_len == LINE_MAX) {
            int dup = (have_prev && cur_len == prev_len &&
                       memcmp(cur, prev, cur_len) == 0);
            if (!dup) {
                sys_write(1, cur, cur_len);
                for (int i = 0; i < cur_len; i++) prev[i] = cur[i];
                prev_len = cur_len;
                have_prev = 1;
            }
            cur_len = 0;
        }
    }
    if (cur_len > 0) {
        int dup = (have_prev && cur_len == prev_len &&
                   memcmp(cur, prev, cur_len) == 0);
        if (!dup) sys_write(1, cur, cur_len);
    }

    if (fd != 0) sys_close(fd);
    return 0;
}
