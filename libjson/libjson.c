/*
 * libjson — implementation. See libjson.h for the contract.
 *
 * The encoder is a small state machine: it tracks how deep we are
 * inside containers and whether the immediate-next emit is the first
 * child (so we know whether to insert a comma). The state goes:
 *
 *     [start]
 *       json_obj_begin -> emit '{' -> push first=1
 *       json_arr_begin -> emit '[' -> push first=1
 *       json_obj_end   -> emit '}' -> pop
 *       json_arr_end   -> emit ']' -> pop
 *
 *     before any value or key-value emit:
 *       if depth == 0: nothing  (top-level scalar; rare)
 *       else if first[d-1]: clear first
 *       else: emit ','
 *
 *     json_key sets expect_value=1 so the next value emit doesn't
 *     try to insert another comma.
 *
 * The parser is recursive-descent. It carves struct json_v nodes
 * and unescaped string bodies out of a single scratch arena that
 * grows from the front. We never free; the parse tree is alive
 * exactly as long as the scratch buffer is.
 */
#include "libjson.h"

/* ============================================================
 * Internal helpers
 * ============================================================ */

static void put_char(struct json_w *w, char c) {
    if (!w->ok) return;
    if (w->len + 1 > w->cap) { w->ok = 0; return; }
    w->buf[w->len++] = c;
}

static void put_n(struct json_w *w, const char *s, int n) {
    if (!w->ok) return;
    if (w->len + n > w->cap) { w->ok = 0; return; }
    for (int i = 0; i < n; i++) w->buf[w->len++] = s[i];
}

static int my_strlen(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Called before every value or key emit; handles the comma. */
static void pre_emit(struct json_w *w) {
    if (!w->ok) return;
    if (w->depth == 0) return;
    int d = w->depth - 1;
    if (w->expect_value) {
        /* The previous token was a key+colon; the value doesn't
         * need its own comma. Clear and proceed. */
        w->expect_value = 0;
        return;
    }
    if (w->first[d]) {
        w->first[d] = 0;
    } else {
        put_char(w, ',');
    }
}

static void put_escaped_str(struct json_w *w, const char *s, int n) {
    put_char(w, '"');
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  put_n(w, "\\\"", 2); break;
            case '\\': put_n(w, "\\\\", 2); break;
            case '\b': put_n(w, "\\b",  2); break;
            case '\f': put_n(w, "\\f",  2); break;
            case '\n': put_n(w, "\\n",  2); break;
            case '\r': put_n(w, "\\r",  2); break;
            case '\t': put_n(w, "\\t",  2); break;
            default:
                if (c < 0x20) {
                    /* RFC 8259 §7: every control character must be
                     * escaped. We use the \u00XX form rather than
                     * the shorter named escapes for the few that
                     * don't have one. */
                    static const char hex[] = "0123456789abcdef";
                    char esc[6] = { '\\', 'u', '0', '0', 0, 0 };
                    esc[4] = hex[(c >> 4) & 0xF];
                    esc[5] = hex[c & 0xF];
                    put_n(w, esc, 6);
                } else {
                    /* Bytes 0x20..0x7E pass through. 0x7F..0xFF also
                     * pass through unescaped — the RFC permits any
                     * Unicode code point inside a JSON string and a
                     * UTF-8 byte sequence is just a sequence of
                     * bytes from this code's point of view. */
                    put_char(w, (char)c);
                }
        }
    }
    put_char(w, '"');
}

/* ============================================================
 * Encoder — public
 * ============================================================ */

void json_w_init(struct json_w *w, char *buf, int cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok  = 1;
    w->depth = 0;
    w->expect_value = 0;
    for (int i = 0; i < JSON_W_MAX_DEPTH; i++) w->first[i] = 0;
}

int json_w_len(const struct json_w *w) { return w->len; }
int json_w_ok (const struct json_w *w) { return w->ok;  }

void json_w_finish(struct json_w *w) {
    if (!w->ok) return;
    if (w->len < w->cap) w->buf[w->len] = 0;
    else { w->ok = 0; w->buf[w->cap - 1] = 0; }
}

void json_obj_begin(struct json_w *w) {
    if (w->depth >= JSON_W_MAX_DEPTH) { w->ok = 0; return; }
    pre_emit(w);
    put_char(w, '{');
    w->first[w->depth] = 1;
    w->depth++;
}

void json_obj_end(struct json_w *w) {
    if (w->depth == 0) { w->ok = 0; return; }
    w->depth--;
    put_char(w, '}');
}

void json_arr_begin(struct json_w *w) {
    if (w->depth >= JSON_W_MAX_DEPTH) { w->ok = 0; return; }
    pre_emit(w);
    put_char(w, '[');
    w->first[w->depth] = 1;
    w->depth++;
}

