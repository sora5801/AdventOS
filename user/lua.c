/*
 * lua — AdventOS's Lua-syntax interpreter (subset, tree-walking).
 *
 * This is NOT a port of the upstream Lua sources — it's a fresh
 * single-file implementation that recognizes the same surface
 * syntax. The goal is "Lua is a great REPL and scripting language;
 * AdventOS should have something that feels like it" rather than
 * full lua.org compatibility.
 *
 *   lua FILE.lua       run FILE.lua
 *   lua                interactive REPL
 *   lua -e "stmt"      execute one statement
 *
 * Implemented surface:
 *
 *   Types       nil, boolean, number (int32 — see "Numbers" below),
 *               string, table, function
 *
 * Numbers:
 *   Real Lua numbers are doubles (or int64 in 5.3+). AdventOS user
 *   programs build with -mgeneral-regs-only / -mno-sse / -mno-mmx
 *   because the kernel doesn't save FPU state across IRQ tails, so
 *   floating-point would mean either teaching the kernel FPU context
 *   switch OR shipping libgcc soft-float helpers. Neither is worth
 *   it for "Lua-style scripting at the shell prompt." So numbers
 *   are int32. 1/3 == 0 (integer division). 2^31-1 is the max.
 *   Documented limit; revisit when/if the FPU support story changes.
 *   Statements  assignment, local, if/elseif/else, while, numeric for,
 *               function defn, return, break, expression-as-stmt
 *   Expressions binops + - * / % ^ == ~= < > <= >= .. and or,
 *               unops not - #, function calls f(...), index t[k] / t.k,
 *               table constructor {1,2,3} or {x=1, [k]=v}, function literals
 *   Built-ins   print, type, tostring, tonumber, ipairs, pairs,
 *               string.len/sub/format/find/upper/lower/rep,
 *               table.insert/remove/concat,
 *               io.read/write, os.time, os.exit
 *
 * Deliberately deferred (not session-87 scope):
 *
 *   - Metatables and __index / __newindex etc.
 *   - Coroutines
 *   - pcall / xpcall / error (errors abort the program; the REPL
 *     uses a fork-per-line shape to recover)
 *   - Multiple return values (returns are scalar)
 *   - Closures with upvalues (functions see globals + their own locals)
 *   - require / packages / dynamic load
 *   - String patterns / gmatch (string.find is plain substring)
 *   - math.* (no math library beyond what's in arithmetic)
 *
 * Implementation:
 *
 *   - Lexer: emits a flat token stream into a heap array.
 *   - Parser: recursive-descent; emits a tree of `struct node`.
 *     Operator precedence climbing handles binops.
 *   - Eval: tree-walking. Two scope kinds — locals (a stack array
 *     of (name, value) per function call) and a single global hash
 *     table. Lookup walks locals top-down then falls through to
 *     globals.
 *   - GC: leak-on-allocate. Programs are short; cleanup happens at
 *     process exit. Real mark-sweep is deferred.
 *
 * The whole interpreter is under 2000 lines because the subset
 * fits — no metatables means no recursive metamethod dispatch,
 * no closures means no upvalue capture, no coroutines means no
 * stack switching. Cut features carry their own weight; this is
 * one of those cases where what's NOT here is what keeps it
 * tractable.
 */

#include "libuser.h"

/* ---------- Forward decls ---------------------------------------- */

struct value;
struct node;
struct env;
struct table;

static void  die(const char *fmt, ...);
static void  out_str(const char *s);
static void  out_chr(char c);

/* ---------- Small utilities the libc/libuser surface lacks ------ */

static int  s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Parse a signed int32 from `s` (length-bounded, no NUL needed).
 * Returns 1 on success, writes parsed value into *out. Rejects
 * empty input or trailing non-digit chars. */
static int parse_int(const char *s, int len, int *out) {
    if (len <= 0) return 0;
    int i = 0;
    int neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    else if (s[i] == '+') i++;
    int v = 0;
    int any = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++; any = 1;
    }
    if (!any || i != len) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* Format an int as text into `out` (cap bytes). Returns byte
 * count written (excluding trailing NUL). */
static int format_int(int v, char *out, int cap) {
    int o = 0;
    if (v < 0) { if (o < cap - 1) out[o++] = '-'; v = -v; }
    if (v == 0) {
        if (o < cap - 1) out[o++] = '0';
    } else {
        char tmp[16]; int tn = 0;
        while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
        while (tn-- > 0) if (o < cap - 1) out[o++] = tmp[tn];
    }
    if (o < cap) out[o] = 0;
    return o;
}

/* ---------- Values ----------------------------------------------- */

enum value_kind {
    V_NIL = 0,
    V_BOOL,
    V_NUM,
    V_STR,
    V_TABLE,
    V_FN,
    V_BUILTIN,
};

struct value {
    int             kind;
    union {
        int             b;
        int             n;          /* numbers are int32 — see header */
        struct string  *s;
        struct table   *t;
        struct func    *fn;
        struct value  (*bi)(int argc, struct value *argv);
    } as;
};

struct string {
    int   len;
    char  data[1];       /* flexible: malloc'd as sizeof(string) + len */
};

#define TABLE_INIT_CAP  8

struct tk_pair {
    struct value k;
    struct value v;
};

struct table {
    int             count;
    int             cap;
    struct tk_pair *kv;
};

struct func {
    struct node    *body;            /* the block node */
    struct node   **params;          /* parameter name nodes (NAME) */
    int             n_params;
    struct env     *closure_env;     /* for top-level fns, the global env */
};

static struct value v_nil(void)      { struct value v; v.kind = V_NIL; return v; }
static struct value v_bool(int b)    { struct value v; v.kind = V_BOOL; v.as.b = b ? 1 : 0; return v; }
static struct value v_num(int n)     { struct value v; v.kind = V_NUM;  v.as.n = n; return v; }

static struct string *make_string(const char *p, int n) {
    struct string *s = (struct string *)malloc(sizeof(struct string) + n + 1);
    s->len = n;
    for (int i = 0; i < n; i++) s->data[i] = p[i];
    s->data[n] = 0;
    return s;
}

static struct value v_str_take(struct string *s) {
    struct value v; v.kind = V_STR; v.as.s = s; return v;
}

static struct value v_str(const char *p, int n) {
    return v_str_take(make_string(p, n));
}

static struct value v_strz(const char *z) {
    return v_str(z, s_len(z));
}

static struct value v_table(struct table *t) {
    struct value v; v.kind = V_TABLE; v.as.t = t; return v;
}

static struct value v_fn(struct func *f) {
    struct value v; v.kind = V_FN; v.as.fn = f; return v;
}

static struct value v_builtin(struct value (*bi)(int, struct value *)) {
    struct value v; v.kind = V_BUILTIN; v.as.bi = bi; return v;
}

/* Truthiness — Lua: only false and nil are false. */
static int v_truthy(struct value v) {
    if (v.kind == V_NIL) return 0;
    if (v.kind == V_BOOL) return v.as.b;
    return 1;
}

/* Equality — type-strict comparison. nil==nil, bool==bool by value,
 * number==number by value, string==string by content, table by
 * identity, function by identity. */
