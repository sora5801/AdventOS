/*
 * cryptotest — verify libcrypto primitives against published test
 * vectors. Spawned by sh's [t26] selftest before the TLS handshake
 * test, so any crypto bug surfaces with a small actionable trace
 * rather than as a TLS handshake "decrypt failed".
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/sha512.h"

static int hexbyte(const char *s) {
    int hi = (s[0] >= 'a') ? s[0] - 'a' + 10 : s[0] - '0';
    int lo = (s[1] >= 'a') ? s[1] - 'a' + 10 : s[1] - '0';
    return (hi << 4) | lo;
}

static void hexdump(const unsigned char *p, int n) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { putchar(d[p[i] >> 4]); putchar(d[p[i] & 0xF]); }
}

static int hex_eq(const unsigned char *got, const char *expect, int n) {
    for (int i = 0; i < n; i++) {
        if (got[i] != hexbyte(expect + 2*i)) return 0;
    }
    return 1;
}

#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS  %s\n", name); pass++; } \
    else      { printf("  FAIL  %s\n", name); fail++; } \
} while (0)

int main(void) {
    int pass = 0, fail = 0;

    /* ---- SHA-256 ----------------------------------------------- */
    puts("== SHA-256 ==\n");
    {
        unsigned char h[32];
        sha256("abc", 3, h);
        /* RFC 6234 vector 1 */
        CHECK(hex_eq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32),
              "SHA256(\"abc\")");
        sha256("", 0, h);
        CHECK(hex_eq(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32),
              "SHA256(\"\")");
    }

    /* ---- HMAC-SHA-256 ------------------------------------------- */
    puts("== HMAC-SHA-256 ==\n");
    {
        /* RFC 4231 test case 1: key = 0x0b * 20, msg = "Hi There" */
        unsigned char key[20];
        for (int i = 0; i < 20; i++) key[i] = 0x0b;
        unsigned char tag[32];
        hmac_sha256(key, 20, (const unsigned char *)"Hi There", 8, tag);
        CHECK(hex_eq(tag, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32),
              "RFC4231 test 1");
    }

    /* ---- HKDF (RFC 5869 test 1) -------------------------------- */
    puts("== HKDF ==\n");
    {
        unsigned char ikm[22], salt[13];
        for (int i = 0; i < 22; i++) ikm[i] = 0x0b;
        const char *salt_hex = "000102030405060708090a0b0c";
        for (int i = 0; i < 13; i++) salt[i] = hexbyte(salt_hex + 2*i);
        const char *info_hex = "f0f1f2f3f4f5f6f7f8f9";
        unsigned char info[10];
        for (int i = 0; i < 10; i++) info[i] = hexbyte(info_hex + 2*i);

        unsigned char prk[32];
        hkdf_extract(salt, 13, ikm, 22, prk);
        CHECK(hex_eq(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32),
              "RFC5869 PRK");

        unsigned char okm[42];
        hkdf_expand(prk, 32, info, 10, okm, 42);
        CHECK(hex_eq(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                          "34007208d5b887185865", 42),
              "RFC5869 OKM");
    }

    /* ---- AES-128 (FIPS 197 Appendix C.1) ----------------------- */
    puts("== AES-128 ==\n");
    {
        unsigned char key[16] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
            0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
        };
        unsigned char in[16] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
            0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
        };
        unsigned char out[16];
        struct aes128 ctx;
        aes128_set_key(&ctx, key);
        aes128_encrypt(&ctx, in, out);
        CHECK(hex_eq(out, "69c4e0d86a7b0430d8cdb78070b4c55a", 16), "FIPS197 C.1");
    }

    /* ---- AES-128-GCM (NIST GCM TV1: empty pt+aad) -------------- */
    puts("== AES-GCM ==\n");
    {
        unsigned char key[16] = {0};
        unsigned char iv[12]  = {0};
        unsigned char tag[16];
        aes_gcm_encrypt(key, iv, 0, 0, 0, 0, 0, tag);
        CHECK(hex_eq(tag, "58e2fccefa7e3061367f1d57a4e7455a", 16), "NIST TV1 tag");

        /* TV3: ENCRYPT 16-byte plaintext (all-zero) with all-zero key
         * Expected ct: 0388dace60b6a392f328c2b971b2fe78
         * Expected tag: ab6e47d42cec13bdf53a67b21257bddf */
        unsigned char pt[16] = {0};
        unsigned char ct[16];
        aes_gcm_encrypt(key, iv, 0, 0, pt, 16, ct, tag);
        CHECK(hex_eq(ct,  "0388dace60b6a392f328c2b971b2fe78", 16), "NIST TV3 ct");
        CHECK(hex_eq(tag, "ab6e47d42cec13bdf53a67b21257bddf", 16), "NIST TV3 tag");

        /* Round-trip: encrypt then decrypt our own data. */
        unsigned char k2[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        unsigned char n2[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
        unsigned char aad[5] = {'h','e','l','l','o'};
        unsigned char p2[20] = "AdventOS GCM round!";    /* incl trailing 0 */
        unsigned char c2[20], t2[16], d2[20];
        aes_gcm_encrypt(k2, n2, aad, 5, p2, 20, c2, t2);
        int r = aes_gcm_decrypt(k2, n2, aad, 5, c2, 20, t2, d2);
        int eq = (r == 0);
        for (int i = 0; i < 20; i++) eq &= (d2[i] == p2[i]);
        CHECK(eq, "GCM round-trip");
    }

    /* ---- SHA-512 (RFC 6234 §8.4) ------------------------------- */
    puts("== SHA-512 ==\n");
    {
        unsigned char h[64];
        sha512("abc", 3, h);
        CHECK(hex_eq(h,
                "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64),
              "SHA512(\"abc\")");
        sha512("", 0, h);
        CHECK(hex_eq(h,
                "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", 64),
              "SHA512(\"\")");
    }

    /* ---- Ed25519 (RFC 8032 §7.1, Test 1) ----------------------- */
    puts("== Ed25519 ==\n");
    {
        const char *seed_hex =
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
        unsigned char seed[32];
        for (int i = 0; i < 32; i++) seed[i] = hexbyte(seed_hex + 2*i);

        unsigned char pk[32], sk[64];
        ed25519_keypair_from_seed(pk, sk, seed);
        CHECK(hex_eq(pk,
                "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", 32),
              "RFC 8032 test 1 public key");

        /* RFC 8032 deterministic signature for the empty message.
         * If this matches, sign + verify + the SHA-512 streaming
         * fix all line up with the spec — and the cert we'll
         * sign in CertificateVerify will satisfy curl's
         * canonical verify. */
        unsigned char sig[64];
        ed25519_sign(sig, (const unsigned char *)"", 0, sk);
        CHECK(hex_eq(sig,
                "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 64),
              "RFC 8032 test 1 signature");
        int v = ed25519_verify(sig, (const unsigned char *)"", 0, pk);
        CHECK(v == 0, "Ed25519 verify RFC signature");

        /* Bad signature: flip a bit, expect rejection. */
        sig[0] ^= 0x01;
        v = ed25519_verify(sig, (const unsigned char *)"", 0, pk);
        CHECK(v != 0, "Ed25519 reject tampered signature");
    }

    /* ---- X25519 (RFC 7748 §6.1) -------------------------------- */
    puts("== X25519 ==\n");
    {
        const char *priv_hex =
            "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
        unsigned char priv[32];
        for (int i = 0; i < 32; i++) priv[i] = hexbyte(priv_hex + 2*i);
        unsigned char pub[32];
        x25519(pub, priv, x25519_basepoint);
        CHECK(hex_eq(pub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32),
              "Alice public from RFC 7748");

        /* Now ECDH: Bob's private + Alice's public should equal
         * Alice's private + Bob's public. */
        const char *bob_priv_hex =
            "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
        unsigned char bob_priv[32], bob_pub[32];
        for (int i = 0; i < 32; i++) bob_priv[i] = hexbyte(bob_priv_hex + 2*i);
        x25519(bob_pub, bob_priv, x25519_basepoint);

        unsigned char shared_a[32], shared_b[32];
        x25519(shared_a, priv,     bob_pub);
        x25519(shared_b, bob_priv, pub);
        int eq = 1;
        for (int i = 0; i < 32; i++) eq &= (shared_a[i] == shared_b[i]);
        CHECK(eq, "ECDHE shared-secret agree");
        CHECK(hex_eq(shared_a, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32),
              "shared secret matches RFC");
    }

    printf("\ncryptotest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
