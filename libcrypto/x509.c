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

/* 1.2.840.10045.2.1 — ecPublicKey (RFC 5480 §2.1).
 *   1*40 + 2 = 42 = 0x2A;
 *   840 = 0x06 0x48 (128 + (8 << 7))   →  bytes 0x86 0x48 in base-128
 *   10045 = 0xCE 0x3D     →  0xCE | 0x80 = 0xCE..., then 0x3D
 *   2 = 0x02, 1 = 0x01 */
static const uint8_t OID_EC_PUBKEY[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };

/* 1.2.840.10045.3.1.7 — prime256v1 / secp256r1 curve identifier. */
static const uint8_t OID_PRIME256V1[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };

/* 1.2.840.10045.4.3.2 — ecdsa-with-SHA256. */
static const uint8_t OID_ECDSA_SHA256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };

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

/* ---- ECDSA-P256 cert (session 43) ------------------------------ */

/* SubjectPublicKeyInfo for ECDSA-P256 per RFC 5480 §2:
 *
 *   SEQUENCE {
 *     SEQUENCE {
 *       OID ecPublicKey,
 *       OID prime256v1            -- the curve parameters, named-curve form
 *     },
 *     BIT STRING {
 *       0x00, 0x04, X(32), Y(32)  -- uncompressed point, RFC 5480 §2.2
 *     }
 *   }
 *
 * The 0x04 byte is the SEC1 point-format tag for "uncompressed";
 * X and Y follow as fixed-width big-endian 32-byte fields.
 */
static void der_spki_p256(dw_t *w, const uint8_t pub[64]) {
    int outer = dw_snap(w);

    /* BIT STRING: unused-bits=0, then 0x04 || X || Y (65 bytes). */
    int bs = dw_snap(w);
    dw_raw(w, pub + 32, 32);      /* Y */
    dw_raw(w, pub,      32);      /* X */
    dw_put(w, 0x04);              /* uncompressed-point tag */
    dw_put(w, 0x00);              /* BIT STRING unused-bits count */
    dw_wrap(w, ASN1_BITSTRING, bs);

    /* AlgorithmIdentifier SEQUENCE { OID ecPublicKey, OID prime256v1 } */
    int alg = dw_snap(w);
    der_oid(w, OID_PRIME256V1,  sizeof(OID_PRIME256V1));
    der_oid(w, OID_EC_PUBKEY,   sizeof(OID_EC_PUBKEY));
    dw_wrap(w, ASN1_SEQUENCE, alg);

    dw_wrap(w, ASN1_SEQUENCE, outer);
}

/* AlgorithmIdentifier { OID ecdsa-with-SHA256 } for cert sig algs.
 * RFC 5758 §3.2: no parameters field — same as Ed25519. */
static void der_alg_ecdsa_sha256(dw_t *w) {
    int s = dw_snap(w);
    der_oid(w, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256));
    dw_wrap(w, ASN1_SEQUENCE, s);
}

static const uint8_t *build_tbs_p256(dw_t *w, const uint8_t pub[64],
                                     const char *cn, int *tbs_len)
{
    int tbs = dw_snap(w);

    /* extensions [3] EXPLICIT — omit (optional). */

    /* subjectPublicKeyInfo */
    der_spki_p256(w, pub);

    /* subject Name */
    der_name_cn(w, cn);

    /* validity 2026..2036 (reuse Ed25519 builder) */
    der_validity_2026_2036(w);

    /* issuer Name (self-signed) */
    der_name_cn(w, cn);

    /* signature AlgorithmIdentifier — must match outer one
     * (RFC 5280 §4.1.1.2). */
    der_alg_ecdsa_sha256(w);

    /* serialNumber */
    der_int_u32(w, 1);

    /* version [0] EXPLICIT INTEGER 2 (= v3) */
    int vc = dw_snap(w);
    der_int_u32(w, 2);
    dw_wrap(w, ASN1_CTX0, vc);

    dw_wrap(w, ASN1_SEQUENCE, tbs);

    *tbs_len = w->pos - tbs;
    return w->buf + (w->cap - w->pos);
}

