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
 *
 * Session 121 (Path B Phase 4 capstone) adds the remaining language
 * polish: struct-by-value RETURNS (hidden-first-arg cdecl ABI), the
 * `static` and `extern` storage-class keywords, and the
 * `typedef RET (*NAME)(ARGS);` function-pointer typedef syntax.
 * Together these close out the C-subset surface area; cc now handles
 * essentially every feature small-to-medium programs need without
 * having to fall back to pointer-out args, manual prototype tracking,
 * or `int *` for function-pointer aliases.
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
    T_END = 0, T_NUM, T_NAME, T_STR,
    T_INT, T_CHAR, T_IF, T_ELSE, T_WHILE, T_RETURN, T_FOR, T_DO,
    T_BREAK, T_CONTINUE, T_GOTO,    /* session 125 */
    T_SWITCH, T_CASE, T_DEFAULT,    /* session 125 */
    T_STRUCT,    /* session 97 */
    T_UNION,     /* session 125 */
    T_SIZEOF,    /* session 99 */
    T_ENUM,      /* session 103 */
    T_TYPEDEF,   /* session 104 */
    T_STATIC,    /* Session 121 — accepted-and-ignored storage-class spec */
    T_EXTERN,    /* Session 121 — function prototypes only */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
    T_LBRACKET, T_RBRACKET,
    T_SEMI, T_COMMA, T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE,
    T_AMP_AMP, T_PIPE_PIPE, T_BANG,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_LSHIFT, T_RSHIFT,
    /* Session 96 — compound operators. */
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
    /* Session 125 — bitwise / shift compound operators. */
    T_AMP_EQ, T_PIPE_EQ, T_CARET_EQ, T_LSHIFT_EQ, T_RSHIFT_EQ,
    T_INC, T_DEC,
    T_QUESTION, T_COLON,
    /* Session 97 — struct member access. */
    T_DOT, T_ARROW,
    /* Session 105 — variadic functions. */
    T_ELLIPSIS,
};

#define NAME_MAX 24

/* Forward — needed by parse_extern_proto in the parser to register a
 * function prototype with the symbol table (which lives in the codegen
 * section below). */
#define MAX_PARAMS_PER_FUNC 8
static void register_func_proto(const char *name, int n_params, int is_variadic,
                                int ret_kind, int ret_meta,
                                int *param_kinds, int *param_metas);

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

/* Session 91 — string pool.
 *
 * String literals from the source are interned into g_str_pool with
 * NUL terminators in order. Each entry's start offset is recorded in
 * g_str_offs. A T_STR token's `num` field is the string index (into
 * g_str_offs), not a raw offset, because the pool's final base VA is
 * not known until codegen finishes. Codegen for N_STR emits a
 * `mov eax, imm32` placeholder and records a g_str_fixup so the imm
 * can be patched once all code has been emitted and we know where
 * the pool will live in the binary. */
#define STR_POOL_MAX 4096
#define MAX_STRS     256
#define MAX_STR_FIXUPS 256
static char g_str_pool[STR_POOL_MAX];
static int  g_str_pool_len;
static int  g_str_offs[MAX_STRS];
static int  g_n_strs;
struct str_fixup {
    int code_off;   /* file offset of the 4-byte imm32 to patch */
    int str_idx;    /* g_str_offs index */
};
static struct str_fixup g_str_fixups[MAX_STR_FIXUPS];
static int              g_n_str_fixups;
static int              g_str_pool_base_va;   /* set during finalization */

/* Session 93 — global variables.
 *
 * Each global declaration at file scope allocates a slot here. The
 * data pool is a parallel byte buffer that mirrors the binary layout
 * — initialized globals get their bytes pre-written; uninitialized
 * globals are left as zero. Globals always occupy 4-byte-aligned
 * chunks so absolute addresses stay aligned for dword loads/stores.
 *
 * Fixups for global accesses behave like the string-fixup table:
 * codegen emits a placeholder imm32, and a later patch pass writes
 * (global_pool_base_va + g_globals[i].offset) at the right code
 * location. */
#define MAX_GLOBALS    128
#define DATA_POOL_MAX  8192
#define MAX_GLOB_FIXUPS 256
struct global_info {
    char name[NAME_MAX];
    int  offset;     /* byte offset within g_data_pool */
    int  size;       /* bytes reserved (padded to 4) */
    int  kind;       /* LK_* */
    int  meta;       /* struct_idx for LK_STRUCT/LK_STRUCT_PTR (s97) */
};
static struct global_info g_globals[MAX_GLOBALS];
static int                g_n_globals;
static unsigned char      g_data_pool[DATA_POOL_MAX];
static int                g_data_pool_len;
struct glob_fixup {
    int code_off;
    int glob_idx;
};
static struct glob_fixup  g_glob_fixups[MAX_GLOB_FIXUPS];
static int                g_n_glob_fixups;
static int                g_data_pool_base_va;

/* Intern a (potentially un-terminated) source-side string. Returns
 * the index into g_str_offs. Re-uses an existing entry if the
 * payload is byte-identical (cheap to do because of NUL terminators).
 * The pool keeps each string NUL-terminated so codegen can hand the
 * address to sys_write + strlen-style helpers directly. */
static int str_intern(const char *src, int len) {
    /* dedupe pass */
    for (int i = 0; i < g_n_strs; i++) {
        const char *p = &g_str_pool[g_str_offs[i]];
        int j = 0;
        while (j < len && p[j] && p[j] == src[j]) j++;
        if (j == len && p[j] == 0) return i;
    }
    if (g_n_strs >= MAX_STRS) die("too many string literals");
    if (g_str_pool_len + len + 1 > STR_POOL_MAX) die("string pool overflow");
    int off = g_str_pool_len;
    for (int j = 0; j < len; j++) g_str_pool[g_str_pool_len++] = src[j];
    g_str_pool[g_str_pool_len++] = 0;
    g_str_offs[g_n_strs] = off;
    return g_n_strs++;
}

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
    if (my_streq(s, "char"))   return T_CHAR;     /* session 92 */
    if (my_streq(s, "struct")) return T_STRUCT;   /* session 97 */
    if (my_streq(s, "union"))  return T_UNION;    /* session 125 */
    if (my_streq(s, "sizeof")) return T_SIZEOF;   /* session 99 */
    if (my_streq(s, "enum"))   return T_ENUM;     /* session 103 */
    if (my_streq(s, "typedef"))return T_TYPEDEF;  /* session 104 */
    if (my_streq(s, "static")) return T_STATIC;   /* Session 121 */
    if (my_streq(s, "extern")) return T_EXTERN;   /* Session 121 */
    if (my_streq(s, "if"))     return T_IF;
    if (my_streq(s, "else"))   return T_ELSE;
    if (my_streq(s, "while"))  return T_WHILE;
    if (my_streq(s, "do"))     return T_DO;       /* session 125 */
    if (my_streq(s, "break"))  return T_BREAK;    /* session 125 */
    if (my_streq(s, "continue"))return T_CONTINUE; /* session 125 */
    if (my_streq(s, "goto"))   return T_GOTO;     /* session 125 */
    if (my_streq(s, "switch")) return T_SWITCH;   /* session 125 */
    if (my_streq(s, "case"))   return T_CASE;     /* session 125 */
    if (my_streq(s, "default"))return T_DEFAULT;  /* session 125 */
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

/* =================================================================
 * Session 95 — Preprocessor.
 *
 * Runs BEFORE the lexer. Reads the source line-by-line; for lines that
 * start (after optional whitespace) with '#', it parses a directive.
 * Other lines are scanned for identifiers, and any identifier that
 * matches an active #define is replaced with its body.
 *
 * Supported directives:
 *   #define NAME body       single-token / single-line body
 *   #undef  NAME
 *   #include "FILE"         relative paths resolve to /FILE
 *   #ifdef  NAME            push true if NAME is defined
 *   #ifndef NAME            push true if NAME is NOT defined
 *   #else                   flip the top of the if-stack
 *   #endif                  pop the if-stack
 *
 * Not supported: function-like macros, line-continuation backslash,
 * #if with expressions, #pragma, #error, #line. Macro bodies are
 * single-pass text substitution (no rescanning, no recursion).
 * ============================================================== */

#define MAX_MACROS    128
#define MACRO_BODY_MAX 128
#define MAX_IF_DEPTH   16
#define MAX_INC_DEPTH  8
#define PP_BUF_MAX     16384

struct macro {
    char name[NAME_MAX];
    char body[MACRO_BODY_MAX];
    int  body_len;
};
static struct macro g_macros[MAX_MACROS];
static int          g_n_macros;

static int  g_if_stack[MAX_IF_DEPTH];    /* 1 = active branch */
static int  g_if_depth;

static char g_pp_buf[PP_BUF_MAX];
static int  g_pp_len;

static int  pp_output_active(void) {
    for (int i = 0; i < g_if_depth; i++) if (!g_if_stack[i]) return 0;
    return 1;
}

static int  pp_find_macro(const char *name) {
    for (int i = 0; i < g_n_macros; i++) {
        if (my_streq(g_macros[i].name, name)) return i;
    }
    return -1;
}

static void pp_define(const char *name, const char *body, int body_len) {
    int idx = pp_find_macro(name);
    if (idx < 0) {
        if (g_n_macros >= MAX_MACROS) die("too many #defines");
        idx = g_n_macros++;
        int j = 0;
        while (name[j] && j < NAME_MAX - 1) {
            g_macros[idx].name[j] = name[j]; j++;
        }
        g_macros[idx].name[j] = 0;
    }
    if (body_len > MACRO_BODY_MAX) body_len = MACRO_BODY_MAX;
    for (int j = 0; j < body_len; j++) g_macros[idx].body[j] = body[j];
    g_macros[idx].body_len = body_len;
}

static void pp_undef(const char *name) {
    int idx = pp_find_macro(name);
    if (idx < 0) return;
    /* Swap-remove. */
    g_macros[idx] = g_macros[--g_n_macros];
}

static void pp_emit_byte(char c) {
    if (g_pp_len >= PP_BUF_MAX) die("preprocessor output overflow");
    g_pp_buf[g_pp_len++] = c;
}

static void pp_emit_bytes(const char *s, int n) {
    for (int i = 0; i < n; i++) pp_emit_byte(s[i]);
}

/* Read an identifier starting at src[*pos]. Writes into out (NUL-
 * terminated) and updates *pos. Caller verifies first char is id_start. */
static int pp_read_ident(const char *src, int len, int *pos,
                         char *out, int cap) {
    int n = 0;
    while (*pos < len && is_id_cont(src[*pos]) && n < cap - 1) {
        out[n++] = src[(*pos)++];
    }
    out[n] = 0;
    return n;
}

/* Skip ASCII whitespace (but not newline) starting at *pos. */
static void pp_skip_hspace(const char *src, int len, int *pos) {
    while (*pos < len && (src[*pos] == ' ' || src[*pos] == '\t' || src[*pos] == '\r'))
        (*pos)++;
}

static int pp_at_eol(const char *src, int len, int pos) {
    return pos >= len || src[pos] == '\n';
}

/* Forward decl — directives may #include, which recurses. */
static void pp_process_buf(const char *src, int len, int depth);

/* Slurp a file from disk and recursively preprocess it. */
static void pp_include_file(const char *path, int depth) {
    if (depth >= MAX_INC_DEPTH) die("#include nesting too deep");
    int sz = sys_fs_size(path);
    if (sz < 0) die_at(0, "#include cannot open", path);
    char *buf = (char *)malloc((unsigned)(sz + 1));
    if (!buf) die("out of memory for #include");
    int fd = sys_open(path);
    if (fd < 0) die_at(0, "#include open failed for", path);
    int got = 0;
    while (got < sz) {
        int n = sys_read(fd, buf + got, sz - got);
        if (n <= 0) break;
        got += n;
    }
    sys_close(fd);
    buf[got] = 0;
    pp_process_buf(buf, got, depth + 1);
    /* `buf` leaks for the lifetime of compilation — that's fine; we
     * don't reuse the heap. */
}

/* Core preprocessor. Walks src line-by-line; expanding macros on
 * regular lines and dispatching directives on '#'-prefixed lines. */
