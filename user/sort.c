/*
 * sort — read lines, sort them, write them.
 *
 *   printf "c\nb\na\n" | sort        -> a b c
 *   sort /etc/inittab | uniq         -> sorted unique-ified inittab
 *
 * Implementation:
 *   - Slurp every line into a fixed-size table (capped at MAX_LINES).
 *   - Insertion sort by byte-wise compare with strcmp semantics
 *     (NUL-padded; \n at the tail counts in the comparison).
 *   - Print in order.
 *
 * No `-r` (reverse), `-n` (numeric), `-u` (unique) — pipe through
 * `tac` (we don't have it) / `awk` / `uniq` for those. The simple
 * lex order suffices for everything our shell pipelines exercise.
 */

#include "libuser.h"
#include "../libjson/libjson.h"

#define MAX_LINES  64
#define LINE_MAX   256

static char g_lines[MAX_LINES][LINE_MAX];
static int  g_len  [MAX_LINES];
static int  g_n;

/* Session 81: JSONL mode. When --advjson is in argv, sort parses
 * each line as a JSON record and uses the named field (-k <field>)
 * as the sort key. Numeric values compare numerically; everything
 * else compares lexically. Lines that fail to parse sink to the
 * top — they're the "missing field" bucket. */
static int  g_advjson;
static const char *g_sort_field = 0;
/* Cached extracted key per line — small enough to fit alongside
 * g_lines. We extract once at slurp time so the O(N^2) sort doesn't
 * re-parse N^2 times. */
static long g_key_num [MAX_LINES];
static char g_key_str [MAX_LINES][64];
static int  g_key_kind[MAX_LINES];   /* 0=none, 1=num, 2=str */

static void slurp(int fd) {
    char c;
    int  n;
    int  cur_len = 0;
    while (g_n < MAX_LINES && (n = sys_read(fd, &c, 1)) > 0) {
        if (cur_len < LINE_MAX) g_lines[g_n][cur_len++] = c;
        if (c == '\n' || cur_len == LINE_MAX) {
            g_len[g_n] = cur_len;
            g_n++;
            cur_len = 0;
        }
    }
    if (cur_len > 0 && g_n < MAX_LINES) {
        g_len[g_n] = cur_len;
        g_n++;
    }
}

/* Extract the sort key for line `i` (JSONL mode). Caches into
 * g_key_*[i]. Called once during slurp. */
static void extract_key(int i) {
    g_key_kind[i] = 0;
    if (!g_sort_field) return;
    /* Strip the trailing newline so json_parse sees a clean record. */
    char *line = g_lines[i];
    int   n    = g_len[i];
    if (n > 0 && line[n-1] == '\n') n--;
    static char scratch[2048];
    struct json_v *root = json_parse(line, n, scratch, sizeof(scratch));
    if (!root || root->type != JSON_OBJ) return;
    const struct json_v *v = json_obj_get(root, g_sort_field);
    if (!v) return;
    if (v->type == JSON_NUM) {
        g_key_kind[i] = 1;
        g_key_num [i] = v->num;
    } else if (v->type == JSON_STR) {
        g_key_kind[i] = 2;
        int len = v->str_len;
        if (len >= (int)sizeof(g_key_str[i])) len = sizeof(g_key_str[i]) - 1;
        for (int k = 0; k < len; k++) g_key_str[i][k] = v->str[k];
        g_key_str[i][len] = 0;
    }
}

static int line_cmp(int i, int j) {
    if (g_advjson && g_sort_field) {
        /* Missing keys sort first. Within each kind, compare. */
        if (g_key_kind[i] != g_key_kind[j])
            return g_key_kind[i] - g_key_kind[j];
        if (g_key_kind[i] == 1) {
            if (g_key_num[i] < g_key_num[j]) return -1;
            if (g_key_num[i] > g_key_num[j]) return  1;
            return 0;
        }
        if (g_key_kind[i] == 2) {
            return strcmp(g_key_str[i], g_key_str[j]);
        }
        /* Both missing -> equal. */
        return 0;
    }
    int li = g_len[i], lj = g_len[j];
    int m  = (li < lj) ? li : lj;
    for (int k = 0; k < m; k++) {
        unsigned char a = (unsigned char)g_lines[i][k];
        unsigned char b = (unsigned char)g_lines[j][k];
        if (a != b) return (int)a - (int)b;
    }
    return li - lj;
}

static void swap_lines(int i, int j) {
    char tmp[LINE_MAX];
    int  tlen = g_len[i];
    for (int k = 0; k < tlen; k++) tmp[k] = g_lines[i][k];
    g_len[i] = g_len[j];
    for (int k = 0; k < g_len[j]; k++) g_lines[i][k] = g_lines[j][k];
    g_len[j] = tlen;
    for (int k = 0; k < tlen; k++) g_lines[j][k] = tmp[k];
}

int main(int argc, char **argv) {
    /* Session 81: parse --advjson and -k <field>. Other args become
     * file names (text-mode behaviour). */
    int n_files = 0;
    const char *files[8];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--advjson") == 0) { g_advjson = 1; continue; }
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            g_sort_field = argv[++i];
            continue;
        }
        if (n_files < 8) files[n_files++] = argv[i];
    }

    if (n_files == 0) {
        slurp(0);
    } else {
        for (int i = 0; i < n_files; i++) {
            int fd = sys_open(files[i]);
            if (fd < 0) {
                sys_write(2, "sort: cannot open ", 18);
                sys_write(2, files[i], (int)strlen(files[i]));
                sys_write(2, "\n", 1);
                continue;
            }
            slurp(fd);
            sys_close(fd);
        }
    }

    /* In JSONL mode, extract the sort key from each line before
     * comparing. Without -k, just compare the raw JSON lexically
     * (works fine for stable ordering, not very useful otherwise). */
    if (g_advjson && g_sort_field) {
        for (int i = 0; i < g_n; i++) extract_key(i);
    }

    /* Insertion sort. Lines table is small so O(n^2) is fine. */
    for (int i = 1; i < g_n; i++) {
        int j = i;
        while (j > 0 && line_cmp(j - 1, j) > 0) {
            swap_lines(j - 1, j);
            /* The cached key moves with the line. */
            int kind = g_key_kind[j-1]; g_key_kind[j-1] = g_key_kind[j]; g_key_kind[j] = kind;
            long n  = g_key_num [j-1]; g_key_num [j-1] = g_key_num [j]; g_key_num [j] = n;
            char tmp[64];
            for (int k = 0; k < 64; k++) tmp[k] = g_key_str[j-1][k];
            for (int k = 0; k < 64; k++) g_key_str[j-1][k] = g_key_str[j][k];
            for (int k = 0; k < 64; k++) g_key_str[j][k] = tmp[k];
            j--;
        }
    }

    for (int i = 0; i < g_n; i++) sys_write(1, g_lines[i], g_len[i]);
    return 0;
}
