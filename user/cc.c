/*
 * cc — AdventOS's tiny C-subset compiler.
 *
 * NOT a port of TinyCC. A from-scratch single-file compiler that
 * reads a C-subset source file and writes a runnable ELF32 binary.
 * Same scope philosophy as user/lua.c (session 87): pick a useful
 * subset, write it directly, ship something that works today; real
 * tcc / full C is a multi-session project that can come later.
 *
 *   cc FILE.c [-o OUT.elf]    compile FILE.c, write OUT.elf
 *                             (default OUT = FILE without .c, with
 *                              .elf appended)
 *
 * SUBSET
 *   Types     int (signed 32-bit). No char/short/long/float/double.
 *             No pointers, no arrays, no structs, no typedefs.
 *
 *   Operators + - * / %    == != < > <= >=    && ||  !
 *             & | ^ ~ << >>    unary -
 *             Assignment is a statement, not an expression.
 *
 *   Statements
 *             local var:        int x;  or  int x = expr;
 *             assignment:       name = expr;
 *             expression-stmt:  expr;     (call results discarded)
 *             if/else:          if (e) stmt [else stmt]
 *             while:            while (e) stmt
 *             return:           return [expr];
 *             block:            { stmts }
 *
 *   Functions
 *             int NAME(int p1, int p2, ...) { body }
 *             Forward references work via two-pass linkage (parse
 *             collects function entries; codegen patches calls).
 *
 *   Built-in intrinsics — recognized by name; emitted inline:
 *             sys_exit(code)            -> syscall 3 (no return)
 *             sys_write(fd, addr, n)    -> syscall 12, returns bytes
 *             sys_getpid()              -> syscall 2, returns pid
 *             print_int(n)              -> emits N + newline to stdout
 *
 *   No preprocessor (no #include, no #define), no global variables,
 *   no string literals, no goto, no switch, no comma-operator, no
 *   ternary, no compound assignment (+= etc.), no ++/--.
 *
 * CODEGEN
 *   Stack-based, EAX-as-result. Every expression computes its result
 *   into EAX. Multi-operand subexpressions push intermediate to the
 *   stack and pop into EBX. Locals live on the stack at EBP-relative
 *   offsets. cdecl: args pushed right-to-left, caller cleans up.
 *
 *   Not optimized. The codegen does no constant folding, no register
 *   allocation past EAX/EBX, no peephole. Produces correct but bulky
 *   machine code — fine for the daily-use scripting target.
 *
 * OUTPUT
 *   ELF32 EXEC binary at VA 0x40000000. One PT_LOAD segment
 *   (RWX) containing code + start-stub. Same shape mkfs.py
 *   produces for the rest of the userland; produced runtime-ready
 *   so `cc hello.c -o hello.elf` followed by `hello.elf` Just Works.
 *
 * KNOWN LIMITS / next-session candidates
 *   - char / pointer / array types
 *   - string literals (currently no way to print "hello world")
 *   - global variables
 *   - preprocessor (#include, #define)
 *   - more operators (compound assign, ++/--, ternary, comma)
 *   - struct / typedef / enum
 *   - function pointers
 *   - the linker step (multi-file compilation)
 */

#include "libuser.h"

/* ---------- Tiny utilities ---------------------------------------- */

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int my_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int g_exit_code;

static void die(const char *msg) {
    sys_write(2, "cc: ", 4);
    sys_write(2, msg, my_strlen(msg));
    sys_write(2, "\n", 1);
    sys_exit(1);
}

static void die_at(int line, const char *what, const char *detail) {
    char buf[80];
    int o = 0;
    const char *p = "cc: line ";
    while (*p) buf[o++] = *p++;
    char tmp[12]; int tn = 0;
    int v = line;
    if (v == 0) tmp[tn++] = '0';
    else while (v) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
    while (tn--) buf[o++] = tmp[tn];
    buf[o++] = ':'; buf[o++] = ' ';
    while (*what && o < (int)sizeof(buf) - 2) buf[o++] = *what++;
    if (detail) {
        buf[o++] = ' ';
        while (*detail && o < (int)sizeof(buf) - 1) buf[o++] = *detail++;
    }
    buf[o++] = '\n';
    sys_write(2, buf, o);
    sys_exit(1);
}

/* ---------- Lexer ------------------------------------------------- */

enum {
    T_END = 0, T_NUM, T_NAME,
    T_INT, T_IF, T_ELSE, T_WHILE, T_RETURN, T_FOR,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
    T_SEMI, T_COMMA, T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE,
    T_AMP_AMP, T_PIPE_PIPE, T_BANG,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_LSHIFT, T_RSHIFT,
};

#define NAME_MAX 24

struct tok_t {
    int  kind;
    int  num;
    char name[NAME_MAX];
    int  line;
};

/* Session 90 sizing: 2048 tokens × 36 bytes = 72 KiB of BSS.
 * That fits a few-hundred-line C program. user.ld folds .bss into
 * .data so the .bin pays for every byte of g_toks even when the
 * source is empty — keep the cap tight. NAME_MAX=24 covers every
 * identifier you'd write by hand (printf, sys_write, calculate_x...). */
#define MAX_TOKS 2048
static struct tok_t g_toks[MAX_TOKS];
static int          g_n_toks;
static int          g_tk;

static const char *g_src;
static int         g_src_len;
static int         g_pos;
static int         g_line = 1;

