/*
 * X.509 v3 self-signed certificate builder for Ed25519.
 *
 * The encoder works backward — start at the END of the output
 * buffer and walk left, writing each TLV (tag, length, value)
 * after its content. This makes nested SEQUENCEs trivial: write
 * the inner content first, snapshot the position, then wrap
 * with the outer tag/length whose length is known once you know
 * how far you walked.
 *
 * The result lives in out[(cap - returned_len) .. cap), but we
 * memcpy it down to out[0..len) at the end so callers see a
 * "normal" forward-aligned buffer.
 *
 * Sources:
 *   - RFC 5280 §4 — X.509 v3 structure
 *   - RFC 8410   — Ed25519 in X.509 (algorithm OID 1.3.101.112)
 *   - X.690      — DER encoding rules (the bits of ASN.1 we need)
 */
#include "x509.h"

/* ---- ASN.1 DER tag constants ----------------------------------- */

#define ASN1_BOOLEAN    0x01
#define ASN1_INTEGER    0x02
#define ASN1_BITSTRING  0x03
#define ASN1_OCTSTRING  0x04
#define ASN1_NULL       0x05
#define ASN1_OID        0x06
#define ASN1_UTF8STR    0x0C
#define ASN1_PRINTSTR   0x13
#define ASN1_UTCTIME    0x17
#define ASN1_SEQUENCE   0x30
#define ASN1_SET        0x31
#define ASN1_CTX0       0xA0     /* [0] EXPLICIT */
#define ASN1_CTX3       0xA3     /* [3] EXPLICIT */

/* ---- OIDs (just the body bytes after the OID tag/len) ---------- */

/* 1.3.101.112 — Ed25519 (RFC 8410). First two arcs collapse to
 * 1*40+3 = 43 = 0x2B; remaining arcs use base-128 encoding with
 * MSB-set continuation. 101 = 0x65 single byte; 112 = 0x70 single. */
static const uint8_t OID_ED25519[] = { 0x2B, 0x65, 0x70 };

/* 2.5.4.3 — id-at-commonName. */
static const uint8_t OID_CN[]      = { 0x55, 0x04, 0x03 };

/* ---- Backward DER writer --------------------------------------- */

/* Tracks "how many bytes written from the right edge of buf".
 * Content lives in buf[(cap - pos) .. cap). */
typedef struct {
    uint8_t *buf;
    int      cap;
    int      pos;
    int      overflow;     /* 1 if any write would have gone OOB */
} dw_t;

static void dw_init(dw_t *w, uint8_t *buf, int cap) {
    w->buf = buf; w->cap = cap; w->pos = 0; w->overflow = 0;
}

static inline void dw_put(dw_t *w, uint8_t b) {
    if (w->pos >= w->cap) { w->overflow = 1; return; }
    w->pos++;
    w->buf[w->cap - w->pos] = b;
}

/* Write n bytes preserving their natural left-to-right order.
 * Caller passes src in normal order; we write the LAST byte first
 * so they end up forward-aligned in the output. */
static void dw_raw(dw_t *w, const uint8_t *src, int n) {
    for (int i = n - 1; i >= 0; i--) dw_put(w, src[i]);
}

/* DER length encoding:
 *   0..127       -> single byte
 *   128..255     -> 0x81 NN
 *   256..65535   -> 0x82 HH LL
 * (Cert sizes never need 3+ length bytes.) */
static void dw_len(dw_t *w, int n) {
    if (n < 0x80) {
        dw_put(w, (uint8_t)n);
    } else if (n < 0x100) {
        dw_put(w, (uint8_t)n);
        dw_put(w, 0x81);
    } else {
        dw_put(w, (uint8_t)(n & 0xFF));
        dw_put(w, (uint8_t)((n >> 8) & 0xFF));
        dw_put(w, 0x82);
    }
}

/* Snapshot the current write position; pair with dw_wrap() to
 * emit a tag+length around content already written between
 * dw_wrap's call and the snapshot. */
static int dw_snap(dw_t *w) { return w->pos; }

static void dw_wrap(dw_t *w, uint8_t tag, int snap) {
    int content_len = w->pos - snap;
    dw_len(w, content_len);
    dw_put(w, tag);
}