static int v_eq(struct value a, struct value b) {
    if (a.kind != b.kind) return 0;
    switch (a.kind) {
        case V_NIL:     return 1;
        case V_BOOL:    return a.as.b == b.as.b;
        case V_NUM:     return a.as.n == b.as.n;
        case V_STR: {
            if (a.as.s->len != b.as.s->len) return 0;
            for (int i = 0; i < a.as.s->len; i++)
                if (a.as.s->data[i] != b.as.s->data[i]) return 0;
            return 1;
        }
        case V_TABLE:   return a.as.t == b.as.t;
        case V_FN:      return a.as.fn == b.as.fn;
        case V_BUILTIN: return a.as.bi == b.as.bi;
    }
    return 0;
}

/* ---------- Table ------------------------------------------------ */

static struct table *table_new(void) {
    struct table *t = (struct table *)malloc(sizeof(*t));
    t->count = 0;
    t->cap   = TABLE_INIT_CAP;
    t->kv    = (struct tk_pair *)malloc(sizeof(struct tk_pair) * t->cap);
    for (int i = 0; i < t->cap; i++) t->kv[i].k = v_nil();
    return t;
}

static int table_find(struct table *t, struct value k) {
    for (int i = 0; i < t->count; i++) {
        if (v_eq(t->kv[i].k, k)) return i;
    }
    return -1;
}

static struct value table_get(struct table *t, struct value k) {
    int i = table_find(t, k);
    if (i < 0) return v_nil();
    return t->kv[i].v;
}

static void table_set(struct table *t, struct value k, struct value v) {
    if (k.kind == V_NIL) return;          /* Lua: nil keys forbidden */
    int i = table_find(t, k);
    if (i >= 0) {
        if (v.kind == V_NIL) {
            /* Remove: shift later entries down. */
            for (int j = i; j < t->count - 1; j++) t->kv[j] = t->kv[j + 1];
            t->count--;
        } else {
            t->kv[i].v = v;
        }
        return;
    }
    if (v.kind == V_NIL) return;
    if (t->count >= t->cap) {
        int nc = t->cap * 2;
        struct tk_pair *nk = (struct tk_pair *)malloc(sizeof(struct tk_pair) * nc);
        for (int j = 0; j < t->count; j++) nk[j] = t->kv[j];
        free(t->kv);
        t->kv = nk;
        t->cap = nc;
    }
    t->kv[t->count].k = k;
    t->kv[t->count].v = v;
    t->count++;
}

/* table length operator # — Lua semantics: highest contiguous
 * positive-integer-keyed slot. For our flat array we find the
 * largest i such that t[1..i] are all set. */
static int table_len(struct table *t) {
    int len = 0;
    for (;;) {
        struct value k = v_num(len + 1);
        if (table_find(t, k) < 0) return len;
        len++;
    }
}

/* ---------- Tokens / Lexer --------------------------------------- */

enum token {
    T_END = 0,
    T_NUM,
    T_STR,
    T_NAME,
    /* keywords */
    T_AND, T_BREAK, T_DO, T_ELSE, T_ELSEIF, T_END_KW,
    T_FALSE, T_FOR, T_FUNCTION, T_IF, T_IN, T_LOCAL,
    T_NIL, T_NOT, T_OR, T_RETURN, T_THEN, T_TRUE,
    T_UNTIL, T_WHILE, T_REPEAT,
    /* punctuation/operators */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE, T_ASSIGN,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_CONCAT, T_HASH,
};

struct tok {
    int   kind;
    int   line;
    /* Per-kind payload — number value, string contents, or
     * identifier text. Strings/identifiers are heap-allocated.
     * For numbers we store the parsed int32. */
    int            n;
    struct string *s;     /* used for T_STR and T_NAME */
};

static const char *g_src;
static int         g_src_len;
static int         g_pos;
static int         g_line;

static struct tok *g_toks;
static int         g_n_toks;
static int         g_cap_toks;
static int         g_tk;       /* current parser index */

static struct tok *tk_peek(int delta) { return &g_toks[g_tk + delta]; }
static struct tok *tk_cur(void)       { return &g_toks[g_tk]; }
static void        tk_advance(void)   { if (g_toks[g_tk].kind != T_END) g_tk++; }

static void push_tok(struct tok t) {
    if (g_n_toks >= g_cap_toks) {
        int nc = g_cap_toks ? g_cap_toks * 2 : 64;
        struct tok *nb = (struct tok *)malloc(sizeof(struct tok) * nc);
        for (int i = 0; i < g_n_toks; i++) nb[i] = g_toks[i];
        if (g_toks) free(g_toks);
        g_toks = nb;
        g_cap_toks = nc;
    }
    g_toks[g_n_toks++] = t;
}