static int is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_id_cont(char c)  { return is_id_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(char c)    { return c >= '0' && c <= '9'; }

static int kw_lookup(const char *s) {
    if (my_streq(s, "int"))    return T_INT;
    if (my_streq(s, "if"))     return T_IF;
    if (my_streq(s, "else"))   return T_ELSE;
    if (my_streq(s, "while"))  return T_WHILE;
    if (my_streq(s, "return")) return T_RETURN;
    if (my_streq(s, "for"))    return T_FOR;
    return T_NAME;
}

static void push_tok(int kind, int line) {
    if (g_n_toks >= MAX_TOKS) die("too many tokens");
    g_toks[g_n_toks].kind = kind;
    g_toks[g_n_toks].num  = 0;
    g_toks[g_n_toks].name[0] = 0;
    g_toks[g_n_toks].line = line;
    g_n_toks++;
}

static void lex_all(const char *src, int len) {
    g_src = src; g_src_len = len; g_pos = 0; g_line = 1;
    g_n_toks = 0;
    while (g_pos < g_src_len) {
        char c = g_src[g_pos];
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '\n') { g_line++; g_pos++; continue; }
        /* `//` to end of line. */
        if (c == '/' && g_pos + 1 < g_src_len && g_src[g_pos + 1] == '/') {
            g_pos += 2;
            while (g_pos < g_src_len && g_src[g_pos] != '\n') g_pos++;
            continue;
        }
        /* Slash-star ... star-slash block comments. */
        if (c == '/' && g_pos + 1 < g_src_len && g_src[g_pos + 1] == '*') {
            g_pos += 2;
            while (g_pos + 1 < g_src_len &&
                   !(g_src[g_pos] == '*' && g_src[g_pos + 1] == '/')) {
                if (g_src[g_pos] == '\n') g_line++;
                g_pos++;
            }
            g_pos += 2;
            continue;
        }
        /* Numbers — decimal only. */
        if (is_digit(c)) {
            int start = g_pos;
            int v = 0;
            while (g_pos < g_src_len && is_digit(g_src[g_pos])) {
                v = v * 10 + (g_src[g_pos] - '0');
                g_pos++;
            }
            (void)start;
            push_tok(T_NUM, g_line);
            g_toks[g_n_toks - 1].num = v;
            continue;
        }
        /* Identifier / keyword. */
        if (is_id_start(c)) {
            int start = g_pos;
            while (g_pos < g_src_len && is_id_cont(g_src[g_pos])) g_pos++;
            int n = g_pos - start;
            if (n >= NAME_MAX) die_at(g_line, "identifier too long", 0);
            char tmp[NAME_MAX];
            for (int i = 0; i < n; i++) tmp[i] = g_src[start + i];
            tmp[n] = 0;
            int kw = kw_lookup(tmp);
            push_tok(kw, g_line);
            if (kw == T_NAME) {
                for (int i = 0; i < n; i++) g_toks[g_n_toks - 1].name[i] = tmp[i];
                g_toks[g_n_toks - 1].name[n] = 0;
            }
            continue;
        }
        /* Operators / punctuation. */
        switch (c) {
            case '(': push_tok(T_LPAREN, g_line); g_pos++; break;
            case ')': push_tok(T_RPAREN, g_line); g_pos++; break;
            case '{': push_tok(T_LBRACE, g_line); g_pos++; break;
            case '}': push_tok(T_RBRACE, g_line); g_pos++; break;
            case ';': push_tok(T_SEMI,   g_line); g_pos++; break;
            case ',': push_tok(T_COMMA,  g_line); g_pos++; break;
            case '+': push_tok(T_PLUS,   g_line); g_pos++; break;
            case '-': push_tok(T_MINUS,  g_line); g_pos++; break;
            case '*': push_tok(T_STAR,   g_line); g_pos++; break;
            case '/': push_tok(T_SLASH,  g_line); g_pos++; break;
            case '%': push_tok(T_PERCENT,g_line); g_pos++; break;
            case '~': push_tok(T_TILDE,  g_line); g_pos++; break;
            case '^': push_tok(T_CARET,  g_line); g_pos++; break;
            case '=':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_EQ, g_line); g_pos += 2;
                } else { push_tok(T_ASSIGN, g_line); g_pos++; }
                break;
            case '!':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_NEQ, g_line); g_pos += 2;
                } else { push_tok(T_BANG, g_line); g_pos++; }
                break;
            case '<':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_LE, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '<') {
                    push_tok(T_LSHIFT, g_line); g_pos += 2;
                } else { push_tok(T_LT, g_line); g_pos++; }
                break;
            case '>':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_GE, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '>') {
                    push_tok(T_RSHIFT, g_line); g_pos += 2;
                } else { push_tok(T_GT, g_line); g_pos++; }
                break;
            case '&':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '&') {
                    push_tok(T_AMP_AMP, g_line); g_pos += 2;
                } else { push_tok(T_AMP, g_line); g_pos++; }
                break;
            case '|':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '|') {
                    push_tok(T_PIPE_PIPE, g_line); g_pos += 2;
                } else { push_tok(T_PIPE, g_line); g_pos++; }
                break;
            default:
                die_at(g_line, "stray character", 0);
        }
    }
    push_tok(T_END, g_line);
    g_tk = 0;
}

static struct tok_t *tk_cur(void) { return &g_toks[g_tk]; }
static struct tok_t *tk_peek(int d) { return &g_toks[g_tk + d]; }
static int  accept(int k) { if (g_toks[g_tk].kind == k) { g_tk++; return 1; } return 0; }
static void expect(int k, const char *what) {
    if (g_toks[g_tk].kind != k) die_at(g_toks[g_tk].line, "expected", what);
    g_tk++;
}

/* ---------- AST --------------------------------------------------- */

enum {
    N_NUM, N_NAME,
    N_BIN, N_UN, N_CALL,
    N_VAR_DECL,   /* int NAME [= expr]; */
    N_ASSIGN,     /* NAME = expr;       */
    N_RETURN,
    N_IF,
    N_WHILE,
    N_BLOCK,
    N_EXPR_STMT,
    N_FUNC_DECL,
    N_PROGRAM,
};

struct node {
    int            kind;
    int            line;
    int            op;
    int            num;
    char           name[NAME_MAX];
    struct node   *a, *b, *c;
    struct node  **list;
    int            n_list;
    struct node  **params;
    int            n_params;
    struct node   *body;
};

static struct node *new_node(int kind) {
    struct node *n = (struct node *)malloc(sizeof(*n));
    n->kind = kind;
    n->line = tk_cur()->line;
    n->op   = 0;
    n->num  = 0;
    n->name[0] = 0;
    n->a = n->b = n->c = 0;
    n->list = 0; n->n_list = 0;
    n->params = 0; n->n_params = 0;
    n->body = 0;
    return n;
}

static void node_push(struct node ***arr, int *n, int *cap, struct node *e) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        struct node **nb = (struct node **)malloc(sizeof(struct node *) * nc);
        for (int i = 0; i < *n; i++) nb[i] = (*arr)[i];
        if (*arr) free(*arr);
        *arr = nb;
        *cap = nc;
    }
    (*arr)[(*n)++] = e;
}

/* ---------- Parser (recursive descent) ---------------------------- */

static struct node *parse_expr(void);
static struct node *parse_block(void);
static struct node *parse_stmt(void);

/* Precedence (higher = tighter). C semantics: */
static int binop_prec(int t) {
    switch (t) {
        case T_PIPE_PIPE:                                                  return 1;
        case T_AMP_AMP:                                                    return 2;
        case T_PIPE:                                                       return 3;
        case T_CARET:                                                      return 4;
        case T_AMP:                                                        return 5;
        case T_EQ: case T_NEQ:                                             return 6;
        case T_LT: case T_GT: case T_LE: case T_GE:                        return 7;
        case T_LSHIFT: case T_RSHIFT:                                      return 8;
        case T_PLUS: case T_MINUS:                                         return 9;
        case T_STAR: case T_SLASH: case T_PERCENT:                         return 10;
    }
    return 0;
}

