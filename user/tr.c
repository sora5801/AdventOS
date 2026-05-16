/*
 * tr — translate or delete characters from stdin.
 *
 *   echo HELLO | tr H J            -> JELLO
 *   echo aXbXc | tr -d X           -> abc
 *   tr a A < /etc/inittab          -> doesn't exist (no `<` redir)
 *                                     — pipe through cat instead.
 *
 * Two modes:
 *
 *   tr SET1 SET2     replace each byte that appears in SET1 with the
 *                    byte at the same index in SET2. SET1 longer than
 *                    SET2 — surplus chars become last byte of SET2.
 *
 *   tr -d SET        delete every byte that appears in SET.
 *
 * No character classes ([:digit:], etc.), no `-c` (complement),
 * no `-s` (squeeze). Plenty for the demo pipelines.
 */

#include "libuser.h"

static int set_index(const char *set, char c) {
    for (int i = 0; set[i]; i++) if (set[i] == c) return i;
    return -1;
}

int main(int argc, char **argv) {
    /* Session 82: tr refuses to run inside a structured pipeline.
     * Char-level substitution would corrupt JSON quoting, object
     * braces, and the `:` between key and value — silently producing
     * malformed JSONL downstream tools would then drop or, worse,
     * misparse. This is the one tool that breaks the "ignore the
     * flag silently" convention because the silent path is actively
     * destructive. Per the agent-learning-surface principle: loud
     * failure beats silent corruption every time. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--advjson") == 0) {
            sys_write(2,
                "tr: refusing to corrupt JSONL stream "
                "— use pluck/where/grep instead\n", 68);
            return 2;
        }
    }

    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'd' && argv[1][2] == 0) {
        const char *del = argv[2];
        char buf[256];
        int  n;
        while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
            char out[256];
            int  o = 0;
            for (int i = 0; i < n; i++) {
                if (set_index(del, buf[i]) < 0) out[o++] = buf[i];
            }
            if (o > 0) sys_write(1, out, o);
        }
        return 0;
    }

    if (argc != 3) {
        sys_write(2, "tr: usage: tr SET1 SET2  |  tr -d SET\n", 38);
        return 2;
    }

    const char *s1 = argv[1];
    const char *s2 = argv[2];
    int s2len = (int)strlen(s2);
    if (s2len == 0) {
        sys_write(2, "tr: SET2 cannot be empty\n", 25);
        return 2;
    }

    char buf[256];
    int  n;
    while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            int idx = set_index(s1, buf[i]);
            if (idx >= 0) {
                buf[i] = s2[idx < s2len ? idx : s2len - 1];
            }
        }
        sys_write(1, buf, n);
    }
    return 0;
}