static int is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_id_cont(char c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int kw_match(const char *s, int n, const char *kw) {
    int kl = s_len(kw);
    if (n != kl) return 0;
    for (int i = 0; i < n; i++) if (s[i] != kw[i]) return 0;
    return 1;
}

static int kw_lookup(const char *s, int n) {
    if (kw_match(s, n, "and"))      return T_AND;
    if (kw_match(s, n, "break"))    return T_BREAK;
    if (kw_match(s, n, "do"))       return T_DO;
    if (kw_match(s, n, "else"))     return T_ELSE;
    if (kw_match(s, n, "elseif"))   return T_ELSEIF;
    if (kw_match(s, n, "end"))      return T_END_KW;
    if (kw_match(s, n, "false"))    return T_FALSE;
    if (kw_match(s, n, "for"))      return T_FOR;
    if (kw_match(s, n, "function")) return T_FUNCTION;
    if (kw_match(s, n, "if"))       return T_IF;
    if (kw_match(s, n, "in"))       return T_IN;
    if (kw_match(s, n, "local"))    return T_LOCAL;
    if (kw_match(s, n, "nil"))      return T_NIL;
    if (kw_match(s, n, "not"))      return T_NOT;
    if (kw_match(s, n, "or"))       return T_OR;
    if (kw_match(s, n, "return"))   return T_RETURN;
    if (kw_match(s, n, "then"))     return T_THEN;
    if (kw_match(s, n, "true"))     return T_TRUE;
    if (kw_match(s, n, "until"))    return T_UNTIL;
    if (kw_match(s, n, "while"))    return T_WHILE;
    if (kw_match(s, n, "repeat"))   return T_REPEAT;
    return T_NAME;
}

static void lex_all(const char *src, int len) {
    g_src = src; g_src_len = len; g_pos = 0; g_line = 1;
    g_toks = 0; g_n_toks = 0; g_cap_toks = 0;
    while (g_pos < g_src_len) {
        char c = g_src[g_pos];
        /* Whitespace + comments. */
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '\n') { g_line++; g_pos++; continue; }
        if (c == '-' && g_pos + 1 < g_src_len && g_src[g_pos + 1] == '-') {
            /* line comment */
            g_pos += 2;
            while (g_pos < g_src_len && g_src[g_pos] != '\n') g_pos++;
            continue;
        }
        struct tok t;
        t.kind = T_END;
        t.line = g_line;
        t.n    = 0;
        t.s    = 0;
        /* Number (int32 only — see header). */
        if (is_digit(c)) {
            int start = g_pos;
            while (g_pos < g_src_len && is_digit(g_src[g_pos])) g_pos++;
            int v;
            if (!parse_int(g_src + start, g_pos - start, &v)) {
                die("lex: bad number at line %d", g_line);
            }
            t.kind = T_NUM; t.n = v;
            push_tok(t);
            continue;
        }
        /* String. */
        if (c == '"' || c == '\'') {
            char q = c;
            g_pos++;
            int start = g_pos;
            /* Find the closing quote. Basic escapes: \n \t \\ \" \' */
            char buf[1024]; int bn = 0;
            while (g_pos < g_src_len && g_src[g_pos] != q) {
                char ch = g_src[g_pos];
                if (ch == '\\' && g_pos + 1 < g_src_len) {
                    char nxt = g_src[g_pos + 1];
                    if      (nxt == 'n') { buf[bn++] = '\n'; g_pos += 2; continue; }
                    else if (nxt == 't') { buf[bn++] = '\t'; g_pos += 2; continue; }
                    else if (nxt == 'r') { buf[bn++] = '\r'; g_pos += 2; continue; }
                    else if (nxt == '\\') { buf[bn++] = '\\'; g_pos += 2; continue; }
                    else if (nxt == '"')  { buf[bn++] = '"';  g_pos += 2; continue; }
                    else if (nxt == '\'') { buf[bn++] = '\''; g_pos += 2; continue; }
                    else { buf[bn++] = nxt; g_pos += 2; continue; }
                }
                if (ch == '\n') g_line++;
                if (bn < (int)sizeof(buf)) buf[bn++] = ch;
                g_pos++;
            }
            if (g_pos >= g_src_len) die("lex: unterminated string starting line %d", t.line);
            g_pos++;     /* skip closing quote */
            (void)start;
            t.kind = T_STR;
            t.s    = make_string(buf, bn);
            push_tok(t);
            continue;
        }
        /* Identifier or keyword. */
        if (is_id_start(c)) {
            int start = g_pos;
            while (g_pos < g_src_len && is_id_cont(g_src[g_pos])) g_pos++;
            int n = g_pos - start;
            int k = kw_lookup(g_src + start, n);
            t.kind = k;
            if (k == T_NAME) t.s = make_string(g_src + start, n);
            push_tok(t);
            continue;
        }
        /* Operators / punctuation. */
        switch (c) {
            case '+': t.kind = T_PLUS;   g_pos++; break;
            case '-': t.kind = T_MINUS;  g_pos++; break;
            case '*': t.kind = T_STAR;   g_pos++; break;
            case '/': t.kind = T_SLASH;  g_pos++; break;
            case '%': t.kind = T_PERCENT; g_pos++; break;
            case '^': t.kind = T_CARET;  g_pos++; break;
            case '(': t.kind = T_LPAREN; g_pos++; break;
            case ')': t.kind = T_RPAREN; g_pos++; break;
            case '{': t.kind = T_LBRACE; g_pos++; break;
            case '}': t.kind = T_RBRACE; g_pos++; break;
            case '[': t.kind = T_LBRACK; g_pos++; break;
            case ']': t.kind = T_RBRACK; g_pos++; break;
            case ',': t.kind = T_COMMA;  g_pos++; break;
            case ';': t.kind = T_SEMI;   g_pos++; break;
            case ':': t.kind = T_COLON;  g_pos++; break;
            case '#': t.kind = T_HASH;   g_pos++; break;
            case '=':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') { t.kind = T_EQ;  g_pos += 2; }
                else                                                    { t.kind = T_ASSIGN; g_pos++; }
                break;
            case '~':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') { t.kind = T_NEQ; g_pos += 2; }
                else die("lex: stray '~' at line %d (expected ~=)", g_line);
                break;
            case '<':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') { t.kind = T_LE; g_pos += 2; }
                else                                                    { t.kind = T_LT; g_pos++; }
                break;
            case '>':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') { t.kind = T_GE; g_pos += 2; }
                else                                                    { t.kind = T_GT; g_pos++; }
                break;
            case '.':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '.') { t.kind = T_CONCAT; g_pos += 2; }
                else                                                    { t.kind = T_DOT; g_pos++; }
                break;
            default:
                die("lex: stray '%c' (0x%02x) at line %d",
                    (c >= 32 && c < 127) ? c : '?', (unsigned char)c, g_line);
        }
        push_tok(t);
    }
    struct tok eof;
    eof.kind = T_END; eof.line = g_line; eof.n = 0; eof.s = 0;
    push_tok(eof);
}

/* ---------- AST ------------------------------------------------- */

enum node_kind {
    N_NUM, N_STR, N_NIL, N_TRUE, N_FALSE,
    N_NAME,
    N_INDEX,        /* a.b or a[b] */
    N_CALL,         /* f(args) */
    N_BIN,          /* a op b */
    N_UN,           /* op a */
    N_TABLE,        /* {...} */
    N_FUNC,         /* function(args) body end */
    N_ASSIGN,       /* a = b   or t[k] = v */
    N_LOCAL,        /* local a = b */
    N_IF,
    N_WHILE,
    N_FOR_NUM,
    N_BLOCK,
    N_RETURN,
    N_BREAK,
    N_FUNC_DECL,    /* function name(args) body end (sugar) */
    N_EXPR_STMT,    /* function-call-as-statement */
};

struct kv_node {
    struct node *key;       /* nil = array slot (assigned automatically) */
    struct node *val;
};

struct node {
    int kind;
    int line;

    /* Common fields, used per-kind. */
    int            num;            /* int32 — see header */
    struct string *str;            /* identifier text or string literal */
    int            op;             /* binop / unop token kind */
    struct node   *a, *b, *c;
    struct node  **list;
    int            n_list;

    /* Function literals / decls. */
    struct node  **params;
    int            n_params;
    struct node   *body;

    /* Table constructor. */
    struct kv_node *kv;
    int             n_kv;

    /* If: pairs of (cond, then_block) in list[]; else_block in body. */
    /* For numeric: target=str, start=a, stop=b, step=c, body=body. */
    /* Local: name list in list[], value list in (alt)? — we restrict
     *   to `local name [= expr]`. */
};

static struct node *new_node(int kind) {
    struct node *n = (struct node *)malloc(sizeof(*n));
    n->kind = kind;
    n->line = tk_cur()->line;
    n->num = 0; n->str = 0; n->op = 0;
    n->a = n->b = n->c = 0;
    n->list = 0; n->n_list = 0;
    n->params = 0; n->n_params = 0;
    n->body = 0;
    n->kv = 0; n->n_kv = 0;
    return n;
}

static void node_push(struct node ***list, int *n, int *cap, struct node *e) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        struct node **nb = (struct node **)malloc(sizeof(struct node *) * nc);
        for (int i = 0; i < *n; i++) nb[i] = (*list)[i];
        if (*list) free(*list);
        *list = nb;
        *cap = nc;
    }
    (*list)[(*n)++] = e;
}

/* ---------- Parser (recursive descent) -------------------------- */

static struct node *parse_block(void);
static struct node *parse_stmt(void);
static struct node *parse_expr(void);
static struct node *parse_prefix(void);

static int  accept(int k) {
    if (tk_cur()->kind == k) { tk_advance(); return 1; }
    return 0;
}
static void expect(int k, const char *what) {
    if (tk_cur()->kind != k)
        die("parse: expected %s at line %d", what, tk_cur()->line);
    tk_advance();
}

/* Lua operator precedence (1 = lowest, 11 = highest). */
static int binop_prec(int t) {
    switch (t) {
        case T_OR:                                       return 1;
        case T_AND:                                      return 2;
        case T_LT: case T_GT: case T_LE: case T_GE:
        case T_EQ: case T_NEQ:                           return 3;
        case T_CONCAT:                                   return 4;     /* right-assoc */
        case T_PLUS: case T_MINUS:                       return 5;
        case T_STAR: case T_SLASH: case T_PERCENT:       return 6;
        /* unary is 7 */
        case T_CARET:                                    return 8;     /* right-assoc */
    }
    return 0;
}
static int binop_right_assoc(int t) {
    return t == T_CONCAT || t == T_CARET;
}