static struct node *parse_primary(void) {
    struct tok_t *t = tk_cur();
    if (t->kind == T_NUM) { struct node *n = new_node(N_NUM); n->num = t->num; g_tk++; return n; }
    if (t->kind == T_NAME) {
        struct node *n = new_node(N_NAME);
        int i = 0;
        while (t->name[i]) { n->name[i] = t->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;
        /* Call? */
        if (tk_cur()->kind == T_LPAREN) {
            g_tk++;
            struct node *call = new_node(N_CALL);
            int j = 0;
            while (n->name[j]) { call->name[j] = n->name[j]; j++; }
            call->name[j] = 0;
            int cap = 0;
            while (tk_cur()->kind != T_RPAREN) {
                node_push(&call->list, &call->n_list, &cap, parse_expr());
                if (!accept(T_COMMA)) break;
            }
            expect(T_RPAREN, "')'");
            return call;
        }
        return n;
    }
    if (t->kind == T_LPAREN) { g_tk++; struct node *e = parse_expr(); expect(T_RPAREN, "')'"); return e; }
    if (t->kind == T_MINUS) {
        g_tk++;
        struct node *n = new_node(N_UN);
        n->op = T_MINUS;
        n->a = parse_primary();
        return n;
    }
    if (t->kind == T_BANG) {
        g_tk++;
        struct node *n = new_node(N_UN);
        n->op = T_BANG;
        n->a = parse_primary();
        return n;
    }
    if (t->kind == T_TILDE) {
        g_tk++;
        struct node *n = new_node(N_UN);
        n->op = T_TILDE;
        n->a = parse_primary();
        return n;
    }
    die_at(t->line, "unexpected token in expression", 0);
    return 0;
}

static struct node *parse_binop_rhs(int min_prec, struct node *lhs) {
    for (;;) {
        int p = binop_prec(tk_cur()->kind);
        if (p < min_prec) return lhs;
        int op = tk_cur()->kind;
        g_tk++;
        struct node *rhs = parse_primary();
        int p2 = binop_prec(tk_cur()->kind);
        if (p2 > p) rhs = parse_binop_rhs(p + 1, rhs);
        struct node *n = new_node(N_BIN);
        n->op = op; n->a = lhs; n->b = rhs;
        lhs = n;
    }
}

static struct node *parse_expr(void) {
    return parse_binop_rhs(1, parse_primary());
}

static struct node *parse_block(void) {
    struct node *blk = new_node(N_BLOCK);
    expect(T_LBRACE, "'{'");
    int cap = 0;
    while (tk_cur()->kind != T_RBRACE) {
        node_push(&blk->list, &blk->n_list, &cap, parse_stmt());
    }
    expect(T_RBRACE, "'}'");
    return blk;
}

static struct node *parse_stmt(void) {
    int t = tk_cur()->kind;
    if (t == T_INT) {
        /* int NAME [= expr]; — local declaration */
        g_tk++;
        if (tk_cur()->kind != T_NAME) die_at(tk_cur()->line, "expected name after 'int'", 0);
        struct node *n = new_node(N_VAR_DECL);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;
        if (accept(T_ASSIGN)) n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    if (t == T_RETURN) {
        g_tk++;
        struct node *n = new_node(N_RETURN);
        if (tk_cur()->kind != T_SEMI) n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    if (t == T_IF) {
        g_tk++;
        expect(T_LPAREN, "'('");
        struct node *n = new_node(N_IF);
        n->a = parse_expr();
        expect(T_RPAREN, "')'");
        n->b = parse_stmt();
        if (accept(T_ELSE)) n->c = parse_stmt();
        return n;
    }
    if (t == T_WHILE) {
        g_tk++;
        expect(T_LPAREN, "'('");
        struct node *n = new_node(N_WHILE);
        n->a = parse_expr();
        expect(T_RPAREN, "')'");
        n->b = parse_stmt();
        return n;
    }
    if (t == T_LBRACE) return parse_block();
    /* Expression-stmt or assignment.
     * `NAME = expr;`  or  `expr;` */
    if (t == T_NAME && tk_peek(1)->kind == T_ASSIGN) {
        struct node *n = new_node(N_ASSIGN);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;
        g_tk++;     /* skip '=' */
        n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    struct node *e = parse_expr();
    expect(T_SEMI, "';'");
    struct node *es = new_node(N_EXPR_STMT);
    es->a = e;
    return es;
}

static struct node *parse_func(void) {
    expect(T_INT, "'int' return type");
    if (tk_cur()->kind != T_NAME) die_at(tk_cur()->line, "expected function name", 0);
    struct node *fn = new_node(N_FUNC_DECL);
    int i = 0;
    while (tk_cur()->name[i]) { fn->name[i] = tk_cur()->name[i]; i++; }
    fn->name[i] = 0;
    g_tk++;
    expect(T_LPAREN, "'('");
    int cap = 0;
    while (tk_cur()->kind != T_RPAREN) {
        expect(T_INT, "'int' (param type)");
        if (tk_cur()->kind != T_NAME) die_at(tk_cur()->line, "expected param name", 0);
        struct node *p = new_node(N_NAME);
        int j = 0;
        while (tk_cur()->name[j]) { p->name[j] = tk_cur()->name[j]; j++; }
        p->name[j] = 0;
        g_tk++;
        node_push(&fn->params, &fn->n_params, &cap, p);
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN, "')'");
    fn->body = parse_block();
    return fn;
}

static struct node *parse_program(void) {
    struct node *p = new_node(N_PROGRAM);
    int cap = 0;
    while (tk_cur()->kind != T_END) {
        node_push(&p->list, &p->n_list, &cap, parse_func());
    }
    return p;
}

/* ---------- Code generator (i386, stack-based, EAX-result) -------- */

/* Session 90 sizing: 32 KiB of emitted code per compiled program.
 * A compiled function is ~50–200 bytes (lots of stack-machine push/pop),
 * so 32 KiB fits ~100–500 functions. */
#define CODE_MAX 32768
static unsigned char g_code[CODE_MAX];
static int           g_code_len;

#define ENTRY_VA  0x40000000u
#define EHDR_SIZE 52
#define PHDR_SIZE 32
#define CODE_OFF  (EHDR_SIZE + PHDR_SIZE)   /* file offset of code */

static void emit_b(unsigned char b) {
    if (g_code_len >= CODE_MAX) die("code section overflow");
    g_code[g_code_len++] = b;
}
static void emit_d(unsigned int d) {
    emit_b((unsigned char)(d & 0xff));
    emit_b((unsigned char)((d >> 8) & 0xff));
    emit_b((unsigned char)((d >> 16) & 0xff));
    emit_b((unsigned char)((d >> 24) & 0xff));
}
static void patch_d(int off, unsigned int d) {
    g_code[off + 0] = (unsigned char)(d & 0xff);
    g_code[off + 1] = (unsigned char)((d >> 8) & 0xff);
    g_code[off + 2] = (unsigned char)((d >> 16) & 0xff);
    g_code[off + 3] = (unsigned char)((d >> 24) & 0xff);
}

/* Instruction primitives. Keep these named after what they do; the
 * compiler logic stays readable while the actual byte patterns are
 * documented here once. */
static void e_push_eax(void)            { emit_b(0x50); }
static void e_push_ebx(void)            { emit_b(0x53); }
static void e_pop_eax(void)             { emit_b(0x58); }
static void e_pop_ebx(void)             { emit_b(0x5b); }
static void e_pop_ecx(void)             { emit_b(0x59); }
static void e_mov_eax_imm(int v)        { emit_b(0xb8); emit_d((unsigned)v); }
static void e_mov_ebx_imm(int v)        { emit_b(0xbb); emit_d((unsigned)v); }
static void e_mov_ebx_eax(void)         { emit_b(0x89); emit_b(0xc3); }
static void e_mov_eax_ebx(void)         { emit_b(0x89); emit_b(0xd8); }
static void e_mov_edx_eax(void)         { emit_b(0x89); emit_b(0xc2); }
static void e_push_esi(void)            { emit_b(0x56); }
static void e_push_edi(void)            { emit_b(0x57); }
static void e_pop_esi(void)             { emit_b(0x5e); }
static void e_pop_edi(void)             { emit_b(0x5f); }
static void e_mov_esp_ebp(void)         { emit_b(0x89); emit_b(0xec); }
static void e_mov_ebp_esp(void)         { emit_b(0x89); emit_b(0xe5); }
static void e_pop_ebp(void)             { emit_b(0x5d); }
static void e_push_ebp(void)            { emit_b(0x55); }
static void e_ret(void)                 { emit_b(0xc3); }
static void e_int_0x80(void)            { emit_b(0xcd); emit_b(0x80); }
static void e_hlt(void)                 { emit_b(0xf4); }
static void e_add_eax_ebx(void)         { emit_b(0x01); emit_b(0xd8); }
static void e_sub_eax_ebx(void)         { emit_b(0x29); emit_b(0xd8); }
static void e_imul_eax_ebx(void)        { emit_b(0x0f); emit_b(0xaf); emit_b(0xc3); }
static void e_idiv_ebx(void)            { /* cdq + idiv ebx */
    emit_b(0x99);
    emit_b(0xf7); emit_b(0xfb);
}
static void e_and_eax_ebx(void)         { emit_b(0x21); emit_b(0xd8); }
static void e_or_eax_ebx(void)          { emit_b(0x09); emit_b(0xd8); }
static void e_xor_eax_ebx(void)         { emit_b(0x31); emit_b(0xd8); }
static void e_not_eax(void)             { emit_b(0xf7); emit_b(0xd0); }
static void e_neg_eax(void)             { emit_b(0xf7); emit_b(0xd8); }
static void e_cmp_eax_ebx(void)         { emit_b(0x39); emit_b(0xd8); }
/* setcc_al + movzx eax, al — Lua-style boolean result */
static void e_set_zf_eq(void)           { emit_b(0x0f); emit_b(0x94); emit_b(0xc0); /* sete al   */
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); /* movzx eax */ }
static void e_set_zf_ne(void)           { emit_b(0x0f); emit_b(0x95); emit_b(0xc0);
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); }
static void e_set_sl_lt(void)           { emit_b(0x0f); emit_b(0x9c); emit_b(0xc0);
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); }
static void e_set_sl_gt(void)           { emit_b(0x0f); emit_b(0x9f); emit_b(0xc0);
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); }
static void e_set_sl_le(void)           { emit_b(0x0f); emit_b(0x9e); emit_b(0xc0);
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); }
static void e_set_sl_ge(void)           { emit_b(0x0f); emit_b(0x9d); emit_b(0xc0);
                                          emit_b(0x0f); emit_b(0xb6); emit_b(0xc0); }
