/*
 * libjson — a tiny JSON encoder + parser for AdventOS.
 *
 * Why a separate library? The kernel never speaks JSON; agentd and
 * the --json variants of coreutils all need it; libcrypto-grade code
 * sharing is the precedent (libcrypto/ is also a static archive
 * linked into the programs that care). libjson stays out of libc.bin
 * so the ABI version doesn't have to bump.
 *
 * Scope (RFC 8259):
 *   - Encoder: builder API that writes into a caller-supplied buffer.
 *     Tracks nesting + commas automatically. Truncation is sticky —
 *     past the cap, further calls are no-ops and `json_w_ok()` flips
 *     to false. Strings get RFC 8259-correct escaping (\" \\ control
 *     chars as \uXXXX). No streaming, no pretty-printing.
 *
 *   - Parser: returns a DOM into a caller-supplied scratch buffer
 *     (single contiguous arena). Numbers are 32-bit signed only — no
 *     floats. \uXXXX is decoded into UTF-8 for BMP code points; the
 *     surrogate-pair path is deliberately unimplemented (we have no
 *     emoji to round-trip).
 *
 * Design choices:
 *   - No malloc anywhere. Caller-owned buffers for encoder + parser.
 *     Lets us run inside agentd without any per-request heap churn,
 *     and keeps the library safe to call from contexts where malloc
 *     hasn't been initialized.
 *   - DOM not callback. Two-pass parse (count then materialize) would
 *     halve memory, but the JSON-RPC requests this thing actually
 *     sees are <2 KiB, and a DOM is materially nicer to dispatch off.
 */
#ifndef ADVENT_LIBJSON_H
#define ADVENT_LIBJSON_H

typedef unsigned int   libjson_size_t;
typedef int            libjson_int_t;

/* ============================================================
 * Encoder
 * ============================================================
 *
 * Usage:
 *
 *     char buf[256];
 *     struct json_w w;
 *     json_w_init(&w, buf, sizeof(buf));
 *     json_obj_begin(&w);
 *       json_key(&w, "id");     json_int(&w, 7);
 *       json_key(&w, "name");   json_str(&w, "guest");
 *       json_key(&w, "tags");
 *         json_arr_begin(&w);
 *           json_str(&w, "admin"); json_str(&w, "dev");
 *         json_arr_end(&w);
 *     json_obj_end(&w);
 *     if (!json_w_ok(&w)) { ... truncated ... }
 *     int n = json_w_len(&w);
 *
 * The writer doesn't NUL-terminate by default; call json_w_finish()
 * if a C string is needed.
 */

#define JSON_W_MAX_DEPTH 32

struct json_w {
    char     *buf;
    int       cap;
    int       len;
    int       ok;             /* 0 once truncated; further ops are no-ops */
    int       depth;
    /* first[d] = 1 means the next push at depth d is the first child
     * of its container (so we skip the leading comma). After every
     * value/key-value emit we set first[depth-1]=0. */
    unsigned char first[JSON_W_MAX_DEPTH];
    int       expect_value;   /* set after json_key; suppresses the comma
                                 logic for the immediately-following
                                 value, since the key already separated. */
};

void json_w_init  (struct json_w *w, char *buf, int cap);
int  json_w_len   (const struct json_w *w);
int  json_w_ok    (const struct json_w *w);
void json_w_finish(struct json_w *w);          /* NUL-terminate */

void json_obj_begin(struct json_w *w);
void json_obj_end  (struct json_w *w);
void json_arr_begin(struct json_w *w);
void json_arr_end  (struct json_w *w);

/* Key inside an object. Must be followed by exactly one value emit
 * before the next key or obj_end. */
void json_key      (struct json_w *w, const char *key);

/* Values. Strings are escaped per RFC 8259. json_str_n lets the
 * caller pass non-NUL-terminated bytes (e.g. shell command output). */
void json_str      (struct json_w *w, const char *s);
void json_str_n    (struct json_w *w, const char *s, int n);
void json_int      (struct json_w *w, libjson_int_t v);
void json_uint     (struct json_w *w, unsigned int v);
void json_bool     (struct json_w *w, int b);
void json_null     (struct json_w *w);

/* Raw insertion — caller already has a valid JSON fragment to splice
 * (e.g. a pre-built result object). Comma/nesting bookkeeping is
 * updated as though one value was emitted. */
void json_raw      (struct json_w *w, const char *s, int n);


/* ============================================================
 * Parser
 * ============================================================
 *
 *     struct json_v *root = json_parse(buf, len, scratch, sizeof(scratch));
 *     if (!root) { ... parse error ... }
 *     const struct json_v *id = json_obj_get(root, "id");
 *     long n = json_to_int(id);
 *
 * Scratch usage breakdown (informational, ~rule of thumb):
 *   - one struct json_v per value (~28 bytes)
 *   - one byte per character of any decoded string body
 *   - object keys consume one struct json_v each
 *
 * Practical budget: 4 KiB of scratch covers JSON-RPC requests with
 * ~50 keys/values and ~3 KiB of cumulative string content. The
 * caller should size based on the largest input it will accept;
 * `json_parse` returns NULL if scratch is exhausted.
 */

enum json_type {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ,
};

struct json_v {
    int          type;       /* enum json_type */
    /* For JSON_NUM. We don't do floats — integers fit in a long. */
    long         num;
    /* For JSON_BOOL: 0/1. Stored in num. (We alias to keep struct tight.) */
    /* For JSON_STR: pointer into scratch, NUL-terminated, plus length. */
    const char  *str;
    int          str_len;
    /* For JSON_ARR / JSON_OBJ: pointer to first child, then linked
     * sibling chain via `next`. obj keys/values alternate: child is
     * key (JSON_STR), child->next is value, then key, value, ... */
    struct json_v *child;
    struct json_v *next;
};

struct json_v *json_parse(const char *src, int len,
                          void *scratch, libjson_size_t scratch_sz);

/* Object accessor: linear scan for the key. NULL if not found or v
 * isn't an object. */
const struct json_v *json_obj_get(const struct json_v *v, const char *key);

/* Array accessor by index. NULL if out of range or v isn't an array. */
const struct json_v *json_arr_at(const struct json_v *v, int i);

/* Array length. -1 if v isn't an array. */
int json_arr_len(const struct json_v *v);

/* Value coercion. The *_or variants return the supplied fallback on
 * type mismatch. */
long        json_to_int    (const struct json_v *v);
long        json_to_int_or (const struct json_v *v, long fallback);
int         json_to_bool   (const struct json_v *v);
const char *json_to_str    (const struct json_v *v, int *out_len);

#endif /* ADVENT_LIBJSON_H */