int x509_build_self_signed_p256(
    const uint8_t pub[64],
    const uint8_t priv[32],
    const char *cn,
    uint8_t *out, int out_cap)
{
    /* Build TBS in scratch buffer so we can hash + sign it. */
    uint8_t scratch[X509_MAX_CERT];
    dw_t s; dw_init(&s, scratch, sizeof(scratch));
    int tbs_len;
    const uint8_t *tbs_ptr = build_tbs_p256(&s, pub, cn, &tbs_len);
    if (s.overflow) return -1;

    /* ECDSA-with-SHA256: hash TBS, sign hash, DER-encode (R, S). */
    uint8_t hash[32];
    sha256(tbs_ptr, tbs_len, hash);
    uint8_t raw_sig[64];
    if (p256_sign(raw_sig, hash, priv) != 0) return -1;
    uint8_t der_sig[72];
    int der_sig_len = p256_sig_to_der(der_sig, sizeof(der_sig), raw_sig);
    if (der_sig_len < 0) return -1;

    /* Outer Certificate SEQUENCE. */
    dw_t w; dw_init(&w, out, out_cap);
    int outer = dw_snap(&w);

    /* signatureValue: BIT STRING { 0x00, DER(r,s) } */
    int bs = dw_snap(&w);
    dw_raw(&w, der_sig, der_sig_len);
    dw_put(&w, 0x00);
    dw_wrap(&w, ASN1_BITSTRING, bs);

    /* signatureAlgorithm */
    der_alg_ecdsa_sha256(&w);

    /* tbsCertificate */
    dw_raw(&w, tbs_ptr, tbs_len);

    dw_wrap(&w, ASN1_SEQUENCE, outer);

    if (w.overflow) return -1;

    /* Compact down to out[0..len). */
    int len = w.pos;
    int src_off = out_cap - len;
    if (src_off > 0) {
        for (int i = 0; i < len; i++) out[i] = out[src_off + i];
    }
    return len;
}

/* ====================================================================
 *  Session 59 — parser + chain validation
 * ==================================================================== */

/* TLV reader: returns the tag byte, sets *vl to the value length, and
 * advances *p past the (tag, length) header so *p points at the value
 * bytes.  -1 on any truncation / unsupported long-form length. Mirror
 * of the helper in tls.c — kept private to each unit. */
static int dr_read_tlv(const uint8_t **p, const uint8_t *end, int *vl) {
    if (*p >= end) return -1;
    int tag = *(*p)++;
    if (*p >= end) return -1;
    int b = *(*p)++;
    int len;
    if ((b & 0x80) == 0) {
        len = b;
    } else {
        int nlen = b & 0x7F;
        if (nlen == 0 || nlen > 3 || *p + nlen > end) return -1;
        len = 0;
        for (int i = 0; i < nlen; i++) len = (len << 8) | *(*p)++;
    }
    if (*p + len > end) return -1;
    *vl = len;
    return tag;
}

/* Cumulative days at the start of each month (non-leap year). Used by
 * the validity-date parser below. */
static const int MONTH_DAYS[12] = {
      0,  31,  59,  90, 120, 151, 181,
    212, 243, 273, 304, 334
};

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Convert (Y, M, D, h, m, s) (UTC) → Unix epoch seconds. Y is the full
 * 4-digit year, M is 1..12, D is 1..31. */
static uint32_t civil_to_epoch(int Y, int M, int D, int h, int mn, int s) {
    /* Days since epoch start. */
    long days = 0;
    for (int y = 1970; y < Y; y++) days += is_leap(y) ? 366 : 365;
    days += MONTH_DAYS[M - 1];
    if (M > 2 && is_leap(Y)) days += 1;
    days += (D - 1);
    return (uint32_t)(days * 86400L + h * 3600 + mn * 60 + s);
}