/* test eax,eax  +  setnz al  +  movzx eax,al — !eax => 0 if eax!=0, 1 if eax==0
 * For unary "not": setz al gives 1 if eax==0. */
static void e_logical_not_eax(void) {
    emit_b(0x85); emit_b(0xc0);        /* test eax, eax */
    emit_b(0x0f); emit_b(0x94); emit_b(0xc0);   /* sete al */
    emit_b(0x0f); emit_b(0xb6); emit_b(0xc0);   /* movzx eax, al */
}
/* shift: mov ecx, ebx; shl/sar eax, cl */
static void e_shl_eax_ebx(void) {
    emit_b(0x89); emit_b(0xd9);                 /* mov ecx, ebx */
    emit_b(0xd3); emit_b(0xe0);                 /* shl eax, cl  */
}
static void e_shr_eax_ebx(void) {
    emit_b(0x89); emit_b(0xd9);                 /* mov ecx, ebx */
    emit_b(0xd3); emit_b(0xf8);                 /* sar eax, cl  */
}
/* mov eax, [ebp + disp8] for local-load; mov [ebp+disp8], eax for store */
static void e_load_local(int off_from_ebp) {
    /* 8B 45 imm8 if -128 <= disp <= 127 ; else 8B 85 imm32 */
    if (off_from_ebp >= -128 && off_from_ebp <= 127) {
        emit_b(0x8b); emit_b(0x45); emit_b((unsigned char)(off_from_ebp & 0xff));
    } else {
        emit_b(0x8b); emit_b(0x85); emit_d((unsigned)off_from_ebp);
    }
}
static void e_store_local(int off_from_ebp) {
    if (off_from_ebp >= -128 && off_from_ebp <= 127) {
        emit_b(0x89); emit_b(0x45); emit_b((unsigned char)(off_from_ebp & 0xff));
    } else {
        emit_b(0x89); emit_b(0x85); emit_d((unsigned)off_from_ebp);
    }
}
/* sub esp, imm32  /  add esp, imm32 */
static void e_sub_esp_imm32(int v) {
    emit_b(0x81); emit_b(0xec); emit_d((unsigned)v);
}
static void e_add_esp_imm32(int v) {
    emit_b(0x81); emit_b(0xc4); emit_d((unsigned)v);
}
/* jmp rel32 / jz rel32 / jnz rel32; returns the file offset of the
 * 4-byte displacement so the caller can patch it after the target is
 * known. */
static int e_jmp_rel32(void) {
    emit_b(0xe9); emit_d(0);
    return g_code_len - 4;
}
static int e_jz_rel32(void) {
    emit_b(0x0f); emit_b(0x84); emit_d(0);
    return g_code_len - 4;
}
static int e_jnz_rel32(void) {
    emit_b(0x0f); emit_b(0x85); emit_d(0);
    return g_code_len - 4;
}
/* call rel32 — returns file offset of the displacement for patching. */
static int e_call_rel32(void) {
    emit_b(0xe8); emit_d(0);
    return g_code_len - 4;
}

/* test eax, eax — sets ZF if eax == 0. Used by if/while conditions. */
static void e_test_eax_eax(void) {
    emit_b(0x85); emit_b(0xc0);
}

/* ---------- Symbol tables ----------------------------------------- */

#define MAX_FUNCS  64
#define MAX_LOCALS 64
#define MAX_FIXUPS 256

struct func_info {
    char name[NAME_MAX];
    int  entry_off;     /* byte offset within g_code where the function starts */
    int  n_params;
    int  defined;       /* once codegen-resolved (set when entry_off is real) */
};

static struct func_info g_funcs[MAX_FUNCS];
static int              g_n_funcs;

/* Pending call-target fixups: rel32 placeholders that need to be
 * patched once the called function's entry_off is known. */
struct fixup {
    int  call_disp_off;     /* file offset of the 4-byte displacement */
    int  func_idx;          /* g_funcs index */
};
static struct fixup g_fixups[MAX_FIXUPS];
static int          g_n_fixups;

/* Per-function local table — reset at the start of each function. */
struct local_slot {
    char name[NAME_MAX];
    int  ebp_off;       /* negative offset from EBP */
};
static struct local_slot g_locals[MAX_LOCALS];
static int               g_n_locals;
static int               g_locals_bytes;    /* total bytes of locals in current fn */

static int func_find(const char *name) {
    for (int i = 0; i < g_n_funcs; i++) {
        if (my_streq(g_funcs[i].name, name)) return i;
    }
    return -1;
}

static int func_intern(const char *name, int n_params) {
    int i = func_find(name);
    if (i >= 0) {
        if (g_funcs[i].n_params != n_params)
            die_at(0, "arg-count mismatch for", name);
        return i;
    }
    if (g_n_funcs >= MAX_FUNCS) die("too many functions");
    int idx = g_n_funcs++;
    int j = 0;
    while (name[j]) { g_funcs[idx].name[j] = name[j]; j++; }
    g_funcs[idx].name[j] = 0;
    g_funcs[idx].entry_off = -1;
    g_funcs[idx].n_params  = n_params;
    g_funcs[idx].defined   = 0;
    return idx;
}