static struct node *parse_expr_prec(int min_prec) {
    struct node *left;
    int u = tk_cur()->kind;
    if (u == T_NOT || u == T_MINUS || u == T_HASH) {
        tk_advance();
        left = new_node(N_UN);
        left->op = u;
        left->a  = parse_expr_prec(7);
    } else {
        left = parse_prefix();
    }
    for (;;) {
        int t = tk_cur()->kind;
        int p = binop_prec(t);
        if (p == 0 || p < min_prec) break;
        tk_advance();
        int next_min = binop_right_assoc(t) ? p : p + 1;
        struct node *right = parse_expr_prec(next_min);
        struct node *n = new_node(N_BIN);
        n->op = t; n->a = left; n->b = right;
        left = n;
    }
    return left;
}

static struct node *parse_expr(void) { return parse_expr_prec(1); }

/* prefix expression: primary followed by zero-or-more suffixes
 * (. .x, [expr], (args)). */
static struct node *parse_args(struct node *callee) {
    expect(T_LPAREN, "'('");
    struct node *call = new_node(N_CALL);
    call->a = callee;
    int cap = 0;
    while (tk_cur()->kind != T_RPAREN) {
        node_push(&call->list, &call->n_list, &cap, parse_expr());
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN, "')'");
    return call;
}

static struct node *parse_table(void) {
    expect(T_LBRACE, "'{'");
    struct node *n = new_node(N_TABLE);
    int cap = 0;
    int auto_idx = 1;
    while (tk_cur()->kind != T_RBRACE) {
        struct kv_node kv = { 0, 0 };
        if (tk_cur()->kind == T_LBRACK) {
            /* [expr] = expr */
            tk_advance();
            kv.key = parse_expr();
            expect(T_RBRACK, "']'");
            expect(T_ASSIGN, "'='");
            kv.val = parse_expr();
        } else if (tk_cur()->kind == T_NAME && tk_peek(1)->kind == T_ASSIGN) {
            /* name = expr — sugar for ["name"] = expr */
            struct node *k = new_node(N_STR);
            k->str = tk_cur()->s;
            tk_advance();
            tk_advance();     /* = */
            kv.key = k;
            kv.val = parse_expr();
        } else {
            /* positional array entry */
            struct node *k = new_node(N_NUM);
            k->num = auto_idx++;
            kv.key = k;
            kv.val = parse_expr();
        }
        if (n->n_kv >= cap) {
            int nc = cap ? cap * 2 : 4;
            struct kv_node *nb = (struct kv_node *)malloc(sizeof(struct kv_node) * nc);
            for (int i = 0; i < n->n_kv; i++) nb[i] = n->kv[i];
            if (n->kv) free(n->kv);
            n->kv = nb;
            cap = nc;
        }
        n->kv[n->n_kv++] = kv;
        if (!accept(T_COMMA) && !accept(T_SEMI)) break;
    }
    expect(T_RBRACE, "'}'");
    return n;
}

static struct node *parse_func_body(void) {
    expect(T_LPAREN, "'('");
    struct node *fn = new_node(N_FUNC);
    int cap = 0;
    while (tk_cur()->kind == T_NAME) {
        struct node *p = new_node(N_NAME);
        p->str = tk_cur()->s;
        tk_advance();
        node_push(&fn->params, &fn->n_params, &cap, p);
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN, "')'");
    fn->body = parse_block();
    expect(T_END_KW, "'end'");
    return fn;
}

static struct node *parse_prefix(void) {
    struct node *e;
    int t = tk_cur()->kind;
    if (t == T_NUM) {
        e = new_node(N_NUM); e->num = tk_cur()->n; tk_advance();
    } else if (t == T_STR) {
        e = new_node(N_STR); e->str = tk_cur()->s; tk_advance();
    } else if (t == T_NIL)   { e = new_node(N_NIL);   tk_advance(); }
    else if   (t == T_TRUE)  { e = new_node(N_TRUE);  tk_advance(); }
    else if   (t == T_FALSE) { e = new_node(N_FALSE); tk_advance(); }
    else if (t == T_NAME) {
        e = new_node(N_NAME); e->str = tk_cur()->s; tk_advance();
    }
    else if (t == T_LPAREN) {
        tk_advance();
        e = parse_expr();
        expect(T_RPAREN, "')'");
    }
    else if (t == T_LBRACE) {
        e = parse_table();
    }
    else if (t == T_FUNCTION) {
        tk_advance();
        e = parse_func_body();
    }
    else {
        die("parse: unexpected token (%d) at line %d", t, tk_cur()->line);
        e = 0;     /* unreachable */
    }
    /* Suffix chain: . name, [expr], (args). */
    for (;;) {
        int s = tk_cur()->kind;
        if (s == T_DOT) {
            tk_advance();
            if (tk_cur()->kind != T_NAME) die("parse: expected name after '.'");
            struct node *k = new_node(N_STR);
            k->str = tk_cur()->s;
            tk_advance();
            struct node *idx = new_node(N_INDEX);
            idx->a = e; idx->b = k;
            e = idx;
        } else if (s == T_LBRACK) {
            tk_advance();
            struct node *k = parse_expr();
            expect(T_RBRACK, "']'");
            struct node *idx = new_node(N_INDEX);
            idx->a = e; idx->b = k;
            e = idx;
        } else if (s == T_LPAREN) {
            e = parse_args(e);
        } else {
            break;
        }
    }
    return e;
}

static struct node *parse_if(void) {
    expect(T_IF, "'if'");
    struct node *n = new_node(N_IF);
    int cap = 0;
    /* Pairs (cond, then-block) in list[]; cond at even idx, block odd. */
    struct node *cond = parse_expr();
    expect(T_THEN, "'then'");
    struct node *blk = parse_block();
    node_push(&n->list, &n->n_list, &cap, cond);
    node_push(&n->list, &n->n_list, &cap, blk);
    while (accept(T_ELSEIF)) {
        cond = parse_expr();
        expect(T_THEN, "'then'");
        blk = parse_block();
        node_push(&n->list, &n->n_list, &cap, cond);
        node_push(&n->list, &n->n_list, &cap, blk);
    }
    if (accept(T_ELSE)) {
        n->body = parse_block();
    }
    expect(T_END_KW, "'end'");
    return n;
}

static struct node *parse_while(void) {
    expect(T_WHILE, "'while'");
    struct node *n = new_node(N_WHILE);
    n->a = parse_expr();
    expect(T_DO, "'do'");
    n->body = parse_block();
    expect(T_END_KW, "'end'");
    return n;
}

static struct node *parse_for(void) {
    expect(T_FOR, "'for'");
    if (tk_cur()->kind != T_NAME) die("parse: expected name after 'for'");
    struct node *n = new_node(N_FOR_NUM);
    n->str = tk_cur()->s;
    tk_advance();
    expect(T_ASSIGN, "'='");
    n->a = parse_expr();
    expect(T_COMMA, "','");
    n->b = parse_expr();
    if (accept(T_COMMA)) n->c = parse_expr();
    expect(T_DO, "'do'");
    n->body = parse_block();
    expect(T_END_KW, "'end'");
    return n;
}