void json_arr_end(struct json_w *w) {
    if (w->depth == 0) { w->ok = 0; return; }
    w->depth--;
    put_char(w, ']');
}

void json_key(struct json_w *w, const char *key) {
    pre_emit(w);
    put_escaped_str(w, key, my_strlen(key));
    put_char(w, ':');
    /* The next value emit must NOT pre-emit another comma. */
    w->expect_value = 1;
}

void json_str(struct json_w *w, const char *s) {
    json_str_n(w, s, my_strlen(s));
}

void json_str_n(struct json_w *w, const char *s, int n) {
    pre_emit(w);
    put_escaped_str(w, s, n);
}

static void put_dec(struct json_w *w, unsigned int u, int neg) {
    char tmp[12];
    int  ti = 0;
    if (u == 0) tmp[ti++] = '0';
    while (u) { tmp[ti++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) put_char(w, '-');
    while (ti) put_char(w, tmp[--ti]);
}

void json_int(struct json_w *w, libjson_int_t v) {
    pre_emit(w);
    unsigned int u;
    int neg = 0;
    if (v < 0) { neg = 1; u = (unsigned int)(-v); } else u = (unsigned int)v;
    put_dec(w, u, neg);
}

void json_uint(struct json_w *w, unsigned int v) {
    pre_emit(w);
    put_dec(w, v, 0);
}

void json_bool(struct json_w *w, int b) {
    pre_emit(w);
    if (b) put_n(w, "true",  4);
    else   put_n(w, "false", 5);
}

void json_null(struct json_w *w) {
    pre_emit(w);
    put_n(w, "null", 4);
}

void json_raw(struct json_w *w, const char *s, int n) {
    pre_emit(w);
    put_n(w, s, n);
}


/* ============================================================
 * Parser
 * ============================================================ */

struct parser {
    const char *src;
    int         len;
    int         pos;
    char       *arena;
    int         arena_cap;
    int         arena_used;
};

static int  p_skip_ws(struct parser *p);
static struct json_v *p_value(struct parser *p);

/* Carve `n` bytes from the arena, unaligned. NULL if exhausted.
 * Used for string body bytes (1 byte each) — alignment at every
 * call would leave 3-byte gaps between characters and shred the
 * string's contiguous storage. */
static void *arena_alloc_bytes(struct parser *p, int n) {
    if (p->arena_used + n > p->arena_cap) return 0;
    void *r = p->arena + p->arena_used;
    p->arena_used += n;
    return r;
}

/* Round arena_used up to a 4-byte boundary, then carve `n` bytes.
 * Use this for fixed-layout structs (struct json_v) whose member
 * loads require natural alignment. */
static void *arena_alloc_aligned(struct parser *p, int n) {
    int aligned = (p->arena_used + 3) & ~3;
    if (aligned + n > p->arena_cap) return 0;
    void *r = p->arena + aligned;
    p->arena_used = aligned + n;
    return r;
}

static struct json_v *node_alloc(struct parser *p, int type) {
    struct json_v *v = (struct json_v *)arena_alloc_aligned(p, (int)sizeof(struct json_v));
    if (!v) return 0;
    v->type = type;
    v->num = 0;
    v->str = 0;
    v->str_len = 0;
    v->child = 0;
    v->next = 0;
    return v;
}

static int p_peek(struct parser *p) {
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->src[p->pos];
}

static int p_next(struct parser *p) {
    int c = p_peek(p);
    if (c >= 0) p->pos++;
    return c;
}

static int p_skip_ws(struct parser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
    return 0;
}

/* Emit one UTF-8 sequence for code point cp into the arena as a
 * contiguous byte run. Returns 1 on success, 0 on arena exhaustion.
 * cp restricted to BMP. */
static int arena_putc_utf8(struct parser *p, unsigned int cp) {
    if (cp < 0x80) {
        char *b = (char *)arena_alloc_bytes(p, 1);
        if (!b) return 0;
        b[0] = (char)cp;
    } else if (cp < 0x800) {
        char *b = (char *)arena_alloc_bytes(p, 2);
        if (!b) return 0;
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
    } else {
        char *b = (char *)arena_alloc_bytes(p, 3);
        if (!b) return 0;
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
    }
    return 1;
}

/* Parse a JSON string starting at p->pos pointing AT the leading '"'.
 * On success advances past closing '"' and returns the unescaped bytes
 * via *out_ptr / *out_len. The body lives in the arena and is NUL-
 * terminated (the NUL byte is past the reported length, so the caller
 * can pass out_ptr to C string APIs). Returns 1 on success, 0 on
 * malformed input or arena exhaustion. */
static int p_string(struct parser *p, const char **out_ptr, int *out_len) {
    if (p_next(p) != '"') return 0;

    /* Mark where the unescaped body starts in the arena. We append a
     * byte at a time via arena_alloc_bytes (deliberately UNALIGNED —
     * a char array doesn't need 4-byte stride, and aligning every
     * push would leave 3-byte gaps that corrupt the string body).
     *
     * We don't know the unescaped length upfront, so we just keep
     * appending bytes and report `start + (used - start_used_offset)`
     * as the final body. */
    char *start = p->arena + p->arena_used;

    for (;;) {
        int c = p_peek(p);
        if (c < 0) return 0;          /* premature EOF */
        if (c == '"') { p->pos++; break; }

        if (c == '\\') {
            p->pos++;
            int esc = p_next(p);
            switch (esc) {
                case '"':  case '\\': case '/':
                    if (!arena_putc_utf8(p, (unsigned)esc)) return 0;
                    break;
                case 'b': if (!arena_putc_utf8(p, 0x08)) return 0; break;
                case 'f': if (!arena_putc_utf8(p, 0x0C)) return 0; break;
                case 'n': if (!arena_putc_utf8(p, 0x0A)) return 0; break;
                case 'r': if (!arena_putc_utf8(p, 0x0D)) return 0; break;
                case 't': if (!arena_putc_utf8(p, 0x09)) return 0; break;
                case 'u': {
                    /* \uXXXX. BMP only — we don't decode UTF-16 surrogates.
                     * Agents talking to this endpoint won't be sending
                     * astral characters in a method name. */
                    int h[4];
                    for (int i = 0; i < 4; i++) {
                        int ch = p_next(p);
                        h[i] = hex_digit(ch);
                        if (h[i] < 0) return 0;
                    }
                    unsigned cp = (unsigned)((h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3]);
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        /* Surrogate. Emit a replacement-character so the
                         * stream stays valid UTF-8 instead of erroring. */
                        if (!arena_putc_utf8(p, 0xFFFD)) return 0;
                    } else {
                        if (!arena_putc_utf8(p, cp)) return 0;
                    }
                    break;
                }
                default:
                    return 0;
            }
        } else if ((unsigned char)c < 0x20) {
            /* Unescaped control char — RFC 8259 §7 forbids this. */
            return 0;
        } else {
            p->pos++;
            if (!arena_putc_utf8(p, (unsigned)c)) return 0;
        }
    }

    int length = (int)((p->arena + p->arena_used) - start);
    /* NUL-terminator past the reported length (does not advance length). */
    char *nul = (char *)arena_alloc_bytes(p, 1);
    if (!nul) return 0;
    *nul = 0;

    *out_ptr = start;
    *out_len = length;
    return 1;
}