/* Read N ASCII digits at *p, advance past them. Returns 0 on success. */
static int read_digits(const uint8_t **p, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = (*p)[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return 0;
}

/* Parse one UTCTime ("YYMMDDHHMMSSZ", 13 chars) or GeneralizedTime
 * ("YYYYMMDDHHMMSSZ", 15 chars) into epoch seconds. RFC 5280 §4.1.2.5
 * mandates UTCTime for dates < 2050, GeneralizedTime for ≥ 2050. */
static int parse_x509_time(const uint8_t *bytes, int len, uint32_t *out) {
    int Y, M, D, h, m, s;
    const uint8_t *p = bytes;
    const uint8_t *end = bytes + len;
    if (len == 13) {
        /* UTCTime: 2-digit year. RFC 5280: YY < 50 → 20YY, else 19YY. */
        int yy;
        if (read_digits(&p, 2, &yy) < 0) return -1;
        Y = (yy < 50) ? (2000 + yy) : (1900 + yy);
    } else if (len == 15) {
        /* GeneralizedTime: 4-digit year. */
        if (read_digits(&p, 4, &Y) < 0) return -1;
    } else {
        return -1;
    }
    if (read_digits(&p, 2, &M) < 0) return -1;
    if (read_digits(&p, 2, &D) < 0) return -1;
    if (read_digits(&p, 2, &h) < 0) return -1;
    if (read_digits(&p, 2, &m) < 0) return -1;
    if (read_digits(&p, 2, &s) < 0) return -1;
    if (p != end - 1 || *p != 'Z') return -1;
    if (M < 1 || M > 12 || D < 1 || D > 31) return -1;
    *out = civil_to_epoch(Y, M, D, h, m, s);
    return 0;
}

/* OID byte signatures we recognize. */
static const uint8_t OID_RSA_SHA256[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};
static const uint8_t OID_RSA_ENCRYPTION[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
};

static int oid_eq(const uint8_t *p, int n, const uint8_t *oid, int oid_len) {
    if (n != oid_len) return 0;
    for (int i = 0; i < n; i++) if (p[i] != oid[i]) return 0;
    return 1;
}

/* Walk an AlgorithmIdentifier SEQUENCE { OID, [params] } at *p, return
 * X509_SIG_* / X509_PK_* (depending on context) for the OID, or 0 if
 * not recognized. Advances *p past the algorithm SEQ. */
static int parse_alg_id(const uint8_t **p, const uint8_t *end, int *sig_or_pk) {
    int vl;
    if (dr_read_tlv(p, end, &vl) != ASN1_SEQUENCE) return -1;
    const uint8_t *alg_end = *p + vl;
    int otag = dr_read_tlv(p, alg_end, &vl);
    if (otag != ASN1_OID) return -1;
    const uint8_t *oid_bytes = *p;
    int oid_len = vl;
    /* Skip past the OID and any parameters (curve params, NULL, etc.) */
    *p = alg_end;

    /* Compare against known OIDs. Same list serves both signature-alg
     * and public-key-alg lookups since both AlgorithmIdentifiers reuse
     * the OID namespace. */
    if (oid_eq(oid_bytes, oid_len, OID_ED25519, sizeof(OID_ED25519))) {
        *sig_or_pk = 1;     /* X509_SIG_ED25519 == X509_PK_ED25519 == 1 */
        return 0;
    }
    if (oid_eq(oid_bytes, oid_len, OID_EC_PUBKEY, sizeof(OID_EC_PUBKEY))) {
        *sig_or_pk = X509_PK_P256;
        return 0;
    }
    if (oid_eq(oid_bytes, oid_len, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) {
        *sig_or_pk = X509_SIG_ECDSA_SHA256;
        return 0;
    }
    if (oid_eq(oid_bytes, oid_len, OID_RSA_SHA256, sizeof(OID_RSA_SHA256))) {
        *sig_or_pk = X509_SIG_RSA_SHA256;
        return 0;
    }
    if (oid_eq(oid_bytes, oid_len, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION))) {
        *sig_or_pk = X509_PK_RSA;
        return 0;
    }
    return -1;
}

/* RSA public key inside SubjectPublicKeyInfo's BIT STRING:
 *   RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
 *
 * Pulls the two INTEGERs out as bignum n + e. */
static int parse_rsa_pubkey(const uint8_t *bs_bytes, int bs_len,
                            bignum *n_out, bignum *e_out) {
    const uint8_t *p = bs_bytes;
    const uint8_t *end = bs_bytes + bs_len;
    int vl;
    if (dr_read_tlv(&p, end, &vl) != ASN1_SEQUENCE) return -1;
    const uint8_t *seq_end = p + vl;
    if (dr_read_tlv(&p, seq_end, &vl) != ASN1_INTEGER) return -1;
    /* INTEGER may be padded with a leading 0x00 to mark it positive
     * — strip it before reading as a magnitude. */
    const uint8_t *n_bytes = p;
    int n_len = vl;
    if (n_len > 1 && n_bytes[0] == 0x00) { n_bytes++; n_len--; }
    bn_from_bytes_be(n_out, n_bytes, n_len);
    p += vl;
    if (dr_read_tlv(&p, seq_end, &vl) != ASN1_INTEGER) return -1;
    const uint8_t *e_bytes = p;
    int e_len = vl;
    if (e_len > 1 && e_bytes[0] == 0x00) { e_bytes++; e_len--; }
    bn_from_bytes_be(e_out, e_bytes, e_len);
    return 0;
}

int x509_parse_cert(const uint8_t *der, int der_len, struct x509_cert *out) {
    if (!der || !out || der_len < 8) return -1;
    /* Zero-out so callers can safely read fields we don't set on
     * failure. */
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    const uint8_t *p   = der;
    const uint8_t *end = der + der_len;
    out->der     = der;
    out->der_len = der_len;

    int vl;
    /* Outer Certificate SEQUENCE */
    if (dr_read_tlv(&p, end, &vl) != ASN1_SEQUENCE) return -1;
    const uint8_t *cert_end = p + vl;
    if (cert_end > end) return -1;

    /* TBS: we need to snapshot the WHOLE tbsCertificate, including
     * its leading SEQUENCE tag + length bytes — that's what the
     * issuer hashed before signing. */
    const uint8_t *tbs_start = p;
    if (dr_read_tlv(&p, cert_end, &vl) != ASN1_SEQUENCE) return -1;
    const uint8_t *tbs_inner_end = p + vl;
    out->tbs     = tbs_start;
    out->tbs_len = (int)((tbs_inner_end) - tbs_start);

    /* [0] EXPLICIT version (optional). */
    if (p < tbs_inner_end && p[0] == ASN1_CTX0) {
        if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_CTX0) return -1;
        p += vl;
    }
    /* serialNumber INTEGER — skip. */
    if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_INTEGER) return -1;
    p += vl;
    /* signature AlgorithmIdentifier (inside TBS; should match the outer
     * one but we don't enforce that). */
    {
        int sig_inner;
        const uint8_t *save = p;
        if (parse_alg_id(&p, tbs_inner_end, &sig_inner) < 0) {
            /* Some certs use OIDs we don't grok here yet; skip. */
            p = save;
            if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_SEQUENCE) return -1;
            p += vl;
        }
    }
    /* issuer Name SEQUENCE — capture whole bytes for DN comparison. */
    {
        const uint8_t *issuer_start = p;
        if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_SEQUENCE) return -1;
        const uint8_t *issuer_end = p + vl;
        out->issuer.bytes = issuer_start;
        out->issuer.len   = (int)(issuer_end - issuer_start);
        p = issuer_end;
    }
    /* validity Validity SEQUENCE { notBefore Time, notAfter Time } */
    {
        if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_SEQUENCE) return -1;
        const uint8_t *val_end = p + vl;
        int t_tag;
        t_tag = dr_read_tlv(&p, val_end, &vl);
        if (t_tag != ASN1_UTCTIME && t_tag != 0x18 /* GeneralizedTime */) return -1;
        if (parse_x509_time(p, vl, &out->not_before) < 0) return -1;
        p += vl;
        t_tag = dr_read_tlv(&p, val_end, &vl);
        if (t_tag != ASN1_UTCTIME && t_tag != 0x18) return -1;
        if (parse_x509_time(p, vl, &out->not_after) < 0) return -1;
        p = val_end;
    }
    /* subject Name — capture as DN slice. */
    {
        const uint8_t *subj_start = p;
        if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_SEQUENCE) return -1;
        const uint8_t *subj_end = p + vl;
        out->subject.bytes = subj_start;
        out->subject.len   = (int)(subj_end - subj_start);
        p = subj_end;
    }
    /* subjectPublicKeyInfo SEQUENCE { algorithm SEQUENCE, subjectPublicKey BIT STRING } */
    {
        if (dr_read_tlv(&p, tbs_inner_end, &vl) != ASN1_SEQUENCE) return -1;
        const uint8_t *spki_end = p + vl;
        int alg_id;
        if (parse_alg_id(&p, spki_end, &alg_id) < 0) return -1;
        out->pk_type = alg_id;
        /* BIT STRING */
        if (dr_read_tlv(&p, spki_end, &vl) != ASN1_BITSTRING) return -1;
        if (vl < 1 || p[0] != 0x00) return -1;     /* unused-bits = 0 */
        int kb = vl - 1;

        if (alg_id == X509_PK_ED25519) {
            if (kb != 32 || kb > (int)sizeof(out->pk_bytes)) return -1;
            for (int i = 0; i < 32; i++) out->pk_bytes[i] = p[1 + i];
            out->pk_len = 32;
        } else if (alg_id == X509_PK_P256) {
            if (kb != 65 || p[1] != 0x04 || kb > (int)sizeof(out->pk_bytes)) return -1;
            for (int i = 0; i < 65; i++) out->pk_bytes[i] = p[1 + i];
            out->pk_len = 65;
        } else if (alg_id == X509_PK_RSA) {
            if (parse_rsa_pubkey(p + 1, kb, &out->rsa_n, &out->rsa_e) < 0) return -1;
            out->pk_len = bn_byte_length(&out->rsa_n);
            /* Cache the modulus in pk_bytes too, for logging. Truncate
             * if it doesn't fit. */
            int cn = out->pk_len > (int)sizeof(out->pk_bytes) ? (int)sizeof(out->pk_bytes) : out->pk_len;
            bn_to_bytes_be(&out->rsa_n, out->pk_bytes, cn);
        } else {
            return -1;
        }
        p = spki_end;
    }
    /* Skip any optional issuerUniqueID, subjectUniqueID, extensions —
     * we don't validate them today. */
    p = tbs_inner_end;

    /* signatureAlgorithm AlgorithmIdentifier — must be a sig OID. */
    {
        int sig_alg;
        if (parse_alg_id(&p, cert_end, &sig_alg) < 0) return -1;
        out->sig_alg = sig_alg;
    }
    /* signature BIT STRING */
    {
        if (dr_read_tlv(&p, cert_end, &vl) != ASN1_BITSTRING) return -1;
        if (vl < 1 || p[0] != 0x00) return -1;
        out->sig     = p + 1;
        out->sig_len = vl - 1;
    }
    return 0;
}

