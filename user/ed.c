/*
 * AdventOS ed — a tiny line editor in the spirit of Ken Thompson's ed.
 *
 *   ed [filename]
 *
 * Buffer is a doubly-linked list of struct line { len, text, prev, next }.
 * Heap-allocated through libuser malloc (session 17). A "current line"
 * cursor tracks where line-relative commands act. Line numbers are
 * 1-based, matching ed.
 *
 * Commands implemented:
 *   p          print current line
 *   Np         print line N
 *   n,mp       print lines n..m
 *   ,p         print whole buffer
 *   a          append after current; subsequent lines until "." enter
 *              the buffer; current line moves to the last appended
 *   Nd         delete line N
 *   n,md       delete lines n..m
 *   s/old/new/ literal substitute on current line (first occurrence)
 *   w [name]   write buffer (joined with \n) to disk via SYS_FS_WRITE
 *   r name     read file into buffer, replacing current contents
 *   =          print current line number (or addressed range count)
 *   q          quit (warn if unsaved)
 *   Q          quit unconditionally
 *   <Enter>    print current line and advance
 *
 * Address forms understood: N (literal), . (current), $ (last),
 * +N / -N (relative), and a comma- or semicolon-separated range.
 * No regex addresses (/pat/). Substitute is literal-only, no flags,
 * first-match-on-current-line.
 */

#include "libuser.h"

struct line {
    struct line *prev, *next;
    int   len;
    char *text;          /* malloc'd, NUL-terminated, NO trailing newline */
};

static struct line *g_head;          /* first real line, or NULL if empty */
static struct line *g_tail;          /* last real line, or NULL if empty  */
static struct line *g_cur;           /* current line, or NULL if empty    */
static int          g_dirty;         /* unsaved changes since last write   */
static char         g_filename[64];

/* ---- helpers ------------------------------------------------------- */

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static int parse_int(const char **pp) {
    const char *p = *pp;
    int n = 0, saw = 0;
    while (*p >= '0' && *p <= '9') { n = n*10 + (*p - '0'); saw = 1; p++; }
    *pp = p;
    return saw ? n : -1;
}

static int line_count(void) {
    int n = 0;
    for (struct line *l = g_head; l; l = l->next) n++;
    return n;
}

static int line_number_of(struct line *target) {
    int n = 0;
    for (struct line *l = g_head; l; l = l->next) {
        n++;
        if (l == target) return n;
    }
    return 0;
}

static struct line *line_at(int n) {
    if (n <= 0) return (struct line *)0;
    struct line *l = g_head;
    while (l && n > 1) { l = l->next; n--; }
    return l;
}

/* ---- buffer mutation ----------------------------------------------- */

/* Insert `nl` (a freshly-allocated line) AFTER `after`, or at head
 * if `after` is NULL. */
static void insert_after(struct line *after, struct line *nl) {
    if (!after) {
        nl->prev = (struct line *)0;
        nl->next = g_head;
        if (g_head) g_head->prev = nl;
        g_head = nl;
        if (!g_tail) g_tail = nl;
    } else {
        nl->prev = after;
        nl->next = after->next;
        if (after->next) after->next->prev = nl;
        else             g_tail = nl;
        after->next = nl;
    }
    g_dirty = 1;
}

static void remove_line(struct line *l) {
    if (l->prev) l->prev->next = l->next;
    else         g_head        = l->next;
    if (l->next) l->next->prev = l->prev;
    else         g_tail        = l->prev;
    if (g_cur == l) g_cur = l->next ? l->next : l->prev;
    free(l->text);
    free(l);
    g_dirty = 1;
}

static struct line *new_line(const char *src, int len) {
    struct line *nl = (struct line *)malloc(sizeof(*nl));
    if (!nl) return (struct line *)0;
    nl->prev = nl->next = (struct line *)0;
    nl->len  = len;
    nl->text = (char *)malloc((unsigned)len + 1);
    if (!nl->text) { free(nl); return (struct line *)0; }
    for (int i = 0; i < len; i++) nl->text[i] = src[i];
    nl->text[len] = 0;
    return nl;
}

static void clear_buffer(void) {
    struct line *l = g_head;
    while (l) {
        struct line *next = l->next;
        free(l->text);
        free(l);
        l = next;
    }
    g_head = g_tail = g_cur = (struct line *)0;
}

/* ---- I/O ----------------------------------------------------------- */