static int p_number(struct parser *p, long *out) {
    int neg = 0;
    if (p_peek(p) == '-') { neg = 1; p->pos++; }
    int c = p_peek(p);
    if (c < '0' || c > '9') return 0;
    long n = 0;
    while (p->pos < p->len) {
        c = (unsigned char)p->src[p->pos];
        if (c < '0' || c > '9') break;
        /* Overflow-guard: cap at INT_MAX/10 to keep this 32-bit clean. */
        if (n > 214748364L) return 0;
        n = n * 10 + (c - '0');
        p->pos++;
    }
    /* Reject fractional / exponent forms — we'd silently truncate. */
    if (p->pos < p->len) {
        c = (unsigned char)p->src[p->pos];
        if (c == '.' || c == 'e' || c == 'E') return 0;
    }
    *out = neg ? -n : n;
    return 1;
}

/* Match a literal keyword (`true`, `false`, `null`) starting at pos. */
static int p_match(struct parser *p, const char *kw) {
    int n = my_strlen(kw);
    if (p->pos + n > p->len) return 0;
    for (int i = 0; i < n; i++) {
        if (p->src[p->pos + i] != kw[i]) return 0;
    }
    p->pos += n;
    return 1;
}

static struct json_v *p_array(struct parser *p) {
    if (p_next(p) != '[') return 0;
    struct json_v *arr = node_alloc(p, JSON_ARR);
    if (!arr) return 0;
    p_skip_ws(p);
    if (p_peek(p) == ']') { p->pos++; return arr; }

    struct json_v *tail = 0;
    for (;;) {
        p_skip_ws(p);
        struct json_v *child = p_value(p);
        if (!child) return 0;
        if (!arr->child) arr->child = child;
        else             tail->next = child;
        tail = child;
        p_skip_ws(p);
        int c = p_next(p);
        if (c == ',') continue;
        if (c == ']') return arr;
        return 0;
    }
}

static struct json_v *p_object(struct parser *p) {
    if (p_next(p) != '{') return 0;
    struct json_v *obj = node_alloc(p, JSON_OBJ);
    if (!obj) return 0;
    p_skip_ws(p);
    if (p_peek(p) == '}') { p->pos++; return obj; }

