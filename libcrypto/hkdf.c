/*
 * HMAC-SHA-256 (RFC 2104) and HKDF (RFC 5869), plus TLS 1.3's
 * HKDF-Expand-Label and Derive-Secret helpers (RFC 8446 §7.1).
 *
 * The TLS 1.3 key schedule is:
 *
 *   PSK or 0  --HKDF-Extract-->  Early Secret
 *                                      |
 *                                Derive-Secret(es, "derived", "")
 *                                      |
 *   ECDHE shared --HKDF-Extract->  Handshake Secret
 *                                      |
 *                                Derive-Secret(hs, "derived", "")
 *                                      |
 *   0              --HKDF-Extract-> Master Secret
 *
 * Each Secret derives traffic keys via further Derive-Secret /
 * HKDF-Expand-Label calls. We implement the primitives; the TLS
 * layer applies them with the right labels and transcripts.
 */
#include "crypto.h"

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[32]) {
    uint8_t k[SHA256_BLOCK_SIZE] = {0};
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k);
    } else {
        for (size_t i = 0; i < key_len; i++) k[i] = key[i];
    }

    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }

    /* inner = SHA256(ipad || msg) */
    struct sha256 s;
    uint8_t       inner[32];
    sha256_init(&s);
    sha256_update(&s, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&s, msg,  msg_len);
    sha256_final(&s, inner);

    /* outer = SHA256(opad || inner) */
    sha256_init(&s);
    sha256_update(&s, opad, SHA256_BLOCK_SIZE);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm,  size_t ikm_len,
                  uint8_t prk[32]) {
    /* RFC 5869: PRK = HMAC(salt, IKM). If salt is NULL/empty, use
     * a zero-filled string of length HashLen. */
    static const uint8_t zero_salt[32] = {0};
    if (!salt || salt_len == 0) { salt = zero_salt; salt_len = 32; }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void hkdf_expand(const uint8_t *prk,  size_t prk_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len) {
    /* RFC 5869: T(0) = empty, T(i) = HMAC(prk, T(i-1) || info || i).
     * okm = T(1) || T(2) || ... truncated to L bytes.
     *
     * The trick is concatenating T(i-1) || info || ctr into one
     * contiguous buffer that hmac_sha256 takes as the "msg". With
     * info up to 64 bytes (TLS 1.3 HkdfLabel max), 32+64+1 = 97
     * bytes is the largest msg we'll ever need. We size larger to
     * be safe for non-TLS callers. */
    uint8_t  t[32];
    size_t   t_len = 0;
    uint8_t  ctr   = 1;
    size_t   done  = 0;
    uint8_t  msg[32 + 320 + 1];   /* T + max-info + counter */

    while (done < okm_len) {
        size_t mp = 0;
        for (size_t i = 0; i < t_len; i++)    msg[mp++] = t[i];
        for (size_t i = 0; i < info_len && i < 320; i++)
            msg[mp++] = info[i];
        msg[mp++] = ctr;

        hmac_sha256(prk, prk_len, msg, mp, t);
        t_len = 32;

        size_t take = (okm_len - done < 32) ? (okm_len - done) : 32;
        for (size_t i = 0; i < take; i++) okm[done + i] = t[i];
        done += take;
        ctr++;
    }
}

void tls13_hkdf_expand_label(
    const uint8_t prk[32],
    const char *label,
    const uint8_t *context, size_t context_len,
    uint8_t *out, size_t out_len)
{
    /* RFC 8446 §7.1: HkdfLabel = struct {
     *     uint16 length;
     *     opaque label<7..255> = "tls13 " + Label;
     *     opaque context<0..255>;
     * };
     * Then HKDF-Expand(prk, HkdfLabel, length). */
    uint8_t info[2 + 1 + 6 + 64 + 1 + 256];
    size_t  pos = 0;

    /* uint16 length, big-endian */
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len);

    /* label vector: 1-byte length + bytes */
    size_t label_n = 0;
    while (label[label_n]) label_n++;
    if (label_n > 64) label_n = 64;
    uint8_t prefixed[6 + 64];
    prefixed[0] = 't'; prefixed[1] = 'l'; prefixed[2] = 's';
    prefixed[3] = '1'; prefixed[4] = '3'; prefixed[5] = ' ';
    for (size_t i = 0; i < label_n; i++) prefixed[6 + i] = (uint8_t)label[i];
    size_t plen = 6 + label_n;
    info[pos++] = (uint8_t)plen;
    for (size_t i = 0; i < plen; i++) info[pos++] = prefixed[i];

    /* context vector: 1-byte length + bytes */
    if (context_len > 255) context_len = 255;
    info[pos++] = (uint8_t)context_len;
    for (size_t i = 0; i < context_len; i++) info[pos++] = context[i];

    hkdf_expand(prk, 32, info, pos, out, out_len);
}

void tls13_derive_secret(
    const uint8_t prk[32],
    const char *label,
    const uint8_t *transcript_hash, size_t hash_len,
    uint8_t out[32])
{
    /* Derive-Secret(secret, label, msgs) =
     *   HKDF-Expand-Label(secret, label, Hash(msgs), Hash.length) */
    tls13_hkdf_expand_label(prk, label, transcript_hash, hash_len, out, 32);
}
