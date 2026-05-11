/*
 * libcrypto/ssh.c — wire-format helpers + AES-GCM packet codec + KDF
 * for the AdventOS SSH-2 transport layer.
 *
 * The protocol state machine lives in user/sshd.c (server side) and
 * user/ssh.c (client side); this file is purely "encode/decode/derive".
 */
#include "ssh.h"
#include "crypto.h"

/* ---- byte-pushers ------------------------------------------------- */

void ssh_put_u8(uint8_t **out, uint8_t v) {
    *(*out)++ = v;
}

void ssh_put_u32(uint8_t **out, uint32_t v) {
    (*out)[0] = (uint8_t)(v >> 24);
    (*out)[1] = (uint8_t)(v >> 16);
    (*out)[2] = (uint8_t)(v >>  8);
    (*out)[3] = (uint8_t)(v);
    *out += 4;
}

void ssh_put_bytes(uint8_t **out, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) (*out)[i] = p[i];
    *out += n;
}

void ssh_put_string(uint8_t **out, const void *data, size_t n) {
    ssh_put_u32(out, (uint32_t)n);
    ssh_put_bytes(out, data, n);
}

void ssh_put_cstring(uint8_t **out, const char *s) {
    size_t n = 0; while (s[n]) n++;
    ssh_put_string(out, s, n);
}

void ssh_put_bool(uint8_t **out, int v) {
    ssh_put_u8(out, v ? 1 : 0);
}

/* mpint encoding (RFC 4251 §5):
 *   - Two's-complement big-endian, variable length.
 *   - Positive numbers whose MSB is set get a leading 0x00 byte to
 *     keep them positive.
 *   - Zero encodes as a 0-length string.
 *   - Leading 0x00 bytes are stripped for non-zero values.
 *
 * Input is an unsigned big-endian integer with arbitrary leading zeros
 * (e.g., the X25519 shared secret is 32 bytes — leading zero common). */
void ssh_put_mpint(uint8_t **out, const uint8_t *be, size_t n) {
    /* Strip leading zero bytes. */
    size_t i = 0;
    while (i < n && be[i] == 0) i++;
    size_t body = n - i;

    if (body == 0) {
        ssh_put_u32(out, 0);
        return;
    }

    int pad = (be[i] & 0x80) ? 1 : 0;     /* MSB set → prepend 0x00 */
    ssh_put_u32(out, (uint32_t)(body + pad));
    if (pad) ssh_put_u8(out, 0);
    ssh_put_bytes(out, be + i, body);
}

/* ---- byte-pullers ------------------------------------------------- */

uint8_t ssh_get_u8(const uint8_t **p) {
    return *(*p)++;
}

uint32_t ssh_get_u32(const uint8_t **p) {
    uint32_t v = ((uint32_t)(*p)[0] << 24) |
                 ((uint32_t)(*p)[1] << 16) |
                 ((uint32_t)(*p)[2] <<  8) |
                 ((uint32_t)(*p)[3]);
    *p += 4;
    return v;
}

const uint8_t *ssh_get_string(const uint8_t **p, const uint8_t *end,
                              uint32_t *out_len) {
    if (end - *p < 4) return 0;
    uint32_t len = ssh_get_u32(p);
    if ((size_t)(end - *p) < len) return 0;
    const uint8_t *data = *p;
    *p += len;
    *out_len = len;
    return data;
}

/* ---- AES-128-GCM packet codec (RFC 5647) ------------------------- */

/* On wire: packet_length(4) || ciphertext(packet_length) || tag(16).
 * packet_length is AAD; ciphertext = encrypt(padding_length || payload || padding).
 *
 * Padding sized so (1 + payload + padding) is a multiple of 16 with
 * at least 4 bytes of padding (RFC 4253 §6, RFC 5647 §6).
 *
 * We assemble + encrypt in place inside the caller's `out` buffer
 * so this routine has no static state. Caller MUST size `out` for
 * 4 + (1+payload+max_pad) + 16 = payload + 36 bytes worst case.
 * (Use SSH_MAX_PACKET for any payload our protocol generates.) */