static int local_find(const char *name) {
    for (int i = g_n_locals - 1; i >= 0; i--) {
        if (my_streq(g_locals[i].name, name)) return g_locals[i].ebp_off;
    }
    return 0;     /* 0 == not found (real locals have negative offsets) */
}

static int local_declare(const char *name) {
    if (g_n_locals >= MAX_LOCALS) die("too many locals");
    g_locals_bytes += 4;
    int off = -g_locals_bytes;
    int j = 0;
    while (name[j]) { g_locals[g_n_locals].name[j] = name[j]; j++; }
    g_locals[g_n_locals].name[j] = 0;
    g_locals[g_n_locals].ebp_off = off;
    g_n_locals++;
    return off;
}

/* ---------- Codegen ----------------------------------------------- */

static void gen_expr(struct node *n);
static void gen_stmt(struct node *n);

/* Emit a syscall: eax=num, args from ARG_REG[]. Used by intrinsics.
 * Caller has already pushed args (right-to-left, cdecl-style) AND
 * adjusted ESP if needed. We pop them into registers explicitly. */
static void emit_syscall_intrinsic(const char *name, struct node *call) {
    /* sys_exit(code) — syscall 3, ebx = code */
    if (my_streq(name, "sys_exit")) {
        if (call->n_list != 1) die_at(call->line, "sys_exit takes 1 arg", 0);
        gen_expr(call->list[0]);
        e_mov_ebx_eax();        /* ebx = code */
        e_mov_eax_imm(3);       /* eax = SYS_EXIT */
        e_int_0x80();
        return;
    }
    /* sys_write(fd, addr, n) — syscall 12, ebx=fd, ecx=addr, edx=n */
    if (my_streq(name, "sys_write")) {
        if (call->n_list != 3) die_at(call->line, "sys_write takes 3 args", 0);
        gen_expr(call->list[0]);  e_push_eax();      /* fd  on stack */
        gen_expr(call->list[1]);  e_push_eax();      /* addr on stack */
        gen_expr(call->list[2]);                     /* eax = n */
        e_mov_edx_eax();
        /* Wait — sys_write is "void sys_write(int fd, const char *buf, int n)"
         * with kernel SYS_WRITE_FD = 12. But the user lib does it as a
         * positional fd/buf/n syscall via ebx/ecx/edx. We need:
         *   ebx = fd  (popped)
         *   ecx = addr (popped)
         *   edx = n   (already eax above, moved to edx)
         */
        e_pop_ecx();    /* ecx = addr */
        e_pop_ebx();    /* ebx = fd   */
        e_mov_eax_imm(12);
        e_int_0x80();
        /* return value already in eax */
        return;
    }
    /* sys_getpid() — syscall 2, no args, returns pid in eax */
    if (my_streq(name, "sys_getpid")) {
        if (call->n_list != 0) die_at(call->line, "sys_getpid takes no args", 0);
        e_mov_eax_imm(2);
        e_int_0x80();
        return;
    }
    /* print_int(n) — emits the decimal of n followed by '\n' to fd=1.
     *
     * Bulky inline expansion. To keep it tractable we emit a stub
     * that calls back to a fixed helper at code offset
     * g_print_int_helper, set up once at the top of the binary. */
    if (my_streq(name, "print_int")) {
        if (call->n_list != 1) die_at(call->line, "print_int takes 1 arg", 0);
        gen_expr(call->list[0]);
        e_push_eax();
        /* call print_int_helper — fixup later. */
        int idx = func_find("__print_int_helper");
        if (idx < 0) die("print_int helper missing");
        int disp_off = e_call_rel32();
        if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
        g_fixups[g_n_fixups].call_disp_off = disp_off;
        g_fixups[g_n_fixups].func_idx = idx;
        g_n_fixups++;
        /* Pop the arg we pushed (cdecl caller-cleanup). */
        e_add_esp_imm32(4);
        return;
    }
    die_at(call->line, "unknown intrinsic", name);
}

/* True if `name` is a built-in intrinsic. */
static int is_intrinsic(const char *name) {
    return my_streq(name, "sys_exit")
        || my_streq(name, "sys_write")
        || my_streq(name, "sys_getpid")
        || my_streq(name, "print_int");
}

static void gen_call(struct node *call) {
    if (is_intrinsic(call->name)) {
        emit_syscall_intrinsic(call->name, call);
        return;
    }
    /* User function call. Push args right-to-left. */
    int argc = call->n_list;
    for (int i = argc - 1; i >= 0; i--) {
        gen_expr(call->list[i]);
        e_push_eax();
    }
    int idx = func_intern(call->name, argc);
    int disp_off = e_call_rel32();
    if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
    g_fixups[g_n_fixups].call_disp_off = disp_off;
    g_fixups[g_n_fixups].func_idx = idx;
    g_n_fixups++;
    /* Caller cleans up. */
    if (argc > 0) e_add_esp_imm32(argc * 4);
}

static void gen_expr(struct node *n) {
    if (!n) { e_mov_eax_imm(0); return; }
    switch (n->kind) {
        case N_NUM: e_mov_eax_imm(n->num); return;
        case N_NAME: {
            int off = local_find(n->name);
            if (off == 0) die_at(n->line, "undefined variable", n->name);
            e_load_local(off);
            return;
        }
        case N_CALL: gen_call(n); return;
        case N_UN: {
            gen_expr(n->a);
            switch (n->op) {
                case T_MINUS: e_neg_eax();          break;
                case T_BANG:  e_logical_not_eax();  break;
                case T_TILDE: e_not_eax();          break;
                default: die_at(n->line, "unknown unary op", 0);
            }
            return;
        }
        case N_BIN: {
            /* Short-circuit && and ||. */
            if (n->op == T_AMP_AMP) {
                gen_expr(n->a);
                e_test_eax_eax();
                int jz = e_jz_rel32();
                gen_expr(n->b);
                e_test_eax_eax();
                e_set_zf_ne();           /* normalize to 0/1 */
                int jend = e_jmp_rel32();
                patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
                e_mov_eax_imm(0);
                patch_d(jend, (unsigned)(g_code_len - (jend + 4)));
                return;
            }
            if (n->op == T_PIPE_PIPE) {
                gen_expr(n->a);
                e_test_eax_eax();
                int jnz = e_jnz_rel32();
                gen_expr(n->b);
                e_test_eax_eax();
                e_set_zf_ne();
                int jend = e_jmp_rel32();
                patch_d(jnz, (unsigned)(g_code_len - (jnz + 4)));
                e_mov_eax_imm(1);
                patch_d(jend, (unsigned)(g_code_len - (jend + 4)));
                return;
            }
            /* Standard arithmetic / comparison: compute LHS in eax,
             * push; compute RHS in eax; pop LHS into ebx -> reorder
             * so that the op operates on (eax = LHS, ebx = RHS). */
            gen_expr(n->a);
            e_push_eax();
            gen_expr(n->b);
            e_mov_ebx_eax();
            e_pop_eax();
            switch (n->op) {
                case T_PLUS:    e_add_eax_ebx();   break;
                case T_MINUS:   e_sub_eax_ebx();   break;
                case T_STAR:    e_imul_eax_ebx();  break;
                case T_SLASH:   e_idiv_ebx();      break;
                case T_PERCENT: e_idiv_ebx();      e_mov_eax_ebx(); /* idiv: eax=quot, edx=rem; we want rem */
                                /* Actually: idiv writes EAX=quot, EDX=rem. We want EDX into EAX. */
                                /* Override the previous mov_eax_ebx — emit mov eax, edx. */
                                /* Easier to undo: backtrack the last 2 bytes and emit mov eax, edx. */
                                g_code_len -= 2;
                                emit_b(0x89); emit_b(0xd0); /* mov eax, edx */
                                break;
                case T_AMP:     e_and_eax_ebx();   break;
                case T_PIPE:    e_or_eax_ebx();    break;
                case T_CARET:   e_xor_eax_ebx();   break;
                case T_LSHIFT:  e_shl_eax_ebx();   break;
                case T_RSHIFT:  e_shr_eax_ebx();   break;
                case T_EQ:      e_cmp_eax_ebx(); e_set_zf_eq(); break;
                case T_NEQ:     e_cmp_eax_ebx(); e_set_zf_ne(); break;
                case T_LT:      e_cmp_eax_ebx(); e_set_sl_lt(); break;
                case T_GT:      e_cmp_eax_ebx(); e_set_sl_gt(); break;
                case T_LE:      e_cmp_eax_ebx(); e_set_sl_le(); break;
                case T_GE:      e_cmp_eax_ebx(); e_set_sl_ge(); break;
                default: die_at(n->line, "unknown binop", 0);
            }
            return;
        }
    }
    die_at(n->line, "unsupported expr kind", 0);
}