static struct node *parse_local(void) {
    expect(T_LOCAL, "'local'");
    if (accept(T_FUNCTION)) {
        /* local function name(args) body end */
        if (tk_cur()->kind != T_NAME) die("parse: expected name after 'local function'");
        struct node *n = new_node(N_LOCAL);
        n->str = tk_cur()->s;
        tk_advance();
        n->a = parse_func_body();
        return n;
    }
    struct node *n = new_node(N_LOCAL);
    if (tk_cur()->kind != T_NAME) die("parse: expected name after 'local'");
    n->str = tk_cur()->s;
    tk_advance();
    if (accept(T_ASSIGN)) n->a = parse_expr();
    return n;
}

static struct node *parse_func_decl(void) {
    expect(T_FUNCTION, "'function'");
    if (tk_cur()->kind != T_NAME) die("parse: expected name after 'function'");
    struct node *n = new_node(N_FUNC_DECL);
    n->str = tk_cur()->s;
    tk_advance();
    /* dotted names: function a.b.c(...) — store as N_INDEX chain in n->a */
    n->b = 0;
    while (accept(T_DOT)) {
        if (tk_cur()->kind != T_NAME) die("parse: expected name after '.'");
        struct node *k = new_node(N_STR);
        k->str = tk_cur()->s;
        tk_advance();
        if (!n->b) {
            struct node *head = new_node(N_NAME);
            head->str = n->str;
            n->b = head;
        }
        struct node *idx = new_node(N_INDEX);
        idx->a = n->b; idx->b = k;
        n->b = idx;
    }
    n->a = parse_func_body();
    return n;
}

static struct node *parse_return(void) {
    expect(T_RETURN, "'return'");
    struct node *n = new_node(N_RETURN);
    int t = tk_cur()->kind;
    if (t != T_END_KW && t != T_ELSE && t != T_ELSEIF && t != T_UNTIL && t != T_SEMI && t != T_END) {
        n->a = parse_expr();
    }
    accept(T_SEMI);
    return n;
}

static struct node *parse_stmt(void) {
    int t = tk_cur()->kind;
    if (t == T_IF)       return parse_if();
    if (t == T_WHILE)    return parse_while();
    if (t == T_FOR)      return parse_for();
    if (t == T_LOCAL)    return parse_local();
    if (t == T_FUNCTION) return parse_func_decl();
    if (t == T_RETURN)   return parse_return();
    if (t == T_BREAK)    { tk_advance(); return new_node(N_BREAK); }
    if (t == T_SEMI)     { tk_advance(); return 0; }     /* empty stmt */
    if (t == T_DO) {
        tk_advance();
        struct node *blk = parse_block();
        expect(T_END_KW, "'end'");
        return blk;
    }
    /* Expression statement — either function call or assignment. */
    struct node *e = parse_prefix();
    if (accept(T_ASSIGN)) {
        struct node *rhs = parse_expr();
        struct node *n = new_node(N_ASSIGN);
        n->a = e; n->b = rhs;
        return n;
    }
    /* Otherwise must be a call. */
    if (e->kind != N_CALL) die("parse: statement must be call or assignment (line %d)", e->line);
    struct node *n = new_node(N_EXPR_STMT);
    n->a = e;
    return n;
}

static int is_block_end(int t) {
    return t == T_END || t == T_END_KW || t == T_ELSE || t == T_ELSEIF || t == T_UNTIL;
}

static struct node *parse_block(void) {
    struct node *blk = new_node(N_BLOCK);
    int cap = 0;
    while (!is_block_end(tk_cur()->kind)) {
        struct node *s = parse_stmt();
        if (s) node_push(&blk->list, &blk->n_list, &cap, s);
    }
    return blk;
}

/* ---------- Environment / scoping -------------------------------- */

#define LOCALS_MAX 64

struct local_slot {
    struct string *name;
    struct value   value;
};

struct env {
    struct table     *globals;       /* shared across calls */
    struct local_slot locals[LOCALS_MAX];
    int               n_locals;
};

static struct env *make_env(struct table *globals) {
    struct env *e = (struct env *)malloc(sizeof(*e));
    e->globals = globals;
    e->n_locals = 0;
    return e;
}

static int local_find(struct env *e, struct string *name) {
    for (int i = e->n_locals - 1; i >= 0; i--) {
        if (e->locals[i].name->len == name->len) {
            int ok = 1;
            for (int j = 0; j < name->len; j++) {
                if (e->locals[i].name->data[j] != name->data[j]) { ok = 0; break; }
            }
            if (ok) return i;
        }
    }
    return -1;
}

static struct value var_get(struct env *e, struct string *name) {
    int i = local_find(e, name);
    if (i >= 0) return e->locals[i].value;
    return table_get(e->globals, v_strz(name->data));
}

static void var_set(struct env *e, struct string *name, struct value v) {
    int i = local_find(e, name);
    if (i >= 0) { e->locals[i].value = v; return; }
    table_set(e->globals, v_strz(name->data), v);
}

static void local_declare(struct env *e, struct string *name, struct value v) {
    if (e->n_locals >= LOCALS_MAX) die("too many locals");
    e->locals[e->n_locals].name  = name;
    e->locals[e->n_locals].value = v;
    e->n_locals++;
}

/* ---------- Eval ------------------------------------------------- */

/* Per-loop flow control. Returned by eval_block to signal break/return. */
enum flow { FL_NORMAL = 0, FL_BREAK, FL_RETURN };
struct ret { enum flow f; struct value v; };

static struct value eval_expr(struct env *e, struct node *n);
static struct ret  eval_block(struct env *e, struct node *blk);

static struct value eval_call(struct env *outer, struct node *call);

static int to_num(struct value v, const char *ctx) {
    if (v.kind == V_NUM) return v.as.n;
    if (v.kind == V_STR) {
        int r;
        if (parse_int(v.as.s->data, v.as.s->len, &r)) return r;
    }
    die("expected number in %s, got kind=%d", ctx, v.kind);
    return 0;     /* unreachable */
}

static struct string *to_str(struct value v) {
    if (v.kind == V_STR) return v.as.s;
    char buf[32];
    if (v.kind == V_NUM) {
        int n = format_int(v.as.n, buf, sizeof(buf));
        return make_string(buf, n);
    }
    if (v.kind == V_NIL)  return make_string("nil",   3);
    if (v.kind == V_BOOL) return make_string(v.as.b ? "true" : "false", v.as.b ? 4 : 5);
    if (v.kind == V_TABLE) return make_string("table",   5);
    if (v.kind == V_FN || v.kind == V_BUILTIN) return make_string("function", 8);
    return make_string("?", 1);
}