int x509_extract_pubkey(const uint8_t *cert, int cert_len,
                        int *out_tls_alg,
                        uint8_t *out_pk, int *out_pk_len) {
    struct x509_cert c;
    if (x509_parse_cert(cert, cert_len, &c) < 0) return -1;
    /* Map X509_PK_* to TLS sig_alg codes for backward compat with
     * libcrypto/tls.c's older callers. */
    if (c.pk_type == X509_PK_ED25519) {
        *out_tls_alg = 0x0807;
        for (int i = 0; i < c.pk_len; i++) out_pk[i] = c.pk_bytes[i];
        *out_pk_len = c.pk_len;
    } else if (c.pk_type == X509_PK_P256) {
        *out_tls_alg = 0x0403;
        for (int i = 0; i < c.pk_len; i++) out_pk[i] = c.pk_bytes[i];
        *out_pk_len = c.pk_len;
    } else if (c.pk_type == X509_PK_RSA) {
        *out_tls_alg = 0x0401;
        /* RSA pubkey caller-side is the (n, e) pair — we don't fit
         * the canonical TLS DER blob in a flat byte array here. The
         * cert chain path uses x509_parse_cert directly. */
        *out_pk_len = c.pk_len;
        for (int i = 0; i < c.pk_len && i < 256; i++) out_pk[i] = c.pk_bytes[i];
    } else {
        return -1;
    }
    return 0;
}