static void gen_stmt(struct node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_BLOCK: {
            /* Block scoping: locals declared inside the block stop being
             * NAME-visible when we leave, but their stack space stays
             * reserved by the function prologue. g_locals_bytes is the
             * watermark used to size `sub esp, N` — never roll it back. */
            int saved_locals = g_n_locals;
            for (int i = 0; i < n->n_list; i++) gen_stmt(n->list[i]);
            g_n_locals = saved_locals;
            return;
        }
        case N_VAR_DECL: {
            local_declare(n->name);
            int off = g_locals[g_n_locals - 1].ebp_off;
            if (n->a) {
                gen_expr(n->a);
                e_store_local(off);
            }
            return;
        }
        case N_ASSIGN: {
            int off = local_find(n->name);
            if (off == 0) die_at(n->line, "undefined variable", n->name);
            gen_expr(n->a);
            e_store_local(off);
            return;
        }
        case N_RETURN: {
            if (n->a) gen_expr(n->a);
            else      e_mov_eax_imm(0);
            /* Function epilogue. */
            e_mov_esp_ebp();
            e_pop_ebp();
            e_ret();
            return;
        }
        case N_IF: {
            gen_expr(n->a);
            e_test_eax_eax();
            int jz = e_jz_rel32();
            gen_stmt(n->b);
            if (n->c) {
                int jend = e_jmp_rel32();
                patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
                gen_stmt(n->c);
                patch_d(jend, (unsigned)(g_code_len - (jend + 4)));
            } else {
                patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
            }
            return;
        }
        case N_WHILE: {
            int top = g_code_len;
            gen_expr(n->a);
            e_test_eax_eax();
            int jz = e_jz_rel32();
            gen_stmt(n->b);
            int jback = e_jmp_rel32();
            patch_d(jback, (unsigned)(top - (jback + 4)));
            patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
            return;
        }
        case N_EXPR_STMT:
            gen_expr(n->a);
            return;
    }
    die_at(n->line, "unsupported stmt kind", 0);
}

static void gen_func(struct node *fn) {
    int idx = func_intern(fn->name, fn->n_params);
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    /* Reset per-function state. */
    g_n_locals = 0;
    g_locals_bytes = 0;

    /* Prologue: push ebp; mov ebp, esp.
     * Params live at [ebp+8], [ebp+12], ... (after saved ebp + return addr). */
    e_push_ebp();
    e_mov_ebp_esp();

    /* Bind parameters as locals with positive offsets. Use a separate
     * loop to register them without bumping g_locals_bytes (which is
     * for the NEGATIVE-offset stack-allocated locals). */
    for (int i = 0; i < fn->n_params; i++) {
        if (g_n_locals >= MAX_LOCALS) die("too many locals");
        int j = 0;
        const char *nm = fn->params[i]->name;
        while (nm[j]) { g_locals[g_n_locals].name[j] = nm[j]; j++; }
        g_locals[g_n_locals].name[j] = 0;
        g_locals[g_n_locals].ebp_off = 8 + i * 4;
        g_n_locals++;
    }

    /* Reserve room for locals — we don't know how many yet; emit a
     * placeholder `sub esp, imm32` and patch it after codegen. */
    int sub_at = g_code_len;
    e_sub_esp_imm32(0);

    gen_stmt(fn->body);

    /* If the function fell through without an explicit return,
     * emit a default return-0 epilogue so we don't run into the next
     * function's bytes. */
    e_mov_eax_imm(0);
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();

    /* Patch the prologue's sub esp size. */
    patch_d(sub_at + 2, (unsigned)g_locals_bytes);
}

/* The print_int helper. Top-of-stack on entry is the int to print
 * (cdecl style). Algorithm:
 *   - Read the int.
 *   - Convert to decimal in a small stack buffer (built right-to-left).
 *   - sys_write fd=1.
 *   - Append '\n' and emit.
 *   - Return.
 * For simplicity we don't handle INT_MIN's lack of |x|; if the input
 * is negative we emit '-' and negate. Negation of INT_MIN gives
 * itself; documented limit.
 */
