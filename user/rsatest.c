/*
 * rsatest — exercises libcrypto/bignum + libcrypto/rsa (session 58).
 *
 * Three pieces of coverage:
 *
 *   A. Verify a precomputed openssl signature.  Pins our PKCS#1 v1.5
 *      decode + modpow against an external implementation: openssl
 *      generated the (n, e, d, sig) bundle in libcrypto/rsa_testvec.h
 *      and signed `RSA_TEST_MSG`.  Our verify must accept it.
 *
 *   B. Sign that same message and byte-compare against openssl's sig.
 *      PKCS#1 v1.5 is deterministic (no random padding), so sig
 *      equality is a stronger check than just "verify-passes" — any
 *      off-by-one in EM padding, modpow, or CRT setup would diverge.
 *
 *   C. Generate a fresh 512-bit keypair, sign, verify.  Closes the
 *      loop on rsa_keygen + Miller-Rabin.  Plus a deliberate tampered-
 *      signature negative test.
 *
 * The output is structured "rsatest: ... PASS/FAIL" lines that the
 * [t41] selftest greps for. */

#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/bignum.h"
#include "../libcrypto/rsa.h"
#include "../libcrypto/rsa_testvec.h"

static const char RSA_TEST_MSG[] = "AdventOS RSA test vector";
#define RSA_TEST_MSG_LEN  (sizeof(RSA_TEST_MSG) - 1)

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void print_hex(const char *label, const uint8_t *buf, int n) {
    printf("%s [", label);
    int cap = n < 16 ? n : 16;
    for (int i = 0; i < cap; i++) printf("%02x", buf[i]);
    if (n > 16) printf("...");
    printf("] (%d bytes)\n", n);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("rsatest: starting\n");

    /* ---- Part A: verify a precomputed openssl signature ----------- */
    bignum n, e;
    bn_from_bytes_be(&n, rsa_n, RSA_N_LEN);
    bn_set_u32(&e, 65537);

    printf("rsatest: modulus bits = %d  (sig %d bytes)\n",
           bn_bit_length(&n), bn_byte_length(&n));
    printf("rsatest: msg = \"%s\" (%d bytes)\n",
           RSA_TEST_MSG, (int)RSA_TEST_MSG_LEN);

    int rc = rsa_verify_pkcs1_sha256(
        (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
        rsa_sig, RSA_SIG_LEN, &n, &e);
    if (rc == 0) puts("rsatest: PASS  verify openssl-signed message\n");
    else         puts("rsatest: FAIL  verify openssl-signed message\n");

    /* ---- Part B: sign with our impl, compare to openssl's sig ----- */
    struct rsa_keypair kp = {0};
    bn_from_bytes_be(&kp.n, rsa_n, RSA_N_LEN);
    bn_set_u32     (&kp.e, 65537);
    bn_from_bytes_be(&kp.d, rsa_d, RSA_D_LEN);
    bn_from_bytes_be(&kp.p, rsa_p, RSA_P_LEN);
    bn_from_bytes_be(&kp.q, rsa_q, RSA_Q_LEN);

    /* Derive CRT helpers so the signer takes the fast path. */
    bignum one, p1, q1;
    bn_set_u32(&one, 1);
    bn_sub(&p1, &kp.p, &one);
    bn_sub(&q1, &kp.q, &one);
    bn_mod(&kp.dp, &kp.d, &p1);
    bn_mod(&kp.dq, &kp.d, &q1);
    if (bn_modinv(&kp.qinv, &kp.q, &kp.p) < 0) {
        puts("rsatest: FAIL  qinv modular inverse computation\n");
    }

    /* Time-check the signing path so a perf regression is visible. */
    uint32_t t0 = (uint32_t)sys_time();
    uint8_t our_sig[RSA_MAX_BYTES];
    int sig_len = 0;
    rc = rsa_sign_pkcs1_sha256(our_sig, sizeof(our_sig), &sig_len,
                               (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
                               &kp);
    uint32_t t1 = (uint32_t)sys_time();

    if (rc < 0) {
        puts("rsatest: FAIL  rsa_sign_pkcs1_sha256 returned -1\n");
    } else if (sig_len != RSA_SIG_LEN) {
        printf("rsatest: FAIL  sig_len=%d (expected %d)\n",
               sig_len, RSA_SIG_LEN);
    } else {
        if (bytes_eq(our_sig, rsa_sig, RSA_SIG_LEN)) {
            puts("rsatest: PASS  our sig == openssl sig (byte-exact)\n");
        } else {
            puts("rsatest: FAIL  our sig differs from openssl sig\n");
            print_hex("  ours    ", our_sig, RSA_SIG_LEN);
            print_hex("  openssl ", rsa_sig, RSA_SIG_LEN);
        }
        printf("rsatest: sign elapsed ~%u sec  (CRT path, %d-bit modulus)\n",
               (unsigned)(t1 - t0), bn_bit_length(&kp.n));
    }

    /* Verify our own sig — should also pass. */
    rc = rsa_verify_pkcs1_sha256(
        (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
        our_sig, sig_len, &kp.n, &kp.e);
    if (rc == 0) puts("rsatest: PASS  verify our own sig roundtrip\n");
    else         puts("rsatest: FAIL  verify our own sig roundtrip\n");

    /* Tampered signature must NOT verify — flip a byte. */
    {
        uint8_t bad[RSA_MAX_BYTES];
        for (int i = 0; i < sig_len; i++) bad[i] = our_sig[i];
        bad[100] ^= 0x01;
        rc = rsa_verify_pkcs1_sha256(
            (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
            bad, sig_len, &kp.n, &kp.e);
        if (rc != 0) puts("rsatest: PASS  tampered sig rejected\n");
        else         puts("rsatest: FAIL  tampered sig wrongly accepted\n");
    }

    /* Tampered message must NOT verify. */
    {
        rc = rsa_verify_pkcs1_sha256(
            (const uint8_t *)"AdventOS RSA test vector!",
            RSA_TEST_MSG_LEN + 1,
            our_sig, sig_len, &kp.n, &kp.e);
        if (rc != 0) puts("rsatest: PASS  wrong-message sig rejected\n");
        else         puts("rsatest: FAIL  wrong-message sig wrongly accepted\n");
    }

    /* ---- Part C: generate a small fresh key, sign + verify -------- */
    puts("rsatest: generating fresh 512-bit RSA keypair\n");
    t0 = (uint32_t)sys_time();
    struct rsa_keypair fresh = {0};
    rc = rsa_keygen(&fresh, 512);
    t1 = (uint32_t)sys_time();
    if (rc < 0) {
        puts("rsatest: FAIL  rsa_keygen returned -1\n");
        return 1;
    }
    printf("rsatest: keygen elapsed ~%u sec  (%d-bit modulus)\n",
           (unsigned)(t1 - t0), bn_bit_length(&fresh.n));

    uint8_t fresh_sig[RSA_MAX_BYTES];
    int fresh_sig_len = 0;
    rc = rsa_sign_pkcs1_sha256(fresh_sig, sizeof(fresh_sig), &fresh_sig_len,
                               (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
                               &fresh);
    if (rc == 0) puts("rsatest: PASS  sign with fresh key\n");
    else         puts("rsatest: FAIL  sign with fresh key\n");

    rc = rsa_verify_pkcs1_sha256(
        (const uint8_t *)RSA_TEST_MSG, RSA_TEST_MSG_LEN,
        fresh_sig, fresh_sig_len, &fresh.n, &fresh.e);
    if (rc == 0) puts("rsatest: PASS  verify fresh-key sig\n");
    else         puts("rsatest: FAIL  verify fresh-key sig\n");

    puts("rsatest: done\n");
    return 0;
}