/* ---- Higher-level emitters ------------------------------------- */

/* INTEGER: emits a positive integer up to 4 bytes wide. DER
 * requires the high bit of the first content byte to be 0 for
 * positive numbers; if it's set we prepend a 0x00 byte. */
static void der_int_u32(dw_t *w, uint32_t v) {
    int s = dw_snap(w);
    /* Write big-endian, stripping leading zero bytes. */
    uint8_t b[5];
    int n = 0;
    do { b[n++] = (uint8_t)(v & 0xFF); v >>= 8; } while (v);
    /* If MSB of top byte set, prepend a 0x00 to keep it positive. */
    if (b[n-1] & 0x80) b[n++] = 0x00;
    /* Write in BE order: top byte goes leftmost, so write last-first
     * in our backward-writer. b[0] is the LE-low byte = BE-rightmost. */
    for (int i = 0; i < n; i++) dw_put(w, b[i]);
    dw_wrap(w, ASN1_INTEGER, s);
}

/* OID: wraps the body bytes with the OID tag. */
static void der_oid(dw_t *w, const uint8_t *body, int n) {
    int s = dw_snap(w);
    dw_raw(w, body, n);
    dw_wrap(w, ASN1_OID, s);
}

/* SubjectPublicKeyInfo for Ed25519:
 *   SEQUENCE {
 *     SEQUENCE { OID 1.3.101.112 }     -- algorithm
 *     BIT STRING { 0x00, raw_pk[32] }  -- subjectPublicKey
 *   } */
static void der_spki_ed25519(dw_t *w, const uint8_t pk[32]) {
    int outer = dw_snap(w);

    /* BIT STRING wrapping pk[32]. The first content byte is the
     * "number of unused bits" — always 0 for whole-byte keys. */
    int bs = dw_snap(w);
    dw_raw(w, pk, 32);
    dw_put(w, 0x00);
    dw_wrap(w, ASN1_BITSTRING, bs);

    /* AlgorithmIdentifier SEQUENCE { OID ed25519 } — RFC 8410
     * specifies NO parameters field for Ed25519. */
    int alg = dw_snap(w);
    der_oid(w, OID_ED25519, sizeof(OID_ED25519));
    dw_wrap(w, ASN1_SEQUENCE, alg);

    dw_wrap(w, ASN1_SEQUENCE, outer);
}

/* Name: SEQUENCE { SET { SEQUENCE { OID cn, PrintableString cn } } }.
 * Single-RDN, single-attribute — the minimum that's well-formed. */
static void der_name_cn(dw_t *w, const char *cn) {
    int n = 0; while (cn[n]) n++;

    int outer = dw_snap(w);
    int set   = dw_snap(w);
    int seq   = dw_snap(w);

    /* PrintableString cn */
    int s = dw_snap(w);
    for (int i = n - 1; i >= 0; i--) dw_put(w, (uint8_t)cn[i]);
    dw_wrap(w, ASN1_PRINTSTR, s);

    /* OID 2.5.4.3 (commonName) */
    der_oid(w, OID_CN, sizeof(OID_CN));

    dw_wrap(w, ASN1_SEQUENCE, seq);
    dw_wrap(w, ASN1_SET,      set);
    dw_wrap(w, ASN1_SEQUENCE, outer);
}

/* Validity: SEQUENCE { UTCTime notBefore, UTCTime notAfter }.
 * UTCTime format: "YYMMDDHHMMSSZ" — 13 ASCII chars. We hardcode
 * 2026-01-01 00:00:00Z .. 2036-01-01 00:00:00Z (10 years). */
static void der_validity_2026_2036(dw_t *w) {
    static const char NB[] = "260101000000Z";
    static const char NA[] = "360101000000Z";

    int outer = dw_snap(w);

    int s2 = dw_snap(w);
    for (int i = 12; i >= 0; i--) dw_put(w, (uint8_t)NA[i]);
    dw_wrap(w, ASN1_UTCTIME, s2);

    int s1 = dw_snap(w);
    for (int i = 12; i >= 0; i--) dw_put(w, (uint8_t)NB[i]);
    dw_wrap(w, ASN1_UTCTIME, s1);

    dw_wrap(w, ASN1_SEQUENCE, outer);
}