static void emit_print_int_helper(int idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    /* Function prologue. */
    e_push_ebp();
    e_mov_ebp_esp();
    /* Allocate 16 bytes of stack for the decimal buffer. */
    e_sub_esp_imm32(16);

    /* edx = ebp - 1 (write cursor, points at one-past-end of buffer). */
    /* lea edx, [ebp - 1]  =>  8D 55 FF */
    emit_b(0x8d); emit_b(0x55); emit_b(0xff);

    /* Tail byte = '\n'. mov byte [edx], 10 */
    emit_b(0xc6); emit_b(0x02); emit_b(0x0a);
    /* dec edx */
    emit_b(0x4a);

    /* eax = arg (at [ebp + 8]). */
    e_load_local(8);

    /* Negative? If so, set ebx=1 marker, eax = -eax. */
    /* test eax, eax */
    emit_b(0x85); emit_b(0xc0);
    /* jge skip_neg (rel8) */
    emit_b(0x7d); emit_b(0x04);
    /* neg eax  (f7 d8) */
    emit_b(0xf7); emit_b(0xd8);
    /* push 1 (negative marker — checked later by popping into ecx). */
    /* mov ecx, 1 */
    emit_b(0xb9); emit_d(1);
    /* jmp after_setpos (rel8 +2) */
    emit_b(0xeb); emit_b(0x02);
    /* skip_neg: */
    emit_b(0xb9); emit_d(0);     /* mov ecx, 0 — non-negative marker */
    /* after_setpos: */

    /* Conversion loop:
     *   loop_top: ebx = 10
     *             cdq
     *             idiv ebx       (eax = quot, edx = rem)
     *             add edx, '0'
     *             mov [esi], dl  -- actually we use [edx_buf]; reusing edx as cursor
     *
     * Hmm but we're using edx as the buffer cursor AND for idiv's
     * remainder. Need a different register. Use ESI as cursor.
     *
     * Refactor: redo from start with ESI as cursor. */
    /* (back-patch above: replace edx-as-cursor with esi-as-cursor) */
    /* In practice, easier to redo the whole helper. Let me restart it. */
    g_code_len = g_funcs[idx].entry_off;     /* roll back */

    /* Restart with esi-as-cursor design. */
    e_push_ebp();
    e_mov_ebp_esp();
    e_push_ebx();
    e_push_esi();
    e_push_edi();
    e_sub_esp_imm32(16);     /* buffer */

    /* esi = ebp - 16 - 12 + 15 ... actually let's just be careful. After
     *   push ebp                 -> esp = original - 4
     *   mov ebp, esp             -> ebp = current esp
     *   push ebx                 -> esp = ebp - 4
     *   push esi                 -> esp = ebp - 8
     *   push edi                 -> esp = ebp - 12
     *   sub esp, 16              -> esp = ebp - 28
     *
     * Buffer occupies [ebp-28, ebp-13]. We want esi pointing at
     * ebp - 13 (one past the highest buffer byte; we'll write
     * descending). */
    /* lea esi, [ebp - 13]  -> 8d 75 f3 */
    emit_b(0x8d); emit_b(0x75); emit_b(0xf3);

    /* Append '\n' first: mov byte [esi], 10; dec esi  */
    emit_b(0xc6); emit_b(0x06); emit_b(0x0a);
    emit_b(0x4e);     /* dec esi */

    /* edi = 1 if value is negative, else 0. eax = |arg|.
     * jge +7 skips BOTH the `mov edi, 1` (5 bytes: bf imm32) AND
     * the `neg eax` (2 bytes: f7 d8). Using +5 was a session-90
     * bug that fell through into `neg eax` on positive values —
     * the loop then ran on a negative number, the i386 idiv
     * produced a negative remainder, and `add dl, 0x30` wrapped
     * around to "0x30 - d" giving '/' for digit 1, '.' for 2,
     * etc. */
    e_load_local(8);
    emit_b(0x31); emit_b(0xff);  /* xor edi, edi */
    emit_b(0x85); emit_b(0xc0);  /* test eax, eax */
    emit_b(0x7d); emit_b(0x07);  /* jge +7 */
    emit_b(0xbf); emit_d(1);     /* mov edi, 1 */
    emit_b(0xf7); emit_b(0xd8);  /* neg eax */

    /* Special case: eax == 0. Write '0' and skip the loop. */
    emit_b(0x85); emit_b(0xc0);  /* test eax, eax */
    emit_b(0x75); emit_b(0x06);  /* jnz +6 (skip to loop) */
    emit_b(0xc6); emit_b(0x06); emit_b(0x30);   /* mov byte [esi], '0' */
    emit_b(0x4e);                                /* dec esi */
    emit_b(0xeb); emit_b(0x12);  /* jmp +18 to neg-sign check */

    /* loop_top: */
    /* mov ebx, 10 */
    emit_b(0xbb); emit_d(10);
    /* cdq */ emit_b(0x99);
    /* idiv ebx */ emit_b(0xf7); emit_b(0xfb);
    /* add dl, '0' */
    emit_b(0x80); emit_b(0xc2); emit_b(0x30);
    /* mov [esi], dl */
    emit_b(0x88); emit_b(0x16);
    /* dec esi */
    emit_b(0x4e);
    /* test eax, eax */
    emit_b(0x85); emit_b(0xc0);
    /* jnz loop_top (rel8 backward — -16 from end of jnz) */
    /* The jnz instruction is `75 imm8`. Distance from end of the
     * jnz back to loop_top: we just emitted (mov ebx imm32 = 5,
     * cdq=1, idiv=2, add dl,'0'=3, mov [esi],dl=2, dec esi=1,
     * test eax=2, jnz=2) = 18 bytes. So rel8 = -18 + 2 = -16.
     *
     * Actually rel is computed from END of the jnz to target. End of
     * jnz at +18 from loop_top start, so disp = loop_top - end =
     * -18. */
    emit_b(0x75); emit_b((unsigned char)(-18 & 0xff));

    /* If edi == 1, prepend '-'. Skip the prepend if edi == 0 (jz +4
     * skips mov(3) + dec(1) — but NOT the inc esi below, which must
     * run unconditionally to point esi at the first printable byte). */
    /* test edi, edi */
    emit_b(0x85); emit_b(0xff);
    /* jz +4  (over mov+dec, but not the inc esi) */
    emit_b(0x74); emit_b(0x04);
    /* mov byte [esi], '-' */
    emit_b(0xc6); emit_b(0x06); emit_b(0x2d);
    /* dec esi */
    emit_b(0x4e);

    /* esi points one-before the first printable byte. Increment to
     * point AT the first byte. Compute length = (ebp - 13) - esi.
     * Or: lea ecx, [esi + 1] for start; length = (ebp-13) - esi.
     *
     * Write call: sys_write(fd=1, addr=esi+1, n = (ebp-13) - esi). */
    /* inc esi */ emit_b(0x46);
    /* mov ecx, esi */
    emit_b(0x89); emit_b(0xf1);
    /* mov edx, ebp; sub edx, esi; sub edx, 12; (= (ebp - esi) - 12 — but
     * we want (ebp-12) - esi == (ebp-esi) - 12). Hmm:
     *  esi after the inc points at the first byte. The last byte we
     *  wrote (highest address) was at ebp - 13. So length =
     *  (ebp - 13) - esi + 1  =  (ebp - 12) - esi.
     *
     * Compute: edx = ebp; sub edx, esi; sub edx, 12 */
    emit_b(0x89); emit_b(0xea);                 /* mov edx, ebp */
    emit_b(0x29); emit_b(0xf2);                 /* sub edx, esi */
    emit_b(0x83); emit_b(0xea); emit_b(0x0c);   /* sub edx, 12  */

    /* eax = SYS_WRITE_FD = 12; ebx = 1 (stdout); ecx already addr; edx already n. */
    e_mov_eax_imm(12);
    e_mov_ebx_imm(1);
    e_int_0x80();

    /* Epilogue. */
    e_add_esp_imm32(16);
    e_pop_edi();
    e_pop_esi();
    e_pop_ebx();
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();
}

/* ---------- Top-level driver -------------------------------------- */

static void emit_start_stub(int main_idx) {
    /* call main      E8 disp32   (disp = main_entry_offset - (end_of_call))
     * mov ebx, eax   89 C3
     * mov eax, 3     B8 03 00 00 00
     * int 0x80       CD 80
     * hlt            F4
     *
     * Total: 5 + 2 + 5 + 2 + 1 = 15 bytes. We register a fixup so
     * the call's disp gets patched when main is emitted. */
    int disp_off = e_call_rel32();
    if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
    g_fixups[g_n_fixups].call_disp_off = disp_off;
    g_fixups[g_n_fixups].func_idx = main_idx;
    g_n_fixups++;
    e_mov_ebx_eax();
    e_mov_eax_imm(3);
    e_int_0x80();
    e_hlt();
}