static void pp_process_buf(const char *src, int len, int depth) {
    int pos = 0;
    while (pos < len) {
        /* Find end of current line. */
        int line_start = pos;
        int line_end   = pos;
        while (line_end < len && src[line_end] != '\n') line_end++;
        int has_newline = (line_end < len);

        /* Quick check: directive? Look at first non-whitespace char. */
        int scan = line_start;
        while (scan < line_end && (src[scan] == ' ' || src[scan] == '\t'))
            scan++;
        if (scan < line_end && src[scan] == '#') {
            /* Directive. Parse keyword. */
            scan++;
            pp_skip_hspace(src, line_end, &scan);
            char kw[16];
            int kw_n = 0;
            while (scan < line_end && is_id_cont(src[scan]) && kw_n < (int)sizeof(kw) - 1) {
                kw[kw_n++] = src[scan++];
            }
            kw[kw_n] = 0;
            pp_skip_hspace(src, line_end, &scan);

            if (my_streq(kw, "define") && pp_output_active()) {
                char name[NAME_MAX];
                if (scan >= line_end || !is_id_start(src[scan]))
                    die_at(0, "#define needs a name", 0);
                pp_read_ident(src, line_end, &scan, name, NAME_MAX);
                pp_skip_hspace(src, line_end, &scan);
                int body_start = scan;
                int body_end   = line_end;
                /* Trim trailing whitespace from body. */
                while (body_end > body_start &&
                       (src[body_end - 1] == ' ' || src[body_end - 1] == '\t' ||
                        src[body_end - 1] == '\r'))
                    body_end--;
                pp_define(name, src + body_start, body_end - body_start);
            } else if (my_streq(kw, "undef") && pp_output_active()) {
                char name[NAME_MAX];
                if (scan >= line_end || !is_id_start(src[scan]))
                    die_at(0, "#undef needs a name", 0);
                pp_read_ident(src, line_end, &scan, name, NAME_MAX);
                pp_undef(name);
            } else if (my_streq(kw, "include") && pp_output_active()) {
                if (scan >= line_end || src[scan] != '"')
                    die_at(0, "#include needs \"FILE\"", 0);
                scan++;
                char path[80];
                int p = 0;
                /* Auto-prefix '/' if the user gave a relative path,
                 * so `#include "x.h"` opens `/x.h`. */
                if (scan < line_end && src[scan] != '/' &&
                    p < (int)sizeof(path) - 1) path[p++] = '/';
                while (scan < line_end && src[scan] != '"' &&
                       p < (int)sizeof(path) - 1) {
                    path[p++] = src[scan++];
                }
                path[p] = 0;
                if (scan >= line_end || src[scan] != '"')
                    die_at(0, "#include missing closing quote", 0);
                pp_include_file(path, depth);
            } else if (my_streq(kw, "ifdef")) {
                char name[NAME_MAX];
                if (scan >= line_end || !is_id_start(src[scan]))
                    die_at(0, "#ifdef needs a name", 0);
                pp_read_ident(src, line_end, &scan, name, NAME_MAX);
                if (g_if_depth >= MAX_IF_DEPTH) die("#ifdef nesting too deep");
                g_if_stack[g_if_depth++] = (pp_find_macro(name) >= 0);
            } else if (my_streq(kw, "ifndef")) {
                char name[NAME_MAX];
                if (scan >= line_end || !is_id_start(src[scan]))
                    die_at(0, "#ifndef needs a name", 0);
                pp_read_ident(src, line_end, &scan, name, NAME_MAX);
                if (g_if_depth >= MAX_IF_DEPTH) die("#ifndef nesting too deep");
                g_if_stack[g_if_depth++] = (pp_find_macro(name) < 0);
            } else if (my_streq(kw, "else")) {
                if (g_if_depth == 0) die("#else without #if");
                g_if_stack[g_if_depth - 1] = !g_if_stack[g_if_depth - 1];
            } else if (my_streq(kw, "endif")) {
                if (g_if_depth == 0) die("#endif without #if");
                g_if_depth--;
            } else {
                /* Unknown directive or directive inside a skip block —
                 * silently drop. We still consumed the line. */
            }
            /* Preserve the newline so line numbers in the lexer track
             * the source. (Even when skipping.) */
            if (has_newline) pp_emit_byte('\n');
        } else if (pp_output_active()) {
            /* Regular line — scan for identifiers, expand macros, emit. */
            int p = line_start;
            while (p < line_end) {
                char c = src[p];
                /* Skip string literals: copy them through verbatim. */
                if (c == '"' || c == '\'') {
                    char q = c;
                    pp_emit_byte(src[p++]);
                    while (p < line_end && src[p] != q) {
                        if (src[p] == '\\' && p + 1 < line_end) {
                            pp_emit_byte(src[p++]);
                        }
                        pp_emit_byte(src[p++]);
                    }
                    if (p < line_end) pp_emit_byte(src[p++]);
                    continue;
                }
                if (is_id_start(c)) {
                    char id[NAME_MAX];
                    int id_n = 0;
                    while (p < line_end && is_id_cont(src[p]) && id_n < NAME_MAX - 1) {
                        id[id_n++] = src[p++];
                    }
                    id[id_n] = 0;
                    int mi = pp_find_macro(id);
                    if (mi >= 0) {
                        pp_emit_bytes(g_macros[mi].body, g_macros[mi].body_len);
                    } else {
                        pp_emit_bytes(id, id_n);
                    }
                    continue;
                }
                pp_emit_byte(src[p++]);
            }
            if (has_newline) pp_emit_byte('\n');
        } else {
            /* Inside a skip block — drop the line, preserve newline
             * so the lexer's line counter stays aligned. */
            if (has_newline) pp_emit_byte('\n');
        }
        pos = line_end + (has_newline ? 1 : 0);
    }
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
        /* Character literals — 'X' or '\n', '\t', etc.
         * Lexed to a T_NUM token holding the byte value. Session 92. */
        if (c == '\'') {
            g_pos++;
            if (g_pos >= g_src_len) die_at(g_line, "unterminated char literal", 0);
            int ch;
            if (g_src[g_pos] == '\\') {
                g_pos++;
                if (g_pos >= g_src_len) die_at(g_line, "unterminated char literal", 0);
                char esc = g_src[g_pos++];
                switch (esc) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case 'r':  ch = '\r'; break;
                    case '0':  ch = '\0'; break;
                    case '\\': ch = '\\'; break;
                    case '"':  ch = '"';  break;
                    case '\'': ch = '\''; break;
                    default:
                        die_at(g_line, "unknown escape in char literal", 0);
                        ch = 0;
                }
            } else {
                ch = (unsigned char)g_src[g_pos++];
            }
            if (g_pos >= g_src_len || g_src[g_pos] != '\'')
                die_at(g_line, "expected closing quote in char literal", 0);
            g_pos++;
            push_tok(T_NUM, g_line);
            g_toks[g_n_toks - 1].num = ch & 0xff;
            continue;
        }
        /* String literals — "..." with the usual escapes. The decoded
         * bytes get NUL-terminated and interned in g_str_pool; the
         * resulting index is stored in the token's `num` field.
         * Session 91. */
        if (c == '"') {
            g_pos++;     /* skip opening quote */
            char tmp[256];
            int  tn = 0;
            while (g_pos < g_src_len && g_src[g_pos] != '"') {
                if (tn >= (int)sizeof(tmp) - 1)
                    die_at(g_line, "string literal too long (max 255)", 0);
                char ch = g_src[g_pos++];
                if (ch == '\\' && g_pos < g_src_len) {
                    char esc = g_src[g_pos++];
                    switch (esc) {
                        case 'n':  ch = '\n'; break;
                        case 't':  ch = '\t'; break;
                        case 'r':  ch = '\r'; break;
                        case '0':  ch = '\0'; break;
                        case '\\': ch = '\\'; break;
                        case '"':  ch = '"';  break;
                        case '\'': ch = '\''; break;
                        default:
                            die_at(g_line, "unknown escape in string", 0);
                    }
                }
                if (ch == '\n') g_line++;
                tmp[tn++] = ch;
            }
            if (g_pos >= g_src_len) die_at(g_line, "unterminated string", 0);
            g_pos++;     /* skip closing quote */
            int sidx = str_intern(tmp, tn);
            push_tok(T_STR, g_line);
            g_toks[g_n_toks - 1].num = sidx;
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
            case '[': push_tok(T_LBRACKET, g_line); g_pos++; break;
            case ']': push_tok(T_RBRACKET, g_line); g_pos++; break;
            case ';': push_tok(T_SEMI,   g_line); g_pos++; break;
            case ',': push_tok(T_COMMA,  g_line); g_pos++; break;
            case '+':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '+') {
                    push_tok(T_INC, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_PLUS_EQ, g_line); g_pos += 2;
                } else { push_tok(T_PLUS, g_line); g_pos++; }
                break;
            case '-':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '-') {
                    push_tok(T_DEC, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_MINUS_EQ, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '>') {
                    push_tok(T_ARROW, g_line); g_pos += 2;
                } else { push_tok(T_MINUS, g_line); g_pos++; }
                break;
            case '.':
                /* Session 105 — recognize `...` as T_ELLIPSIS for
                 * variadic function declarations. Otherwise plain `.`
                 * for struct member access (session 97). */
                if (g_pos + 2 < g_src_len && g_src[g_pos + 1] == '.' && g_src[g_pos + 2] == '.') {
                    push_tok(T_ELLIPSIS, g_line); g_pos += 3;
                } else {
                    push_tok(T_DOT, g_line); g_pos++;
                }
                break;
            case '*':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_STAR_EQ, g_line); g_pos += 2;
                } else { push_tok(T_STAR, g_line); g_pos++; }
                break;
            case '/':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_SLASH_EQ, g_line); g_pos += 2;
                } else { push_tok(T_SLASH, g_line); g_pos++; }
                break;
            case '%':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_PERCENT_EQ, g_line); g_pos += 2;
                } else { push_tok(T_PERCENT, g_line); g_pos++; }
                break;
            case '?': push_tok(T_QUESTION, g_line); g_pos++; break;
            case ':': push_tok(T_COLON,    g_line); g_pos++; break;
            case '~': push_tok(T_TILDE,  g_line); g_pos++; break;
            case '^':
                /* Session 125 — `^=` compound assign. */
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_CARET_EQ, g_line); g_pos += 2;
                } else { push_tok(T_CARET, g_line); g_pos++; }
                break;
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
                /* Session 125 — `<<=` compound shift-assign. */
                if (g_pos + 2 < g_src_len && g_src[g_pos + 1] == '<' && g_src[g_pos + 2] == '=') {
                    push_tok(T_LSHIFT_EQ, g_line); g_pos += 3;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_LE, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '<') {
                    push_tok(T_LSHIFT, g_line); g_pos += 2;
                } else { push_tok(T_LT, g_line); g_pos++; }
                break;
            case '>':
                if (g_pos + 2 < g_src_len && g_src[g_pos + 1] == '>' && g_src[g_pos + 2] == '=') {
                    push_tok(T_RSHIFT_EQ, g_line); g_pos += 3;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_GE, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '>') {
                    push_tok(T_RSHIFT, g_line); g_pos += 2;
                } else { push_tok(T_GT, g_line); g_pos++; }
                break;
            case '&':
                /* Session 125 — `&=` compound bitwise-and-assign. */
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '&') {
                    push_tok(T_AMP_AMP, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_AMP_EQ, g_line); g_pos += 2;
                } else { push_tok(T_AMP, g_line); g_pos++; }
                break;
            case '|':
                if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '|') {
                    push_tok(T_PIPE_PIPE, g_line); g_pos += 2;
                } else if (g_pos + 1 < g_src_len && g_src[g_pos + 1] == '=') {
                    push_tok(T_PIPE_EQ, g_line); g_pos += 2;
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

/* Session 92 — type-kinds for locals + function-param entries.
 *
 * The compiler stays untyped at the expression level: gen_expr drops
 * its result in eax and the caller has to know what shape that is.
 * But DECLARATIONS need a type tag so we know:
 *   - how much stack to reserve (4 bytes for int/ptr, 1*N for char[N])
 *   - what to emit for `a[i]` / `*p` (byte vs word loads/stores)
 *   - whether to lea-an-address or load-the-value when an array name
 *     appears in an rvalue context
 *
 * LK_INT_PTR is treated identically to LK_INT internally — we only
 * really distinguish "1-byte-element" from "4-byte-element" pointers
 * because that's what affects load/store size and `a[i]` scaling.
 * Same for params. */
enum {
    LK_INT        = 0,   /* int x */
    LK_INT_PTR    = 1,   /* int *p (4-byte loads/stores via *p, scale i by 4) */
    LK_CHAR_PTR   = 2,   /* char *p (1-byte loads/stores via *p, scale i by 1) */
    LK_INT_ARR    = 3,   /* int x[N]  — name yields &x[0] (an int*) */
    LK_CHAR_ARR   = 4,   /* char x[N] — name yields &x[0] (a  char*) */
    LK_STRUCT     = 5,   /* struct T x  — session 97. struct_idx in `meta`. */
    LK_STRUCT_PTR = 6,   /* struct T *p — session 97. struct_idx in `meta`. */
    LK_STRUCT_ARR = 7,   /* struct T x[N] — session 102. struct_idx in `meta`. */
};

/* Forward decls — these are referenced from the parser but defined in
 * the symbol-table section further down. */
static int  kind_is_array(int k);
static int  kind_is_pointerlike(int k);
static int  kind_elem_size(int k);
static int  global_declare(const char *name, int size, int kind);
static int  global_find(const char *name);
/* Session 97 — struct helpers. */
static int  global_declare_struct(const char *name, int size, int kind, int meta);
/* Session 125 — local lookup forward decls (used by parse_primary's
 * `sizeof NAME` codegen). */
static int  local_find(const char *name);
static int  local_kind(const char *name);
static int  local_meta(const char *name);

/* Session 97 — struct registry. Each struct type has a name, a list
 * of fields (with offsets and kinds), and a total size. Field kinds
 * may be LK_INT, LK_CHAR_PTR, LK_INT_PTR, or LK_STRUCT_PTR (for
 * linked-list-style structs). Char fields stored as int-sized slots
 * for simplicity — no field-level byte packing. Defined here (before
 * the parser uses it) but the table's local_slot.meta companion lives
 * down in the codegen-helpers section.
 *
 * The registry is *parse-time* state; the codegen doesn't allocate
 * runtime struct descriptors. Struct types are entirely compile-time;
 * runtime sees just N bytes of memory at the right offsets. */
#define MAX_STRUCTS    32
#define MAX_FIELDS     16
struct field_info {
    char name[NAME_MAX];
    int  offset;
    int  size;        /* always 4 for now (field-aligned) */
    int  kind;        /* LK_* */
    int  meta;        /* for LK_STRUCT_PTR fields: target struct_idx */
};
struct struct_info {
    char name[NAME_MAX];
    struct field_info fields[MAX_FIELDS];
    int  n_fields;
    int  size;        /* total bytes (== n_fields * 4 currently) */
    int  defined;     /* 0 = forward-declared only */
    int  is_union;    /* session 125 — 1 = union (all fields at offset 0) */
};
static struct struct_info g_structs[MAX_STRUCTS];
static int                g_n_structs;

static int struct_find(const char *name) {
    for (int i = 0; i < g_n_structs; i++) {
        if (my_streq(g_structs[i].name, name)) return i;
    }
    return -1;
}

static int struct_field_find(int sidx, const char *fname) {
    if (sidx < 0 || sidx >= g_n_structs) return -1;
    for (int i = 0; i < g_structs[sidx].n_fields; i++) {
        if (my_streq(g_structs[sidx].fields[i].name, fname)) return i;
    }
    return -1;
}

/* Session 103 — enum constants.
 *
 * `enum [TAG] { A, B = 5, C, ... };` defines compile-time integer
 * constants. The tag (if present) is ignored — cc doesn't distinguish
 * enum types from ints. The constants land in g_enum_consts and are
 * looked up by name in `parse_primary` (after local/global lookup),
 * substituting N_NUM nodes for matching identifiers. */
#define MAX_ENUM_CONSTS 128
struct enum_const {
    char name[NAME_MAX];
    int  value;
};
static struct enum_const g_enum_consts[MAX_ENUM_CONSTS];
static int               g_n_enum_consts;

static int enum_find(const char *name) {
    for (int i = 0; i < g_n_enum_consts; i++) {
        if (my_streq(g_enum_consts[i].name, name)) return i;
    }
    return -1;
}
static void enum_add(const char *name, int value) {
    if (g_n_enum_consts >= MAX_ENUM_CONSTS) die("too many enum constants");
    if (enum_find(name) >= 0)
        die_at(0, "duplicate enum constant", name);
    int idx = g_n_enum_consts++;
    int j = 0;
    while (name[j] && j < NAME_MAX - 1) {
        g_enum_consts[idx].name[j] = name[j]; j++;
    }
    g_enum_consts[idx].name[j] = 0;
    g_enum_consts[idx].value = value;
}

/* Session 104 — typedef registry.
 *
 * `typedef BASE NAME;` records a (name, kind, meta) entry. Anywhere
 * the parser looks for a type spec, it checks this table first if
 * the current token is a T_NAME. The typedef is replaced inline
 * with its underlying type info; no separate "typedef type" exists
 * at codegen time. */
#define MAX_TYPEDEFS 64
struct typedef_entry {
    char name[NAME_MAX];
    int  kind;     /* LK_INT / LK_INT_PTR / LK_CHAR_PTR / LK_STRUCT / LK_STRUCT_PTR */
    int  meta;     /* struct_idx for struct kinds */
};
static struct typedef_entry g_typedefs[MAX_TYPEDEFS];
static int                  g_n_typedefs;

static int typedef_find(const char *name) {
    for (int i = 0; i < g_n_typedefs; i++) {
        if (my_streq(g_typedefs[i].name, name)) return i;
    }
    return -1;
}
static void typedef_add(const char *name, int kind, int meta) {
    if (g_n_typedefs >= MAX_TYPEDEFS) die("too many typedefs");
    if (typedef_find(name) >= 0)
        die_at(0, "duplicate typedef", name);
    int idx = g_n_typedefs++;
    int j = 0;
    while (name[j] && j < NAME_MAX - 1) {
        g_typedefs[idx].name[j] = name[j]; j++;
    }
    g_typedefs[idx].name[j] = 0;
    g_typedefs[idx].kind = kind;
    g_typedefs[idx].meta = meta;
}

/* Resolves the current token-stream position into a type spec.
 * Returns 1 if a type was consumed (kind/meta filled in); 0 otherwise
 * (no tokens consumed). Handles int, char, int*, char*, struct TAG,
 * struct TAG*, and typedef-NAME (recursively). */
static int try_consume_type(int *out_kind, int *out_meta) {
    int tk = tk_cur()->kind;
    if (tk == T_INT) {
        g_tk++;
        *out_kind = accept(T_STAR) ? LK_INT_PTR : LK_INT;
        *out_meta = 0;
        return 1;
    }
    if (tk == T_CHAR) {
        g_tk++;
        *out_kind = accept(T_STAR) ? LK_CHAR_PTR : LK_INT;  /* scalar char = int */
        *out_meta = 0;
        return 1;
    }
    if (tk == T_STRUCT || tk == T_UNION) {
        /* Session 125 — unions share the LK_STRUCT/LK_STRUCT_PTR kinds.
         * The is_union flag on the struct_info handles the field-offset
         * difference; downstream codegen doesn't need to distinguish. */
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected struct/union tag", 0);
        int sidx = struct_find(tk_cur()->name);
        if (sidx < 0)
            die_at(tk_cur()->line, "undefined struct/union", tk_cur()->name);
        g_tk++;
        *out_kind = accept(T_STAR) ? LK_STRUCT_PTR : LK_STRUCT;
        *out_meta = sidx;
        return 1;
    }
    if (tk == T_NAME) {
        int ti = typedef_find(tk_cur()->name);
        if (ti >= 0) {
            g_tk++;
            *out_kind = g_typedefs[ti].kind;
            *out_meta = g_typedefs[ti].meta;
            return 1;
        }
    }
    return 0;
}

enum {
    N_NUM, N_NAME, N_STR,
    N_BIN, N_UN, N_CALL,
    N_VAR_DECL,   /* TYPE NAME [= expr]; */
    N_ARR_DECL,   /* TYPE NAME[N];      session 92 */
    N_STRUCT_DECL,/* struct T NAME;     session 97 */
    N_ASSIGN,     /* NAME = expr;       */
    N_DEREF,      /* *expr              session 92 — unary */
    N_ADDR_OF,    /* &NAME              session 92 — unary */
    N_INDEX,      /* base[idx]          session 92 — binary, op=elem_size */
    N_MEMBER,     /* NAME.field         session 97 — name=base, n->name2-ish stored elsewhere */
    N_ARROW,      /* NAME->field        session 97 */
    N_INDEX_MEMBER,    /* NAME[i].field       session 102 */
    N_DEREF_ASSIGN,    /* *p   = expr;  session 92 — store thru pointer */
    N_INDEX_ASSIGN,    /* a[i] = expr;  session 92 */
    N_MEMBER_ASSIGN,   /* NAME.field = expr;  session 97 */
    N_ARROW_ASSIGN,    /* NAME->field = expr; session 97 */
    N_INDEX_MEMBER_ASSIGN, /* NAME[i].field = expr;  session 102 */
    /* Session 96 — compound operators. */
    N_COMPOUND_ASSIGN, /* NAME op= expr;  op stored in n->op (T_PLUS,
                        * T_MINUS, T_STAR, T_SLASH, T_PERCENT) */
    N_INC_DEC,         /* ++x / --x / x++ / x-- — op = T_INC|T_DEC,
                        * num = 1 for prefix, 0 for postfix */
    N_TERNARY,         /* c ? a : b — a=cond, b=then, c=else */
    N_RETURN,
    N_IF,
    N_WHILE,
    N_BLOCK,
    N_EXPR_STMT,
    N_FUNC_DECL,
    N_PROGRAM,
    /* Session 125 — language-corners batch. */
    N_COMMA,         /* a, b — evaluate a, return b */
    N_DO_WHILE,      /* do stmt while (cond);  — body in a, cond in b */
    N_BREAK,         /* break;     — jump to end of enclosing loop/switch */
    N_CONTINUE,      /* continue;  — jump to top/cond of enclosing loop */
    N_SIZEOF_NAME,   /* sizeof NAME — folded at codegen when locals are bound */
    N_LABEL,         /* NAME: stmt — body in n->body */
    N_GOTO,          /* goto NAME; */
    N_SWITCH,        /* switch (n->a) n->b — body must be a block */
    N_CASE,          /* case N: — n->num is the case value */
    N_DEFAULT,       /* default: */
    N_INDEX2,        /* NAME[i][j]      — 2D read */
    N_INDEX2_ASSIGN, /* NAME[i][j] = expr — 2D write (val in n->c) */
};

struct node {
    int            kind;
    int            line;
    int            op;
    int            num;
    char           name[NAME_MAX];
    char           field_name[NAME_MAX];  /* session 97 — for N_MEMBER/N_ARROW */
    struct node   *a, *b, *c;
    struct node  **list;
    int            n_list;
    struct node  **params;
    int            n_params;
    struct node   *body;
    /* Session 121 — for N_FUNC_DECL only: return-type info. Default
     * (LK_INT / 0) covers every non-struct return. LK_STRUCT/LK_STRUCT_PTR
     * with ret_meta = struct_idx mark struct-by-value / struct-pointer
     * returns. SBV returns use the hidden-first-arg cdecl convention. */
    int            ret_kind;
    int            ret_meta;
};

static struct node *new_node(int kind) {
    struct node *n = (struct node *)malloc(sizeof(*n));
    n->kind = kind;
    n->line = tk_cur()->line;
    n->op   = 0;
    n->num  = 0;
    n->name[0] = 0;
    n->field_name[0] = 0;
    n->a = n->b = n->c = 0;
    n->list = 0; n->n_list = 0;
    n->params = 0; n->n_params = 0;
    n->body = 0;
    n->ret_kind = LK_INT;
    n->ret_meta = 0;
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
static struct node *parse_comma_expr(void);
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
    if (t->kind == T_SIZEOF) {
        /* Session 99 — sizeof(TYPE). Folded at parse time into an
         * N_NUM. Accepted shapes:
         *   sizeof(int)            -> 4
         *   sizeof(char)           -> 1
         *   sizeof(int *)          -> 4
         *   sizeof(char *)         -> 4
         *   sizeof(struct TAG)     -> g_structs[idx].size
         *   sizeof(struct TAG *)   -> 4
         *
         * Session 125 — also accept `sizeof NAME` and `sizeof(NAME)`
         * for scalar / pointer / struct-value variables. Arrays are
         * deliberately NOT supported (cc doesn't track array length
         * past the parse-time decl; recovering it would require an
         * extra field on local_slot/global_info). Use `N * sizeof(int)`
         * for array byte counts, or a #define for the length. */
        g_tk++;
        int has_paren = accept(T_LPAREN);
        int sz;
        if (tk_cur()->kind == T_STRUCT || tk_cur()->kind == T_UNION) {
            g_tk++;
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "sizeof: expected struct/union tag", 0);
            int idx = struct_find(tk_cur()->name);
            if (idx < 0)
                die_at(tk_cur()->line, "sizeof: unknown struct/union", tk_cur()->name);
            g_tk++;
            if (accept(T_STAR)) sz = 4;
            else                sz = g_structs[idx].size;
        } else if (tk_cur()->kind == T_INT) {
            g_tk++;
            (void)accept(T_STAR);   /* int* same size as int */
            sz = 4;
        } else if (tk_cur()->kind == T_CHAR) {
            g_tk++;
            if (accept(T_STAR)) sz = 4;
            else                sz = 1;
        } else if (tk_cur()->kind == T_NAME) {
            /* Session 125 — sizeof NAME. typedef-NAMEs resolve at parse
             * time (the typedef registry is populated during parse);
             * variable NAMEs defer to codegen via N_SIZEOF_NAME because
             * the local/global symbol tables aren't populated yet. */
            int ti = typedef_find(tk_cur()->name);
            if (ti >= 0) {
                int kk = g_typedefs[ti].kind;
                int mm = g_typedefs[ti].meta;
                g_tk++;
                (void)accept(T_STAR);
                if (kk == LK_STRUCT && mm >= 0 && mm < g_n_structs) sz = g_structs[mm].size;
                else                                                sz = 4;
            } else {
                /* Defer to codegen. Build an N_SIZEOF_NAME node. */
                struct node *n = new_node(N_SIZEOF_NAME);
                int k = 0;
                while (tk_cur()->name[k]) { n->name[k] = tk_cur()->name[k]; k++; }
                n->name[k] = 0;
                g_tk++;
                if (has_paren) expect(T_RPAREN, "')'");
                return n;
            }
        } else {
            die_at(tk_cur()->line, "sizeof: expected type or NAME", 0);
            sz = 0;
        }
        if (has_paren) expect(T_RPAREN, "')'");
        struct node *n = new_node(N_NUM);
        n->num = sz;
        return n;
    }
    if (t->kind == T_STR) {
        /* Session 91 — string literal carries the pool index in `num`.
         * Codegen emits a placeholder `mov eax, imm32` and records a
         * fixup so the imm gets patched once the pool base VA is known. */
        struct node *n = new_node(N_STR);
        n->num = t->num;
        g_tk++;
        return n;
    }
    if (t->kind == T_NAME) {
        /* Session 103 — enum constants are looked up first. If the
         * name matches an enum constant AND is not followed by `(`,
         * substitute its integer value. The `(` check is so a function
         * accidentally named the same as an enumerator still resolves
         * as a call. (Currently impossible since enum/func use the
         * same namespace anyway, but this keeps things explicit.) */
        if (tk_peek(1)->kind != T_LPAREN) {
            int ei = enum_find(t->name);
            if (ei >= 0) {
                struct node *n = new_node(N_NUM);
                n->num = g_enum_consts[ei].value;
                g_tk++;
                return n;
            }
        }
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
        /* Session 92 — postfix `[idx]` (array/pointer indexing).
         * Restricted: the base must be a name. `a[i][j]` and
         * `f()[i]` aren't supported by this parse (multidimensional
         * indexing isn't supported by codegen either).
         *
         * Session 102 — `NAME[idx].field` for struct arrays: after
         * the closing `]`, look for `.field` and make an
         * N_INDEX_MEMBER instead. */
        if (tk_cur()->kind == T_LBRACKET) {
            g_tk++;
            struct node *idx = parse_expr();
            expect(T_RBRACKET, "']'");
            /* Session 125 — second `[idx]` makes it a 2D access. */
            if (tk_cur()->kind == T_LBRACKET) {
                g_tk++;
                struct node *idx2 = parse_expr();
                expect(T_RBRACKET, "']'");
                struct node *ix = new_node(N_INDEX2);
                int k = 0;
                while (n->name[k]) { ix->name[k] = n->name[k]; k++; }
                ix->name[k] = 0;
                ix->a = idx;
                ix->b = idx2;
                return ix;
            }
            if (tk_cur()->kind == T_DOT) {
                g_tk++;
                if (tk_cur()->kind != T_NAME)
                    die_at(tk_cur()->line, "expected field name after .", 0);
                struct node *im = new_node(N_INDEX_MEMBER);
                int k = 0;
                while (n->name[k]) { im->name[k] = n->name[k]; k++; }
                im->name[k] = 0;
                int f = 0;
                while (tk_cur()->name[f]) { im->field_name[f] = tk_cur()->name[f]; f++; }
                im->field_name[f] = 0;
                im->a = idx;
                g_tk++;
                return im;
            }
            struct node *ix = new_node(N_INDEX);
            int k = 0;
            while (n->name[k]) { ix->name[k] = n->name[k]; k++; }
            ix->name[k] = 0;
            ix->a = idx;
            return ix;
        }
        /* Session 96 — postfix `NAME++` / `NAME--`. */
        if (tk_cur()->kind == T_INC || tk_cur()->kind == T_DEC) {
            int op = tk_cur()->kind;
            g_tk++;
            struct node *id = new_node(N_INC_DEC);
            int k = 0;
            while (n->name[k]) { id->name[k] = n->name[k]; k++; }
            id->name[k] = 0;
            id->op  = op;
            id->num = 0;    /* 0 = postfix */
            return id;
        }
        /* Session 97 — postfix `NAME.field` and `NAME->field` as rvalues.
         * The store-side cases are handled separately in parse_stmt. */
        if (tk_cur()->kind == T_DOT || tk_cur()->kind == T_ARROW) {
            int is_arrow = (tk_cur()->kind == T_ARROW);
            g_tk++;
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected field name after . or ->", 0);
            struct node *m = new_node(is_arrow ? N_ARROW : N_MEMBER);
            int k = 0;
            while (n->name[k]) { m->name[k] = n->name[k]; k++; }
            m->name[k] = 0;
            int f = 0;
            while (tk_cur()->name[f]) { m->field_name[f] = tk_cur()->name[f]; f++; }
            m->field_name[f] = 0;
            g_tk++;
            return m;
        }
        return n;
    }
    if (t->kind == T_LPAREN) {
        /* Session 125 — accept the comma operator inside parens.
         * `(a, b)` evaluates a then returns b. */
        g_tk++;
        struct node *e = parse_comma_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
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
    /* Session 92 — unary & (address-of) and * (dereference).
     * `&NAME` is the only addr-of we accept; `*expr` accepts any
     * primary as the pointee expression (typically a NAME or another
     * deref). */
    if (t->kind == T_AMP) {
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "& must be followed by a local name", 0);
        struct node *n = new_node(N_ADDR_OF);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;
        return n;
    }
    if (t->kind == T_STAR) {
        g_tk++;
        struct node *n = new_node(N_DEREF);
        n->a = parse_primary();
        return n;
    }
    /* Session 96 — prefix ++NAME / --NAME. Only NAMES are supported
     * as the target; `++arr[i]` and `++*p` aren't in scope. */
    if (t->kind == T_INC || t->kind == T_DEC) {
        int op = t->kind;
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "++/-- must target a local name", 0);
        struct node *n = new_node(N_INC_DEC);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        n->op  = op;
        n->num = 1;     /* 1 = prefix */
        g_tk++;
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
    struct node *lhs = parse_binop_rhs(1, parse_primary());
    /* Session 96 — ternary `c ? a : b` lives at the lowest precedence
     * above assignment. */
    if (tk_cur()->kind == T_QUESTION) {
        g_tk++;
        struct node *t = parse_expr();
        expect(T_COLON, "':' in ternary");
        struct node *e = parse_expr();
        struct node *n = new_node(N_TERNARY);
        n->a = lhs;
        n->b = t;
        n->c = e;
        return n;
    }
    /* Session 125 — assignment as expression. `NAME = rhs` returns the
     * stored value, so `if ((x = f()))` and `a = b = c` both work.
     * Right-associative via the recursive parse_expr on the RHS. LHS
     * must be a plain NAME — `*p = ...`, `a[i] = ...`, and field
     * stores stay statement-only. */
    if (lhs && lhs->kind == N_NAME && tk_cur()->kind == T_ASSIGN) {
        g_tk++;
        struct node *rhs = parse_expr();
        struct node *n = new_node(N_ASSIGN);
        int i = 0;
        while (lhs->name[i]) { n->name[i] = lhs->name[i]; i++; }
        n->name[i] = 0;
        n->a = rhs;
        return n;
    }
    return lhs;
}

/* Session 125 — top-level comma-chained expression.
 *
 * The C comma operator has the lowest precedence: `a, b, c` evaluates
 * each in turn and returns the last. We can't fold comma into
 * parse_expr because parse_expr is *also* called inside argument
 * lists, where comma is the argument separator (not the operator).
 *
 * parse_comma_expr is used in statement-level contexts and inside
 * parenthesized sub-expressions — anywhere the inner comma is
 * unambiguously the operator. */