/* AlgorithmIdentifier { OID ed25519 } — used both inside TBS
 * (signature field) and outside TBS (signatureAlgorithm). */
static void der_alg_ed25519(dw_t *w) {
    int s = dw_snap(w);
    der_oid(w, OID_ED25519, sizeof(OID_ED25519));
    dw_wrap(w, ASN1_SEQUENCE, s);
}

/* ---- TBSCertificate -------------------------------------------- */

/* Build the TBS (To-Be-Signed) cert structure. Returns the start
 * pointer of the TBS bytes (inside w->buf) and writes its length
 * into *tbs_len. The TBS is what gets signed. */
static const uint8_t *build_tbs(dw_t *w, const uint8_t pk[32],
                                const char *cn, int *tbs_len) {
    int tbs = dw_snap(w);

    /* extensions [3] EXPLICIT — omit (optional, fine for v3). */

    /* subjectPublicKeyInfo */
    der_spki_ed25519(w, pk);

    /* subject Name (= CN, since self-signed) */
    der_name_cn(w, cn);

    /* validity */
    der_validity_2026_2036(w);

    /* issuer Name (= CN, since self-signed) */
    der_name_cn(w, cn);

    /* signature AlgorithmIdentifier (must match outer
     * signatureAlgorithm — RFC 5280 §4.1.1.2) */
    der_alg_ed25519(w);

    /* serialNumber: arbitrary positive INTEGER. Use 1. */
    der_int_u32(w, 1);

    /* version [0] EXPLICIT INTEGER 2 (= v3). */
    int vc = dw_snap(w);
    der_int_u32(w, 2);
    dw_wrap(w, ASN1_CTX0, vc);

    dw_wrap(w, ASN1_SEQUENCE, tbs);

    *tbs_len = w->pos - tbs;
    return w->buf + (w->cap - w->pos);
}

/* ---- Public API ------------------------------------------------- */

int x509_build_self_signed_ed25519(
    const uint8_t pk[32],
    const uint8_t sk[64],
    const char *cn,
    uint8_t *out, int out_cap)
{
    /* Two-pass: first build TBS into a scratch buffer to compute
     * its bytes (so we can sign), then build the final cert that
     * wraps TBS + signatureAlgorithm + signatureValue.
     *
     * Could be done in one pass since DER is canonical and TBS
     * bytes don't depend on the signature, but two passes keeps
     * the code legible. */
    uint8_t scratch[X509_MAX_CERT];
    dw_t s; dw_init(&s, scratch, sizeof(scratch));
    int tbs_len;
    const uint8_t *tbs_ptr = build_tbs(&s, pk, cn, &tbs_len);
    if (s.overflow) return -1;

    /* Sign the TBS bytes with Ed25519. The signature is over the
     * TLV-wrapped TBS SEQUENCE — i.e., including the outer SEQUENCE
     * tag and length, NOT just the content. Conveniently that's
     * exactly what build_tbs returned. */
    uint8_t sig[64];
    ed25519_sign(sig, tbs_ptr, tbs_len, sk);

    /* Now assemble the final Certificate SEQUENCE backward into
     * the caller's buffer. */
    dw_t w; dw_init(&w, out, out_cap);
    int outer = dw_snap(&w);

    /* signatureValue BIT STRING { 0x00, sig[64] } */
    int bs = dw_snap(&w);
    dw_raw(&w, sig, 64);
    dw_put(&w, 0x00);                  /* unused-bits count */
    dw_wrap(&w, ASN1_BITSTRING, bs);

    /* signatureAlgorithm SEQUENCE { OID ed25519 } */
    der_alg_ed25519(&w);

    /* tbsCertificate — copy the bytes we already built in scratch. */
    dw_raw(&w, tbs_ptr, tbs_len);

    /* Wrap it all in the outer Certificate SEQUENCE. */
    dw_wrap(&w, ASN1_SEQUENCE, outer);

    if (w.overflow) return -1;

    /* Compact: shift the cert bytes from the right edge of `out`
     * down to out[0..len). */
    int len = w.pos;
    int src_off = out_cap - len;
    if (src_off > 0) {
        for (int i = 0; i < len; i++) out[i] = out[src_off + i];
    }
    return len;
}