int ssh_packet_seal(const uint8_t key[16], uint8_t iv[12],
                    const uint8_t *plaintext, size_t plaintext_len,
                    uint8_t *out, size_t *out_len) {
    size_t payload_n = plaintext_len;
    size_t pad_n = 16 - ((1 + payload_n) % 16);
    if (pad_n < 4) pad_n += 16;
    size_t enc_n = 1 + payload_n + pad_n;
    size_t total = 4 + enc_n + 16;
    if (total > SSH_MAX_PACKET) return -1;

    /* Length field — cleartext, but AAD for the tag. */
    out[0] = (uint8_t)(enc_n >> 24);
    out[1] = (uint8_t)(enc_n >> 16);
    out[2] = (uint8_t)(enc_n >>  8);
    out[3] = (uint8_t)(enc_n);

    /* Plaintext layout INSIDE the output buffer:
     *   out[4]                     = padding_length
     *   out[5..5+payload_n-1]      = payload
     *   out[5+payload_n..]         = random padding (pad_n bytes)
     * The encrypt loop reads from out+4 and writes to out+4, byte at
     * a time. aes_gcm_encrypt's CTR loop is `ct[i] = pt[i] ^ ks[i]` —
     * read before write — so in-place is safe. */
    out[4] = (uint8_t)pad_n;
    for (size_t i = 0; i < payload_n; i++) out[5 + i] = plaintext[i];
    rand_bytes(out + 5 + payload_n, pad_n);

    /* Encrypt in place. AAD is the 4-byte length; tag lands after ct. */
    aes_gcm_encrypt(key, iv, out, 4, out + 4, enc_n, out + 4, out + 4 + enc_n);

    *out_len = total;
    ssh_iv_increment(iv);
    return 0;
}

/* Decrypt + verify. The caller's `packet` buffer holds the full
 * wire bytes (length || ct || tag) and is mutable — we decrypt in
 * place inside it. Then we copy just the payload to `plaintext`.
 *
 * No static state. */
int ssh_packet_open(const uint8_t key[16], uint8_t iv[12],
                    uint8_t *packet, size_t packet_len,
                    uint8_t *plaintext, size_t max_plaintext,
                    size_t *plaintext_len) {
    if (packet_len < 4 + 16 + 16) return -1;
    uint32_t enc_n = ((uint32_t)packet[0] << 24) |
                     ((uint32_t)packet[1] << 16) |
                     ((uint32_t)packet[2] <<  8) |
                     ((uint32_t)packet[3]);
    if (enc_n < 16 || enc_n > SSH_MAX_PACKET - 4 - 16) return -1;
    if (packet_len != 4 + enc_n + 16) return -1;
    /* Worst-case payload size (with min pad_n=4) — refuse early if it
     * couldn't possibly fit. */
    if (enc_n > 5 && enc_n - 5 > max_plaintext) return -1;

    /* Decrypt in place: aes_gcm_decrypt with pt and ct pointing at the
     * same place is fine — the CTR loop reads ct[i] then writes pt[i],
     * read-before-write so the XOR is correct. Tag is checked first
     * (over the ciphertext before decryption), so an invalid tag
     * returns -1 without ever exposing decrypted bytes to the caller. */
    uint8_t *p = packet;
    int rc = aes_gcm_decrypt(key, iv, p, 4, p + 4, enc_n,
                             p + 4 + enc_n, p + 4);
    if (rc != 0) return -1;

    uint8_t pad_n = p[4];
    if (pad_n < 4 || (size_t)pad_n + 1 > enc_n) return -1;
    size_t payload_n = enc_n - 1 - pad_n;
    if (payload_n > max_plaintext) return -1;     /* re-check after pad_n */
    for (size_t i = 0; i < payload_n; i++) plaintext[i] = p[5 + i];
    *plaintext_len = payload_n;

    ssh_iv_increment(iv);
    return 0;
}

/* Increment the 8-byte big-endian counter at iv[4..11]. The first 4
 * bytes (the "fixed" field per RFC 5647 §7.1) stay put. */
void ssh_iv_increment(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--) {
        if (++iv[i] != 0) return;
    }
}

/* ---- KDF (RFC 4253 §7.2) ----------------------------------------- */

void ssh_kdf(uint8_t out[32],
             const uint8_t *K_mpint, size_t K_mpint_len,
             const uint8_t H[32], char letter,
             const uint8_t session_id[32]) {
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, K_mpint, K_mpint_len);
    sha256_update(&s, H, 32);
    uint8_t lb = (uint8_t)letter;
    sha256_update(&s, &lb, 1);
    sha256_update(&s, session_id, 32);
    sha256_final(&s, out);
}