/* ---- Signature verification ----------------------------------- */

/* Hash the TBS bytes the issuer signed, then run the appropriate
 * verifier against `issuer`'s public key. Returns 0 on success. */
static int verify_cert_signature(const struct x509_cert *leaf,
                                  const struct x509_cert *issuer) {
    uint8_t hash[32];
    sha256(leaf->tbs, leaf->tbs_len, hash);

    if (leaf->sig_alg == X509_SIG_ED25519) {
        if (issuer->pk_type != X509_PK_ED25519) return -1;
        if (leaf->sig_len != 64) return -1;
        return ed25519_verify(leaf->sig, leaf->tbs, leaf->tbs_len,
                              issuer->pk_bytes);
    }
    if (leaf->sig_alg == X509_SIG_ECDSA_SHA256) {
        if (issuer->pk_type != X509_PK_P256) return -1;
        /* ECDSA sig in cert is DER-encoded; convert to raw R||S. */
        uint8_t rs[64];
        if (p256_sig_from_der(rs, leaf->sig, leaf->sig_len) != 0) return -1;
        /* issuer pk is 65 bytes (0x04 || X || Y); strip the prefix. */
        return p256_verify(rs, hash, issuer->pk_bytes + 1);
    }
    if (leaf->sig_alg == X509_SIG_RSA_SHA256) {
        if (issuer->pk_type != X509_PK_RSA) return -1;
        return rsa_verify_pkcs1_sha256(leaf->tbs, leaf->tbs_len,
                                       leaf->sig, leaf->sig_len,
                                       &issuer->rsa_n, &issuer->rsa_e);
    }
    return -1;
}