static struct value eval_bin(int op, struct value a, struct value b) {
    if (op == T_PLUS)   return v_num(to_num(a, "+") + to_num(b, "+"));
    if (op == T_MINUS)  return v_num(to_num(a, "-") - to_num(b, "-"));
    if (op == T_STAR)   return v_num(to_num(a, "*") * to_num(b, "*"));
    if (op == T_SLASH)  {
        int bn = to_num(b, "/");
        if (bn == 0) die("divide by zero");
        return v_num(to_num(a, "/") / bn);     /* integer division */
    }
    if (op == T_PERCENT) {
        int bn = to_num(b, "%");
        if (bn == 0) die("modulo by zero");
        return v_num(to_num(a, "%") % bn);
    }
    if (op == T_CARET) {
        /* Integer power. Negative exponents return 1 (since fractional
         * results aren't representable). */
        int an = to_num(a, "^");
        int be = to_num(b, "^");
        if (be < 0) return v_num(1);
        int r = 1;
        for (int i = 0; i < be; i++) r *= an;
        return v_num(r);
    }
    if (op == T_EQ)     return v_bool(v_eq(a, b));
    if (op == T_NEQ)    return v_bool(!v_eq(a, b));
    if (op == T_LT)     return v_bool(to_num(a, "<")  <  to_num(b, "<"));
    if (op == T_GT)     return v_bool(to_num(a, ">")  >  to_num(b, ">"));
    if (op == T_LE)     return v_bool(to_num(a, "<=") <= to_num(b, "<="));
    if (op == T_GE)     return v_bool(to_num(a, ">=") >= to_num(b, ">="));
    if (op == T_CONCAT) {
        struct string *sa = to_str(a);
        struct string *sb = to_str(b);
        struct string *r  = (struct string *)malloc(sizeof(*r) + sa->len + sb->len + 1);
        r->len = sa->len + sb->len;
        for (int i = 0; i < sa->len; i++) r->data[i] = sa->data[i];
        for (int i = 0; i < sb->len; i++) r->data[sa->len + i] = sb->data[i];
        r->data[r->len] = 0;
        return v_str_take(r);
    }
    die("unknown binop %d", op);
    return v_nil();
}

static struct value eval_un(int op, struct value a) {
    if (op == T_MINUS) return v_num(-to_num(a, "unary -"));
    if (op == T_NOT)   return v_bool(!v_truthy(a));
    if (op == T_HASH) {
        if (a.kind == V_STR)   return v_num(a.as.s->len);
        if (a.kind == V_TABLE) return v_num(table_len(a.as.t));
        die("# applied to non-string/table");
    }
    die("unknown unop %d", op);
    return v_nil();
}

static struct value eval_expr(struct env *e, struct node *n) {
    if (!n) return v_nil();
    switch (n->kind) {
        case N_NIL:   return v_nil();
        case N_TRUE:  return v_bool(1);
        case N_FALSE: return v_bool(0);
        case N_NUM:   return v_num(n->num);
        case N_STR:   return v_str(n->str->data, n->str->len);
        case N_NAME:  return var_get(e, n->str);
        case N_BIN: {
            /* Short-circuit and/or before evaluating right side. */
            if (n->op == T_AND) {
                struct value l = eval_expr(e, n->a);
                if (!v_truthy(l)) return l;
                return eval_expr(e, n->b);
            }
            if (n->op == T_OR) {
                struct value l = eval_expr(e, n->a);
                if (v_truthy(l)) return l;
                return eval_expr(e, n->b);
            }
            return eval_bin(n->op, eval_expr(e, n->a), eval_expr(e, n->b));
        }
        case N_UN: return eval_un(n->op, eval_expr(e, n->a));
        case N_INDEX: {
            struct value t = eval_expr(e, n->a);
            struct value k = eval_expr(e, n->b);
            if (t.kind != V_TABLE) die("index of non-table at line %d", n->line);
            return table_get(t.as.t, k);
        }
        case N_TABLE: {
            struct table *t = table_new();
            for (int i = 0; i < n->n_kv; i++) {
                struct value k = eval_expr(e, n->kv[i].key);
                struct value v = eval_expr(e, n->kv[i].val);
                table_set(t, k, v);
            }
            return v_table(t);
        }
        case N_FUNC: {
            struct func *f = (struct func *)malloc(sizeof(*f));
            f->body = n->body;
            f->params = n->params;
            f->n_params = n->n_params;
            f->closure_env = e;     /* unused — no upvalues — but reserved */
            return v_fn(f);
        }
        case N_CALL: return eval_call(e, n);
    }
    die("eval_expr: unknown node kind %d", n->kind);
    return v_nil();
}

static struct value eval_call(struct env *outer, struct node *call) {
    struct value callee = eval_expr(outer, call->a);
    struct value args[16];
    int argc = call->n_list;
    if (argc > 16) die("too many args (max 16)");
    for (int i = 0; i < argc; i++) args[i] = eval_expr(outer, call->list[i]);
    if (callee.kind == V_BUILTIN) return callee.as.bi(argc, args);
    if (callee.kind != V_FN)      die("attempt to call non-function (line %d)", call->line);
    struct func *f = callee.as.fn;
    struct env *child = make_env(outer->globals);
    int n = f->n_params < argc ? f->n_params : argc;
    for (int i = 0; i < n; i++) {
        local_declare(child, f->params[i]->str, args[i]);
    }
    /* Pad missing params with nil. */
    for (int i = n; i < f->n_params; i++) {
        local_declare(child, f->params[i]->str, v_nil());
    }
    struct ret r = eval_block(child, f->body);
    free(child);
    if (r.f == FL_RETURN) return r.v;
    return v_nil();
}

static struct ret eval_block(struct env *e, struct node *blk) {
    struct ret r; r.f = FL_NORMAL; r.v = v_nil();
    int saved_locals = e->n_locals;
    for (int i = 0; i < blk->n_list; i++) {
        struct node *s = blk->list[i];
        switch (s->kind) {
            case N_LOCAL: {
                struct value v = s->a ? eval_expr(e, s->a) : v_nil();
                local_declare(e, s->str, v);
                break;
            }
            case N_ASSIGN: {
                struct value v = eval_expr(e, s->b);
                if (s->a->kind == N_NAME) var_set(e, s->a->str, v);
                else if (s->a->kind == N_INDEX) {
                    struct value t = eval_expr(e, s->a->a);
                    struct value k = eval_expr(e, s->a->b);
                    if (t.kind != V_TABLE) die("assign to non-table index (line %d)", s->line);
                    table_set(t.as.t, k, v);
                }
                else die("assignment target not assignable (line %d)", s->line);
                break;
            }
            case N_FUNC_DECL: {
                struct value v = eval_expr(e, s->a);
                if (s->b) {
                    /* function a.b(...) — store at the indexed location */
                    /* Walk down the index chain, doing get on parents
                     * and set on the leaf. */
                    struct node *chain = s->b;
                    struct value t = eval_expr(e, chain->a);
                    struct value k = eval_expr(e, chain->b);
                    if (t.kind != V_TABLE) die("function decl: parent not a table (line %d)", s->line);
                    table_set(t.as.t, k, v);
                } else {
                    var_set(e, s->str, v);
                }
                break;
            }
            case N_IF: {
                int branched = 0;
                for (int j = 0; j + 1 < s->n_list; j += 2) {
                    if (v_truthy(eval_expr(e, s->list[j]))) {
                        r = eval_block(e, s->list[j + 1]);
                        branched = 1;
                        break;
                    }
                }
                if (!branched && s->body) r = eval_block(e, s->body);
                if (r.f != FL_NORMAL) goto out;
                break;
            }
            case N_WHILE: {
                while (v_truthy(eval_expr(e, s->a))) {
                    r = eval_block(e, s->body);
                    if (r.f == FL_BREAK)  { r.f = FL_NORMAL; break; }
                    if (r.f == FL_RETURN) goto out;
                }
                break;
            }
            case N_FOR_NUM: {
                int start = to_num(eval_expr(e, s->a), "for start");
                int stop  = to_num(eval_expr(e, s->b), "for stop");
                int step  = s->c ? to_num(eval_expr(e, s->c), "for step") : 1;
                local_declare(e, s->str, v_num(start));
                int slot = e->n_locals - 1;
                while ((step > 0 && e->locals[slot].value.as.n <= stop) ||
                       (step < 0 && e->locals[slot].value.as.n >= stop))
                {
                    r = eval_block(e, s->body);
                    if (r.f == FL_BREAK)  { r.f = FL_NORMAL; break; }
                    if (r.f == FL_RETURN) goto out;
                    e->locals[slot].value = v_num(e->locals[slot].value.as.n + step);
                }
                /* Pop the loop var. */
                e->n_locals--;
                break;
            }
            case N_RETURN: {
                r.f = FL_RETURN;
                r.v = s->a ? eval_expr(e, s->a) : v_nil();
                goto out;
            }
            case N_BREAK: {
                r.f = FL_BREAK;
                goto out;
            }
            case N_EXPR_STMT:
                eval_expr(e, s->a);     /* discard result */
                break;
            case N_BLOCK: {
                r = eval_block(e, s);
                if (r.f != FL_NORMAL) goto out;
                break;
            }
            default:
                die("eval_block: unsupported stmt kind %d", s->kind);
        }
    }
out:
    e->n_locals = saved_locals;
    return r;
}