static struct node *parse_comma_expr(void) {
    struct node *e = parse_expr();
    while (tk_cur()->kind == T_COMMA) {
        g_tk++;
        struct node *rhs = parse_expr();
        struct node *n = new_node(N_COMMA);
        n->a = e;
        n->b = rhs;
        e = n;
    }
    return e;
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
    /* Session 104 — local declaration via a typedef-NAME at type
     * position. We expand to whatever the typedef resolves to and
     * emit the matching AST. Restriction: arrays (`Td var[N];`)
     * aren't supported via typedef-name yet. */
    if (t == T_NAME) {
        int ti = typedef_find(tk_cur()->name);
        if (ti >= 0) {
            int kind = g_typedefs[ti].kind;
            int meta = g_typedefs[ti].meta;
            g_tk++;     /* consume typedef name */
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected variable name after typedef-name", 0);
            char nm[NAME_MAX];
            int i = 0;
            while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
            nm[i] = 0;
            g_tk++;
            if (kind == LK_STRUCT || kind == LK_STRUCT_PTR) {
                expect(T_SEMI, "';'");
                struct node *n = new_node(N_STRUCT_DECL);
                for (int j = 0; j <= i; j++) n->name[j] = nm[j];
                n->op  = kind;
                n->num = meta;
                return n;
            }
            /* Scalar/pointer typedef (LK_INT, LK_INT_PTR, LK_CHAR_PTR). */
            struct node *n = new_node(N_VAR_DECL);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->op = kind;
            if (accept(T_ASSIGN)) n->a = parse_expr();
            expect(T_SEMI, "';'");
            return n;
        }
    }
    /* Session 97/102 — local struct declarations.
     *   struct TAG NAME;        a struct value
     *   struct TAG *NAME;       a pointer to a struct
     *   struct TAG NAME[N];     array of N struct values     (s102)
     *   No initializer support (would need brace-init parsing). */
    if (t == T_STRUCT || t == T_UNION) {
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected struct/union tag", 0);
        char tag[NAME_MAX];
        int ti = 0;
        while (tk_cur()->name[ti]) { tag[ti] = tk_cur()->name[ti]; ti++; }
        tag[ti] = 0;
        g_tk++;
        int sidx = struct_find(tag);
        if (sidx < 0)
            die_at(tk_cur()->line, "undefined struct/union", tag);
        int is_ptr = accept(T_STAR);
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected variable name", 0);
        char nm[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
        nm[i] = 0;
        g_tk++;
        /* Array-of-struct: `struct TAG NAME[N];` */
        if (!is_ptr && accept(T_LBRACKET)) {
            if (tk_cur()->kind != T_NUM)
                die_at(tk_cur()->line, "array size must be integer literal", 0);
            int sz = tk_cur()->num;
            if (sz <= 0) die_at(tk_cur()->line, "array size must be positive", 0);
            g_tk++;
            expect(T_RBRACKET, "']'");
            expect(T_SEMI, "';'");
            struct node *n = new_node(N_STRUCT_DECL);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->op     = LK_STRUCT_ARR;
            n->num    = sidx;
            n->n_list = sz;     /* repurposed: array length */
            return n;
        }
        struct node *n = new_node(N_STRUCT_DECL);
        for (int j = 0; j <= i; j++) n->name[j] = nm[j];
        n->op  = is_ptr ? LK_STRUCT_PTR : LK_STRUCT;
        n->num = sidx;
        expect(T_SEMI, "';'");
        return n;
    }
    if (t == T_INT || t == T_CHAR) {
        /* Local declaration. Accepted shapes (session 92):
         *   int  NAME [= expr];           kind = LK_INT
         *   char NAME [= expr];           kind = LK_INT (scalar char ≡ int)
         *   int *NAME [= expr];           kind = LK_INT_PTR
         *   char *NAME [= expr];          kind = LK_CHAR_PTR
         *   int  NAME[N];                 kind = LK_INT_ARR,  num=N
         *   char NAME[N];                 kind = LK_CHAR_ARR, num=N
         *
         * The base type drives the kind; `*` upgrades a scalar to a
         * pointer; `[N]` upgrades to an array (no initializer for
         * arrays yet — would need brace-init syntax). */
        int is_char = (t == T_CHAR);
        g_tk++;
        int is_ptr = accept(T_STAR);
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected name in declaration", 0);
        char nm[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
        nm[i] = 0;
        g_tk++;
        if (accept(T_LBRACKET)) {
            /* Array declaration. Session 125 — also accept a second
             * `[M]` for 2D arrays: `int a[N][M];`. The inner dim M is
             * stored in n->n_list. 1D arrays leave it at 0. */
            if (is_ptr) die_at(tk_cur()->line, "ptr-to-array not supported", 0);
            if (tk_cur()->kind != T_NUM)
                die_at(tk_cur()->line, "array size must be an integer literal", 0);
            int sz = tk_cur()->num;
            if (sz <= 0) die_at(tk_cur()->line, "array size must be positive", 0);
            g_tk++;
            expect(T_RBRACKET, "']'");
            int dim2 = 0;
            if (accept(T_LBRACKET)) {
                if (tk_cur()->kind != T_NUM)
                    die_at(tk_cur()->line, "inner array size must be integer literal", 0);
                dim2 = tk_cur()->num;
                if (dim2 <= 0) die_at(tk_cur()->line, "inner array size must be positive", 0);
                g_tk++;
                expect(T_RBRACKET, "']'");
            }
            expect(T_SEMI, "';'");
            struct node *n = new_node(N_ARR_DECL);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->num    = sz;
            n->n_list = dim2;       /* inner-dim count or 0 for 1D */
            n->op     = is_char ? LK_CHAR_ARR : LK_INT_ARR;
            return n;
        }
        struct node *n = new_node(N_VAR_DECL);
        for (int j = 0; j <= i; j++) n->name[j] = nm[j];
        n->op = is_ptr ? (is_char ? LK_CHAR_PTR : LK_INT_PTR) : LK_INT;
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
    /* Session 125 — break/continue statements. The codegen enforces
     * "must be inside a loop"; parser just builds the node. */
    if (t == T_BREAK) {
        g_tk++;
        expect(T_SEMI, "';'");
        return new_node(N_BREAK);
    }
    if (t == T_CONTINUE) {
        g_tk++;
        expect(T_SEMI, "';'");
        return new_node(N_CONTINUE);
    }
    /* Session 125 — goto NAME; */
    if (t == T_GOTO) {
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected label name after goto", 0);
        struct node *n = new_node(N_GOTO);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;
        expect(T_SEMI, "';'");
        return n;
    }
    /* Session 125 — labeled statement: `NAME: stmt`. The label is
     * resolved per-function in the goto-fixup table. */
    if (t == T_NAME && tk_peek(1)->kind == T_COLON) {
        char nm[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
        nm[i] = 0;
        g_tk++;   /* consume NAME */
        g_tk++;   /* consume : */
        struct node *n = new_node(N_LABEL);
        for (int j = 0; j <= i; j++) n->name[j] = nm[j];
        n->body = parse_stmt();
        return n;
    }
    /* Session 125 — switch (expr) stmt.  Body is typically a block of
     * `case N:`, `default:`, and regular statements. */
    if (t == T_SWITCH) {
        g_tk++;
        expect(T_LPAREN, "'('");
        struct node *e = parse_comma_expr();
        expect(T_RPAREN, "')'");
        struct node *body = parse_stmt();
        struct node *n = new_node(N_SWITCH);
        n->a = e;
        n->b = body;
        return n;
    }
    /* Session 125 — case N: — only integer-literal values supported
     * (cc has no parse-time constant folding for arithmetic in this
     * branch). For `case -N:` accept a leading minus. */
    if (t == T_CASE) {
        g_tk++;
        int neg = 0;
        if (accept(T_MINUS)) neg = 1;
        if (tk_cur()->kind != T_NUM)
            die_at(tk_cur()->line, "case value must be integer literal", 0);
        int v = tk_cur()->num;
        if (neg) v = -v;
        g_tk++;
        expect(T_COLON, "':' after case value");
        struct node *n = new_node(N_CASE);
        n->num = v;
        return n;
    }
    if (t == T_DEFAULT) {
        g_tk++;
        expect(T_COLON, "':' after default");
        return new_node(N_DEFAULT);
    }
    /* Session 125 — `do stmt while (cond);`. Body in n->a, cond in n->b. */
    if (t == T_DO) {
        g_tk++;
        struct node *n = new_node(N_DO_WHILE);
        n->a = parse_stmt();
        if (tk_cur()->kind != T_WHILE)
            die_at(tk_cur()->line, "expected 'while' after do-body", 0);
        g_tk++;
        expect(T_LPAREN, "'('");
        n->b = parse_expr();
        expect(T_RPAREN, "')'");
        expect(T_SEMI, "';'");
        return n;
    }
    if (t == T_LBRACE) return parse_block();
    /* Expression-stmt or assignment.
     * `NAME = expr;`  or  `expr;`
     * Session 92 also: `*NAME = expr;` and `NAME[idx] = expr;`
     * Session 96 also: `NAME op= expr;` */
    if (t == T_NAME) {
        int nx = tk_peek(1)->kind;
        int op = 0;
        if (nx == T_PLUS_EQ)         op = T_PLUS;
        else if (nx == T_MINUS_EQ)   op = T_MINUS;
        else if (nx == T_STAR_EQ)    op = T_STAR;
        else if (nx == T_SLASH_EQ)   op = T_SLASH;
        else if (nx == T_PERCENT_EQ) op = T_PERCENT;
        /* Session 125 — bitwise / shift compound assigns. Same
         * rewrite trick as session 96. */
        else if (nx == T_AMP_EQ)     op = T_AMP;
        else if (nx == T_PIPE_EQ)    op = T_PIPE;
        else if (nx == T_CARET_EQ)   op = T_CARET;
        else if (nx == T_LSHIFT_EQ)  op = T_LSHIFT;
        else if (nx == T_RSHIFT_EQ)  op = T_RSHIFT;
        if (op) {
            /* Session 96 — rewrite `x op= expr` as `x = x op expr` so
             * we don't need a separate codegen path. Safe because
             * NAME is a pure reference (no side-effects per re-read). */
            char nm[NAME_MAX];
            int i = 0;
            while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
            nm[i] = 0;
            g_tk++;       /* skip name */
            g_tk++;       /* skip op= */
            struct node *rhs = parse_expr();
            expect(T_SEMI, "';'");
            struct node *left = new_node(N_NAME);
            for (int j = 0; j <= i; j++) left->name[j] = nm[j];
            struct node *bin = new_node(N_BIN);
            bin->op = op;
            bin->a  = left;
            bin->b  = rhs;
            struct node *n = new_node(N_ASSIGN);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->a = bin;
            return n;
        }
    }
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
    /* Session 97 — `NAME.field = expr;` and `NAME->field = expr;`. */
    if (t == T_NAME && (tk_peek(1)->kind == T_DOT || tk_peek(1)->kind == T_ARROW)
        && tk_peek(2)->kind == T_NAME && tk_peek(3)->kind == T_ASSIGN) {
        int is_arrow = (tk_peek(1)->kind == T_ARROW);
        struct node *n = new_node(is_arrow ? N_ARROW_ASSIGN : N_MEMBER_ASSIGN);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;     /* skip name */
        g_tk++;     /* skip . or -> */
        int f = 0;
        while (tk_cur()->name[f]) { n->field_name[f] = tk_cur()->name[f]; f++; }
        n->field_name[f] = 0;
        g_tk++;     /* skip field */
        g_tk++;     /* skip = */
        n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    if (t == T_NAME && tk_peek(1)->kind == T_LBRACKET) {
        /* Could be either `a[i] = expr;` or an indexing expression-stmt
         * `a[i];`. Peek further: parse the index, then look for '='.
         *
         * Session 102 also accepts `NAME[i].field = expr;` for struct
         * arrays — after the closing `]`, if we see `.NAME`, we
         * branch into N_INDEX_MEMBER_ASSIGN instead. */
        int save_tk = g_tk;
        char nm[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
        nm[i] = 0;
        g_tk++;       /* skip name */
        g_tk++;       /* skip '[' */
        struct node *idx = parse_expr();
        expect(T_RBRACKET, "']'");
        /* Session 125 — `NAME[i][j] = expr;` 2D store. */
        if (tk_cur()->kind == T_LBRACKET) {
            g_tk++;
            struct node *idx2 = parse_expr();
            expect(T_RBRACKET, "']'");
            expect(T_ASSIGN, "'='");
            struct node *val = parse_expr();
            expect(T_SEMI, "';'");
            struct node *n = new_node(N_INDEX2_ASSIGN);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->a = idx;
            n->b = idx2;
            n->c = val;
            return n;
        }
        if (tk_cur()->kind == T_DOT) {
            g_tk++;
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected field name after .", 0);
            char fn[NAME_MAX];
            int f = 0;
            while (tk_cur()->name[f]) { fn[f] = tk_cur()->name[f]; f++; }
            fn[f] = 0;
            g_tk++;     /* skip field name */
            expect(T_ASSIGN, "'='");
            struct node *val = parse_expr();
            expect(T_SEMI, "';'");
            struct node *n = new_node(N_INDEX_MEMBER_ASSIGN);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            for (int j = 0; j <= f; j++) n->field_name[j] = fn[j];
            n->a = idx;
            n->b = val;
            return n;
        }
        if (tk_cur()->kind == T_ASSIGN) {
            g_tk++;
            struct node *val = parse_expr();
            expect(T_SEMI, "';'");
            struct node *n = new_node(N_INDEX_ASSIGN);
            for (int j = 0; j <= i; j++) n->name[j] = nm[j];
            n->a = idx;
            n->b = val;
            return n;
        }
        /* No '=' — must be an expression-stmt: a[i];   roll back and
         * fall through to the generic expression-stmt parse. */
        g_tk = save_tk;
    }
    if (t == T_STAR && tk_peek(1)->kind == T_NAME && tk_peek(2)->kind == T_ASSIGN) {
        /* `*NAME = expr;` */
        g_tk++;     /* skip '*' */
        struct node *n = new_node(N_DEREF_ASSIGN);
        int i = 0;
        while (tk_cur()->name[i]) { n->name[i] = tk_cur()->name[i]; i++; }
        n->name[i] = 0;
        g_tk++;     /* skip NAME */
        g_tk++;     /* skip '=' */
        n->a = parse_expr();
        expect(T_SEMI, "';'");
        return n;
    }
    /* Session 125 — comma-expressions are allowed as statement
     * expressions (e.g. `(a = 1, b = 2);` evaluates both). */
    struct node *e = parse_comma_expr();
    expect(T_SEMI, "';'");
    struct node *es = new_node(N_EXPR_STMT);
    es->a = e;
    return es;
}

/* Session 121 — parse a function return type into kind+meta.
 *
 *   int [*]              -> LK_INT  / LK_INT_PTR
 *   char [*]             -> LK_INT  / LK_CHAR_PTR  (scalar char ≡ int)
 *   struct T [*]         -> LK_STRUCT / LK_STRUCT_PTR  (s121: SBV return)
 *   typedef-NAME         -> whatever it resolves to
 *
 * For non-struct returns we don't actually need the kind at codegen (eax
 * carries the result regardless), but we DO need to know `LK_STRUCT` so
 * gen_func can install the hidden-first-arg ABI for SBV returns. */
static void parse_return_type(int *out_kind, int *out_meta) {
    *out_kind = LK_INT;
    *out_meta = 0;
    if (tk_cur()->kind == T_INT || tk_cur()->kind == T_CHAR) {
        int is_char = (tk_cur()->kind == T_CHAR);
        g_tk++;
        if (accept(T_STAR)) {
            *out_kind = is_char ? LK_CHAR_PTR : LK_INT_PTR;
        }
        return;
    }
    if (tk_cur()->kind == T_STRUCT || tk_cur()->kind == T_UNION) {
        g_tk++;
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected struct/union tag in return type", 0);
        int sidx = struct_find(tk_cur()->name);
        if (sidx < 0)
            die_at(tk_cur()->line, "undefined struct/union in return type", tk_cur()->name);
        g_tk++;
        if (accept(T_STAR)) {
            *out_kind = LK_STRUCT_PTR;
        } else {
            *out_kind = LK_STRUCT;
        }
        *out_meta = sidx;
        return;
    }
    if (tk_cur()->kind == T_NAME) {
        int ti = typedef_find(tk_cur()->name);
        if (ti >= 0) {
            g_tk++;
            *out_kind = g_typedefs[ti].kind;
            *out_meta = g_typedefs[ti].meta;
            return;
        }
    }
    die_at(tk_cur()->line,
           "expected 'int' / 'char' / 'struct TAG' / typedef-name (return type)", 0);
}

/* Session 121 — parse a parenthesized parameter list into fn->params /
 * fn->n_params, setting fn->op = 1 if variadic. Consumes the leading
 * '(' and the trailing ')'. Used by parse_func and parse_extern_proto. */
static void parse_param_list(struct node *fn) {
    expect(T_LPAREN, "'('");
    int cap = 0;
    while (tk_cur()->kind != T_RPAREN) {
        /* Session 105 — `...` at end of param list = variadic. */
        if (tk_cur()->kind == T_ELLIPSIS) {
            g_tk++;
            fn->op = 1;     /* mark as variadic (reuse op as the flag) */
            if (tk_cur()->kind != T_RPAREN)
                die_at(tk_cur()->line, "... must be last in param list", 0);
            break;
        }
        int kind, struct_idx = 0;
        /* Session 97/106 — `struct T *p` (pointer) or `struct T p`
         * (by-value) parameter. */
        if (tk_cur()->kind == T_STRUCT || tk_cur()->kind == T_UNION) {
            g_tk++;
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected struct/union tag in param", 0);
            char tag[NAME_MAX];
            int x = 0;
            while (tk_cur()->name[x]) { tag[x] = tk_cur()->name[x]; x++; }
            tag[x] = 0;
            g_tk++;
            struct_idx = struct_find(tag);
            if (struct_idx < 0)
                die_at(tk_cur()->line, "undefined struct/union in param", tag);
            int is_ptr = accept(T_STAR);
            kind = is_ptr ? LK_STRUCT_PTR : LK_STRUCT;
        } else if (tk_cur()->kind == T_INT || tk_cur()->kind == T_CHAR) {
            int is_char = (tk_cur()->kind == T_CHAR);
            g_tk++;
            int is_ptr = accept(T_STAR);
            kind = is_ptr ? (is_char ? LK_CHAR_PTR : LK_INT_PTR) : LK_INT;
        } else if (tk_cur()->kind == T_NAME) {
            /* Session 104 — typedef-NAME as param type. */
            int ti = typedef_find(tk_cur()->name);
            if (ti < 0)
                die_at(tk_cur()->line, "expected param type (int/char/struct/typedef)", 0);
            g_tk++;
            kind = g_typedefs[ti].kind;
            struct_idx = g_typedefs[ti].meta;
        } else {
            die_at(tk_cur()->line, "expected param type (int/char/struct/typedef)", 0);
            kind = LK_INT;
        }
        /* Session 121: param name is optional. Real C lets you omit it
         * in prototypes (`int squared(int);`). We accept the same form
         * but synthesize a sentinel name `__anonN` so the locals table
         * doesn't choke on duplicate empty names if a body refers to
         * `__anonN` (which it won't, since the user never spelled the
         * name in source). */
        struct node *p = new_node(N_NAME);
        if (tk_cur()->kind == T_NAME) {
            int j = 0;
            while (tk_cur()->name[j]) { p->name[j] = tk_cur()->name[j]; j++; }
            p->name[j] = 0;
            g_tk++;
        } else if (tk_cur()->kind == T_COMMA || tk_cur()->kind == T_RPAREN) {
            /* Anonymous param. Generate a unique sentinel name; no body
             * code can reference it. */
            static int anon_ctr;
            const char *prefix = "__anon";
            int j = 0;
            while (prefix[j]) { p->name[j] = prefix[j]; j++; }
            int v = anon_ctr++;
            if (v == 0) { p->name[j++] = '0'; }
            else {
                char dig[12]; int dn = 0;
                while (v) { dig[dn++] = (char)('0' + v % 10); v /= 10; }
                while (dn--) p->name[j++] = dig[dn];
            }
            p->name[j] = 0;
        } else {
            die_at(tk_cur()->line, "expected param name or end of list", 0);
        }
        p->op  = kind;          /* param kind for local-binding */
        p->num = struct_idx;    /* meta (struct_idx for LK_STRUCT_PTR) */
        node_push(&fn->params, &fn->n_params, &cap, p);
        if (!accept(T_COMMA)) break;
    }
    expect(T_RPAREN, "')'");
}

static struct node *parse_func(void) {
    /* Return type — int/char (with optional *), struct TAG (with optional
     * *), or a typedef name. Session 121 — `struct T NAME(...)` is a
     * struct-by-value-return function. */
    int ret_kind, ret_meta;
    parse_return_type(&ret_kind, &ret_meta);
    if (tk_cur()->kind != T_NAME) die_at(tk_cur()->line, "expected function name", 0);
    struct node *fn = new_node(N_FUNC_DECL);
    int i = 0;
    while (tk_cur()->name[i]) { fn->name[i] = tk_cur()->name[i]; i++; }
    fn->name[i] = 0;
    fn->ret_kind = ret_kind;
    fn->ret_meta = ret_meta;
    g_tk++;
    parse_param_list(fn);
    fn->body = parse_block();
    return fn;
}

/* Session 121 — parse a function prototype after `extern`:
 *   extern RET NAME ( PARAMS );
 * No body. We register the function in g_funcs immediately so any later
 * call site sees the correct param/return info. No AST node is emitted
 * (gen_func would try to write code we don't have a body for). */
static void parse_extern_proto(void) {
    int ret_kind, ret_meta;
    parse_return_type(&ret_kind, &ret_meta);
    if (tk_cur()->kind != T_NAME)
        die_at(tk_cur()->line, "extern: expected function name", 0);
    char nm[NAME_MAX];
    int i = 0;
    while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
    nm[i] = 0;
    g_tk++;
    struct node *proto = new_node(N_FUNC_DECL);
    int j = 0;
    while (nm[j]) { proto->name[j] = nm[j]; j++; }
    proto->name[j] = 0;
    parse_param_list(proto);
    expect(T_SEMI, "';'");
    /* Register in g_funcs now so call sites see correct argc/kinds.
     * register_func_proto is defined in the symbol-table section; the
     * forward declaration sits at the top of the file. */
    int np = proto->n_params;
    if (np > MAX_PARAMS_PER_FUNC) np = MAX_PARAMS_PER_FUNC;
    int kinds[MAX_PARAMS_PER_FUNC];
    int metas[MAX_PARAMS_PER_FUNC];
    for (int k = 0; k < np; k++) {
        kinds[k] = proto->params[k]->op;
        metas[k] = proto->params[k]->num;
    }
    register_func_proto(nm, proto->n_params, proto->op ? 1 : 0,
                        ret_kind, ret_meta, kinds, metas);
    /* No AST emission — the prototype is just metadata. */
}

/* Session 93 — parse a top-level global declaration. The current
 * token is T_INT or T_CHAR. We peek ahead to distinguish the function
 * form (`int name(...)`) from the global form (`int name;` or
 * `int name = ...;` or `int *name;` or `int name[N];`).
 *
 * Initializers: only NUMBER literals (positive or negative) are
 * accepted as global init values for scalars; arrays default to all
 * zeros (no brace-init yet). Strings as global init are NOT supported
 * yet — they'd need to allocate-then-fixup the pool address into
 * the data section, which requires running string-pool finalization
 * before global finalization. Future work. */
static void parse_global_decl(void) {
    /* Session 104 — typedef-NAME global. Resolve and emit. */
    if (tk_cur()->kind == T_NAME) {
        int ti = typedef_find(tk_cur()->name);
        if (ti >= 0) {
            int td_kind = g_typedefs[ti].kind;
            int td_meta = g_typedefs[ti].meta;
            g_tk++;
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected variable name in global", 0);
            char nm[NAME_MAX];
            int i = 0;
            while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
            nm[i] = 0;
            g_tk++;
            expect(T_SEMI, "';'");
            if (td_kind == LK_STRUCT)
                global_declare_struct(nm, g_structs[td_meta].size, LK_STRUCT, td_meta);
            else if (td_kind == LK_STRUCT_PTR)
                global_declare_struct(nm, 4, LK_STRUCT_PTR, td_meta);
            else
                global_declare(nm, 4, td_kind);
            return;
        }
    }
    int is_char = (tk_cur()->kind == T_CHAR);
    g_tk++;
    int is_ptr = accept(T_STAR);
    if (tk_cur()->kind != T_NAME)
        die_at(tk_cur()->line, "expected name in global declaration", 0);
    char nm[NAME_MAX];
    int i = 0;
    while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
    nm[i] = 0;
    g_tk++;
    if (accept(T_LBRACKET)) {
        if (is_ptr) die_at(tk_cur()->line, "ptr-to-array global not supported", 0);
        if (tk_cur()->kind != T_NUM)
            die_at(tk_cur()->line, "array size must be integer literal", 0);
        int sz = tk_cur()->num;
        if (sz <= 0) die_at(tk_cur()->line, "array size must be positive", 0);
        g_tk++;
        expect(T_RBRACKET, "']'");
        expect(T_SEMI, "';'");
        int elem = is_char ? 1 : 4;
        int kind = is_char ? LK_CHAR_ARR : LK_INT_ARR;
        global_declare(nm, elem * sz, kind);
        return;
    }
    int kind = is_ptr ? (is_char ? LK_CHAR_PTR : LK_INT_PTR) : LK_INT;
    int gi = global_declare(nm, 4, kind);
    if (accept(T_ASSIGN)) {
        /* Accept signed-integer literal as the initial value. */
        int neg = 0;
        if (accept(T_MINUS)) neg = 1;
        if (tk_cur()->kind != T_NUM)
            die_at(tk_cur()->line, "global init must be an integer literal", 0);
        int v = tk_cur()->num;
        if (neg) v = -v;
        g_tk++;
        /* Write the int into g_data_pool at this global's offset. */
        int off = g_globals[gi].offset;
        g_data_pool[off + 0] = (unsigned char)(v & 0xff);
        g_data_pool[off + 1] = (unsigned char)((v >> 8) & 0xff);
        g_data_pool[off + 2] = (unsigned char)((v >> 16) & 0xff);
        g_data_pool[off + 3] = (unsigned char)((v >> 24) & 0xff);
    }
    expect(T_SEMI, "';'");
}

/* Session 97 — parse `struct TAG { int field; ... };` or `struct TAG NAME;`
 * at file scope. Disambiguates by peeking past `struct TAG` to look for
 * a brace.
 *
 * Session 125 — also handles `union TAG { ... };` / `union TAG NAME;`.
 * Internally a union is just a struct_info with `is_union=1`, all field
 * offsets stamped at 0, and size = max(field sizes) — since every cc
 * field is 4 bytes that's just 4. The existing field-access codegen
 * works without modification: u.field becomes
 * `mov eax, [ebp + off + 0]` and writes to the same memory slot for
 * every field. */
static void parse_struct_top(void) {
    int is_union;
    if (tk_cur()->kind == T_UNION) {
        is_union = 1;
        g_tk++;
    } else {
        expect(T_STRUCT, "'struct' or 'union'");
        is_union = 0;
    }
    if (tk_cur()->kind != T_NAME)
        die_at(tk_cur()->line, "expected struct/union tag name", 0);
    char tag[NAME_MAX];
    int ti = 0;
    while (tk_cur()->name[ti]) { tag[ti] = tk_cur()->name[ti]; ti++; }
    tag[ti] = 0;
    g_tk++;

    if (tk_cur()->kind == T_LBRACE) {
        /* Definition. */
        g_tk++;
        if (g_n_structs >= MAX_STRUCTS) die("too many struct types");
        int sidx = struct_find(tag);
        if (sidx < 0) {
            sidx = g_n_structs++;
            int j = 0;
            while (tag[j]) { g_structs[sidx].name[j] = tag[j]; j++; }
            g_structs[sidx].name[j] = 0;
            g_structs[sidx].n_fields = 0;
            g_structs[sidx].size = 0;
            g_structs[sidx].defined = 0;
            g_structs[sidx].is_union = is_union;
        }
        if (g_structs[sidx].defined)
            die_at(tk_cur()->line, "struct/union redefined", tag);
        g_structs[sidx].is_union = is_union;

        int field_off = 0;
        while (tk_cur()->kind != T_RBRACE) {
            /* int NAME;  char NAME;  int *NAME;  char *NAME;
             * struct OTHER *NAME;   — last is for linked-list-style. */
            int field_kind;
            int field_meta = 0;
            if (tk_cur()->kind == T_STRUCT || tk_cur()->kind == T_UNION) {
                g_tk++;
                if (tk_cur()->kind != T_NAME)
                    die_at(tk_cur()->line, "field: expected struct/union tag", 0);
                char inner_tag[NAME_MAX];
                int k = 0;
                while (tk_cur()->name[k]) { inner_tag[k] = tk_cur()->name[k]; k++; }
                inner_tag[k] = 0;
                g_tk++;
                int inner_idx = struct_find(inner_tag);
                if (inner_idx < 0) {
                    /* Allow forward reference for self/other-struct pointers. */
                    if (g_n_structs >= MAX_STRUCTS) die("too many struct types");
                    inner_idx = g_n_structs++;
                    int x = 0;
                    while (inner_tag[x]) { g_structs[inner_idx].name[x] = inner_tag[x]; x++; }
                    g_structs[inner_idx].name[x] = 0;
                    g_structs[inner_idx].n_fields = 0;
                    g_structs[inner_idx].size = 0;
                    g_structs[inner_idx].defined = 0;
                }
                if (!accept(T_STAR))
                    die_at(tk_cur()->line, "struct-typed fields must be pointers", 0);
                field_kind = LK_STRUCT_PTR;
                field_meta = inner_idx;
            } else if (tk_cur()->kind == T_INT || tk_cur()->kind == T_CHAR) {
                int is_char = (tk_cur()->kind == T_CHAR);
                g_tk++;
                int is_ptr = accept(T_STAR);
                if (is_ptr) field_kind = is_char ? LK_CHAR_PTR : LK_INT_PTR;
                else        field_kind = LK_INT;     /* scalar char ≡ int */
            } else {
                die_at(tk_cur()->line, "unsupported field type", 0);
                field_kind = LK_INT;
            }
            if (tk_cur()->kind != T_NAME)
                die_at(tk_cur()->line, "expected field name", 0);
            int fi = g_structs[sidx].n_fields;
            if (fi >= MAX_FIELDS) die("too many fields in struct");
            int k = 0;
            while (tk_cur()->name[k]) {
                g_structs[sidx].fields[fi].name[k] = tk_cur()->name[k]; k++;
            }
            g_structs[sidx].fields[fi].name[k] = 0;
            /* Session 125 — union: every field shares offset 0. struct:
             * offsets march by 4 (field_off advances after each). */
            g_structs[sidx].fields[fi].offset = is_union ? 0 : field_off;
            g_structs[sidx].fields[fi].size   = 4;
            g_structs[sidx].fields[fi].kind   = field_kind;
            g_structs[sidx].fields[fi].meta   = field_meta;
            g_structs[sidx].n_fields++;
            if (!is_union) field_off += 4;
            g_tk++;
            expect(T_SEMI, "';'");
        }
        expect(T_RBRACE, "'}'");
        expect(T_SEMI, "';'");
        /* Session 125 — for unions, size is max(field sizes). Since
         * every cc field is 4 bytes that's just 4. */
        g_structs[sidx].size    = is_union ? (g_structs[sidx].n_fields ? 4 : 0) : field_off;
        g_structs[sidx].defined = 1;
        return;
    }

    /* Otherwise: global variable declaration `struct TAG NAME;` or
     * `struct TAG *NAME;`. */
    int sidx = struct_find(tag);
    if (sidx < 0)
        die_at(tk_cur()->line, "undefined struct (must be defined before use)", tag);
    int is_ptr = accept(T_STAR);
    if (tk_cur()->kind != T_NAME)
        die_at(tk_cur()->line, "expected variable name", 0);
    char nm[NAME_MAX];
    int i = 0;
    while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
    nm[i] = 0;
    g_tk++;
    expect(T_SEMI, "';'");
    int sz = is_ptr ? 4 : g_structs[sidx].size;
    int kind = is_ptr ? LK_STRUCT_PTR : LK_STRUCT;
    global_declare_struct(nm, sz, kind, sidx);
}

/* Session 103 — parse a top-level enum:
 *   enum [TAG] { CONST [= NUM] [, CONST [= NUM]]... };
 *
 * Tag (if present) is ignored — cc doesn't distinguish enum types
 * from ints. Values auto-increment from 0 (or from the last explicit
 * value + 1). Trailing comma allowed. */
static void parse_enum_top(void) {
    expect(T_ENUM, "'enum'");
    /* Optional tag. */
    if (tk_cur()->kind == T_NAME) g_tk++;
    expect(T_LBRACE, "'{'");
    int next_val = 0;
    while (tk_cur()->kind != T_RBRACE) {
        if (tk_cur()->kind != T_NAME)
            die_at(tk_cur()->line, "expected enumerator name", 0);
        char nm[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { nm[i] = tk_cur()->name[i]; i++; }
        nm[i] = 0;
        g_tk++;
        int v = next_val;
        if (accept(T_ASSIGN)) {
            int neg = 0;
            if (accept(T_MINUS)) neg = 1;
            if (tk_cur()->kind != T_NUM)
                die_at(tk_cur()->line, "enumerator init must be int literal", 0);
            v = tk_cur()->num;
            if (neg) v = -v;
            g_tk++;
        }
        enum_add(nm, v);
        next_val = v + 1;
        if (!accept(T_COMMA)) break;
    }
    expect(T_RBRACE, "'}'");
    expect(T_SEMI, "';'");
}

/* Session 104 — parse a top-level typedef:
 *   typedef BASE NAME;
 * where BASE is int [*] / char [*] / struct TAG [*].
 *
 * Session 121 — also accept the function-pointer form:
 *   typedef RET (*NAME)(PARAMS);
 * where PARAMS is any token sequence with balanced parens (we don't
 * record arg types — cc doesn't typecheck indirect calls). The
 * resulting alias has kind LK_INT_PTR (same shape as the bare
 * `int *fp` representation cc has used for function pointers since
 * session 98). */
static void parse_typedef_top(void) {
    expect(T_TYPEDEF, "'typedef'");
    int kind, meta;
    if (!try_consume_type(&kind, &meta))
        die_at(tk_cur()->line, "typedef: expected base type", 0);
    /* Function-pointer typedef:  ( * NAME ) ( ARGS ) ; */
    if (tk_cur()->kind == T_LPAREN
        && tk_peek(1)->kind == T_STAR
        && tk_peek(2)->kind == T_NAME
        && tk_peek(3)->kind == T_RPAREN
        && tk_peek(4)->kind == T_LPAREN) {
        g_tk++;     /* ( */
        g_tk++;     /* * */
        char fp_name[NAME_MAX];
        int i = 0;
        while (tk_cur()->name[i]) { fp_name[i] = tk_cur()->name[i]; i++; }
        fp_name[i] = 0;
        g_tk++;     /* NAME */
        g_tk++;     /* ) */
        expect(T_LPAREN, "'(' before fp arg list");
        /* Skip args, balance parens — we don't typecheck them. */
        int depth = 1;
        while (depth > 0 && tk_cur()->kind != T_END) {
            if (tk_cur()->kind == T_LPAREN) depth++;
            else if (tk_cur()->kind == T_RPAREN) {
                depth--;
                if (depth == 0) break;
            }
            g_tk++;
        }
        expect(T_RPAREN, "')' after fp arg list");
        expect(T_SEMI, "';'");
        typedef_add(fp_name, LK_INT_PTR, 0);
        return;
    }
    if (tk_cur()->kind != T_NAME)
        die_at(tk_cur()->line, "typedef: expected new type name", 0);
    typedef_add(tk_cur()->name, kind, meta);
    g_tk++;
    expect(T_SEMI, "';'");
}

static struct node *parse_program(void) {
    struct node *p = new_node(N_PROGRAM);
    int cap = 0;
    while (tk_cur()->kind != T_END) {
        /* Session 103 — `enum` at top level. */
        if (tk_cur()->kind == T_ENUM) {
            parse_enum_top();
            continue;
        }
        /* Session 104 — `typedef` at top level. */
        if (tk_cur()->kind == T_TYPEDEF) {
            parse_typedef_top();
            continue;
        }
        /* Session 121 — `static` is accepted as a storage-class modifier
         * but has no semantic effect: every cc-compiled symbol is already
         * private to the translation unit. Just swallow the keyword
         * and fall through to the underlying decl. */
        if (tk_cur()->kind == T_STATIC) g_tk++;
        /* Session 121 — `extern RET NAME(PARAMS);` is a function prototype.
         * Registers signature info in g_funcs so subsequent calls see
         * argc/kind info even if the definition appears in a later TU
         * (or never, for cross-file declarations in multi-file builds —
         * the linker step would catch undefined refs). No body. */
        if (tk_cur()->kind == T_EXTERN) {
            g_tk++;
            parse_extern_proto();
            continue;
        }
        /* Session 97 / 120 — `struct` at top level.
         *   struct T { ... };        definition
         *   struct T [*] NAME;       global value/pointer decl
         *   struct T NAME[N];        global array-of-struct (parse_struct_top)
         *   struct T [*] NAME(...)   struct-by-value-return function (s121)
         *
         * Distinguish by peeking past the optional `*` and the var/func
         * name. */
        if (tk_cur()->kind == T_STRUCT || tk_cur()->kind == T_UNION) {
            /* tk_peek(0)=struct/union, tk_peek(1)=TAG. */
            if (tk_peek(2)->kind == T_LBRACE) {
                /* Definition. */
                parse_struct_top();
                continue;
            }
            int peek = 2;
            if (tk_peek(peek)->kind == T_STAR) peek++;
            if (tk_peek(peek)->kind != T_NAME)
                die_at(tk_peek(peek)->line, "expected name after struct/union TAG", 0);
            peek++;
            if (tk_peek(peek)->kind == T_LPAREN) {
                /* Struct-returning function. parse_func handles the
                 * full `struct T [*] NAME(...)` signature. */
                node_push(&p->list, &p->n_list, &cap, parse_func());
            } else {
                parse_struct_top();
            }
            continue;
        }
        /* Top-level disambiguation between function and global decl.
         * The type can be `int|char [*]` (and via session 104,
         * a typedef-name resolving to one of those). After the type
         * we expect a NAME, then `(` → function or `;`/`=`/`[` → global. */
        int td_is_type = 0;
        if (tk_cur()->kind == T_NAME && typedef_find(tk_cur()->name) >= 0)
            td_is_type = 1;
        if (tk_cur()->kind != T_INT && tk_cur()->kind != T_CHAR && !td_is_type)
            die_at(tk_cur()->line,
                   "expected 'int', 'char', 'struct', 'enum', 'typedef' or "
                   "a typedef name at top level", 0);
        /* Peek past the type (and optional '*' and name) to find the
         * disambiguator. For typedef names, the type is one token (no
         * trailing '*' is part of the typedef per session 104's design). */
        int peek;
        if (td_is_type) {
            peek = 1;            /* tk_peek(0)=typedef name; (1) is the var name */
        } else {
            peek = 1;            /* tk_peek(0)=int/char */
            if (tk_peek(peek)->kind == T_STAR) peek++;
        }
        if (tk_peek(peek)->kind != T_NAME)
            die_at(tk_peek(peek)->line, "expected name", 0);
        peek++;
        int after = tk_peek(peek)->kind;
        if (after == T_LPAREN) {
            /* Function. */
            node_push(&p->list, &p->n_list, &cap, parse_func());
        } else {
            /* Global declaration. */
            parse_global_decl();
        }
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

/* Session 122 — peephole pass (rolling).
 *
 * Returns 1 if any fixup table records an imm32 starting at code_off.
 * Used by the rolling peephole to avoid rewriting a `mov eax, imm32`
 * whose imm is actually a placeholder waiting to be patched (string
 * address, global address, function VA). Forward-declared here so
 * e_push_eax can call it; defined further down once the fixup tables
 * exist. */
static int has_imm_fixup_at(int code_off);

static void e_push_eax(void) {
    /* Rolling peephole: collapse `mov eax, imm32; push eax` (6 bytes)
     * into a single `push imm8` (2 bytes) or `push imm32` (5 bytes).
     * Common pattern from cdecl-style call-arg setup where each
     * scalar arg is emitted as mov+push.
     *
     * Skipped if the previous mov's imm32 has a fixup attached — the
     * imm bytes are placeholders for string/global/function VAs, not
     * the actual value. */
    if (g_code_len >= 5
        && g_code[g_code_len - 5] == 0xb8
        && !has_imm_fixup_at(g_code_len - 4)) {
        int imm = (int)(
              (unsigned)g_code[g_code_len - 4]
           | ((unsigned)g_code[g_code_len - 3] <<  8)
           | ((unsigned)g_code[g_code_len - 2] << 16)
           | ((unsigned)g_code[g_code_len - 1] << 24));
        g_code_len -= 5;
        if (imm >= -128 && imm <= 127) {
            emit_b(0x6a); emit_b((unsigned char)(imm & 0xff));
        } else {
            emit_b(0x68); emit_d((unsigned)imm);
        }
        return;
    }
    emit_b(0x50);
}
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

/* Session 92 — pointer / array helpers. */

/* lea eax, [ebp + disp] — load the ADDRESS of a local. Used for &x
 * and for array names in rvalue contexts (they decay to &arr[0]). */
static void e_lea_eax_ebp(int off_from_ebp) {
    if (off_from_ebp >= -128 && off_from_ebp <= 127) {
        emit_b(0x8d); emit_b(0x45); emit_b((unsigned char)(off_from_ebp & 0xff));
    } else {
        emit_b(0x8d); emit_b(0x85); emit_d((unsigned)off_from_ebp);
    }
}

/* Session 122 — EBX-targeted variants used by the smart-codegen
 * register-allocator pass. They mirror the EAX-targeted ones above so
 * a binop's RHS can be loaded directly into EBX without going through
 * EAX (skipping push/pop on the common simple-RHS case). */

/* mov ebx, [ebp + disp] */
static void e_load_local_ebx(int off_from_ebp) {
    if (off_from_ebp >= -128 && off_from_ebp <= 127) {
        emit_b(0x8b); emit_b(0x5d); emit_b((unsigned char)(off_from_ebp & 0xff));
    } else {
        emit_b(0x8b); emit_b(0x9d); emit_d((unsigned)off_from_ebp);
    }
}

/* lea ebx, [ebp + disp] */
static void e_lea_ebx_ebp(int off_from_ebp) {
    if (off_from_ebp >= -128 && off_from_ebp <= 127) {
        emit_b(0x8d); emit_b(0x5d); emit_b((unsigned char)(off_from_ebp & 0xff));
    } else {
        emit_b(0x8d); emit_b(0x9d); emit_d((unsigned)off_from_ebp);
    }
}

/* mov ebx, [imm32]   →  8b 1d imm32  (no special EBX-from-memoffs32 form;
 * use modrm). Returns file offset of the imm32 for fixup-recording. */
static int e_mov_ebx_at_abs(void) {
    emit_b(0x8b); emit_b(0x1d);
    int off = g_code_len;
    emit_d(0);
    return off;
}

/* mov ebx, imm32 with offset returned for fixup. Used for &global and
 * function-address loads into EBX. */
static int e_mov_ebx_imm_for_fixup(void) {
    emit_b(0xbb);
    int off = g_code_len;
    emit_d(0);
    return off;
}

/* mov eax, [eax]  →  8b 00  (dword load through eax) */
static void e_load_eax_at_eax(void) { emit_b(0x8b); emit_b(0x00); }

/* Session 122 — mov eax, [eax + disp]. Used to collapse the
 * "deref-with-offset" pattern (e.g. N_ARROW: `(*p).field` =
 * `*(p + field_off)`) into a single addressed load. */
static void e_load_eax_at_eax_disp(int disp) {
    if (disp == 0) {
        emit_b(0x8b); emit_b(0x00);
    } else if (disp >= -128 && disp <= 127) {
        emit_b(0x8b); emit_b(0x40); emit_b((unsigned char)(disp & 0xff));
    } else {
        emit_b(0x8b); emit_b(0x80); emit_d((unsigned)disp);
    }
}

/* movzx eax, byte [eax]  →  0f b6 00  (byte load, zero-extended) */
static void e_loadb_eax_at_eax(void) { emit_b(0x0f); emit_b(0xb6); emit_b(0x00); }

/* mov [ebx], eax  →  89 03  (dword store, addr in ebx) */
static void e_store_eax_at_ebx(void) { emit_b(0x89); emit_b(0x03); }

/* mov [ebx], al   →  88 03  (byte store, addr in ebx) */
static void e_storeb_al_at_ebx(void) { emit_b(0x88); emit_b(0x03); }

/* shl eax, imm8   →  c1 e0 imm8.  Used for index scaling. */
static void e_shl_eax_imm8(int imm) {
    emit_b(0xc1); emit_b(0xe0); emit_b((unsigned char)(imm & 0xff));
}

/* Session 93 — absolute-address forms (used by global accesses).
 * Each emit returns the file offset of the imm32 so the caller can
 * record a fixup; the imm itself is emitted as 0 and patched later. */

/* mov eax, [imm32]   →  a1 imm32   (special form for EAX-from-memoffs32) */
static int e_mov_eax_at_abs(void) {
    emit_b(0xa1);
    int off = g_code_len;
    emit_d(0);
    return off;
}
/* mov [imm32], eax   →  a3 imm32 */
static int e_mov_at_abs_eax(void) {
    emit_b(0xa3);
    int off = g_code_len;
    emit_d(0);
    return off;
}
/* movzx eax, byte [imm32]  →  0f b6 05 imm32 */
static int e_movzx_eax_at_abs_b(void) {
    emit_b(0x0f); emit_b(0xb6); emit_b(0x05);
    int off = g_code_len;
    emit_d(0);
    return off;
}
/* mov [imm32], al   →  a2 imm32 */
static int e_mov_at_abs_al(void) {
    emit_b(0xa2);
    int off = g_code_len;
    emit_d(0);
    return off;
}
/* mov eax, imm32-as-address (used as `lea` substitute for &global)
 *   b8 imm32 — same as e_mov_eax_imm but exposing the imm-offset. */
static int e_mov_eax_imm_for_fixup(void) {
    emit_b(0xb8);
    int off = g_code_len;
    emit_d(0);
    return off;
}

/* Session 93 — record a code-offset → global-index fixup. */
static void record_glob_fixup(int code_off, int glob_idx) {
    if (g_n_glob_fixups >= MAX_GLOB_FIXUPS) die("too many global fixups");
    g_glob_fixups[g_n_glob_fixups].code_off = code_off;
    g_glob_fixups[g_n_glob_fixups].glob_idx = glob_idx;
    g_n_glob_fixups++;
}

/* Emit a load-the-value-of-a-global into eax. Width depends on the
 * global's kind: dword for int / int* / char*; byte (movzx) for
 * future LK_CHAR scalars; for arrays we emit the BASE ADDRESS
 * (array names decay). */
static void emit_load_global(int gi) {
    int k = g_globals[gi].kind;
    if (kind_is_array(k)) {
        int off = e_mov_eax_imm_for_fixup();
        record_glob_fixup(off, gi);
        return;
    }
    /* All scalar globals stored as 4 bytes; load as dword. */
    int off = e_mov_eax_at_abs();
    record_glob_fixup(off, gi);
}
/* Emit `mov eax, GLOBAL_VA` — the address of the global. */
static void emit_addrof_global(int gi) {
    int off = e_mov_eax_imm_for_fixup();
    record_glob_fixup(off, gi);
}
/* Emit `mov [GLOBAL_VA], eax` — scalar store. */
static void emit_store_global(int gi) {
    if (kind_is_array(g_globals[gi].kind))
        die("can't assign to whole array");
    int off = e_mov_at_abs_eax();
    record_glob_fixup(off, gi);
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

/* MAX_PARAMS_PER_FUNC is declared near the top of the file so the
 * parser's parse_extern_proto can reference it. */
struct func_info {
    char name[NAME_MAX];
    int  entry_off;     /* byte offset within g_code where the function starts */
    int  n_params;      /* named params (excludes the trailing ...) for variadic */
    int  defined;       /* once codegen-resolved (set when entry_off is real) */
    int  is_variadic;   /* 1 if declared with `...` — session 105 */
    /* Session 106 — per-param kind & meta (for struct-by-value calls).
     * Pre-populated by main() before codegen runs so call sites have
     * full param info regardless of definition order. */
    int  param_kinds[MAX_PARAMS_PER_FUNC];
    int  param_metas[MAX_PARAMS_PER_FUNC];
    /* Session 121 — return-type info. LK_INT default; LK_STRUCT with
     * ret_meta = struct_idx means struct-by-value return (caller passes
     * hidden first arg = dest pointer, callee writes through it). */
    int  ret_kind;
    int  ret_meta;
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

/* Session 92 — type-kinds for locals + function-param entries.
 *
 * The compiler stays untyped at the expression level: gen_expr drops
 * its result in eax and the caller has to know what shape that is.
 * But DECLARATIONS need a type tag so we know:
 *   - how much stack to reserve (4 bytes for int/ptr, 1*N for char[N])
 *   - what to emit for `a[i]` / `*p` (byte vs word loads/stores)
 *   - whether to lea-an-address or load-the-value when an array name
 *     appears in an rvalue context
 *
 * LK_INT_PTR is treated identically to LK_INT internally — we only
 * really distinguish "1-byte-element" from "4-byte-element" pointers
 * because that's what affects load/store size and `a[i]` scaling.
 * Same for params. */
/* Per-function local table — reset at the start of each function. */
struct local_slot {
    char name[NAME_MAX];
    int  ebp_off;       /* negative offset from EBP */
    int  kind;          /* LK_* — session 92 */
    int  meta;          /* struct_idx for LK_STRUCT/LK_STRUCT_PTR (s97) */
    int  dim2;          /* session 125 — inner dim for 2D arrays
                         * (M in `int a[N][M]`); 0 for 1D / non-array */
};
static struct local_slot g_locals[MAX_LOCALS];
static int               g_n_locals;
static int               g_locals_bytes;    /* total bytes of locals in current fn */

/* Session 121 — current function's return-type info. Used by
 * gen_stmt(N_RETURN) to decide whether to emit a scalar epilogue
 * (mov eax, ...; ret) or a struct-return memcpy + ret. Set by
 * gen_func at the top of each function, restored on exit (we don't
 * have nested function defs so this can stay a flat pair). */
static int g_cur_ret_kind;
static int g_cur_ret_meta;


/* Returns the size (in bytes) the pointee of this pointer occupies.
 * Used both for `*p` load/store width and for `a[i]` index scaling.
 * For non-pointer kinds returns 0 (caller should never ask). */
static int kind_elem_size(int k) {
    if (k == LK_CHAR_PTR || k == LK_CHAR_ARR) return 1;
    if (k == LK_INT_PTR  || k == LK_INT_ARR)  return 4;
    return 0;
}
static int kind_is_array(int k) {
    return k == LK_INT_ARR || k == LK_CHAR_ARR || k == LK_STRUCT_ARR;
}
static int kind_is_pointerlike(int k) {
    return k == LK_INT_PTR    || k == LK_CHAR_PTR
        || k == LK_INT_ARR    || k == LK_CHAR_ARR
        || k == LK_STRUCT_PTR || k == LK_STRUCT_ARR;
}

static int func_find(const char *name) {
    for (int i = 0; i < g_n_funcs; i++) {
        if (my_streq(g_funcs[i].name, name)) return i;
    }
    return -1;
}

static int func_intern(const char *name, int n_params) {
    int i = func_find(name);
    if (i >= 0) {
        /* Session 98 — accept a sentinel n_params of -1 in the table
         * (created by address-of-function before the real decl/call).
         * The first non-sentinel value wins. */
        if (g_funcs[i].n_params == -1) {
            g_funcs[i].n_params = n_params;
            return i;
        }
        /* Session 105 — variadic functions accept any argc from call
         * sites. We only enforce the check for non-variadic functions.
         * (Forward calls to variadic functions still need the variadic
         * flag to be set — the definition typically comes first.) */
        if (g_funcs[i].is_variadic) return i;
        if (n_params >= 0 && g_funcs[i].n_params != n_params)
            die_at(0, "arg-count mismatch for", name);
        return i;
    }
    if (g_n_funcs >= MAX_FUNCS) die("too many functions");
    int idx = g_n_funcs++;
    int j = 0;
    while (name[j]) { g_funcs[idx].name[j] = name[j]; j++; }
    g_funcs[idx].name[j] = 0;
    g_funcs[idx].entry_off  = -1;
    g_funcs[idx].n_params   = n_params;
    g_funcs[idx].defined    = 0;
    g_funcs[idx].is_variadic = 0;
    g_funcs[idx].ret_kind   = LK_INT;
    g_funcs[idx].ret_meta   = 0;
    return idx;
}

/* Session 121 — record a function prototype (from `extern RET NAME(...);`)
 * in the symbol table without emitting a body. Called from
 * parse_extern_proto via a forward declaration at the top of the file
 * (so the parser doesn't need to know the layout of struct func_info). */
static void register_func_proto(const char *name, int n_params, int is_variadic,
                                int ret_kind, int ret_meta,
                                int *param_kinds, int *param_metas) {
    int idx = func_intern(name, n_params);
    g_funcs[idx].is_variadic = is_variadic;
    g_funcs[idx].ret_kind = ret_kind;
    g_funcs[idx].ret_meta = ret_meta;
    int np = n_params;
    if (np > MAX_PARAMS_PER_FUNC) np = MAX_PARAMS_PER_FUNC;
    for (int k = 0; k < np; k++) {
        g_funcs[idx].param_kinds[k] = param_kinds[k];
        g_funcs[idx].param_metas[k] = param_metas[k];
    }
}

/* Session 98 — address-fixup table.
 *
 * Used when codegen emits a function's address (for `fp = my_func;`
 * or `&my_func`). The imm32 in `mov eax, imm32` is filled with the
 * absolute VA (ENTRY_VA + g_funcs[i].entry_off) once codegen for
 * every function is done. */
#define MAX_ADDR_FIXUPS 128
struct addr_fixup {
    int imm_off;
    int func_idx;
};
static struct addr_fixup g_addr_fixups[MAX_ADDR_FIXUPS];
static int               g_n_addr_fixups;

static void record_addr_fixup(int imm_off, int func_idx) {
    if (g_n_addr_fixups >= MAX_ADDR_FIXUPS) die("too many addr fixups");
    g_addr_fixups[g_n_addr_fixups].imm_off  = imm_off;
    g_addr_fixups[g_n_addr_fixups].func_idx = func_idx;
    g_n_addr_fixups++;
}

/* Session 122 — implementation of the forward-declared peephole helper.
 * Scans the three imm32-style fixup tables (string, global, addr-of-
 * function) and returns 1 if any of them recorded the offset. The
 * call-rel32 fixup table (g_fixups) records DISPLACEMENTS for
 * `e8 disp32` / `0f 84 disp32`, never imm-loads — so it's not scanned
 * here. */
static int has_imm_fixup_at(int code_off) {
    for (int i = 0; i < g_n_str_fixups; i++)
        if (g_str_fixups[i].code_off == code_off) return 1;
    for (int i = 0; i < g_n_glob_fixups; i++)
        if (g_glob_fixups[i].code_off == code_off) return 1;
    for (int i = 0; i < g_n_addr_fixups; i++)
        if (g_addr_fixups[i].imm_off == code_off) return 1;
    return 0;
}

static int local_find(const char *name) {
    for (int i = g_n_locals - 1; i >= 0; i--) {
        if (my_streq(g_locals[i].name, name)) return g_locals[i].ebp_off;
    }
    return 0;     /* 0 == not found (real locals have negative offsets) */
}
/* Companion: return the LK_* kind for a local by name, or -1 if unknown.
 * Session 92. */
static int local_kind(const char *name) {
    for (int i = g_n_locals - 1; i >= 0; i--) {
        if (my_streq(g_locals[i].name, name)) return g_locals[i].kind;
    }
    return -1;
}

/* `size` is the number of bytes to reserve on the stack for this
 * local. For int / int* / char* / char that's 4; for arrays it's
 * elem_size * n. The stack offset is rounded down to a 4-byte boundary
 * because every other access is dword-sized — i.e. we always reserve
 * a multiple of 4 even when storing a char[3]. */
static int local_declare_sized(const char *name, int size, int kind) {
    if (g_n_locals >= MAX_LOCALS) die("too many locals");
    int padded = (size + 3) & ~3;       /* round up to multiple of 4 */
    g_locals_bytes += padded;
    int off = -g_locals_bytes;
    int j = 0;
    while (name[j]) { g_locals[g_n_locals].name[j] = name[j]; j++; }
    g_locals[g_n_locals].name[j] = 0;
    g_locals[g_n_locals].ebp_off = off;
    g_locals[g_n_locals].kind    = kind;
    g_locals[g_n_locals].meta    = 0;
    g_locals[g_n_locals].dim2    = 0;     /* session 125 — set later for 2D arrays */
    g_n_locals++;
    return off;
}

/* Session 97 — like local_declare_sized but also stashes `meta`.
 * Used for `struct T x;` (kind=LK_STRUCT, meta=struct_idx) and for
 * `struct T *p;` (kind=LK_STRUCT_PTR, meta=struct_idx). */
static int local_declare_struct(const char *name, int size, int kind, int meta) {
    int off = local_declare_sized(name, size, kind);
    g_locals[g_n_locals - 1].meta = meta;
    return off;
}

/* Shorthand for the common int-local case. */
static int local_declare(const char *name) {
    return local_declare_sized(name, 4, LK_INT);
}

/* Session 97 — find a local's meta (struct_idx). Returns -1 if not
 * found. Companion to local_kind. */
static int local_meta(const char *name) {
    for (int i = g_n_locals - 1; i >= 0; i--) {
        if (my_streq(g_locals[i].name, name)) return g_locals[i].meta;
    }
    return -1;
}

/* Session 93 — global lookup + declaration. */
static int global_find(const char *name) {
    for (int i = 0; i < g_n_globals; i++) {
        if (my_streq(g_globals[i].name, name)) return i;
    }
    return -1;
}
static int global_declare(const char *name, int size, int kind) {
    if (g_n_globals >= MAX_GLOBALS) die("too many globals");
    if (global_find(name) >= 0)
        die_at(0, "duplicate global", name);
    int padded = (size + 3) & ~3;
    if (g_data_pool_len + padded > DATA_POOL_MAX) die("global data overflow");
    int idx = g_n_globals++;
    int j = 0;
    while (name[j]) { g_globals[idx].name[j] = name[j]; j++; }
    g_globals[idx].name[j] = 0;
    g_globals[idx].offset = g_data_pool_len;
    g_globals[idx].size   = padded;
    g_globals[idx].kind   = kind;
    g_globals[idx].meta   = 0;
    /* Bytes are zero by default (g_data_pool lives in .bss). The
     * parser writes initial bytes directly afterward if there's an
     * initializer. */
    g_data_pool_len += padded;
    return idx;
}

/* Session 97 — global_declare for struct types. */
static int global_declare_struct(const char *name, int size, int kind, int meta) {
    int gi = global_declare(name, size, kind);
    g_globals[gi].meta = meta;
    return gi;
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
    /* Session 91 — puts(s) / print_str(s). Both take 1 arg (char*) and
     * trampoline into a fixed helper. */
    if (my_streq(name, "puts") || my_streq(name, "print_str")) {
        if (call->n_list != 1) die_at(call->line, "puts/print_str takes 1 arg", 0);
        gen_expr(call->list[0]);
        e_push_eax();
        const char *helper = my_streq(name, "puts")
            ? "__puts_helper" : "__print_str_helper";
        int idx = func_find(helper);
        if (idx < 0) die("puts/print_str helper missing");
        int disp_off = e_call_rel32();
        if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
        g_fixups[g_n_fixups].call_disp_off = disp_off;
        g_fixups[g_n_fixups].func_idx = idx;
        g_n_fixups++;
        e_add_esp_imm32(4);
        return;
    }
    /* Session 94 — printf(fmt_literal, args...). Decomposed at compile
     * time: the format string MUST be a string literal. Each %X expands
     * into a call to the matching helper; literal text between specs
     * is interned as a separate pool entry and printed via print_str.
     *
     * Specifiers: %d %s %c %x %% — anything else is a parse-error at
     * compile time. */
    if (my_streq(name, "printf")) {
        if (call->n_list < 1) die_at(call->line, "printf needs at least 1 arg", 0);
        struct node *fmt_node = call->list[0];
        if (fmt_node->kind != N_STR)
            die_at(call->line, "printf format must be a literal string", 0);
        const char *fmt = &g_str_pool[g_str_offs[fmt_node->num]];
        int arg_idx = 1;
        char chunk[256];
        int  chunk_n = 0;
        /* Helper indices — all resolved up-front. */
        int idx_str  = func_find("__print_str_helper");
        int idx_int  = func_find("__print_int_nonl_helper");
        int idx_char = func_find("__print_char_helper");
        int idx_hex  = func_find("__print_hex_helper");
        if (idx_str < 0 || idx_int < 0 || idx_char < 0 || idx_hex < 0)
            die("printf helpers missing");
        for (int i = 0; fmt[i]; i++) {
            char c = fmt[i];
            if (c != '%') {
                if (chunk_n >= (int)sizeof(chunk) - 1) {
                    /* Force-flush the chunk we have to keep tmp[] small. */
                    int sidx = str_intern(chunk, chunk_n);
                    if (g_n_str_fixups >= MAX_STR_FIXUPS) die("too many str fixups");
                    emit_b(0xb8);
                    int imm_off = g_code_len; emit_d(0);
                    g_str_fixups[g_n_str_fixups].code_off = imm_off;
                    g_str_fixups[g_n_str_fixups].str_idx  = sidx;
                    g_n_str_fixups++;
                    e_push_eax();
                    int disp_off = e_call_rel32();
                    if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
                    g_fixups[g_n_fixups].call_disp_off = disp_off;
                    g_fixups[g_n_fixups].func_idx = idx_str;
                    g_n_fixups++;
                    e_add_esp_imm32(4);
                    chunk_n = 0;
                }
                chunk[chunk_n++] = c;
                continue;
            }
            /* Hit '%'. Flush any pending plain-text chunk first. */
            if (chunk_n > 0) {
                int sidx = str_intern(chunk, chunk_n);
                if (g_n_str_fixups >= MAX_STR_FIXUPS) die("too many str fixups");
                emit_b(0xb8);
                int imm_off = g_code_len; emit_d(0);
                g_str_fixups[g_n_str_fixups].code_off = imm_off;
                g_str_fixups[g_n_str_fixups].str_idx  = sidx;
                g_n_str_fixups++;
                e_push_eax();
                int disp_off = e_call_rel32();
                if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
                g_fixups[g_n_fixups].call_disp_off = disp_off;
                g_fixups[g_n_fixups].func_idx = idx_str;
                g_n_fixups++;
                e_add_esp_imm32(4);
                chunk_n = 0;
            }
            char spec = fmt[++i];
            if (spec == 0) die_at(call->line, "trailing % in printf format", 0);
            if (spec == '%') {
                /* Literal %. Re-add to the chunk. */
                chunk[chunk_n++] = '%';
                continue;
            }
            int helper_idx;
            switch (spec) {
                case 'd': helper_idx = idx_int;  break;
                case 's': helper_idx = idx_str;  break;
                case 'c': helper_idx = idx_char; break;
                case 'x': helper_idx = idx_hex;  break;
                default:
                    die_at(call->line, "unknown printf specifier", 0);
                    helper_idx = -1;
            }
            if (arg_idx >= call->n_list)
                die_at(call->line, "not enough args for printf format", 0);
            gen_expr(call->list[arg_idx++]);
            e_push_eax();
            int disp_off = e_call_rel32();
            if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
            g_fixups[g_n_fixups].call_disp_off = disp_off;
            g_fixups[g_n_fixups].func_idx = helper_idx;
            g_n_fixups++;
            e_add_esp_imm32(4);
        }
        /* Flush final chunk. */
        if (chunk_n > 0) {
            int sidx = str_intern(chunk, chunk_n);
            if (g_n_str_fixups >= MAX_STR_FIXUPS) die("too many str fixups");
            emit_b(0xb8);
            int imm_off = g_code_len; emit_d(0);
            g_str_fixups[g_n_str_fixups].code_off = imm_off;
            g_str_fixups[g_n_str_fixups].str_idx  = sidx;
            g_n_str_fixups++;
            e_push_eax();
            int disp_off = e_call_rel32();
            if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
            g_fixups[g_n_fixups].call_disp_off = disp_off;
            g_fixups[g_n_fixups].func_idx = idx_str;
            g_n_fixups++;
            e_add_esp_imm32(4);
        }
        if (arg_idx != call->n_list)
            die_at(call->line, "too many args for printf format", 0);
        /* printf returns "number of bytes printed" in real C. We just
         * leave eax with whatever the last helper returned — caller's
         * problem. */
        return;
    }
    /* Session 105 — variadic-arg intrinsics.
     *   va_start(ap, last_named)  ap = &last_named + 4 (next stack slot)
     *   va_arg(ap)                returns *ap, then ap += 4
     *   va_end(ap)                no-op
     *
     * `ap` must be a local int variable (we treat it as a pointer to
     * the next unread arg). `last_named` must be a function parameter
     * (positive ebp_off). Restriction: va_arg always reads 4 bytes;
     * cc has no way to specify the type so the caller is responsible. */
    if (my_streq(name, "va_start")) {
        if (call->n_list != 2)
            die_at(call->line, "va_start takes 2 args", 0);
        struct node *ap_n   = call->list[0];
        struct node *last_n = call->list[1];
        if (!ap_n || ap_n->kind != N_NAME)
            die_at(call->line, "va_start first arg must be a local name", 0);
        if (!last_n || last_n->kind != N_NAME)
            die_at(call->line, "va_start second arg must be a param name", 0);
        int ap_off = local_find(ap_n->name);
        int last_off = local_find(last_n->name);
        if (ap_off == 0 || last_off == 0)
            die_at(call->line, "va_start: name not a local", 0);
        if (last_off < 8)
            die_at(call->line, "va_start: second arg must be a param (ebp+N)", 0);
        /* lea eax, [ebp + last_off + 4]; store to ap. */
        e_lea_eax_ebp(last_off + 4);
        e_store_local(ap_off);
        return;
    }
    if (my_streq(name, "va_arg")) {
        if (call->n_list != 1)
            die_at(call->line, "va_arg takes 1 arg", 0);
        struct node *ap_n = call->list[0];
        if (!ap_n || ap_n->kind != N_NAME)
            die_at(call->line, "va_arg arg must be a local name", 0);
        int ap_off = local_find(ap_n->name);
        if (ap_off == 0)
            die_at(call->line, "va_arg: name not a local", 0);
        /* load ap, deref to eax, advance ap by 4, write back. */
        /* mov ebx, [ebp + ap_off]   →  8b 5d disp8 / 8b 9d disp32 */
        if (ap_off >= -128 && ap_off <= 127) {
            emit_b(0x8b); emit_b(0x5d); emit_b((unsigned char)(ap_off & 0xff));
        } else {
            emit_b(0x8b); emit_b(0x9d); emit_d((unsigned)ap_off);
        }
        /* mov eax, [ebx]   →  8b 03 */
        emit_b(0x8b); emit_b(0x03);
        /* add ebx, 4       →  83 c3 04 */
        emit_b(0x83); emit_b(0xc3); emit_b(0x04);
        /* mov [ebp + ap_off], ebx */
        if (ap_off >= -128 && ap_off <= 127) {
            emit_b(0x89); emit_b(0x5d); emit_b((unsigned char)(ap_off & 0xff));
        } else {
            emit_b(0x89); emit_b(0x9d); emit_d((unsigned)ap_off);
        }
        return;
    }
    if (my_streq(name, "va_end")) {
        /* no-op — we don't have heap-allocated va_list state. */
        if (call->n_list != 1)
            die_at(call->line, "va_end takes 1 arg", 0);
        return;
    }
    die_at(call->line, "unknown intrinsic", name);
}

/* True if `name` is a built-in intrinsic. */
static int is_intrinsic(const char *name) {
    return my_streq(name, "sys_exit")
        || my_streq(name, "sys_write")
        || my_streq(name, "sys_getpid")
        || my_streq(name, "print_int")
        || my_streq(name, "puts")
        || my_streq(name, "print_str")
        || my_streq(name, "printf")          /* session 94 */
        || my_streq(name, "va_start")        /* session 105 */
        || my_streq(name, "va_arg")
        || my_streq(name, "va_end");
}

/* Session 121 — emit the cdecl right-to-left arg push for `call`.
 * Returns total bytes pushed (so the caller can `add esp, N` to clean
 * up). Used by gen_call and by struct-returning call sites in
 * gen_stmt(N_ASSIGN) / gen_stmt(N_RETURN). */
static int push_call_args(struct node *call, int callee_idx) {
    int argc = call->n_list;
    int total_push = 0;
    for (int i = argc - 1; i >= 0; i--) {
        int p_kind = LK_INT;
        int p_meta = 0;
        if (callee_idx >= 0 && i < g_funcs[callee_idx].n_params
                            && i < MAX_PARAMS_PER_FUNC) {
            p_kind = g_funcs[callee_idx].param_kinds[i];
            p_meta = g_funcs[callee_idx].param_metas[i];
        }
        if (p_kind == LK_STRUCT) {
            /* Struct-by-value: copy the source struct onto the stack.
             * Source must be a NAME of LK_STRUCT (same struct type). */
            struct node *arg = call->list[i];
            if (!arg || arg->kind != N_NAME)
                die_at(call->line, "struct-by-value arg must be a NAME", call->name);
            int arg_off = local_find(arg->name);
            int arg_k, arg_sidx;
            if (arg_off != 0) {
                arg_k = local_kind(arg->name);
                arg_sidx = local_meta(arg->name);
            } else {
                int ggi = global_find(arg->name);
                if (ggi < 0)
                    die_at(call->line, "struct-by-value arg: undefined", arg->name);
                arg_k = g_globals[ggi].kind;
                arg_sidx = g_globals[ggi].meta;
                /* Globals not supported for now in struct-by-value
                 * (would need an additional address-load path here). */
                die_at(call->line, "struct-by-value from global not yet supported", arg->name);
            }
            if (arg_k != LK_STRUCT || arg_sidx != p_meta)
                die_at(call->line, "struct-by-value arg type mismatch", arg->name);

            int sz = (g_structs[p_meta].size + 3) & ~3;
            int dwords = sz / 4;
            /* sub esp, sz */
            e_sub_esp_imm32(sz);
            /* push esi; push edi */
            emit_b(0x56); emit_b(0x57);
            /* lea edi, [esp + 8]  — dst (skips the 2 pushed regs) */
            emit_b(0x8d); emit_b(0x7c); emit_b(0x24); emit_b(0x08);
            /* lea esi, [ebp + arg_off] — src */
            e_lea_eax_ebp(arg_off);
            emit_b(0x89); emit_b(0xc6);  /* mov esi, eax */
            /* mov ecx, dwords */
            emit_b(0xb9); emit_d((unsigned)dwords);
            /* rep movsd */
            emit_b(0xf3); emit_b(0xa5);
            /* pop edi; pop esi */
            emit_b(0x5f); emit_b(0x5e);
            total_push += sz;
        } else {
            gen_expr(call->list[i]);
            e_push_eax();
            total_push += 4;
        }
    }
    return total_push;
}

static void gen_call(struct node *call) {
    if (is_intrinsic(call->name)) {
        emit_syscall_intrinsic(call->name, call);
        return;
    }
    int argc = call->n_list;
    /* Session 106 — look up the called function's param kinds so we
     * know which args are struct-by-value (LK_STRUCT). For indirect
     * calls and not-yet-seen forward refs we default to 4-byte ints. */
    int callee_idx = -1;
    int local_off = local_find(call->name);
    int global_gi = (local_off == 0) ? global_find(call->name) : -1;
    int is_indirect = (local_off != 0 || global_gi >= 0);
    if (!is_indirect) callee_idx = func_find(call->name);

    /* Session 121 — struct-returning calls can't appear as bare
     * expressions because we'd have nowhere to put the result. They
     * must be the RHS of an assignment (handled in gen_stmt) or a
     * return (handled there). */
    if (callee_idx >= 0 && g_funcs[callee_idx].ret_kind == LK_STRUCT)
        die_at(call->line,
               "struct-returning call must be assigned to a struct lvalue",
               call->name);

    int total_push = push_call_args(call, callee_idx);
    /* Session 98 — indirect (variable holds function pointer) or
     * direct call. */
    if (is_indirect) {
        if (local_off != 0) e_load_local(local_off);
        else                emit_load_global(global_gi);
        /* call eax  →  ff d0 */
        emit_b(0xff); emit_b(0xd0);
    } else {
        int idx = func_intern(call->name, argc);
        int disp_off = e_call_rel32();
        if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
        g_fixups[g_n_fixups].call_disp_off = disp_off;
        g_fixups[g_n_fixups].func_idx = idx;
        g_n_fixups++;
    }
    /* Caller cleans up. */
    if (total_push > 0) e_add_esp_imm32(total_push);
}

/* Session 99 — returns the pointee element size if `n` is a NAME of
 * a pointer-like kind (LK_*_PTR / LK_*_ARR / LK_STRUCT_PTR). Returns 0
 * if `n` is an int / char / non-pointer / non-NAME. Used by N_BIN
 * (for scaled pointer arithmetic) and N_INC_DEC (for `p++`). */
static int expr_ptr_elem_size(struct node *n) {
    if (!n || n->kind != N_NAME) return 0;
    int k, sidx = -1;
    int off = local_find(n->name);
    if (off != 0) {
        k = local_kind(n->name);
        sidx = local_meta(n->name);
    } else {
        int gi = global_find(n->name);
        if (gi < 0) return 0;
        k = g_globals[gi].kind;
        sidx = g_globals[gi].meta;
    }
    if (k == LK_CHAR_PTR || k == LK_CHAR_ARR) return 1;
    if (k == LK_INT_PTR  || k == LK_INT_ARR)  return 4;
    if (k == LK_STRUCT_PTR || k == LK_STRUCT_ARR) {
        if (sidx >= 0 && sidx < g_n_structs) return g_structs[sidx].size;
        return 4;
    }
    return 0;
}

/* ---- Session 122 — constant folding ------------------------------- *
 *
 * Pre-codegen pass that walks the AST and replaces N_UN / N_BIN /
 * N_TERNARY nodes whose operands are N_NUM (integer literals) with
 * a single N_NUM holding the folded result. Eliminates compile-time
 * arithmetic and gives the dead-code-elimination pass (#4) something
 * to chew on: `if (CONST)` becomes `if (0)` or `if (N)`, and DCE
 * strips the dead branch entirely.
 *
 * Walk is post-order: children fold first so their parents can see
 * the literal values. Division by zero is left as-is (codegen will
 * emit the divide and the program will trap at runtime — same
 * semantics as before). Comparison operators fold to 0/1. */
static void fold_node(struct node *n) {
    if (!n) return;
    /* Recurse first so children become literals before we look. */
    fold_node(n->a);
    fold_node(n->b);
    fold_node(n->c);
    fold_node(n->body);
    for (int i = 0; i < n->n_list; i++)   fold_node(n->list[i]);
    for (int i = 0; i < n->n_params; i++) fold_node(n->params[i]);

    if (n->kind == N_UN && n->a && n->a->kind == N_NUM) {
        int v = n->a->num, r;
        switch (n->op) {
            case T_MINUS: r = -v;     break;
            case T_BANG:  r = !v;     break;
            case T_TILDE: r = ~v;     break;
            default: return;
        }
        n->kind = N_NUM;
        n->num  = r;
        n->a    = 0;
        return;
    }

    if (n->kind == N_BIN
        && n->a && n->a->kind == N_NUM
        && n->b && n->b->kind == N_NUM)
    {
        int a = n->a->num, b = n->b->num, r;
        switch (n->op) {
            case T_PLUS:      r = a + b;  break;
            case T_MINUS:     r = a - b;  break;
            case T_STAR:      r = a * b;  break;
            case T_SLASH:     if (b == 0) return; r = a / b;  break;
            case T_PERCENT:   if (b == 0) return; r = a % b;  break;
            case T_AMP:       r = a & b;  break;
            case T_PIPE:      r = a | b;  break;
            case T_CARET:     r = a ^ b;  break;
            case T_LSHIFT:    r = a << b; break;
            case T_RSHIFT:    r = a >> b; break;
            case T_EQ:        r = (a == b); break;
            case T_NEQ:       r = (a != b); break;
            case T_LT:        r = (a <  b); break;
            case T_GT:        r = (a >  b); break;
            case T_LE:        r = (a <= b); break;
            case T_GE:        r = (a >= b); break;
            case T_AMP_AMP:   r = (a && b); break;
            case T_PIPE_PIPE: r = (a || b); break;
            default: return;
        }
        n->kind = N_NUM;
        n->num  = r;
        n->a    = 0;
        n->b    = 0;
        return;
    }

    /* Ternary `c ? t : e` with constant c — splice the chosen branch
     * in place of the ternary node. Children have already been folded
     * post-order so the splice doesn't re-fold them. */
    if (n->kind == N_TERNARY && n->a && n->a->kind == N_NUM) {
        struct node *chosen = n->a->num ? n->b : n->c;
        if (chosen) *n = *chosen;   /* shallow copy — children stay shared */
    }
}

/* ---- Session 122 — register-allocator helpers --------------------- *
 *
 * The default codegen evaluates every binop with a push/pop round-trip
 * around EAX:
 *
 *     gen_expr(lhs)   ; eax = lhs
 *     push eax
 *     gen_expr(rhs)   ; eax = rhs (clobbers eax)
 *     mov  ebx, eax   ; ebx = rhs
 *     pop  eax        ; eax = lhs (restored)
 *     <op>            ; eax = lhs op rhs
 *
 * For the *very* common case where one operand is "simple" — an
 * N_NUM, N_STR, N_ADDR_OF, or N_NAME that loads from a fixed local
 * /global slot — we can compute the simple side directly into EBX
 * without touching EAX. That saves a push + pop (2 bytes) per binop,
 * which compounds quickly. The same pattern shows up in N_INDEX,
 * N_INDEX_ASSIGN, N_DEREF_ASSIGN, and N_INDEX_MEMBER_ASSIGN.
 *
 * `is_simple_load` says whether `gen_simple_into_ebx` can handle the
 * node. The helper is intentionally narrow — it only fires on cases
 * where we can guarantee EAX is preserved. Anything that *might*
 * clobber EAX (calls, nested binops, derefs, indexing) falls back to
 * the push/pop path. */
static int is_simple_load(struct node *n) {
    if (!n) return 0;
    if (n->kind == N_NUM)     return 1;
    if (n->kind == N_STR)     return 1;
    if (n->kind == N_NAME)    return 1;
    if (n->kind == N_ADDR_OF) return 1;
    return 0;
}

/* Emit a load of `n` into EBX. Mirrors the EAX-targeted code paths
 * in gen_expr's N_NUM / N_STR / N_NAME / N_ADDR_OF cases. Caller has
 * checked is_simple_load(n). */
static void gen_simple_into_ebx(struct node *n) {
    if (n->kind == N_NUM) {
        e_mov_ebx_imm(n->num);
        return;
    }
    if (n->kind == N_STR) {
        if (g_n_str_fixups >= MAX_STR_FIXUPS) die("too many string fixups");
        int imm_off = e_mov_ebx_imm_for_fixup();
        g_str_fixups[g_n_str_fixups].code_off = imm_off;
        g_str_fixups[g_n_str_fixups].str_idx  = n->num;
        g_n_str_fixups++;
        return;
    }
    if (n->kind == N_NAME) {
        int off = local_find(n->name);
        if (off != 0) {
            int k = local_kind(n->name);
            if (kind_is_array(k)) e_lea_ebx_ebp(off);
            else                  e_load_local_ebx(off);
            return;
        }
        int gi = global_find(n->name);
        if (gi >= 0) {
            if (kind_is_array(g_globals[gi].kind)) {
                int imm_off = e_mov_ebx_imm_for_fixup();
                record_glob_fixup(imm_off, gi);
            } else {
                int imm_off = e_mov_ebx_at_abs();
                record_glob_fixup(imm_off, gi);
            }
            return;
        }
        /* Function name decays to its VA. */
        int fi = func_find(n->name);
        if (fi < 0) fi = func_intern(n->name, -1);
        int imm_off = e_mov_ebx_imm_for_fixup();
        record_addr_fixup(imm_off, fi);
        return;
    }
    if (n->kind == N_ADDR_OF) {
        int off = local_find(n->name);
        if (off != 0) { e_lea_ebx_ebp(off); return; }
        int gi = global_find(n->name);
        if (gi >= 0) {
            int imm_off = e_mov_ebx_imm_for_fixup();
            record_glob_fixup(imm_off, gi);
            return;
        }
        int fi = func_find(n->name);
        if (fi < 0) fi = func_intern(n->name, -1);
        int imm_off = e_mov_ebx_imm_for_fixup();
        record_addr_fixup(imm_off, fi);
        return;
    }
    die_at(n->line, "gen_simple_into_ebx: not a simple node", 0);
}

static void gen_expr(struct node *n) {
    if (!n) { e_mov_eax_imm(0); return; }
    switch (n->kind) {
        case N_NUM: e_mov_eax_imm(n->num); return;
        case N_STR: {
            /* Session 91 — emit `mov eax, 0` as a 5-byte placeholder
             * and record a fixup to patch the imm32 with the string's
             * final VA once the pool base is known. */
            if (g_n_str_fixups >= MAX_STR_FIXUPS) die("too many string fixups");
            emit_b(0xb8);
            int imm_off = g_code_len;
            emit_d(0);
            g_str_fixups[g_n_str_fixups].code_off = imm_off;
            g_str_fixups[g_n_str_fixups].str_idx  = n->num;
            g_n_str_fixups++;
            return;
        }
        case N_NAME: {
            int off = local_find(n->name);
            if (off != 0) {
                /* Session 92 — local. Array names decay to address;
                 * everything else is a dword load through ebp. */
                int k = local_kind(n->name);
                if (kind_is_array(k)) e_lea_eax_ebp(off);
                else                  e_load_local(off);
                return;
            }
            /* Session 93 — fall through to globals. */
            int gi = global_find(n->name);
            if (gi >= 0) { emit_load_global(gi); return; }
            /* Session 98 — function name as rvalue. Decays to the
             * function's absolute VA (an address). Use a sentinel
             * n_params of -1 if we haven't seen this function yet —
             * a later definition or call site fills it in. */
            int fi = func_find(n->name);
            if (fi < 0) fi = func_intern(n->name, -1);
            emit_b(0xb8);
            int imm_off = g_code_len;
            emit_d(0);
            record_addr_fixup(imm_off, fi);
            return;
        }
        case N_ADDR_OF: {
            /* Session 92 — `&NAME`. */
            int off = local_find(n->name);
            if (off != 0) { e_lea_eax_ebp(off); return; }
            int gi = global_find(n->name);
            if (gi >= 0) { emit_addrof_global(gi); return; }
            /* Session 98 — `&func_name` also yields the function's
             * VA; identical behavior to bare name. */
            int fi = func_find(n->name);
            if (fi < 0) fi = func_intern(n->name, -1);
            emit_b(0xb8);
            int imm_off = g_code_len;
            emit_d(0);
            record_addr_fixup(imm_off, fi);
            return;
        }
        case N_DEREF: {
            /* Session 92 — `*expr`. Evaluate the pointer, then load.
             * Width depends on the pointer's kind: char* → byte,
             * everything else (int*, plain int treated as ptr) → dword.
             * Session 93 — also check the globals table for the
             * common `*p` case where p is a NAME. */
            gen_expr(n->a);
            int k = LK_INT;
            if (n->a && n->a->kind == N_NAME) {
                int lk = local_kind(n->a->name);
                if (lk >= 0) k = lk;
                else {
                    int gi = global_find(n->a->name);
                    if (gi >= 0) k = g_globals[gi].kind;
                }
            }
            if (k == LK_CHAR_PTR || k == LK_CHAR_ARR) e_loadb_eax_at_eax();
            else                                       e_load_eax_at_eax();
            return;
        }
        case N_INDEX: {
            /* Session 92/93 — `NAME[idx]`. Parser restricts the base
             * to a NAME, so we can look up its kind cheaply (local
             * first, then global). */
            int off = local_find(n->name);
            int k, is_local = (off != 0);
            int gi = -1;
            if (is_local) {
                k = local_kind(n->name);
            } else {
                gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "indexing undefined name", n->name);
                k = g_globals[gi].kind;
            }
            if (!kind_is_pointerlike(k))
                die_at(n->line, "indexing a non-pointer/non-array", n->name);
            int elem = kind_elem_size(k);
            /* base address into eax: array → lea/&global, pointer → load. */
            if (is_local) {
                if (kind_is_array(k)) e_lea_eax_ebp(off);
                else                  e_load_local(off);
            } else {
                if (kind_is_array(k)) emit_addrof_global(gi);
                else                  emit_load_global(gi);
            }
            e_push_eax();
            gen_expr(n->a);
            if (elem == 4) e_shl_eax_imm8(2);
            e_pop_ebx();
            e_add_eax_ebx();
            if (elem == 1) e_loadb_eax_at_eax();
            else           e_load_eax_at_eax();
            return;
        }
        case N_MEMBER:
        case N_ARROW: {
            /* Session 97 — `NAME.field` (member) or `NAME->field` (arrow).
             *
             * For N_MEMBER: NAME is a struct value; we LEA its address,
             *   then add the field offset, then load the field.
             * For N_ARROW: NAME is a struct pointer; we LOAD its value
             *   (which is the address of a struct), then add the field
             *   offset, then load the field. */
            int is_arrow = (n->kind == N_ARROW);
            int off = local_find(n->name);
            int k, sidx;
            int is_local = (off != 0);
            int gi = -1;
            if (is_local) {
                k = local_kind(n->name);
                sidx = local_meta(n->name);
            } else {
                gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "member of undefined", n->name);
                k = g_globals[gi].kind;
                sidx = g_globals[gi].meta;
            }
            if (is_arrow) {
                if (k != LK_STRUCT_PTR)
                    die_at(n->line, "-> requires struct pointer", n->name);
            } else {
                if (k != LK_STRUCT)
                    die_at(n->line, ". requires struct value", n->name);
            }
            int fi = struct_field_find(sidx, n->field_name);
            if (fi < 0) die_at(n->line, "no such field", n->field_name);
            int field_off  = g_structs[sidx].fields[fi].offset;
            int field_kind = g_structs[sidx].fields[fi].kind;
            (void)field_kind;
            /* Session 122 — collapse the base-address + field-offset +
             * deref chain into a single addressed load where possible.
             *
             *   N_MEMBER local:   mov eax, [ebp + off + field_off]
             *                     (was lea+add+load = 3+5+2 = 10 bytes;
             *                      now 3-6 bytes depending on disp size)
             *
             *   N_ARROW local:    mov eax, [ebp + off]      (load ptr)
             *                     mov eax, [eax + field_off] (deref+off)
             *                     (was 3+5+2 = 10 bytes; now 3 + 2-6).
             *
             * Globals are similar but field_off is folded into the
             * fixup target VA at patch time. */
            if (!is_arrow) {
                /* Member: address = base + field_off, then dereference. */
                if (is_local) {
                    /* mov eax, [ebp + off + field_off] — one instruction. */
                    int total = off + field_off;
                    e_load_local(total);
                } else {
                    /* Globals: load through [GLOBAL_VA + field_off].
                     * Encode as mov eax, [imm32]; the fixup adds
                     * `field_off` on top of the global's VA so the
                     * final imm32 is GLOBAL_VA + field_off. */
                    if (field_off == 0) {
                        emit_load_global(gi);
                    } else {
                        emit_b(0xa1);
                        int imm_off = g_code_len;
                        emit_d((unsigned)field_off);  /* base is the field_off; fixup ADDS the VA */
                        record_glob_fixup(imm_off, gi);
                    }
                }
            } else {
                /* Arrow: load pointer, then deref-with-offset. */
                if (is_local) e_load_local(off);
                else          emit_load_global(gi);
                e_load_eax_at_eax_disp(field_off);
            }
            return;
        }
        case N_INDEX2: {
            /* Session 125 — `NAME[i][j]` 2D array read.
             *
             *   addr = &NAME[0][0] + (i * M + j) * elem
             *
             * Where M is the inner dimension stored on the local_slot
             * at decl time (or as g_globals[gi].meta for globals — not
             * yet supported for globals).
             *
             *   eax := i
             *   ebx := M (constant) → eax := i*M (imul)
             *   push eax
             *   eax := j
             *   eax += stacked (i*M)
             *   ebx := elem; eax := eax * elem
             *   ebx := base addr; eax += ebx
             *   eax := *eax */
            int off = local_find(n->name);
            if (off == 0) die_at(n->line, "2D index requires a local array", n->name);
            int kk = local_kind(n->name);
            if (kk != LK_INT_ARR && kk != LK_CHAR_ARR)
                die_at(n->line, "NAME[i][j] requires a 1D-decl[..][..] array", n->name);
            /* Find dim2 directly from g_locals (local_meta is for struct meta). */
            int M = 0;
            for (int i = g_n_locals - 1; i >= 0; i--) {
                if (my_streq(g_locals[i].name, n->name)) { M = g_locals[i].dim2; break; }
            }
            if (M <= 0) die_at(n->line, "NAME[i][j] requires `int NAME[N][M]` decl", n->name);
            int elem = (kk == LK_CHAR_ARR) ? 1 : 4;
            /* i*M */
            gen_expr(n->a);
            e_mov_ebx_imm(M);
            e_imul_eax_ebx();
            e_push_eax();
            /* j */
            gen_expr(n->b);
            e_pop_ebx();             /* ebx = i*M */
            e_add_eax_ebx();         /* eax = i*M + j */
            if (elem == 4) {
                e_shl_eax_imm8(2);
            } else if (elem != 1) {
                e_mov_ebx_imm(elem);
                e_imul_eax_ebx();
            }
            /* base addr */
            e_push_eax();
            e_lea_eax_ebp(off);      /* eax = base */
            e_pop_ebx();             /* ebx = offset */
            e_add_eax_ebx();         /* eax = base + offset */
            if (elem == 1) e_loadb_eax_at_eax();
            else           e_load_eax_at_eax();
            return;
        }
        case N_INDEX_MEMBER: {
            /* Session 102 — `NAME[idx].field` for struct arrays.
             *   address-of-element = base_va + idx * struct.size
             *   address-of-field   = address-of-element + field_offset
             * The base is always a NAME of LK_STRUCT_ARR (we require
             * that — pointers to struct arrays aren't supported). */
            int off = local_find(n->name);
            int is_local = (off != 0);
            int gi = is_local ? -1 : global_find(n->name);
            if (off == 0 && gi < 0)
                die_at(n->line, "indexed-member on undefined", n->name);
            int k = is_local ? local_kind(n->name) : g_globals[gi].kind;
            int sidx = is_local ? local_meta(n->name) : g_globals[gi].meta;
            if (k != LK_STRUCT_ARR)
                die_at(n->line, "NAME[i].f requires struct array", n->name);
            int elem = g_structs[sidx].size;
            int fi = struct_field_find(sidx, n->field_name);
            if (fi < 0) die_at(n->line, "no such field", n->field_name);
            int field_off = g_structs[sidx].fields[fi].offset;

            /* base address */
            if (is_local) e_lea_eax_ebp(off);
            else          emit_addrof_global(gi);
            e_push_eax();
            /* idx → eax, scale by elem */
            gen_expr(n->a);
            if (elem == 4) {
                e_shl_eax_imm8(2);
            } else {
                e_mov_ebx_imm(elem);
                e_imul_eax_ebx();
            }
            e_pop_ebx();              /* ebx = base */
            e_add_eax_ebx();          /* eax = base + idx*elem */
            if (field_off != 0) {
                emit_b(0x05); emit_d((unsigned)field_off);  /* add eax, field_off */
            }
            e_load_eax_at_eax();
            return;
        }
        case N_CALL: gen_call(n); return;
        case N_INC_DEC: {
            /* Session 96 — ++x / --x / x++ / x--. Restriction: target
             * is a NAME (local or global).
             *
             * Session 99 — scaled for pointers: `int *p; p++` advances
             * by 4, not 1. For non-pointer kinds we still emit the
             * 1-byte inc/dec eax. Pointer kinds get a 3-byte
             * add eax, imm8 (or sub).
             *
             *   prefix:  x += d; result = x
             *   postfix: save_old = x; x += d; result = save_old
             *
             * Encoded by op=T_INC|T_DEC and num=1 (pre) / 0 (post). */
            int off = local_find(n->name);
            int gi  = (off == 0) ? global_find(n->name) : -1;
            if (off == 0 && gi < 0)
                die_at(n->line, "++/-- of undefined", n->name);
            int is_local = (off != 0);

            /* Pick the delta. For pointer-like kinds, use the pointee
             * size; otherwise 1. */
            int k    = is_local ? local_kind(n->name) : g_globals[gi].kind;
            int meta = is_local ? local_meta(n->name) : g_globals[gi].meta;
            int delta = 1;
            if (k == LK_INT_PTR || k == LK_INT_ARR) delta = 4;
            else if (k == LK_STRUCT_PTR) {
                if (meta >= 0 && meta < g_n_structs) delta = g_structs[meta].size;
                if (delta <= 0) delta = 4;
            }
            /* (char* / char[] stay at delta=1; int-scalar stays at 1.) */

            /* Load current value. */
            if (is_local) e_load_local(off);
            else          emit_load_global(gi);

            /* Modify and store. The "modify" depends on (op, delta):
             *   op=T_INC, delta=1: inc eax    (1 byte, 0x40)
             *   op=T_DEC, delta=1: dec eax    (1 byte, 0x48)
             *   op=T_INC, delta!=1: add eax, imm8  (83 c0 imm8)
             *   op=T_DEC, delta!=1: sub eax, imm8  (83 e8 imm8)
             * imm8 fits if delta <= 127, which covers every struct we'd
             * realistically see. */
            if (n->num == 0) e_push_eax();  /* postfix saves old */
            if (delta == 1) {
                emit_b((unsigned char)((n->op == T_INC) ? 0x40 : 0x48));
            } else {
                emit_b(0x83);
                emit_b((unsigned char)((n->op == T_INC) ? 0xc0 : 0xe8));
                emit_b((unsigned char)(delta & 0xff));
            }
            if (is_local) e_store_local(off);
            else          emit_store_global(gi);
            if (n->num == 0) e_pop_eax();   /* postfix restores old */
            return;
        }
        case N_TERNARY: {
            /* Session 96 — c ? a : b. Branch around the unused arm so
             * side-effects only occur for the chosen branch. */
            gen_expr(n->a);
            e_test_eax_eax();
            int jz = e_jz_rel32();
            gen_expr(n->b);
            int jend = e_jmp_rel32();
            patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
            gen_expr(n->c);
            patch_d(jend, (unsigned)(g_code_len - (jend + 4)));
            return;
        }
        case N_COMMA: {
            /* Session 125 — `a, b` evaluates a (discarding the result)
             * then evaluates b. The whole expression's value is b. */
            gen_expr(n->a);   /* side-effects only; result in eax discarded */
            gen_expr(n->b);   /* result stays in eax */
            return;
        }
        case N_ASSIGN: {
            /* Session 125 — assignment as an expression. Reuse the
             * statement-level codegen; the scalar paths leave the
             * stored value in eax (struct-assign paths don't, but
             * struct-as-expression isn't supported anyway). */
            gen_stmt(n);
            return;
        }
        case N_SIZEOF_NAME: {
            /* Session 125 — sizeof NAME, resolved at codegen because
             * the locals/globals symbol tables aren't populated until
             * gen_func runs. */
            int sz;
            int off = local_find(n->name);
            int kk, mm;
            if (off != 0) {
                kk = local_kind(n->name);
                mm = local_meta(n->name);
            } else {
                int gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "sizeof: unknown name", n->name);
                kk = g_globals[gi].kind;
                mm = g_globals[gi].meta;
            }
            if (kk == LK_STRUCT && mm >= 0 && mm < g_n_structs)
                sz = g_structs[mm].size;
            else if (kind_is_array(kk))
                die_at(n->line, "sizeof of array variable not supported (use N * sizeof(elem))", 0);
            else
                sz = 4;
            e_mov_eax_imm(sz);
            return;
        }
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
            /* Session 99 — scaled pointer arithmetic.
             *
             * For `p + n` / `n + p` / `p - n` where p is a pointer-kind
             * variable (int*, char*, struct*, array), scale n by the
             * pointee's size before combining. The scaling matches what
             * real C does: `int *p; p+1` advances by 4 bytes, not 1.
             *
             * Pointer-minus-pointer (p - p) intentionally falls through
             * unchanged — that's a raw byte distance, the user is
             * responsible for dividing by sizeof(*p) if they want
             * element distance.
             *
             * Only NAME operands are inspected for pointer-ness. Other
             * expressions (e.g. `(p + 1) + 1`) aren't recognized and
             * fall through to integer arithmetic. Documented limit. */
            if (n->op == T_PLUS || n->op == T_MINUS) {
                int ea = expr_ptr_elem_size(n->a);
                int eb = expr_ptr_elem_size(n->b);
                struct node *ptr_node = 0;
                struct node *idx_node = 0;
                int elem = 0;
                if (ea > 0 && eb == 0) {
                    ptr_node = n->a; idx_node = n->b; elem = ea;
                } else if (eb > 0 && ea == 0 && n->op == T_PLUS) {
                    /* n + p — only legal for +. */
                    ptr_node = n->b; idx_node = n->a; elem = eb;
                }
                if (elem > 1) {
                    gen_expr(idx_node);
                    if (elem == 4) {
                        e_shl_eax_imm8(2);
                    } else {
                        e_mov_ebx_imm(elem);
                        e_imul_eax_ebx();
                    }
                    e_push_eax();
                    gen_expr(ptr_node);
                    e_pop_ebx();
                    if (n->op == T_PLUS) e_add_eax_ebx();
                    else                 e_sub_eax_ebx();
                    return;
                }
            }
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
             * so that the op operates on (eax = LHS, ebx = RHS).
             *
             * Session 122 — register-allocator fast paths:
             *
             *   (a) RHS is simple (NUM/STR/NAME/ADDR_OF): load directly
             *       into EBX after computing LHS in EAX. Saves a
             *       push+pop pair (2 bytes) per binop.
             *   (b) LHS is simple AND RHS would otherwise need push/pop:
             *       compute RHS first into EAX, move to EBX, then load
             *       LHS into EAX. Saves the same 2 bytes.
             *
             * Both paths require neither side to clobber the other; the
             * simple-load helpers guarantee they don't touch the
             * non-target register, so the guarantee follows from
             * is_simple_load. */
            if (is_simple_load(n->b)) {
                gen_expr(n->a);
                gen_simple_into_ebx(n->b);
            } else if (is_simple_load(n->a)) {
                gen_expr(n->b);
                e_mov_ebx_eax();
                /* Load LHS into EAX without disturbing EBX. The
                 * existing N_NUM/N_NAME/N_ADDR_OF/N_STR codegen for the
                 * EAX side doesn't touch EBX — that's by inspection
                 * of the N_NAME, N_NUM, N_STR, N_ADDR_OF cases above. */
                gen_expr(n->a);
            } else {
                gen_expr(n->a);
                e_push_eax();
                gen_expr(n->b);
                e_mov_ebx_eax();
                e_pop_eax();
            }
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

/* Session 125 — goto / label table. Per-function: reset at the top of
 * gen_func, resolved at the bottom. Forward and backward gotos both
 * use the fixup list (we don't know which until end of function).
 *
 * Labels live in the same flat namespace as variable names — that's
 * standard C: a label can shadow a function-scope name. cc doesn't
 * try to be strict about this; if your label collides with a typedef
 * or function name, the goto codegen will use the label and the rest
 * of the program won't notice. */
#define MAX_LABELS_PER_FUNC 32
#define MAX_GOTOS_PER_FUNC  64
struct label_info {
    char name[NAME_MAX];
    int  code_off;   /* -1 if declared but not yet defined */
};
struct goto_fixup {
    char name[NAME_MAX];
    int  jmp_disp_off;
    int  line;
};
static struct label_info g_labels[MAX_LABELS_PER_FUNC];
static int               g_n_labels;
static struct goto_fixup g_gotos[MAX_GOTOS_PER_FUNC];
static int               g_n_gotos;

static int label_find_or_add(const char *name) {
    for (int i = 0; i < g_n_labels; i++)
        if (my_streq(g_labels[i].name, name)) return i;
    if (g_n_labels >= MAX_LABELS_PER_FUNC) die("too many labels in function");
    int idx = g_n_labels++;
    int j = 0;
    while (name[j] && j < NAME_MAX - 1) { g_labels[idx].name[j] = name[j]; j++; }
    g_labels[idx].name[j] = 0;
    g_labels[idx].code_off = -1;
    return idx;
}

/* Session 125 — loop context stack for break/continue codegen.
 *
 * Each enclosing while/do-while/for/switch pushes a loop_ctx so
 * break/continue inside the body can record patch-later jump
 * placeholders. Break is always a forward jump (to after the loop);
 * continue is a backward jump for while (to top) or a forward jump
 * for do-while (to the cond eval at the bottom).
 *
 * Switch (added in feature 9/11) uses break too — N_SWITCH pushes
 * a loop ctx with cont_target_known = 0 so continue can't be used
 * inside a bare switch (matches C semantics). */
#define MAX_LOOP_CTX        16
#define MAX_BREAKS_PER_LOOP 32
struct loop_ctx {
    int  break_jmps[MAX_BREAKS_PER_LOOP];
    int  n_break_jmps;
    int  cont_jmps[MAX_BREAKS_PER_LOOP];
    int  n_cont_jmps;
    int  cont_target;            /* absolute code_off; -1 if not yet known */
    int  is_switch;              /* 1 = switch, 0 = real loop. switch only handles break. */
};
static struct loop_ctx g_loop_ctx[MAX_LOOP_CTX];
static int             g_n_loop_ctx;

static struct loop_ctx *loop_push(int cont_target, int is_switch) {
    if (g_n_loop_ctx >= MAX_LOOP_CTX) die("loop nesting too deep");
    struct loop_ctx *c = &g_loop_ctx[g_n_loop_ctx++];
    c->n_break_jmps  = 0;
    c->n_cont_jmps   = 0;
    c->cont_target   = cont_target;
    c->is_switch     = is_switch;
    return c;
}

static void loop_pop(void) {
    if (g_n_loop_ctx <= 0) die("loop_pop with empty stack");
    g_n_loop_ctx--;
}

static struct loop_ctx *loop_top(void) {
    return g_n_loop_ctx > 0 ? &g_loop_ctx[g_n_loop_ctx - 1] : 0;
}

/* Find the nearest enclosing loop_ctx that accepts continue. switch
 * frames are skipped (continue inside `switch (...) { while (...) ...
 * continue; ... }` belongs to the while, not the switch). */
static struct loop_ctx *loop_top_for_continue(void) {
    for (int i = g_n_loop_ctx - 1; i >= 0; i--) {
        if (!g_loop_ctx[i].is_switch) return &g_loop_ctx[i];
    }
    return 0;
}

static void gen_stmt(struct node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_BLOCK: {
            /* Block scoping: locals declared inside the block stop being
             * NAME-visible when we leave, but their stack space stays
             * reserved by the function prologue. g_locals_bytes is the
             * watermark used to size `sub esp, N` — never roll it back.
             *
             * Session 122 — DCE: stop emitting at the first `return` in
             * a block. Anything after is unreachable. */
            int saved_locals = g_n_locals;
            for (int i = 0; i < n->n_list; i++) {
                gen_stmt(n->list[i]);
                if (n->list[i] && n->list[i]->kind == N_RETURN) break;
            }
            g_n_locals = saved_locals;
            return;
        }
        case N_VAR_DECL: {
            /* Session 92 — kind is in n->op: LK_INT / LK_INT_PTR /
             * LK_CHAR_PTR. All three reserve 4 bytes. */
            local_declare_sized(n->name, 4, n->op ? n->op : LK_INT);
            int off = g_locals[g_n_locals - 1].ebp_off;
            if (n->a) {
                gen_expr(n->a);
                e_store_local(off);
            }
            return;
        }
        case N_ARR_DECL: {
            /* Session 92 — `TYPE NAME[N];`. Reserve elem_size * N
             * bytes; the local's ebp_off points to the LOW byte of
             * the array (lowest address = element 0).
             *
             * Session 125 — also handles `TYPE NAME[N][M];` (2D). The
             * inner dim M is in n->n_list; if non-zero, total bytes is
             * N * M * elem_size, and dim2 is recorded on the local_slot
             * so N_INDEX2 can compute (i * M + j) * elem. */
            int kind = n->op;     /* LK_INT_ARR or LK_CHAR_ARR */
            int elem = (kind == LK_CHAR_ARR) ? 1 : 4;
            int n_elements = (n->n_list > 0) ? (n->num * n->n_list) : n->num;
            int bytes = elem * n_elements;
            local_declare_sized(n->name, bytes, kind);
            if (n->n_list > 0) {
                g_locals[g_n_locals - 1].dim2 = n->n_list;
            }
            return;
        }
        case N_STRUCT_DECL: {
            /* Session 97/102 — `struct T NAME;`, `struct T *NAME;`,
             * or `struct T NAME[N];`. */
            int kind = n->op;     /* LK_STRUCT / LK_STRUCT_PTR / LK_STRUCT_ARR */
            int sidx = n->num;
            int size;
            if      (kind == LK_STRUCT_PTR) size = 4;
            else if (kind == LK_STRUCT_ARR) size = g_structs[sidx].size * n->n_list;
            else                            size = g_structs[sidx].size;
            if (size <= 0) die_at(n->line, "struct size is zero", n->name);
            local_declare_struct(n->name, size, kind, sidx);
            return;
        }
        case N_ASSIGN: {
            int off = local_find(n->name);
            int is_local = (off != 0);
            int gi = is_local ? -1 : global_find(n->name);
            int k, sidx = -1;
            if (is_local) {
                k = local_kind(n->name); sidx = local_meta(n->name);
            } else if (gi >= 0) {
                k = g_globals[gi].kind; sidx = g_globals[gi].meta;
            } else {
                die_at(n->line, "undefined variable", n->name);
                k = LK_INT;
            }
            if (kind_is_array(k))
                die_at(n->line, "can't assign to whole array", n->name);

            /* Session 101 — struct value assignment. Both sides must
             * be NAMEs of the same struct kind. Emit a memcpy via
             * rep movsd. Restriction documented: RHS must be a plain
             * struct-name; `p = *q` or `p = func_returning_struct()`
             * aren't supported (we don't have struct-by-value calls).
             *
             * Session 121: also accepts `lhs = FUNC(...)` where FUNC
             * returns a struct by value. In that case we route through
             * the hidden-dest-pointer ABI: push &lhs as the hidden
             * first arg, push the rest of the args, call, clean up. */
            if (k == LK_STRUCT) {
                struct node *rhs = n->a;
                /* Session 121 — struct-returning call as RHS. */
                if (rhs && rhs->kind == N_CALL && !is_intrinsic(rhs->name)) {
                    int fi = func_find(rhs->name);
                    if (fi >= 0 && g_funcs[fi].ret_kind == LK_STRUCT) {
                        if (g_funcs[fi].ret_meta != sidx)
                            die_at(n->line,
                                   "SBV call return type doesn't match lhs", n->name);
                        int total_push = push_call_args(rhs, fi);
                        /* Push hidden dest pointer = &lhs. */
                        if (is_local) e_lea_eax_ebp(off);
                        else          emit_addrof_global(gi);
                        e_push_eax();
                        total_push += 4;
                        int disp_off = e_call_rel32();
                        if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
                        g_fixups[g_n_fixups].call_disp_off = disp_off;
                        g_fixups[g_n_fixups].func_idx = fi;
                        g_n_fixups++;
                        if (total_push > 0) e_add_esp_imm32(total_push);
                        return;
                    }
                }
                if (!rhs || rhs->kind != N_NAME)
                    die_at(n->line, "struct = must be struct-NAME = struct-NAME or = SBV_call", n->name);
                int r_off = local_find(rhs->name);
                int r_is_local = (r_off != 0);
                int r_gi = r_is_local ? -1 : global_find(rhs->name);
                int r_k = -1, r_sidx = -1;
                if (r_is_local) {
                    r_k = local_kind(rhs->name); r_sidx = local_meta(rhs->name);
                } else if (r_gi >= 0) {
                    r_k = g_globals[r_gi].kind; r_sidx = g_globals[r_gi].meta;
                }
                if (r_k != LK_STRUCT || r_sidx != sidx)
                    die_at(n->line, "struct = type mismatch", n->name);

                int sz = g_structs[sidx].size;
                int dwords = sz / 4;     /* size is field_count * 4, divisible */

                /* Preserve esi/edi (callee-saved in cdecl). */
                emit_b(0x56);                  /* push esi */
                emit_b(0x57);                  /* push edi */
                /* lea esi, [src]. */
                if (r_is_local) {
                    e_lea_eax_ebp(r_off);
                    emit_b(0x89); emit_b(0xc6);      /* mov esi, eax */
                } else {
                    emit_b(0xbe);                    /* mov esi, imm32 */
                    int off_imm = g_code_len; emit_d(0);
                    record_glob_fixup(off_imm, r_gi);
                }
                /* lea edi, [dst]. */
                if (is_local) {
                    e_lea_eax_ebp(off);
                    emit_b(0x89); emit_b(0xc7);      /* mov edi, eax */
                } else {
                    emit_b(0xbf);                    /* mov edi, imm32 */
                    int off_imm = g_code_len; emit_d(0);
                    record_glob_fixup(off_imm, gi);
                }
                /* mov ecx, dwords */
                emit_b(0xb9); emit_d((unsigned)dwords);
                /* rep movsd  →  f3 a5 */
                emit_b(0xf3); emit_b(0xa5);
                emit_b(0x5f);                  /* pop edi */
                emit_b(0x5e);                  /* pop esi */
                return;
            }

            /* Default scalar path. */
            gen_expr(n->a);
            if (is_local) e_store_local(off);
            else          emit_store_global(gi);
            return;
        }
        case N_DEREF_ASSIGN: {
            /* Session 92/93 — `*NAME = expr;`. NAME can be local or
             * global; load its pointer value, then byte/dword-store
             * `expr` at that address. */
            int off = local_find(n->name);
            int k, is_local = (off != 0);
            int gi = -1;
            if (is_local) {
                k = local_kind(n->name);
            } else {
                gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "* of undefined variable", n->name);
                k = g_globals[gi].kind;
            }
            if (!kind_is_pointerlike(k))
                die_at(n->line, "* applied to non-pointer", n->name);
            int elem = kind_elem_size(k);
            /* val first → eax → push. */
            gen_expr(n->a);
            e_push_eax();
            /* address: load (pointer) or lea (array). */
            if (is_local) {
                if (kind_is_array(k)) e_lea_eax_ebp(off);
                else                  e_load_local(off);
            } else {
                if (kind_is_array(k)) emit_addrof_global(gi);
                else                  emit_load_global(gi);
            }
            e_mov_ebx_eax();
            e_pop_eax();
            if (elem == 1) e_storeb_al_at_ebx();
            else           e_store_eax_at_ebx();
            return;
        }
        case N_INDEX_ASSIGN: {
            /* Session 92/93 — `NAME[idx] = val;`. */
            int off = local_find(n->name);
            int k, is_local = (off != 0);
            int gi = -1;
            if (is_local) {
                k = local_kind(n->name);
            } else {
                gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "indexing undefined name", n->name);
                k = g_globals[gi].kind;
            }
            if (!kind_is_pointerlike(k))
                die_at(n->line, "indexing a non-pointer/non-array", n->name);
            int elem = kind_elem_size(k);
            /* val first. */
            gen_expr(n->b);
            e_push_eax();
            /* base address. */
            if (is_local) {
                if (kind_is_array(k)) e_lea_eax_ebp(off);
                else                  e_load_local(off);
            } else {
                if (kind_is_array(k)) emit_addrof_global(gi);
                else                  emit_load_global(gi);
            }
            e_push_eax();
            gen_expr(n->a);
            if (elem == 4) e_shl_eax_imm8(2);
            e_pop_ebx();
            e_add_eax_ebx();
            e_mov_ebx_eax();
            e_pop_eax();
            if (elem == 1) e_storeb_al_at_ebx();
            else           e_store_eax_at_ebx();
            return;
        }
        case N_MEMBER_ASSIGN:
        case N_ARROW_ASSIGN: {
            /* Session 97 — `NAME.field = expr;` or `NAME->field = expr;`.
             * Compute the rhs into eax, push, then compute the
             * destination address (struct base + field offset), pop
             * rhs into eax, store. */
            int is_arrow = (n->kind == N_ARROW_ASSIGN);
            int off = local_find(n->name);
            int k, sidx;
            int is_local = (off != 0);
            int gi = -1;
            if (is_local) {
                k = local_kind(n->name);
                sidx = local_meta(n->name);
            } else {
                gi = global_find(n->name);
                if (gi < 0) die_at(n->line, "member-assign undefined", n->name);
                k = g_globals[gi].kind;
                sidx = g_globals[gi].meta;
            }
            if (is_arrow) {
                if (k != LK_STRUCT_PTR)
                    die_at(n->line, "-> requires struct pointer", n->name);
            } else {
                if (k != LK_STRUCT)
                    die_at(n->line, ". requires struct value", n->name);
            }
            int fi = struct_field_find(sidx, n->field_name);
            if (fi < 0) die_at(n->line, "no such field", n->field_name);
            int field_off = g_structs[sidx].fields[fi].offset;

            /* Session 122 — fast paths that collapse the addressed-store
             * into a single instruction.
             *
             *   N_MEMBER_ASSIGN local:   mov [ebp + off + field_off], eax
             *   N_MEMBER_ASSIGN global:  mov [GLOBAL_VA + field_off], eax
             *
             * For N_ARROW_ASSIGN we still need EBX to hold the pointer,
             * so we keep the existing push/pop path — RHS could clobber
             * EAX freely. */
            if (!is_arrow) {
                gen_expr(n->a);          /* eax = rhs */
                if (is_local) {
                    e_store_local(off + field_off);
                } else {
                    /* mov [imm32], eax with fixup that adds field_off. */
                    emit_b(0xa3);
                    int imm_off = g_code_len;
                    emit_d((unsigned)field_off);
                    record_glob_fixup(imm_off, gi);
                }
                return;
            }
            /* Arrow path — keep original push/pop until we have an
             * EBX-disp store helper. */
            gen_expr(n->a);          /* eax = rhs */
            e_push_eax();
            /* Compute destination address into eax. */
            if (is_local) e_load_local(off);
            else          emit_load_global(gi);
            if (field_off != 0) {
                emit_b(0x05); emit_d((unsigned)field_off);
            }
            e_mov_ebx_eax();
            e_pop_eax();
            e_store_eax_at_ebx();
            return;
        }
        case N_INDEX2_ASSIGN: {
            /* Session 125 — `NAME[i][j] = expr;` 2D store. Mirrors the
             * N_INDEX2 read path: compute (i*M + j)*elem + base, then
             * store EAX at that address. Value is in n->c. */
            int off = local_find(n->name);
            if (off == 0) die_at(n->line, "2D store requires a local array", n->name);
            int kk = local_kind(n->name);
            if (kk != LK_INT_ARR && kk != LK_CHAR_ARR)
                die_at(n->line, "NAME[i][j]= requires a 2D array", n->name);
            int M = 0;
            for (int i = g_n_locals - 1; i >= 0; i--) {
                if (my_streq(g_locals[i].name, n->name)) { M = g_locals[i].dim2; break; }
            }
            if (M <= 0) die_at(n->line, "NAME[i][j]= requires 2D decl", n->name);
            int elem = (kk == LK_CHAR_ARR) ? 1 : 4;
            /* val → eax → push */
            gen_expr(n->c);
            e_push_eax();
            /* i*M */
            gen_expr(n->a);
            e_mov_ebx_imm(M);
            e_imul_eax_ebx();
            e_push_eax();
            /* j */
            gen_expr(n->b);
            e_pop_ebx();
            e_add_eax_ebx();            /* eax = i*M + j */
            if (elem == 4) {
                e_shl_eax_imm8(2);
            } else if (elem != 1) {
                e_mov_ebx_imm(elem);
                e_imul_eax_ebx();
            }
            e_push_eax();
            e_lea_eax_ebp(off);         /* eax = base */
            e_pop_ebx();                /* ebx = offset */
            e_add_eax_ebx();            /* eax = addr */
            e_mov_ebx_eax();            /* ebx = addr */
            e_pop_eax();                /* eax = val */
            if (elem == 1) e_storeb_al_at_ebx();
            else           e_store_eax_at_ebx();
            return;
        }
        case N_INDEX_MEMBER_ASSIGN: {
            /* Session 102 — `NAME[i].field = expr;` for struct arrays. */
            int off = local_find(n->name);
            int is_local = (off != 0);
            int gi = is_local ? -1 : global_find(n->name);
            if (off == 0 && gi < 0)
                die_at(n->line, "indexed-member-assign undefined", n->name);
            int k = is_local ? local_kind(n->name) : g_globals[gi].kind;
            int sidx = is_local ? local_meta(n->name) : g_globals[gi].meta;
            if (k != LK_STRUCT_ARR)
                die_at(n->line, "NAME[i].f = requires struct array", n->name);
            int elem = g_structs[sidx].size;
            int fi = struct_field_find(sidx, n->field_name);
            if (fi < 0) die_at(n->line, "no such field", n->field_name);
            int field_off = g_structs[sidx].fields[fi].offset;

            /* rhs → eax → push */
            gen_expr(n->b);
            e_push_eax();
            /* base address → push */
            if (is_local) e_lea_eax_ebp(off);
            else          emit_addrof_global(gi);
            e_push_eax();
            /* idx → eax, scale by elem */
            gen_expr(n->a);
            if (elem == 4) {
                e_shl_eax_imm8(2);
            } else {
                e_mov_ebx_imm(elem);
                e_imul_eax_ebx();
            }
            e_pop_ebx();
            e_add_eax_ebx();          /* eax = base + idx*elem */
            if (field_off != 0) {
                emit_b(0x05); emit_d((unsigned)field_off);  /* add eax, field_off */
            }
            e_mov_ebx_eax();
            e_pop_eax();
            e_store_eax_at_ebx();
            return;
        }
        case N_RETURN: {
            /* Session 121 — struct-by-value-return path. The callee
             * received a hidden destination pointer at [ebp+8]; we
             * memcpy the source struct into that slot, then return
             * the pointer (some callers chain on it; harmless if not).
             *
             * Supported RHS shapes for SBV returns:
             *   return STRUCT_LOCAL_NAME;
             *   return FUNC(...);     // forward another SBV call into
             *                          // OUR caller's dest slot
             *
             * `return;` (no expression) in an SBV function is a bug;
             * we tolerate it by leaving the dest untouched. */
            if (g_cur_ret_kind == LK_STRUCT) {
                if (!n->a) {
                    /* No expression. Just return whatever's in the
                     * dest pointer slot (the slot's contents are
                     * undefined; this is documented as a user bug). */
                    e_load_local(8);
                    e_mov_esp_ebp();
                    e_pop_ebp();
                    e_ret();
                    return;
                }
                /* `return FUNC(...);` where FUNC also returns SBV of
                 * the same struct — forward our hidden dest pointer
                 * directly. Avoids an intermediate copy. */
                if (n->a->kind == N_CALL && !is_intrinsic(n->a->name)) {
                    int fi = func_find(n->a->name);
                    if (fi >= 0 && g_funcs[fi].ret_kind == LK_STRUCT
                                && g_funcs[fi].ret_meta == g_cur_ret_meta) {
                        int total_push = push_call_args(n->a, fi);
                        e_load_local(8);          /* eax = our hidden dest */
                        e_push_eax();
                        total_push += 4;
                        int disp_off = e_call_rel32();
                        if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
                        g_fixups[g_n_fixups].call_disp_off = disp_off;
                        g_fixups[g_n_fixups].func_idx = fi;
                        g_n_fixups++;
                        if (total_push > 0) e_add_esp_imm32(total_push);
                        e_load_local(8);
                        e_mov_esp_ebp();
                        e_pop_ebp();
                        e_ret();
                        return;
                    }
                }
                /* `return STRUCT_LOCAL_NAME;` — memcpy the named struct
                 * into [ebp+8]. (Globals as the source aren't supported
                 * here for the same reason struct-by-value args from
                 * globals aren't — would need an extra address-load
                 * branch. Documented limit.) */
                if (n->a->kind != N_NAME)
                    die_at(n->line,
                           "SBV return: RHS must be a struct NAME or a "
                           "matching-struct-returning call", 0);
                int r_off = local_find(n->a->name);
                if (r_off == 0)
                    die_at(n->line, "SBV return: undefined or non-local", n->a->name);
                int r_k = local_kind(n->a->name);
                int r_sidx = local_meta(n->a->name);
                if (r_k != LK_STRUCT || r_sidx != g_cur_ret_meta)
                    die_at(n->line, "SBV return: struct type mismatch", n->a->name);
                int sz = g_structs[r_sidx].size;
                int dwords = sz / 4;
                /* push esi; push edi; copy from [ebp+r_off] to [[ebp+8]] */
                emit_b(0x56); emit_b(0x57);         /* push esi; push edi */
                e_load_local(8);                    /* eax = hidden dest ptr */
                emit_b(0x89); emit_b(0xc7);         /* mov edi, eax */
                e_lea_eax_ebp(r_off);
                emit_b(0x89); emit_b(0xc6);         /* mov esi, eax */
                emit_b(0xb9); emit_d((unsigned)dwords);  /* mov ecx, dwords */
                emit_b(0xf3); emit_b(0xa5);         /* rep movsd */
                emit_b(0x5f); emit_b(0x5e);         /* pop edi; pop esi */
                e_load_local(8);                    /* return dest pointer */
                e_mov_esp_ebp();
                e_pop_ebp();
                e_ret();
                return;
            }
            /* Scalar return path. */
            if (n->a) gen_expr(n->a);
            else      e_mov_eax_imm(0);
            e_mov_esp_ebp();
            e_pop_ebp();
            e_ret();
            return;
        }
        case N_IF: {
            /* Session 122 — DCE: if the condition was constant-folded
             * to a literal, only emit the taken branch. Skips both the
             * conditional emit + jz pair AND the dead branch's body. */
            if (n->a && n->a->kind == N_NUM) {
                if (n->a->num) {
                    /* Then-branch is live. */
                    gen_stmt(n->b);
                } else if (n->c) {
                    /* Else-branch is live. */
                    gen_stmt(n->c);
                }
                return;
            }
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
            /* Session 122 — DCE: `while (0)` emits nothing. `while (1)`
             * (or any nonzero const) emits the body in a tight infinite
             * loop without the conditional test. */
            if (n->a && n->a->kind == N_NUM) {
                if (n->a->num == 0) return;
                int top = g_code_len;
                gen_stmt(n->b);
                int jback = e_jmp_rel32();
                patch_d(jback, (unsigned)(top - (jback + 4)));
                return;
            }
            int top = g_code_len;
            gen_expr(n->a);
            e_test_eax_eax();
            int jz = e_jz_rel32();
            /* Session 125 — push loop ctx so break/continue inside the
             * body find the right targets. Continue jumps back to
             * `top` (cond eval). Break jumps forward to the end. */
            struct loop_ctx *lc = loop_push(top, 0);
            gen_stmt(n->b);
            /* Patch any continue jumps to the top (cond eval). */
            for (int i = 0; i < lc->n_cont_jmps; i++)
                patch_d(lc->cont_jmps[i], (unsigned)(top - (lc->cont_jmps[i] + 4)));
            int jback = e_jmp_rel32();
            patch_d(jback, (unsigned)(top - (jback + 4)));
            patch_d(jz, (unsigned)(g_code_len - (jz + 4)));
            /* Patch break jumps to the end of the loop. */
            for (int i = 0; i < lc->n_break_jmps; i++)
                patch_d(lc->break_jmps[i], (unsigned)(g_code_len - (lc->break_jmps[i] + 4)));
            loop_pop();
            return;
        }
        case N_DO_WHILE: {
            /* Session 125 — `do { body } while (cond);` runs the body
             * at least once, then loops back if the condition is true.
             * Body in n->a, cond in n->b.
             *
             *   top:
             *     body
             *     cond_eval:        <- continue lands here
             *       cond → eax
             *       test eax, eax
             *       jnz top
             *     end:              <- break lands here
             *
             * cont_target is initially -1; patched to cond_eval after
             * body emit. */
            int top = g_code_len;
            struct loop_ctx *lc = loop_push(-1, 0);
            gen_stmt(n->a);
            int cond_pos = g_code_len;
            /* Patch continue jumps to point at cond eval. */
            for (int i = 0; i < lc->n_cont_jmps; i++)
                patch_d(lc->cont_jmps[i], (unsigned)(cond_pos - (lc->cont_jmps[i] + 4)));
            gen_expr(n->b);
            e_test_eax_eax();
            int jnz = e_jnz_rel32();
            patch_d(jnz, (unsigned)(top - (jnz + 4)));
            /* Patch break jumps to end. */
            for (int i = 0; i < lc->n_break_jmps; i++)
                patch_d(lc->break_jmps[i], (unsigned)(g_code_len - (lc->break_jmps[i] + 4)));
            loop_pop();
            return;
        }
        case N_BREAK: {
            /* Find nearest enclosing loop or switch. */
            struct loop_ctx *lc = loop_top();
            if (!lc) die_at(n->line, "break outside loop/switch", 0);
            if (lc->n_break_jmps >= MAX_BREAKS_PER_LOOP)
                die_at(n->line, "too many breaks in one loop", 0);
            int jb = e_jmp_rel32();
            lc->break_jmps[lc->n_break_jmps++] = jb;
            return;
        }
        case N_LABEL: {
            /* Session 125 — record label position then emit body. */
            int li = label_find_or_add(n->name);
            if (g_labels[li].code_off >= 0)
                die_at(n->line, "duplicate label", n->name);
            g_labels[li].code_off = g_code_len;
            gen_stmt(n->body);
            return;
        }
        case N_SWITCH: {
            /* Session 125 — switch (expr) { case N: ... default: ... }.
             *
             * Codegen:
             *   1. Evaluate the switch expression into EAX.
             *   2. Emit a dispatch chain: cmp eax, N; je case_N_target
             *      for each non-default case. The case_target offsets
             *      are recorded as we walk the body.
             *   3. After the dispatch chain, emit one unconditional jmp:
             *      to `default:` if present, else to end of switch.
             *   4. Walk the body — each N_CASE / N_DEFAULT records its
             *      g_code_len position; everything else gen_stmts.
             *   5. Patch the dispatch chain and break jumps.
             *
             * Restrictions: case/default labels must be top-level in
             * the switch body (no nested cases inside if/while inside
             * the switch). Case values must be integer literals (no
             * compile-time folding of expressions in this branch).
             *
             * Fall-through between cases works because we don't insert
             * any jumps between case targets — users add `break;` to
             * end a case explicitly. */
            if (!n->b || n->b->kind != N_BLOCK)
                die_at(n->line, "switch body must be a block", 0);

            gen_expr(n->a);                     /* eax = switch value */

            #define MAX_CASES_PER_SWITCH 32
            int    case_vals[MAX_CASES_PER_SWITCH];
            int    case_is_default[MAX_CASES_PER_SWITCH];
            int    case_targets[MAX_CASES_PER_SWITCH];
            int    dispatch_jmps[MAX_CASES_PER_SWITCH];
            int    n_cases = 0;
            int    default_idx = -1;

            /* Pre-scan top-level body stmts to find cases / default. */
            for (int i = 0; i < n->b->n_list; i++) {
                struct node *s = n->b->list[i];
                if (!s) continue;
                if (s->kind == N_CASE) {
                    if (n_cases >= MAX_CASES_PER_SWITCH) die_at(s->line, "too many cases", 0);
                    case_vals[n_cases]       = s->num;
                    case_is_default[n_cases] = 0;
                    n_cases++;
                } else if (s->kind == N_DEFAULT) {
                    if (n_cases >= MAX_CASES_PER_SWITCH) die_at(s->line, "too many cases", 0);
                    case_is_default[n_cases] = 1;
                    default_idx = n_cases;
                    n_cases++;
                }
            }

            /* Emit dispatch chain. */
            for (int i = 0; i < n_cases; i++) {
                if (case_is_default[i]) continue;
                /* cmp eax, imm32  →  3d imm32 */
                emit_b(0x3d); emit_d((unsigned)case_vals[i]);
                /* je rel32  →  0f 84 imm32 (same as jz) */
                dispatch_jmps[i] = e_jz_rel32();
            }
            /* Fall-through jmp: to default if present, else to end. */
            int default_jmp = e_jmp_rel32();

            /* Push loop ctx (is_switch=1) so break works, continue
             * passes through to any enclosing loop. */
            struct loop_ctx *lc = loop_push(-1, 1);

            /* Walk the body — record case-label positions and emit
             * everything else. */
            int case_idx = 0;
            for (int i = 0; i < n->b->n_list; i++) {
                struct node *s = n->b->list[i];
                if (!s) continue;
                if (s->kind == N_CASE || s->kind == N_DEFAULT) {
                    case_targets[case_idx++] = g_code_len;
                    /* No body — case labels are just position markers.
                     * Fall-through to the next non-label stmt. */
                } else {
                    gen_stmt(s);
                }
            }

            /* Patch each non-default dispatch jmp. */
            for (int i = 0; i < n_cases; i++) {
                if (case_is_default[i]) continue;
                patch_d(dispatch_jmps[i],
                        (unsigned)(case_targets[i] - (dispatch_jmps[i] + 4)));
            }
            /* Patch the default / end jmp. */
            if (default_idx >= 0) {
                patch_d(default_jmp,
                        (unsigned)(case_targets[default_idx] - (default_jmp + 4)));
            } else {
                patch_d(default_jmp,
                        (unsigned)(g_code_len - (default_jmp + 4)));
            }
            /* Patch break jumps to end. */
            for (int i = 0; i < lc->n_break_jmps; i++)
                patch_d(lc->break_jmps[i],
                        (unsigned)(g_code_len - (lc->break_jmps[i] + 4)));
            loop_pop();
            return;
        }
        case N_CASE:
        case N_DEFAULT:
            /* Only reachable as a top-level switch-body stmt; otherwise
             * an error. The N_SWITCH codegen handles them inline. */
            die_at(n->line, "case/default outside switch", 0);
            return;
        case N_GOTO: {
            /* Session 125 — emit forward jmp placeholder, record fixup
             * for resolution at end of gen_func. */
            if (g_n_gotos >= MAX_GOTOS_PER_FUNC) die("too many gotos in function");
            int j = e_jmp_rel32();
            int idx = g_n_gotos++;
            int i = 0;
            while (n->name[i] && i < NAME_MAX - 1) {
                g_gotos[idx].name[i] = n->name[i]; i++;
            }
            g_gotos[idx].name[i] = 0;
            g_gotos[idx].jmp_disp_off = j;
            g_gotos[idx].line = n->line;
            return;
        }
        case N_CONTINUE: {
            /* Find nearest enclosing real loop (skip switch frames). */
            struct loop_ctx *lc = loop_top_for_continue();
            if (!lc) die_at(n->line, "continue outside loop", 0);
            if (lc->cont_target >= 0) {
                /* Backward continue (while) — patch immediately. */
                int jc = e_jmp_rel32();
                patch_d(jc, (unsigned)(lc->cont_target - (jc + 4)));
            } else {
                /* Forward continue (do-while) — record for patch later. */
                if (lc->n_cont_jmps >= MAX_BREAKS_PER_LOOP)
                    die_at(n->line, "too many continues in one loop", 0);
                int jc = e_jmp_rel32();
                lc->cont_jmps[lc->n_cont_jmps++] = jc;
            }
            return;
        }
        case N_EXPR_STMT:
            gen_expr(n->a);
            return;
    }
    die_at(n->line, "unsupported stmt kind", 0);
}

static void gen_func(struct node *fn) {
    /* Session 105 — register variadic-ness BEFORE func_intern so the
     * intern's argc-check sees the correct flag. */
    int pre_idx = func_find(fn->name);
    if (pre_idx >= 0) g_funcs[pre_idx].is_variadic = fn->op ? 1 : 0;
    int idx = func_intern(fn->name, fn->n_params);
    g_funcs[idx].is_variadic = fn->op ? 1 : 0;
    /* Session 121 — propagate ret_kind so gen_stmt(N_RETURN) and any
     * gen_call seeing this function knows the ABI. */
    g_funcs[idx].ret_kind = fn->ret_kind;
    g_funcs[idx].ret_meta = fn->ret_meta;
    g_cur_ret_kind = fn->ret_kind;
    g_cur_ret_meta = fn->ret_meta;
    /* Session 100 — multi-file compilation can produce duplicate
     * function definitions if a user puts the same function body in
     * two source files. Catch it here. (For single-file builds this
     * never triggers because the parser would already have errored
     * at the second `int foo(...) { ... }`.) */
    if (g_funcs[idx].defined)
        die_at(fn->line, "duplicate function definition", fn->name);
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    /* Reset per-function state. */
    g_n_locals = 0;
    g_locals_bytes = 0;

    /* Prologue: push ebp; mov ebp, esp.
     * Params live at [ebp+8], [ebp+12], ... (after saved ebp + return addr).
     *
     * Session 121 — struct-by-value-returning functions receive a HIDDEN
     * first arg at [ebp+8] that points to the caller-allocated return
     * slot. Real params start at [ebp+12]. The hidden ptr is not named
     * — N_RETURN-time codegen loads it directly via e_load_local(8). */
    e_push_ebp();
    e_mov_ebp_esp();

    /* Session 125 — reset per-function goto + label tables. */
    g_n_labels = 0;
    g_n_gotos  = 0;

    /* Bind parameters as locals with positive offsets. Use a separate
     * loop to register them without bumping g_locals_bytes (which is
     * for the NEGATIVE-offset stack-allocated locals).
     *
     * Session 92: each param node carries its declared kind in `op`
     * (LK_INT / LK_INT_PTR / LK_CHAR_PTR); preserve it so the body
     * can emit byte vs dword ops correctly.
     *
     * Session 106: support struct-by-value params. Each LK_STRUCT
     * param occupies (struct.size padded to 4) bytes on the stack
     * rather than the standard 4. Use a cumulative offset. */
    int cum_off = (fn->ret_kind == LK_STRUCT) ? 12 : 8;
    for (int i = 0; i < fn->n_params; i++) {
        if (g_n_locals >= MAX_LOCALS) die("too many locals");
        int j = 0;
        const char *nm = fn->params[i]->name;
        while (nm[j]) { g_locals[g_n_locals].name[j] = nm[j]; j++; }
        g_locals[g_n_locals].name[j] = 0;
        int kind = fn->params[i]->op ? fn->params[i]->op : LK_INT;
        int meta = fn->params[i]->num;
        g_locals[g_n_locals].ebp_off = cum_off;
        g_locals[g_n_locals].kind    = kind;
        g_locals[g_n_locals].meta    = meta;
        g_n_locals++;
        int p_size = 4;
        if (kind == LK_STRUCT && meta >= 0 && meta < g_n_structs)
            p_size = (g_structs[meta].size + 3) & ~3;
        cum_off += p_size;
    }

    /* Reserve room for locals — we don't know how many yet; emit a
     * placeholder `sub esp, imm32` and patch it after codegen. */
    int sub_at = g_code_len;
    e_sub_esp_imm32(0);

    gen_stmt(fn->body);

    /* If the function fell through without an explicit return,
     * emit a default return-0 epilogue so we don't run into the next
     * function's bytes. Session 121 — for struct-return functions,
     * "default return" returns the hidden dest pointer (which sits
     * at [ebp+8]); the slot's contents are whatever the caller left
     * there. Falling through without an explicit return in an SBV
     * function is technically a bug; we just don't crash. */
    if (g_cur_ret_kind == LK_STRUCT) {
        e_load_local(8);
    } else {
        e_mov_eax_imm(0);
    }
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();

    /* Patch the prologue's sub esp size. */
    patch_d(sub_at + 2, (unsigned)g_locals_bytes);

    /* Session 125 — resolve any goto fixups in this function against
     * the label table. Both forward and backward gotos go through here
     * (we don't try to do backward gotos eagerly during codegen). */
    for (int i = 0; i < g_n_gotos; i++) {
        struct goto_fixup *gf = &g_gotos[i];
        int li = -1;
        for (int j = 0; j < g_n_labels; j++)
            if (my_streq(g_labels[j].name, gf->name)) { li = j; break; }
        if (li < 0 || g_labels[li].code_off < 0)
            die_at(gf->line, "undefined label", gf->name);
        int disp = g_labels[li].code_off - (gf->jmp_disp_off + 4);
        patch_d(gf->jmp_disp_off, (unsigned)disp);
    }
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

/* Session 91 — print_str helper. One arg: a char* (NUL-terminated).
 * Computes strlen inline, calls sys_write(1, ptr, len). No newline.
 *
 * Layout:
 *   push ebp; mov ebp, esp        ; standard prologue
 *   push ebx                       ; we clobber ebx for sys_write
 *   mov ecx, [ebp+8]               ; ecx = ptr (also the sys_write addr)
 *   xor edx, edx                   ; edx = scan index / final length
 *   .loop:
 *     cmp byte [ecx + edx], 0
 *     je .done
 *     inc edx
 *     jmp .loop
 *   .done:
 *   mov ebx, 1                     ; stdout
 *   mov eax, 12                    ; SYS_WRITE_FD
 *   int 0x80
 *   pop ebx                        ; restore
 *   pop ebp
 *   ret
 *
 * Note: print_str is also used internally by the `puts` helper,
 * which adds a trailing newline write afterward. */
static void emit_print_str_helper(int idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    e_push_ebp();
    e_mov_ebp_esp();
    e_push_ebx();
    /* mov ecx, [ebp+8]   →  8b 4d 08 */
    emit_b(0x8b); emit_b(0x4d); emit_b(0x08);
    /* xor edx, edx       →  31 d2 */
    emit_b(0x31); emit_b(0xd2);

    /* .loop: */
    int loop_top = g_code_len;
    /* cmp byte [ecx + edx], 0   →  80 3c 11 00 (modrm 00.111.100 + sib 00.010.001) */
    emit_b(0x80); emit_b(0x3c); emit_b(0x11); emit_b(0x00);
    /* je .done — forward jump, patched after we know the offset.
     * Use rel8: 74 imm8. */
    emit_b(0x74);
    int je_off = g_code_len;
    emit_b(0);     /* placeholder disp8 */
    /* inc edx  →  42 */
    emit_b(0x42);
    /* jmp .loop (rel8 backward) */
    emit_b(0xeb);
    int jmp_disp = loop_top - (g_code_len + 1);
    emit_b((unsigned char)(jmp_disp & 0xff));

    /* .done: */
    int done = g_code_len;
    /* patch je: distance from byte AFTER je's imm8 (= je_off+1) to .done */
    g_code[je_off] = (unsigned char)((done - (je_off + 1)) & 0xff);

    /* mov ebx, 1; mov eax, 12; int 0x80 */
    e_mov_ebx_imm(1);
    e_mov_eax_imm(12);
    e_int_0x80();

    /* Epilogue. */
    e_pop_ebx();
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();
}

/* Session 91 — puts helper. Calls print_str, then writes "\n" to fd 1.
 * Single arg: char* ptr. The trailing-newline write reuses a one-byte
 * scratch slot built from `push 10; mov ecx, esp; …; add esp, 4`. */
static void emit_puts_helper(int idx, int print_str_idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    e_push_ebp();
    e_mov_ebp_esp();

    /* Forward the arg to print_str. */
    /* push [ebp+8]   →  ff 75 08 */
    emit_b(0xff); emit_b(0x75); emit_b(0x08);
    /* call print_str — rel32 with fixup. */
    int disp_off = e_call_rel32();
    if (g_n_fixups >= MAX_FIXUPS) die("too many fixups");
    g_fixups[g_n_fixups].call_disp_off = disp_off;
    g_fixups[g_n_fixups].func_idx = print_str_idx;
    g_n_fixups++;
    /* Pop the arg we pushed (cdecl caller-cleanup). */
    e_add_esp_imm32(4);

    /* Now emit a 1-byte '\n' on the stack and sys_write it.
     *   push 10            6a 0a       (push imm8 sign-extended to 32)
     *   mov ecx, esp       89 e1
     *   mov edx, 1         ba 01 00 00 00
     *   mov ebx, 1         bb 01 00 00 00
     *   mov eax, 12        b8 0c 00 00 00
     *   int 0x80           cd 80
     *   add esp, 4         (drop the pushed value)
     */
    emit_b(0x6a); emit_b(0x0a);
    emit_b(0x89); emit_b(0xe1);
    /* mov edx, 1 — use the existing e_mov_edx_imm... wait we removed it
     * in session 90's cleanup. Inline the encoding. */
    emit_b(0xba); emit_d(1);
    e_mov_ebx_imm(1);
    e_mov_eax_imm(12);
    e_int_0x80();
    e_add_esp_imm32(4);

    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();
}

/* Session 94 — print_int helper without the trailing newline.
 *
 * Same byte-shape as __print_int_helper but starts esi one position
 * higher (ebp-12 instead of ebp-13, so there's no '\n' slot) and the
 * loop pre-decrements esi instead of post-decrementing. Used by the
 * printf %d expansion; the original __print_int_helper still adds a
 * newline as `print_int(n)` users expect.
 *
 * Algorithm:
 *   esi = ebp - 12              ; one past the highest buffer byte
 *   if eax < 0: edi = 1, neg eax
 *   if eax == 0: dec esi; write '0' at [esi]; jmp end
 *   loop: idiv 10; dec esi; write digit at [esi]; if eax != 0 loop
 *   if edi == 1: dec esi; write '-'
 *   end: sys_write(1, esi, (ebp-12) - esi)
 */
static void emit_print_int_nonl_helper(int idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;

    e_push_ebp();
    e_mov_ebp_esp();
    e_push_ebx();
    e_push_esi();
    e_push_edi();
    e_sub_esp_imm32(16);     /* 16-byte buffer */

    /* lea esi, [ebp - 12]  →  8d 75 f4   (one past highest writable byte) */
    emit_b(0x8d); emit_b(0x75); emit_b(0xf4);

    e_load_local(8);
    emit_b(0x31); emit_b(0xff);  /* xor edi, edi */
    emit_b(0x85); emit_b(0xc0);  /* test eax, eax */
    emit_b(0x7d); emit_b(0x07);  /* jge +7  (skip mov-edi + neg-eax) */
    emit_b(0xbf); emit_d(1);     /* mov edi, 1 */
    emit_b(0xf7); emit_b(0xd8);  /* neg eax */

    /* Special case eax == 0: write '0' and jump past the loop. */
    emit_b(0x85); emit_b(0xc0);  /* test eax, eax */
    emit_b(0x75); emit_b(0x06);  /* jnz +6  (skip to loop) */
    emit_b(0x4e);                                /* dec esi */
    emit_b(0xc6); emit_b(0x06); emit_b(0x30);   /* mov byte [esi], '0' */
    emit_b(0xeb); emit_b(0x12);                  /* jmp +18 to neg-sign check */

    /* loop_top: */
    emit_b(0xbb); emit_d(10);                    /* mov ebx, 10        (5) */
    emit_b(0x99);                                 /* cdq                 (1) */
    emit_b(0xf7); emit_b(0xfb);                   /* idiv ebx            (2) */
    emit_b(0x80); emit_b(0xc2); emit_b(0x30);     /* add dl, '0'         (3) */
    emit_b(0x4e);                                 /* dec esi             (1) */
    emit_b(0x88); emit_b(0x16);                   /* mov [esi], dl       (2) */
    emit_b(0x85); emit_b(0xc0);                   /* test eax, eax       (2) */
    emit_b(0x75); emit_b((unsigned char)(-18 & 0xff));  /* jnz -18      (2) = 18 total */

    /* If edi == 1, prepend '-'. */
    emit_b(0x85); emit_b(0xff);  /* test edi, edi */
    emit_b(0x74); emit_b(0x04);  /* jz +4 (skip prepend) */
    emit_b(0x4e);                                /* dec esi */
    emit_b(0xc6); emit_b(0x06); emit_b(0x2d);   /* mov byte [esi], '-' */

    /* sys_write(fd=1, addr=esi, n=(ebp-12)-esi). */
    emit_b(0x89); emit_b(0xf1);                  /* mov ecx, esi */
    emit_b(0x89); emit_b(0xea);                  /* mov edx, ebp */
    emit_b(0x29); emit_b(0xf2);                  /* sub edx, esi */
    emit_b(0x83); emit_b(0xea); emit_b(0x0c);    /* sub edx, 12 */
    e_mov_eax_imm(12);
    e_mov_ebx_imm(1);
    e_int_0x80();

    e_add_esp_imm32(16);
    e_pop_edi();
    e_pop_esi();
    e_pop_ebx();
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();
}

/* Session 94 — write a single byte (the low 8 bits of the arg) to fd 1.
 * Used by printf's %c. Reuses the puts trailing-newline pattern:
 * `push imm32; mov ecx, esp; sys_write 1 byte; add esp, 4`. */
static void emit_print_char_helper(int idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;
    e_push_ebp();
    e_mov_ebp_esp();
    /* push dword [ebp+8]   →  ff 75 08   (push the arg as 4 bytes;
     * sys_write only reads the first byte). */
    emit_b(0xff); emit_b(0x75); emit_b(0x08);
    emit_b(0x89); emit_b(0xe1);     /* mov ecx, esp */
    emit_b(0xba); emit_d(1);        /* mov edx, 1 */
    e_mov_ebx_imm(1);
    e_mov_eax_imm(12);
    e_int_0x80();
    e_add_esp_imm32(4);
    e_mov_esp_ebp();
    e_pop_ebp();
    e_ret();
}

/* Session 94 — print arg as lowercase hex, no padding, no newline.
 * Treats the int as unsigned (uses `div` not `idiv`), so negative
 * numbers display as the 8-digit two's-complement representation
 * (e.g. -1 → "ffffffff").
 *
 * Same buffer/cursor pattern as __print_int_nonl_helper but with
 * 16 as the divisor and an extra 'a'..'f' offset for the 10..15
 * digits.
 */
static void emit_print_hex_helper(int idx) {
    g_funcs[idx].entry_off = g_code_len;
    g_funcs[idx].defined   = 1;
    e_push_ebp();
    e_mov_ebp_esp();
    e_push_ebx();
    e_push_esi();
    e_sub_esp_imm32(16);

    /* lea esi, [ebp - 8]   (one past the highest byte we'll write — we
     * only need at most 8 hex digits for a 32-bit value; reserved 16
     * to keep alignment same). */
    emit_b(0x8d); emit_b(0x75); emit_b(0xf8);

    e_load_local(8);     /* eax = arg */

    /* Zero special-case. Loop body below is exactly 29 bytes, so we
     * jump +29 from end-of-jmp to land at the sys_write call. */
    emit_b(0x85); emit_b(0xc0);                   /* test eax, eax */
    emit_b(0x75); emit_b(0x06);                   /* jnz +6 (skip to loop) */
    emit_b(0x4e);                                  /* dec esi */
    emit_b(0xc6); emit_b(0x06); emit_b(0x30);     /* mov byte [esi], '0' */
    emit_b(0xeb); emit_b(0x1d);                   /* jmp +29 to write call */

    /* loop_top — total 29 bytes. */
    emit_b(0x31); emit_b(0xd2);                    /* xor edx, edx    (2) */
    emit_b(0xbb); emit_d(16);                       /* mov ebx, 16     (5) */
    emit_b(0xf7); emit_b(0xf3);                     /* div ebx         (2) */
    emit_b(0x80); emit_b(0xfa); emit_b(0x0a);       /* cmp dl, 10      (3) */
    emit_b(0x7c); emit_b(0x05);                     /* jl +5 (numeric) (2) */
    /* alpha branch: dl = 10..15 → 'a'..'f' via add 0x57 = 'a' - 10. */
    emit_b(0x80); emit_b(0xc2); emit_b(0x57);       /* add dl, 0x57    (3) */
    emit_b(0xeb); emit_b(0x03);                     /* jmp +3 over numeric (2) */
    /* numeric branch: dl = 0..9 → '0'..'9' via add 0x30. */
    emit_b(0x80); emit_b(0xc2); emit_b(0x30);       /* add dl, '0'     (3) */
    emit_b(0x4e);                                   /* dec esi         (1) */
    emit_b(0x88); emit_b(0x16);                     /* mov [esi], dl   (2) */
    emit_b(0x85); emit_b(0xc0);                     /* test eax, eax   (2) */
    /* End of jnz at +29 from loop_top; loop_top at 0; disp = -29. */
    emit_b(0x75); emit_b((unsigned char)(-29 & 0xff));  /* jnz -29 (2) */

    /* sys_write(1, esi, (ebp-8) - esi). */
    emit_b(0x89); emit_b(0xf1);                  /* mov ecx, esi */
    emit_b(0x89); emit_b(0xea);                  /* mov edx, ebp */
    emit_b(0x29); emit_b(0xf2);                  /* sub edx, esi */
    emit_b(0x83); emit_b(0xea); emit_b(0x08);    /* sub edx, 8 */
    e_mov_eax_imm(12);
    e_mov_ebx_imm(1);
    e_int_0x80();

    e_add_esp_imm32(16);
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
        sys_write(2, "usage: cc FILE.c [FILE.c ...] [-o OUT.elf]\n", 43);
        return 1;
    }
    /* Session 100 — multi-file compilation. Accept any number of input
     * files; the preprocessor state persists across them so a header
     * `#ifndef GUARD / #define GUARD / ... / #endif` shared via #include
     * is included only once. The concatenated preprocessed source is
     * lexed and parsed as a single translation unit. */
    #define MAX_INPUTS 16
    const char *in_paths[MAX_INPUTS];
    int   n_inputs = 0;
    char  out_path[80];
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
        } else {
            if (n_inputs >= MAX_INPUTS) die("too many input files");
            in_paths[n_inputs++] = argv[i];
        }
    }
    if (n_inputs == 0) die("missing input file");
    if (!out_path[0]) default_outpath(in_paths[0], out_path, sizeof(out_path));

    /* Session 95/100 — run the preprocessor over each input file, with
     * state persisting across them. The result of concatenating each
     * file's processed bytes ends up in g_pp_buf. Single lex+parse
     * follows. */
    g_pp_len = 0;
    g_n_macros = 0;
    g_if_depth = 0;
    for (int i = 0; i < n_inputs; i++) {
        int sz = 0;
        char *src = slurp(in_paths[i], &sz);
        if (!src) die_at(0, "cannot read", in_paths[i]);
        pp_process_buf(src, sz, 0);
    }
    if (g_if_depth != 0) die("unterminated #ifdef/#ifndef");

    lex_all(g_pp_buf, g_pp_len);
    struct node *prog = parse_program();

    /* Session 122 — constant-folding pass over every function body.
     * Runs before pre-population so any folded expressions are visible
     * to subsequent passes (DCE in particular). */
    for (int i = 0; i < prog->n_list; i++) fold_node(prog->list[i]);

    /* Session 106 — pre-populate function param info BEFORE any
     * codegen runs. This makes per-function param kinds available
     * to call sites regardless of source order, which is required
     * for struct-by-value passing (the caller needs to know each
     * arg's expected kind). For non-struct params this is also
     * harmless — the existing arg-count / variadic checks see
     * the same info, just earlier. */
    for (int i = 0; i < prog->n_list; i++) {
        struct node *fn = prog->list[i];
        if (!fn || fn->kind != N_FUNC_DECL) continue;
        int idx = func_intern(fn->name, fn->n_params);
        g_funcs[idx].is_variadic = fn->op ? 1 : 0;
        /* Session 121 — also propagate return-type info. */
        g_funcs[idx].ret_kind = fn->ret_kind;
        g_funcs[idx].ret_meta = fn->ret_meta;
        int np = fn->n_params;
        if (np > MAX_PARAMS_PER_FUNC) np = MAX_PARAMS_PER_FUNC;
        for (int j = 0; j < np; j++) {
            g_funcs[idx].param_kinds[j] = fn->params[j]->op;
            g_funcs[idx].param_metas[j] = fn->params[j]->num;
        }
    }

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

    /* Session 91 — also reserve puts / print_str helpers (1 arg each).
     * puts is layered on top of print_str (it forwards then writes '\n'),
     * so print_str must be emitted FIRST. */
    int print_str_idx = func_intern("__print_str_helper", 1);
    emit_print_str_helper(print_str_idx);
    int puts_idx = func_intern("__puts_helper", 1);
    emit_puts_helper(puts_idx, print_str_idx);

    /* Session 94 — printf helpers. The format-string dispatch happens
     * at compile time inside emit_syscall_intrinsic, but the three
     * extra runtime helpers must exist in the binary too. */
    int pi_nonl_idx = func_intern("__print_int_nonl_helper", 1);
    emit_print_int_nonl_helper(pi_nonl_idx);
    int pchar_idx = func_intern("__print_char_helper", 1);
    emit_print_char_helper(pchar_idx);
    int phex_idx = func_intern("__print_hex_helper", 1);
    emit_print_hex_helper(phex_idx);

    /* Generate code for every user function in source order. */
    for (int i = 0; i < prog->n_list; i++) {
        gen_func(prog->list[i]);
    }

    /* Verify main was defined. */
    if (!g_funcs[main_idx].defined) die("'main' function not found");

    resolve_fixups();

    /* Session 98 — patch address-of-function fixups. Each emits a
     * `mov eax, imm32` placeholder; the imm becomes ENTRY_VA + the
     * target function's entry offset. Functions referenced by address
     * but never defined are caught here. */
    for (int i = 0; i < g_n_addr_fixups; i++) {
        int fi = g_addr_fixups[i].func_idx;
        if (g_funcs[fi].entry_off < 0)
            die_at(0, "address-of undefined function", g_funcs[fi].name);
        unsigned int va = (unsigned int)ENTRY_VA + (unsigned int)g_funcs[fi].entry_off;
        patch_d(g_addr_fixups[i].imm_off, va);
    }

    /* Session 91 — append the string pool right after the last code
     * byte (no padding). Each string's VA is ENTRY_VA + (its offset
     * in the final code+strings image). Patch every N_STR fixup. */
    int pool_start_off = g_code_len;
    g_str_pool_base_va = ENTRY_VA + pool_start_off;
    if (g_str_pool_len + g_code_len > CODE_MAX)
        die("code+strings would exceed CODE_MAX");
    for (int i = 0; i < g_str_pool_len; i++) g_code[g_code_len++] = g_str_pool[i];
    for (int i = 0; i < g_n_str_fixups; i++) {
        int sidx = g_str_fixups[i].str_idx;
        unsigned int va = (unsigned int)g_str_pool_base_va + (unsigned int)g_str_offs[sidx];
        patch_d(g_str_fixups[i].code_off, va);
    }

    /* Session 93 — append the global data pool after the string pool
     * and patch every N_NAME/etc fixup that touches a global. The
     * single PT_LOAD covers code + strings + globals, all mapped RWX. */
    int data_start_off = g_code_len;
    g_data_pool_base_va = ENTRY_VA + data_start_off;
    if (g_data_pool_len + g_code_len > CODE_MAX)
        die("code+strings+globals would exceed CODE_MAX");
    for (int i = 0; i < g_data_pool_len; i++) g_code[g_code_len++] = g_data_pool[i];
    for (int i = 0; i < g_n_glob_fixups; i++) {
        int gi = g_glob_fixups[i].glob_idx;
        unsigned int va = (unsigned int)g_data_pool_base_va + (unsigned int)g_globals[gi].offset;
        /* Session 122 — read the current imm32 as an ADDEND. Existing
         * call sites emit emit_d(0) so addend is zero (no change). The
         * N_MEMBER global fast path emits emit_d(field_off) so the
         * final patched value is GLOBAL_VA + field_off — one addressed
         * load instead of mov-load-add-load. */
        int code_off = g_glob_fixups[i].code_off;
        unsigned int addend =
              (unsigned int)g_code[code_off + 0]
            | ((unsigned int)g_code[code_off + 1] << 8)
            | ((unsigned int)g_code[code_off + 2] << 16)
            | ((unsigned int)g_code[code_off + 3] << 24);
        patch_d(code_off, va + addend);
    }

    if (write_elf(out_path, g_code_len) < 0) return 1;

    /* Tiny success line. */
    sys_write(1, "cc: wrote ", 10);
    sys_write(1, out_path, my_strlen(out_path));
    sys_write(1, "\n", 1);

    (void)g_exit_code;
    return 0;
}