static int dn_eq(const struct x509_dn *a, const struct x509_dn *b) {
    if (a->len != b->len) return 0;
    for (int i = 0; i < a->len; i++) if (a->bytes[i] != b->bytes[i]) return 0;
    return 1;
}

/* ---- CA store -------------------------------------------------- */

void ca_store_init(struct ca_store *s) {
    s->n_roots = 0;
    s->storage_used = 0;
    /* Don't bother zeroing roots[] — n_roots gates iteration. */
}

int ca_store_add(struct ca_store *s, const uint8_t *der, int der_len) {
    if (!s || !der || der_len <= 0) return -1;
    if (s->n_roots >= CA_STORE_MAX_CERTS) return -1;
    if (s->storage_used + der_len > (int)sizeof(s->roots_storage)) return -1;
    /* Copy DER into our backing store so the cert struct's pointers
     * stay live regardless of where the caller's buffer came from. */
    uint8_t *dst = s->roots_storage + s->storage_used;
    for (int i = 0; i < der_len; i++) dst[i] = der[i];
    s->storage_used += der_len;
    if (x509_parse_cert(dst, der_len, &s->roots[s->n_roots]) < 0) {
        /* Bad parse — give back the storage we just claimed. */
        s->storage_used -= der_len;
        return -1;
    }
    s->n_roots++;
    return 0;
}

int ca_store_load_from_etc_ssl(struct ca_store *s) {
    int loaded = 0;
    int iter = 0;
    char name[16];
    /* Walk /etc/ssl entries; if a file's name ends in ".crt" or ".der",
     * read it (must be DER) and stuff into the store.  Errors on
     * individual files are non-fatal — we report the count of
     * SUCCESSFUL loads. */
    while (sys_readdir("/etc/ssl", &iter, name) >= 0) {
        int nl = 0; while (name[nl]) nl++;
        if (nl < 4) continue;
        if (!((name[nl-4] == '.' && name[nl-3] == 'c' && name[nl-2] == 'r' && name[nl-1] == 't') ||
              (name[nl-4] == '.' && name[nl-3] == 'd' && name[nl-2] == 'e' && name[nl-1] == 'r')))
            continue;
        char path[64];
        int np = 0;
        const char *pref = "/etc/ssl/";
        while (pref[np-(np?0:0)] && np < 9) { path[np] = pref[np]; np++; }
        for (int i = 0; i < nl && np < 60; i++) path[np++] = name[i];
        path[np] = 0;
        int fd = sys_open(path);
        if (fd < 0) continue;
        static uint8_t buf[X509_MAX_CERT];
        int n = sys_read(fd, buf, sizeof(buf));
        sys_close(fd);
        if (n <= 0) continue;
        if (ca_store_add(s, buf, n) == 0) loaded++;
    }
    return loaded;
}

/* ---- Chain verification --------------------------------------- */

int x509_verify_chain(const uint8_t *leaf_der, int leaf_der_len,
                      const uint8_t *const *intermediates,
                      const int *intermediate_lens,
                      int n_intermediates,
                      const struct ca_store *store,
                      uint32_t now) {
    struct x509_cert leaf;
    if (x509_parse_cert(leaf_der, leaf_der_len, &leaf) < 0) return -1;

    /* Validate the leaf's own validity dates before doing any
     * signature work. */
    if (now != 0) {
        if (now < leaf.not_before || now > leaf.not_after) return -1;
    }

    const struct x509_cert *current = &leaf;
    struct x509_cert inter_parsed[8];
    if (n_intermediates > 8) return -1;
    for (int i = 0; i < n_intermediates; i++) {
        if (x509_parse_cert(intermediates[i], intermediate_lens[i],
                            &inter_parsed[i]) < 0) return -1;
        if (now != 0) {
            if (now < inter_parsed[i].not_before ||
                now > inter_parsed[i].not_after) return -1;
        }
        /* Each intermediate must be issued to (subject ==) the current
         * issuer of the prior cert. */
        if (!dn_eq(&current->issuer, &inter_parsed[i].subject)) return -1;
        /* And it must have signed the prior cert. */
        if (verify_cert_signature(current, &inter_parsed[i]) != 0) return -1;
        current = &inter_parsed[i];
    }

    /* Final step: `current`'s issuer must match some root's subject,
     * and that root must have signed `current`. */
    for (int i = 0; i < store->n_roots; i++) {
        const struct x509_cert *root = &store->roots[i];
        if (!dn_eq(&current->issuer, &root->subject)) continue;
        if (verify_cert_signature(current, root) == 0) return 0;
    }
    return -1;
}