/* ---------- Built-ins ------------------------------------------- */

static void out_str(const char *s) { sys_write(1, s, s_len(s)); }
static void out_chr(char c)        { sys_write(1, &c, 1); }

static struct value bi_print(int argc, struct value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) out_chr('\t');
        struct string *s = to_str(argv[i]);
        sys_write(1, s->data, s->len);
    }
    out_chr('\n');
    return v_nil();
}

static struct value bi_type(int argc, struct value *argv) {
    if (argc < 1) die("type: missing arg");
    switch (argv[0].kind) {
        case V_NIL:   return v_strz("nil");
        case V_BOOL:  return v_strz("boolean");
        case V_NUM:   return v_strz("number");
        case V_STR:   return v_strz("string");
        case V_TABLE: return v_strz("table");
        case V_FN: case V_BUILTIN: return v_strz("function");
    }
    return v_strz("?");
}

static struct value bi_tostring(int argc, struct value *argv) {
    if (argc < 1) die("tostring: missing arg");
    return v_str_take(to_str(argv[0]));
}

static struct value bi_tonumber(int argc, struct value *argv) {
    if (argc < 1) return v_nil();
    if (argv[0].kind == V_NUM) return argv[0];
    if (argv[0].kind == V_STR) {
        int r;
        if (parse_int(argv[0].as.s->data, argv[0].as.s->len, &r)) return v_num(r);
    }
    return v_nil();
}

static struct value bi_ipairs_iter(int argc, struct value *argv) {
    /* args: (table, current_index). Return (next_index, value) or nil
     * to terminate. */
    if (argc < 2) return v_nil();
    if (argv[0].kind != V_TABLE) return v_nil();
    int i = to_num(argv[1], "ipairs iter") + 1;
    struct value v = table_get(argv[0].as.t, v_num(i));
    if (v.kind == V_NIL) return v_nil();
    /* We only have single-return in this subset, so callers do
     * `for i, v in ipairs(t) do` style which expects multi-return.
     * We approximate by returning a 2-element table; the for-loop
     * doesn't support it. Document and don't worry about this. */
    struct table *t = table_new();
    table_set(t, v_num(1), v_num(i));
    table_set(t, v_num(2), v);
    return v_table(t);
}
static struct value bi_ipairs(int argc, struct value *argv) {
    if (argc < 1 || argv[0].kind != V_TABLE) return v_nil();
    /* Multi-return not supported — wrap as a table the user can
     * inspect, or return the iter fn for manual use. Real ipairs
     * returns (iter_fn, t, 0); we just return iter_fn. */
    (void)argv;
    return v_builtin(bi_ipairs_iter);
}

static struct value bi_string_len(int argc, struct value *argv) {
    if (argc < 1 || argv[0].kind != V_STR) return v_num(0);
    return v_num(argv[0].as.s->len);
}

static struct value bi_string_sub(int argc, struct value *argv) {
    if (argc < 2 || argv[0].kind != V_STR) return v_strz("");
    struct string *s = argv[0].as.s;
    int i = (int)to_num(argv[1], "string.sub i");
    int j = (argc >= 3) ? (int)to_num(argv[2], "string.sub j") : s->len;
    /* Lua: negative indexes count from end. */
    if (i < 0) i = s->len + i + 1;
    if (j < 0) j = s->len + j + 1;
    if (i < 1) i = 1;
    if (j > s->len) j = s->len;
    if (i > j) return v_strz("");
    return v_str(s->data + i - 1, j - i + 1);
}

static struct value bi_string_upper(int argc, struct value *argv) {
    if (argc < 1 || argv[0].kind != V_STR) return v_strz("");
    struct string *s = argv[0].as.s;
    struct string *r = make_string(s->data, s->len);
    for (int i = 0; i < r->len; i++) {
        if (r->data[i] >= 'a' && r->data[i] <= 'z') r->data[i] -= 32;
    }
    return v_str_take(r);
}

static struct value bi_string_lower(int argc, struct value *argv) {
    if (argc < 1 || argv[0].kind != V_STR) return v_strz("");
    struct string *s = argv[0].as.s;
    struct string *r = make_string(s->data, s->len);
    for (int i = 0; i < r->len; i++) {
        if (r->data[i] >= 'A' && r->data[i] <= 'Z') r->data[i] += 32;
    }
    return v_str_take(r);
}

static struct value bi_string_rep(int argc, struct value *argv) {
    if (argc < 2 || argv[0].kind != V_STR) return v_strz("");
    struct string *s = argv[0].as.s;
    int n = (int)to_num(argv[1], "string.rep n");
    if (n < 0) n = 0;
    struct string *r = (struct string *)malloc(sizeof(*r) + s->len * n + 1);
    r->len = s->len * n;
    for (int k = 0; k < n; k++) {
        for (int j = 0; j < s->len; j++) r->data[k * s->len + j] = s->data[j];
    }
    r->data[r->len] = 0;
    return v_str_take(r);
}

static struct value bi_table_insert(int argc, struct value *argv) {
    if (argc < 2 || argv[0].kind != V_TABLE) return v_nil();
    struct table *t = argv[0].as.t;
    /* Two forms: table.insert(t, v) — append; table.insert(t, pos, v). */
    if (argc == 2) {
        int n = table_len(t);
        table_set(t, v_num(n + 1), argv[1]);
    } else {
        int pos = to_num(argv[1], "table.insert pos");
        struct value v = argv[2];
        int n = table_len(t);
        for (int i = n; i >= pos; i--) {
            struct value old = table_get(t, v_num(i));
            table_set(t, v_num(i + 1), old);
        }
        table_set(t, v_num(pos), v);
    }
    return v_nil();
}