static int load_file(const char *name) {
    int fd = sys_open(name);
    if (fd < 0) return -1;

    clear_buffer();

    /* Slurp the whole file into a single buffer, then split on newlines. */
    char    chunk[512];
    char   *buf = (char *)0;
    int     cap = 0, total = 0, rd;
    while ((rd = sys_read(fd, chunk, sizeof(chunk))) > 0) {
        if (total + rd > cap) {
            int new_cap = cap ? cap * 2 : 1024;
            while (new_cap < total + rd) new_cap *= 2;
            char *nb = (char *)malloc((unsigned)new_cap);
            if (!nb) { free(buf); sys_close(fd); return -1; }
            for (int i = 0; i < total; i++) nb[i] = buf[i];
            free(buf);
            buf = nb; cap = new_cap;
        }
        for (int i = 0; i < rd; i++) buf[total + i] = chunk[i];
        total += rd;
    }
    sys_close(fd);

    int start = 0;
    for (int i = 0; i < total; i++) {
        if (buf[i] == '\n') {
            struct line *nl = new_line(buf + start, i - start);
            if (nl) insert_after(g_tail, nl);
            start = i + 1;
        }
    }
    /* Trailing partial line (no \n at EOF). */
    if (start < total) {
        struct line *nl = new_line(buf + start, total - start);
        if (nl) insert_after(g_tail, nl);
    }
    free(buf);

    g_cur = g_tail;
    g_dirty = 0;
    return total;
}

static int save_file(const char *name) {
    /* Compute total size first (sum of line lengths + 1 for each \n). */
    int total = 0;
    for (struct line *l = g_head; l; l = l->next) total += l->len + 1;

    char *buf = (char *)malloc((unsigned)(total + 1));
    if (!buf) return -1;

    int off = 0;
    for (struct line *l = g_head; l; l = l->next) {
        for (int i = 0; i < l->len; i++) buf[off++] = l->text[i];
        buf[off++] = '\n';
    }

    int rc = sys_fs_write(name, buf, (uint32_t)total);
    free(buf);

    if (rc == 0) g_dirty = 0;
    return rc == 0 ? total : -1;
}

/* ---- command parsing ----------------------------------------------- */

/* Resolve an address starting at *pp. Returns the line number (1-based),
 * or 0 if no address parsed, or -1 on parse error. Updates *pp past
 * the parsed address. Understands: N, ., $, +N, -N. */
static int parse_addr(const char **pp) {
    const char *p = *pp;
    int base = -2;
    if (*p == '.') { base = g_cur ? line_number_of(g_cur) : 0; p++; }
    else if (*p == '$') { base = line_count(); p++; }
    else if (*p >= '0' && *p <= '9') { base = parse_int(&p); }

    if (base == -2) {
        /* No anchor — maybe pure relative like "+1" or "-3". */
        if (*p != '+' && *p != '-') { *pp = p; return 0; }
        base = g_cur ? line_number_of(g_cur) : 0;
    }

    /* Apply +/- offsets repeatedly. */
    while (*p == '+' || *p == '-') {
        char op = *p++;
        int  delta = 1;
        if (*p >= '0' && *p <= '9') delta = parse_int(&p);
        if (op == '+') base += delta; else base -= delta;
    }

    *pp = p;
    if (base < 0) base = 0;
    if (base > line_count()) base = line_count();
    return base;
}

/* ---- command handlers --------------------------------------------- */

static void print_range(int from, int to) {
    if (from < 1) from = 1;
    if (to > line_count()) to = line_count();
    if (from > to) { puts("?\n"); return; }
    int i = 1;
    for (struct line *l = g_head; l; l = l->next, i++) {
        if (i < from) continue;
        if (i > to)   break;
        puts(l->text);
        puts("\n");
        g_cur = l;
    }
}

static void delete_range(int from, int to) {
    if (from < 1) from = 1;
    if (to > line_count()) to = line_count();
    if (from > to) { puts("?\n"); return; }
    int i = 1;
    struct line *l = g_head;
    while (l && i < from) { l = l->next; i++; }
    while (l && i <= to) {
        struct line *next = l->next;
        remove_line(l);
        l = next;
        i++;
    }
}

/* `a` — append: read lines from stdin until "." on its own. */
static void cmd_append(void) {
    char buf[256];
    for (;;) {
        int n = sys_read_line(buf, sizeof(buf));
        if (n < 0) break;
        if (n == 1 && buf[0] == '.') break;
        struct line *nl = new_line(buf, n);
        if (!nl) { puts("?out of memory\n"); return; }
        insert_after(g_cur, nl);
        g_cur = nl;
    }
}

/* `s/old/new/` — substitute first occurrence on current line.
 * Literal match, no flags. The `/` separator can be any char. */