static void resolve_fixups(void) {
    for (int i = 0; i < g_n_fixups; i++) {
        struct fixup *fx = &g_fixups[i];
        int target = g_funcs[fx->func_idx].entry_off;
        if (target < 0)
            die_at(0, "undefined function called", g_funcs[fx->func_idx].name);
        int disp = target - (fx->call_disp_off + 4);
        patch_d(fx->call_disp_off, (unsigned)disp);
    }
}

/* Output buffer for write_elf — held in .bss instead of on the stack so
 * the recursive parser has its full 64 KiB user stack to itself. */
static unsigned char g_elf_buf[CODE_MAX + EHDR_SIZE + PHDR_SIZE];

static int write_elf(const char *path, int code_size) {
    /* Minimal ELF32 EXEC, one PT_LOAD segment (RWX). Mirrors mkfs.py
     * make_elf so the resulting binary loads via the kernel's
     * elf_load path the same way as build-time userland. */
    unsigned char *buf = g_elf_buf;
    int o = 0;
    /* e_ident */
    buf[o++] = 0x7f; buf[o++] = 'E'; buf[o++] = 'L'; buf[o++] = 'F';
    buf[o++] = 1;    /* ELFCLASS32 */
    buf[o++] = 1;    /* ELFDATA2LSB */
    buf[o++] = 1;    /* EV_CURRENT */
    buf[o++] = 0;
    for (int i = 0; i < 8; i++) buf[o++] = 0;
    /* e_type=ET_EXEC=2, e_machine=EM_386=3 */
    buf[o++] = 2; buf[o++] = 0;
    buf[o++] = 3; buf[o++] = 0;
    /* e_version */
    buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
    /* e_entry */
    unsigned int entry = ENTRY_VA;
    buf[o++] = (unsigned char)(entry & 0xff);
    buf[o++] = (unsigned char)((entry >> 8) & 0xff);
    buf[o++] = (unsigned char)((entry >> 16) & 0xff);
    buf[o++] = (unsigned char)((entry >> 24) & 0xff);
    /* e_phoff */
    unsigned int phoff = EHDR_SIZE;
    buf[o++] = (unsigned char)(phoff & 0xff);
    buf[o++] = (unsigned char)((phoff >> 8) & 0xff);
    buf[o++] = (unsigned char)((phoff >> 16) & 0xff);
    buf[o++] = (unsigned char)((phoff >> 24) & 0xff);
    /* e_shoff (none) */
    for (int i = 0; i < 4; i++) buf[o++] = 0;
    /* e_flags */
    for (int i = 0; i < 4; i++) buf[o++] = 0;
    /* e_ehsize */
    buf[o++] = EHDR_SIZE; buf[o++] = 0;
    /* e_phentsize */
    buf[o++] = PHDR_SIZE; buf[o++] = 0;
    /* e_phnum */
    buf[o++] = 1; buf[o++] = 0;
    /* e_shentsize, e_shnum, e_shstrndx — all 0 */
    for (int i = 0; i < 6; i++) buf[o++] = 0;
    /* phdr: type=PT_LOAD=1, offset=84, vaddr=entry, paddr=entry,
     *       filesz=code_size, memsz=code_size, flags=PF_R|PF_W|PF_X=7,
     *       align=0x1000 */
    unsigned int phvals[8] = {
        1u, (unsigned int)CODE_OFF, entry, entry,
        (unsigned int)code_size, (unsigned int)code_size, 7u, 0x1000u
    };
    for (int k = 0; k < 8; k++) {
        unsigned int v = phvals[k];
        buf[o++] = (unsigned char)(v & 0xff);
        buf[o++] = (unsigned char)((v >> 8) & 0xff);
        buf[o++] = (unsigned char)((v >> 16) & 0xff);
        buf[o++] = (unsigned char)((v >> 24) & 0xff);
    }
    /* Code. */
    for (int i = 0; i < code_size; i++) buf[o++] = g_code[i];

    if (sys_fs_write(path, buf, (unsigned)o) < 0) {
        die_at(0, "cannot write output to", path);
        return -1;
    }
    return 0;
}

static char *slurp(const char *path, int *out_size) {
    int size = sys_fs_size(path);
    if (size < 0) return 0;
    /* Use libc malloc rather than raw sys_brk: libc's free-list and the
     * kernel brk are aliased at the same VA, so a bare sys_brk window
     * gets clobbered the first time the parser allocates a node. */
    char *buf = (char *)malloc((unsigned)(size + 1));
    if (!buf) return 0;
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

/* Build a default output path from input: strip optional .c suffix
 * and append .elf. If input doesn't end in .c, just append .elf. */
static void default_outpath(const char *in, char *out, int cap) {
    int n = my_strlen(in);
    int copy = n;
    if (n >= 2 && in[n - 2] == '.' && in[n - 1] == 'c') copy = n - 2;
    int o = 0;
    for (int i = 0; i < copy && o < cap - 5; i++) out[o++] = in[i];
    const char *suf = ".elf";
    while (*suf && o < cap - 1) out[o++] = *suf++;
    out[o] = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write(2, "usage: cc FILE.c [-o OUT.elf]\n", 30);
        return 1;
    }
    const char *in_path  = 0;
    char        out_path[80];
    out_path[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'o' && argv[i][2] == 0) {
            if (i + 1 >= argc) die("missing argument to -o");
            int j = 0;
            while (argv[i + 1][j] && j < (int)sizeof(out_path) - 1) {
                out_path[j] = argv[i + 1][j];
                j++;
            }
            out_path[j] = 0;
            i++;
        } else if (!in_path) {
            in_path = argv[i];
        } else {
            die("unexpected extra argument");
        }
    }
    if (!in_path) die("missing input file");
    if (!out_path[0]) default_outpath(in_path, out_path, sizeof(out_path));

    int sz = 0;
    char *src = slurp(in_path, &sz);
    if (!src) die_at(0, "cannot read", in_path);

    lex_all(src, sz);
    struct node *prog = parse_program();

    /* Emit start stub at offset 0 — calls main. We don't know
     * main's entry offset yet; emit a fixup so it gets patched
     * when main is generated. */
    int main_idx = func_intern("main", 0);
    /* Note: arg-count mismatch is checked at func_intern time. If
     * the user wrote `int main(int argc, char **argv)` with two
     * params we'd see a mismatch later when we encounter the
     * function — for our int-only subset, we just accept "int
     * main(int...)" with whatever n_params and pass 0 args from
     * the start stub. Adjust by re-interning at codegen time. */
    emit_start_stub(main_idx);

    /* Reserve the print_int helper slot — must exist before any
     * print_int call resolves. */
    int helper_idx = func_intern("__print_int_helper", 0);
    emit_print_int_helper(helper_idx);

    /* Generate code for every user function in source order. */
    for (int i = 0; i < prog->n_list; i++) {
        gen_func(prog->list[i]);
    }

    /* Verify main was defined. */
    if (!g_funcs[main_idx].defined) die("'main' function not found");

    resolve_fixups();

    if (write_elf(out_path, g_code_len) < 0) return 1;

    /* Tiny success line. */
    sys_write(1, "cc: wrote ", 10);
    sys_write(1, out_path, my_strlen(out_path));
    sys_write(1, "\n", 1);

    (void)g_exit_code;
    return 0;
}
