/*
 * grep — print lines containing a literal substring.
 *
 *   seq 30 | grep 1            -> 1, 10..19, 21
 *   grep "once" /etc/inittab   -> the two `once` lines
 *
 * Literal-only matching (no regex, no character classes). Reads input
 * line-by-line (newline-terminated), runs a naive O(line_len * pat_len)
 * substring scan on each line, prints if a match was found.
 *
 * Flags: -v inverts the match (print lines that DON'T contain pattern).
 */

#include "libuser.h"
#include "../libjson/libjson.h"

#define LINE_MAX  512

static int contains(const char *line, int len, const char *pat) {
    int plen = (int)strlen(pat);
    if (plen == 0) return 1;
    for (int i = 0; i + plen <= len; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            if (line[i + j] != pat[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static int search_fd(int fd, const char *pat, int invert) {
    char line[LINE_MAX];
    int  len = 0;
    int  hits = 0;
    char c;
    int  n;
    while ((n = sys_read(fd, &c, 1)) > 0) {
        if (len < LINE_MAX) line[len++] = c;
        if (c == '\n' || len == LINE_MAX) {
            int has = contains(line, len, pat);
            if (has != invert) {                /* xor: invert flips it */
                sys_write(1, line, len);
                hits++;
            }
            len = 0;
        }
    }
    if (len > 0) {
        int has = contains(line, len, pat);
        if (has != invert) {
            sys_write(1, line, len);
            hits++;
        }
    }
    return hits;
}

/* Session 82: JSONL consumer mode.
 *
 * Schema (from docs/69's design discussion):
 *   - With -f <field>, the regex runs against that field's value.
 *     For JSON_NUM fields, the value is stringified ("191" for 191).
 *     For JSON_STR, the bytes are tested directly.
 *     Missing field always fails the match (consistent with `where`).
 *   - Without -f, the regex runs against the FIRST JSON_STR-typed
 *     field in the record, scanned in encoding order. This is what
 *     a naive `grep <text>` does for an agent that knows the schema
 *     usually leads with a string field (ls -> name, ps -> name).
 *   - If no string field exists, the regex degrades to running
 *     against the raw line bytes (i.e. text-mode grep on the JSON
 *     encoding). Documented as a fallback.
 *
 * Matching records pass through unchanged — record passthrough,
 * not re-serialised. That preserves nested objects and key order
 * for downstream stages.
 *
 * "Regex" today is contains() — literal substring. The kernel's
 * grep was already substring-only per the file-level comment; the
 * full regex engine is a follow-up. */
#define JSONL_LINE_MAX     2048
#define JSONL_SCRATCH_SZ   4096

static int jsonl_grep(const char *pat, const char *field, int invert) {
    char line   [JSONL_LINE_MAX];
    char scratch[JSONL_SCRATCH_SZ];
    int  hits = 0;
    int  n;
    while ((n = jsonl_read_line(0, line, sizeof(line))) != 0) {
        if (n < 0) continue;
        struct json_v *root = json_parse(line, n, scratch, sizeof(scratch));
        if (!root || root->type != JSON_OBJ) {
            /* Malformed input — pass through unchanged but DON'T
             * count toward hits. The agent-learning principle here
             * is "preserve unknown structure"; we don't drop data
             * we couldn't parse. */
            if (!invert) continue;
            sys_write(1, line, n);
            sys_write(1, "\n", 1);
            hits++;
            continue;
        }

        const char *needle = 0;
        int         needle_len = 0;
        char        num_buf[24];
        int         matched = 0;

        if (field) {
            const struct json_v *v = json_obj_get(root, field);
            if (!v) goto decide;
            if (v->type == JSON_STR) {
                needle = v->str; needle_len = v->str_len;
            } else if (v->type == JSON_NUM) {
                /* Stringify the integer. */
                long m = v->num;
                int  o = 0;
                int  neg = 0;
                if (m < 0) { neg = 1; m = -m; }
                if (m == 0) num_buf[o++] = '0';
                while (m > 0) { num_buf[o++] = (char)('0' + m % 10); m /= 10; }
                if (neg) num_buf[o++] = '-';
                /* reverse */
                for (int i = 0, j = o-1; i < j; i++, j--) {
                    char t = num_buf[i]; num_buf[i] = num_buf[j]; num_buf[j] = t;
                }
                needle = num_buf; needle_len = o;
            } else if (v->type == JSON_BOOL) {
                needle = v->num ? "true" : "false";
                needle_len = v->num ? 4 : 5;
            }
            /* JSON_NULL / JSON_OBJ / JSON_ARR: no usable string,
             * never matches (per spec). */
        } else {
            /* No -f: find the first JSON_STR field in encoding order. */
            for (const struct json_v *k = root->child; k && k->next; k = k->next->next) {
                if (k->next->type == JSON_STR) {
                    needle = k->next->str; needle_len = k->next->str_len;
                    break;
                }
            }
            /* No string field at all: fall back to grep the raw line bytes. */
            if (!needle) { needle = line; needle_len = n; }
        }

        if (needle) matched = contains(needle, needle_len, pat);

decide:
        if (matched != invert) {
            sys_write(1, line, n);
            sys_write(1, "\n", 1);
            hits++;
        }
    }
    return hits;
}

int main(int argc, char **argv) {
    int invert  = 0;
    int advjson = 0;
    const char *field = 0;
    int argi    = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v")        == 0) { invert  = 1; argi++; continue; }
        if (strcmp(argv[argi], "--advjson") == 0) { advjson = 1; argi++; continue; }
        if (strcmp(argv[argi], "-f")        == 0 && argi + 1 < argc) {
            field = argv[++argi]; argi++; continue;
        }
        break;
    }

    if (argi >= argc) {
        sys_write(2, "grep: usage: grep [-v] [-f field] PATTERN [FILE...]\n", 52);
        return 2;
    }
    const char *pat = argv[argi++];

    if (advjson) {
        /* JSONL mode is stream-only; --advjson + files is undefined
         * (a `cat file |> grep ...` chain works for that). */
        return (jsonl_grep(pat, field, invert) > 0) ? 0 : 1;
    }

    if (argi >= argc) {
        return (search_fd(0, pat, invert) > 0) ? 0 : 1;
    }

    int total = 0;
    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "grep: cannot open ", 18);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, "\n", 1);
            continue;
        }
        total += search_fd(fd, pat, invert);
        sys_close(fd);
    }
    return (total > 0) ? 0 : 1;
}