static void cmd_substitute(const char *args) {
    if (!g_cur)  { puts("?no current line\n"); return; }
    char delim = *args++;
    if (!delim)  { puts("?bad substitute\n"); return; }

    /* Walk to find old/new boundaries (assume single trailing delim). */
    const char *old_s = args;
    const char *p = args;
    while (*p && *p != delim) p++;
    if (*p != delim) { puts("?bad substitute\n"); return; }
    int old_len = (int)(p - old_s);
    p++;
    const char *new_s = p;
    while (*p && *p != delim) p++;
    int new_len = (int)(p - new_s);

    if (old_len == 0) { puts("?empty pattern\n"); return; }

    /* Find first occurrence of old_s in g_cur->text. */
    int hit = -1;
    for (int i = 0; i + old_len <= g_cur->len; i++) {
        int match = 1;
        for (int j = 0; j < old_len; j++) {
            if (g_cur->text[i + j] != old_s[j]) { match = 0; break; }
        }
        if (match) { hit = i; break; }
    }
    if (hit < 0) { puts("?no match\n"); return; }

    /* Build the replacement line. */
    int new_total = g_cur->len - old_len + new_len;
    char *nt = (char *)malloc((unsigned)(new_total + 1));
    if (!nt) { puts("?out of memory\n"); return; }
    int off = 0;
    for (int i = 0; i < hit;        i++) nt[off++] = g_cur->text[i];
    for (int i = 0; i < new_len;    i++) nt[off++] = new_s[i];
    for (int i = hit + old_len; i < g_cur->len; i++) nt[off++] = g_cur->text[i];
    nt[off] = 0;

    free(g_cur->text);
    g_cur->text = nt;
    g_cur->len  = new_total;
    g_dirty = 1;
}

/* ---- main loop ----------------------------------------------------- */

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

int main(int argc, char **argv) {
    /* Optional initial filename. */
    g_filename[0] = 0;
    if (argc >= 2) {
        int i;
        for (i = 0; argv[1][i] && i < (int)sizeof(g_filename) - 1; i++) {
            g_filename[i] = argv[1][i];
        }
        g_filename[i] = 0;
        int n = load_file(g_filename);
        if (n < 0) printf("?cannot open %s (new file)\n", g_filename);
        else       printf("%d\n", n);   /* ed prints byte count on load */
    }

    char buf[256];
    for (;;) {
        int n = sys_read_line(buf, sizeof(buf));
        if (n < 0) break;
        const char *p = buf;

        /* Two-address range, then one-letter command. */
        int a = parse_addr(&p);
        int b = -1;
        if (*p == ',' || *p == ';') {
            p++;
            b = parse_addr(&p);
            if (a == 0) a = 1;
            if (b == 0) b = line_count();
        }

        char cmd = *p ? *p : 0;
        if (cmd) p++;

        switch (cmd) {
            case 0:   /* empty input — print + advance */
                if (g_cur && g_cur->next) {
                    g_cur = g_cur->next;
                    puts(g_cur->text); puts("\n");
                }
                break;

            case 'p':
                if (b >= 0) print_range(a, b);
                else if (a > 0) print_range(a, a);
                else if (g_cur) print_range(line_number_of(g_cur),
                                            line_number_of(g_cur));
                else            puts("?\n");
                break;

            case 'a':
                if (a > 0) g_cur = line_at(a);
                cmd_append();
                break;

            case 'd':
                if (b >= 0) delete_range(a, b);
                else if (a > 0) delete_range(a, a);
                else if (g_cur) {
                    int ln = line_number_of(g_cur);
                    delete_range(ln, ln);
                } else puts("?\n");
                break;

            case 's':
                cmd_substitute(p);
                break;

            case 'w': {
                /* Optional filename; fall back to g_filename. */
                while (*p == ' ') p++;
                const char *target = *p ? p : g_filename;
                if (!*target) { puts("?no filename\n"); break; }
                int written = save_file(target);
                if (written < 0) puts("?write failed\n");
                else             printf("%d\n", written);
                break;
            }

            case 'r': {
                while (*p == ' ') p++;
                if (!*p) { puts("?no filename\n"); break; }
                int rd = load_file(p);
                if (rd < 0) printf("?cannot read %s\n", p);
                else        printf("%d\n", rd);
                break;
            }

            case '=': {
                int target = a > 0 ? a : (g_cur ? line_number_of(g_cur) : 0);
                printf("%d\n", target);
                break;
            }

            case 'q':
                if (g_dirty) { puts("?unsaved changes (use Q to force)\n"); break; }
                return 0;

            case 'Q':
                return 0;

            default:
                /* Match "s/.../.../" without the explicit case fall-through:
                 * the leading 's' is consumed above. Anything else is bad. */
                puts("?\n");
                break;
        }

        (void)starts_with;
    }
    return 0;
}