    struct json_v *tail = 0;
    for (;;) {
        p_skip_ws(p);
        if (p_peek(p) != '"') return 0;
        struct json_v *k = node_alloc(p, JSON_STR);
        if (!k) return 0;
        if (!p_string(p, &k->str, &k->str_len)) return 0;
        p_skip_ws(p);
        if (p_next(p) != ':') return 0;
        p_skip_ws(p);
        struct json_v *v = p_value(p);
        if (!v) return 0;
        k->next = v;
        if (!obj->child) obj->child = k;
        else             tail->next = k;
        tail = v;
        p_skip_ws(p);
        int c = p_next(p);
        if (c == ',') continue;
        if (c == '}') return obj;
        return 0;
    }
}

static struct json_v *p_value(struct parser *p) {
    p_skip_ws(p);
    int c = p_peek(p);
    if (c < 0) return 0;

    if (c == '{') return p_object(p);
    if (c == '[') return p_array(p);
    if (c == '"') {
        struct json_v *v = node_alloc(p, JSON_STR);
        if (!v) return 0;
        if (!p_string(p, &v->str, &v->str_len)) return 0;
        return v;
    }
    if (c == 't') {
        if (!p_match(p, "true")) return 0;
        struct json_v *v = node_alloc(p, JSON_BOOL);
        if (!v) return 0;
        v->num = 1; return v;
    }
    if (c == 'f') {
        if (!p_match(p, "false")) return 0;
        struct json_v *v = node_alloc(p, JSON_BOOL);
        if (!v) return 0;
        v->num = 0; return v;
    }
    if (c == 'n') {
        if (!p_match(p, "null")) return 0;
        struct json_v *v = node_alloc(p, JSON_NULL);
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        long n;
        if (!p_number(p, &n)) return 0;
        struct json_v *v = node_alloc(p, JSON_NUM);
        if (!v) return 0;
        v->num = n; return v;
    }
    return 0;
}

struct json_v *json_parse(const char *src, int len,
                          void *scratch, libjson_size_t scratch_sz) {
    if (!src || !scratch || scratch_sz < sizeof(struct json_v)) return 0;
    struct parser p;
    p.src         = src;
    p.len         = len;
    p.pos         = 0;
    p.arena       = (char *)scratch;
    p.arena_cap   = (int)scratch_sz;
    p.arena_used  = 0;

    p_skip_ws(&p);
    struct json_v *root = p_value(&p);
    if (!root) return 0;
    p_skip_ws(&p);
    /* Any trailing content past the root value (other than whitespace)
     * is an error per RFC 8259 §2. agentd serves one request per line
     * so trailing content would mean a framing bug. */
    if (p.pos != p.len) return 0;
    return root;
}


/* ============================================================
 * Accessors
 * ============================================================ */

static int str_eq_n(const char *a, int an, const char *b) {
    int bn = my_strlen(b);
    if (an != bn) return 0;
    for (int i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

const struct json_v *json_obj_get(const struct json_v *v, const char *key) {
    if (!v || v->type != JSON_OBJ) return 0;
    for (struct json_v *k = v->child; k; k = k->next ? k->next->next : 0) {
        if (!k->next) break;
        if (k->type == JSON_STR && str_eq_n(k->str, k->str_len, key)) {
            return k->next;
        }
    }
    return 0;
}

const struct json_v *json_arr_at(const struct json_v *v, int i) {
    if (!v || v->type != JSON_ARR) return 0;
    struct json_v *c = v->child;
    while (c && i > 0) { c = c->next; i--; }
    return c;
}

int json_arr_len(const struct json_v *v) {
    if (!v || v->type != JSON_ARR) return -1;
    int n = 0;
    for (struct json_v *c = v->child; c; c = c->next) n++;
    return n;
}

long json_to_int(const struct json_v *v) {
    if (!v) return 0;
    if (v->type == JSON_NUM)  return v->num;
    if (v->type == JSON_BOOL) return v->num;
    return 0;
}

long json_to_int_or(const struct json_v *v, long fallback) {
    if (!v || (v->type != JSON_NUM && v->type != JSON_BOOL)) return fallback;
    return v->num;
}

int json_to_bool(const struct json_v *v) {
    if (!v) return 0;
    if (v->type == JSON_BOOL) return (int)v->num;
    if (v->type == JSON_NUM)  return v->num != 0;
    return 0;
}

const char *json_to_str(const struct json_v *v, int *out_len) {
    if (!v || v->type != JSON_STR) { if (out_len) *out_len = 0; return 0; }
    if (out_len) *out_len = v->str_len;
    return v->str;
}