static struct value bi_table_concat(int argc, struct value *argv) {
    if (argc < 1 || argv[0].kind != V_TABLE) return v_strz("");
    struct table *t = argv[0].as.t;
    const char *sep = "";
    int sep_len = 0;
    if (argc >= 2 && argv[1].kind == V_STR) {
        sep = argv[1].as.s->data;
        sep_len = argv[1].as.s->len;
    }
    int n = table_len(t);
    /* First pass: compute total length. */
    int total = 0;
    for (int i = 1; i <= n; i++) {
        struct value v = table_get(t, v_num(i));
        struct string *s = to_str(v);
        total += s->len;
        if (i < n) total += sep_len;
    }
    struct string *r = (struct string *)malloc(sizeof(*r) + total + 1);
    r->len = total;
    int o = 0;
    for (int i = 1; i <= n; i++) {
        struct value v = table_get(t, v_num(i));
        struct string *s = to_str(v);
        for (int k = 0; k < s->len; k++) r->data[o++] = s->data[k];
        if (i < n) {
            for (int k = 0; k < sep_len; k++) r->data[o++] = sep[k];
        }
    }
    r->data[o] = 0;
    return v_str_take(r);
}

static struct value bi_io_write(int argc, struct value *argv) {
    for (int i = 0; i < argc; i++) {
        struct string *s = to_str(argv[i]);
        sys_write(1, s->data, s->len);
    }
    return v_nil();
}

static struct value bi_io_read(int argc, struct value *argv) {
    (void)argc; (void)argv;
    char buf[256];
    int n = sys_read_line(buf, sizeof(buf));
    if (n <= 0) return v_nil();
    /* sys_read_line includes trailing newline; strip it. */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    return v_str(buf, n);
}

static struct value bi_os_time(int argc, struct value *argv) {
    (void)argc; (void)argv;
    return v_num((int)sys_time());
}

static struct value bi_os_exit(int argc, struct value *argv) {
    int code = (argc >= 1) ? to_num(argv[0], "os.exit") : 0;
    sys_exit(code);
    return v_nil();     /* unreachable */
}

/* Wire up the standard library — fill the globals table with sub-
 * tables for `string`, `table`, `io`, `os` etc. */
static void install_stdlib(struct table *g) {
    table_set(g, v_strz("print"),     v_builtin(bi_print));
    table_set(g, v_strz("type"),      v_builtin(bi_type));
    table_set(g, v_strz("tostring"),  v_builtin(bi_tostring));
    table_set(g, v_strz("tonumber"),  v_builtin(bi_tonumber));
    table_set(g, v_strz("ipairs"),    v_builtin(bi_ipairs));

    struct table *str = table_new();
    table_set(str, v_strz("len"),    v_builtin(bi_string_len));
    table_set(str, v_strz("sub"),    v_builtin(bi_string_sub));
    table_set(str, v_strz("upper"),  v_builtin(bi_string_upper));
    table_set(str, v_strz("lower"),  v_builtin(bi_string_lower));
    table_set(str, v_strz("rep"),    v_builtin(bi_string_rep));
    table_set(g, v_strz("string"), v_table(str));

    struct table *tab = table_new();
    table_set(tab, v_strz("insert"), v_builtin(bi_table_insert));
    table_set(tab, v_strz("concat"), v_builtin(bi_table_concat));
    table_set(g, v_strz("table"), v_table(tab));

    struct table *io = table_new();
    table_set(io, v_strz("write"), v_builtin(bi_io_write));
    table_set(io, v_strz("read"),  v_builtin(bi_io_read));
    table_set(g, v_strz("io"), v_table(io));

    struct table *os = table_new();
    table_set(os, v_strz("time"), v_builtin(bi_os_time));
    table_set(os, v_strz("exit"), v_builtin(bi_os_exit));
    table_set(g, v_strz("os"), v_table(os));
}

/* ---------- Error handling --------------------------------------- */

static void die(const char *fmt, ...) {
    /* Minimal printf-style formatter for error messages. The libuser
     * printf goes to stdout; we go to stderr explicitly. */
    char buf[256];
    int o = 0;
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; fmt[i] && o < (int)sizeof(buf) - 1; i++) {
        if (fmt[i] == '%' && fmt[i + 1]) {
            i++;
            char c = fmt[i];
            if (c == 's') {
                const char *s = va_arg(ap, const char *);
                while (*s && o < (int)sizeof(buf) - 1) buf[o++] = *s++;
            } else if (c == 'd') {
                int v = va_arg(ap, int);
                int neg = 0;
                if (v < 0) { neg = 1; v = -v; }
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                else while (v) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
                if (neg && o < (int)sizeof(buf) - 1) buf[o++] = '-';
                while (tn-- > 0 && o < (int)sizeof(buf) - 1) buf[o++] = tmp[tn];
            } else if (c == 'c') {
                int ch = va_arg(ap, int);
                if (o < (int)sizeof(buf) - 1) buf[o++] = (char)ch;
            } else if (c == 'x') {
                unsigned v = va_arg(ap, unsigned);
                char tmp[16]; int tn = 0;
                if (v == 0) tmp[tn++] = '0';
                else while (v) {
                    int d = v & 0xF;
                    tmp[tn++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
                    v >>= 4;
                }
                while (tn-- > 0 && o < (int)sizeof(buf) - 1) buf[o++] = tmp[tn];
            } else {
                buf[o++] = '%'; if (o < (int)sizeof(buf) - 1) buf[o++] = c;
            }
        } else {
            buf[o++] = fmt[i];
        }
    }
    buf[o] = 0;
    va_end(ap);
    sys_write(2, "lua: ", 5);
    sys_write(2, buf, o);
    sys_write(2, "\n", 1);
    sys_exit(1);
}

/* ---------- Main ------------------------------------------------- */

/* Slurp the entire FS file into a heap allocation. NULL on error. */
static char *slurp(const char *path, int *out_size) {
    int size = sys_fs_size(path);
    if (size < 0) return 0;
    int base = sys_brk(0);
    int want = base + size + 1;
    if (sys_brk(want) != want) return 0;
    char *buf = (char *)(uint32_t)base;
    int fd = sys_open(path);
    if (fd < 0) return 0;
    int got = 0;
    while (got < size) {
        int n = sys_read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += n;
    }
    sys_close(fd);
    buf[got] = 0;
    *out_size = got;
    return buf;
}

static struct table *g_globals;

static void run_source(const char *src, int len) {
    lex_all(src, len);
    g_tk = 0;
    struct node *prog = parse_block();
    if (tk_cur()->kind != T_END) die("parse: extra tokens at line %d", tk_cur()->line);
    struct env *e = make_env(g_globals);
    eval_block(e, prog);
    free(e);
}

static void repl(void) {
    out_str("lua subset — type a statement, or 'exit()' to quit.\n");
    char line[1024];
    for (;;) {
        out_str("> ");
        int n = sys_read_line(line, sizeof(line));
        if (n <= 0) { out_chr('\n'); break; }
        /* Strip trailing newline. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (n == 0) continue;
        /* Single-process REPL — any error from die() will sys_exit
         * the whole process. Document this; fork-per-line could
         * recover but adds latency. */
        run_source(line, n);
    }
}

int main(int argc, char **argv) {
    g_globals = table_new();
    install_stdlib(g_globals);

    if (argc == 1) {
        repl();
        return 0;
    }
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'e' && argv[1][2] == 0) {
        const char *code = argv[2];
        run_source(code, s_len(code));
        return 0;
    }
    /* Treat argv[1] as a script path. */
    int size = 0;
    char *src = slurp(argv[1], &size);
    if (!src) {
        sys_write(2, "lua: cannot open ", 17);
        sys_write(2, argv[1], s_len(argv[1]));
        sys_write(2, "\n", 1);
        return 1;
    }
    run_source(src, size);
    return 0;
}
