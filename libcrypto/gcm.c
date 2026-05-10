/*
 * AES-GCM AEAD (NIST SP 800-38D). The Galois Counter Mode wraps a
 * 128-bit block cipher (AES here) into an AEAD: encrypt with CTR
 * mode, authenticate with GHASH.
 *
 * GCM picks 12-byte nonces as the "natural" size — pad with 0x00..01
 * to get J0, the initial counter. Each subsequent block uses the
 * counter from the previous block + 1 (only the low 32 bits of J0
 * are incremented; the high 96 stay).
 *
 * GHASH operates in GF(2^128) with reduction polynomial
 *   x^128 + x^7 + x^2 + x + 1.
 * The "byte 0 bit 0" of the 16-byte buffer is the highest-order
 * coefficient — i.e., the polynomial is read MSB-first per byte
 * but bytes are in natural order.
 *
 * Test vector (NIST GCM, key=0, iv=0, pt=empty, aad=empty):
 *   tag = 58e2fcce fa7e3061 367f1d57 a4e7455a
 */
#include "crypto.h"

static void gf_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    /* Bit-by-bit shift-XOR multiplication in GF(2^128) with the
     * GCM-specific bit ordering. Slow (~4500 cycles per call) but
     * straightforward to audit. The "shift" is right-shift because
     * GCM treats bit 0 of byte 0 as the highest power; we walk x
     * MSB-first, picking up x_i * Y in the accumulator. */
    uint8_t z[16] = {0};
    uint8_t v[16];
    for (int i = 0; i < 16; i++) v[i] = y[i];

    for (int i = 0; i < 16; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (x[i] & (1 << bit)) {
                for (int k = 0; k < 16; k++) z[k] ^= v[k];
            }
            /* v = v >> 1, with reduction by R = 0xE1 << 120 if LSB set. */
            int lsb = v[15] & 1;
            for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k-1] << 7));
            v[0] >>= 1;
            if (lsb) v[0] ^= 0xE1;
        }
    }
    for (int i = 0; i < 16; i++) out[i] = z[i];
}

static void ghash(const uint8_t H[16],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct,  size_t ct_len,
                  uint8_t out[16]) {
    uint8_t y[16] = {0};
    uint8_t blk[16];

    /* Process AAD in 16-byte chunks, zero-padded. */
    size_t off = 0;
    while (off < aad_len) {
        size_t take = aad_len - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < 16; i++) blk[i] = (i < take) ? aad[off + i] : 0;
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gf_mul(y, H, y);
        off += take;
    }

    /* Then ciphertext, zero-padded. */
    off = 0;
    while (off < ct_len) {
        size_t take = ct_len - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < 16; i++) blk[i] = (i < take) ? ct[off + i] : 0;
        for (int i = 0; i < 16; i++) y[i] ^= blk[i];
        gf_mul(y, H, y);
        off += take;
    }

    /* Length block: 64-bit big-endian aad bit-len || 64-bit be ct bit-len. */
    uint64_t aad_bits = (uint64_t)aad_len * 8;
    uint64_t ct_bits  = (uint64_t)ct_len  * 8;
    uint8_t len_blk[16];
    for (int i = 0; i < 8; i++) len_blk[i]   = (uint8_t)(aad_bits >> (56 - i*8));
    for (int i = 0; i < 8; i++) len_blk[8+i] = (uint8_t)(ct_bits  >> (56 - i*8));
    for (int i = 0; i < 16; i++) y[i] ^= len_blk[i];
    gf_mul(y, H, y);

    for (int i = 0; i < 16; i++) out[i] = y[i];
}

/* Increment the low 32 bits of a 16-byte counter, big-endian. */
static void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) {
        ctr[i]++;
        if (ctr[i]) return;
    }
}

void aes_gcm_encrypt(const uint8_t key[16],
                     const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *pt, size_t pt_len,
                     uint8_t *ct, uint8_t tag[16])
{
    struct aes128 aes;
    aes128_set_key(&aes, key);

    /* H = AES_K(0^128) — the GHASH subkey. */
    uint8_t zero[16] = {0};
    uint8_t H[16];
    aes128_encrypt(&aes, zero, H);

    /* J0 = nonce (12B) || 0x00 0x00 0x00 0x01 (when nonce is 12B). */
    uint8_t j0[16];
    for (int i = 0; i < 12; i++) j0[i] = nonce[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    /* CTR mode encryption starts from J0 + 1. */
    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = j0[i];
    inc32(ctr);

    size_t off = 0;
    while (off < pt_len) {
        uint8_t ks[16];
        aes128_encrypt(&aes, ctr, ks);
        size_t take = pt_len - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < take; i++) ct[off + i] = pt[off + i] ^ ks[i];
        inc32(ctr);
        off += take;
    }

    /* tag = GHASH(H, aad, ct) ^ AES_K(J0) */
    uint8_t s[16];
    ghash(H, aad, aad_len, ct, pt_len, s);
    uint8_t e_j0[16];
    aes128_encrypt(&aes, j0, e_j0);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ e_j0[i];
}

int aes_gcm_decrypt(const uint8_t key[16],
                    const uint8_t nonce[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len,
                    const uint8_t tag[16],
                    uint8_t *pt)
{
    struct aes128 aes;
    aes128_set_key(&aes, key);

    uint8_t zero[16] = {0};
    uint8_t H[16];
    aes128_encrypt(&aes, zero, H);

    uint8_t j0[16];
    for (int i = 0; i < 12; i++) j0[i] = nonce[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    /* Compute expected tag FIRST (over ciphertext, before decrypt). */
    uint8_t s[16];
    ghash(H, aad, aad_len, ct, ct_len, s);
    uint8_t e_j0[16];
    aes128_encrypt(&aes, j0, e_j0);
    uint8_t expect[16];
    for (int i = 0; i < 16; i++) expect[i] = s[i] ^ e_j0[i];

    /* Constant-time tag compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (expect[i] ^ tag[i]);

    /* CTR decrypt regardless — caller must check return value before
     * trusting the plaintext. */
    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = j0[i];
    inc32(ctr);

    size_t off = 0;
    while (off < ct_len) {
        uint8_t ks[16];
        aes128_encrypt(&aes, ctr, ks);
        size_t take = ct_len - off;
        if (take > 16) take = 16;
        for (size_t i = 0; i < take; i++) pt[off + i] = ct[off + i] ^ ks[i];
        inc32(ctr);
        off += take;
    }
    return diff ? -1 : 0;
}
