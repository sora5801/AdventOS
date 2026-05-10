/*
 * Tiny PRNG for the demo. Seeded from sys_time + sys_getpid +
 * a state mixer at first call. NOT cryptographically secure —
 * a real OS would use IRQ entropy + a hash chain. Documented in
 * the deep dive.
 */
#include "crypto.h"

static uint32_t s0 = 0x12345678;
static uint32_t s1 = 0x9abcdef0;
static uint32_t s2 = 0xdeadbeef;
static uint32_t s3 = 0xcafebabe;
static int      seeded;

static void seed_once(void) {
    if (seeded) return;
    /* sys_time and sys_getpid are part of libuser. */
    s0 ^= sys_time() * 0x9E3779B1u;
    s1 ^= (uint32_t)sys_getpid() * 0xBB67AE85u;
    s2 ^= s0 * 0x3C6EF372u;
    s3 ^= s1 * 0xA54FF53Au;
    seeded = 1;
}

/* xoshiro128** — small, decent statistical quality. Cryptographic
 * security: NOT GIVEN. We mix a SHA-256-of-state hash on every chunk
 * boundary so output isn't trivially predictable from a single
 * sample, but a real RFC-7748 ECDHE deserves a HW RNG seed. */
static uint32_t xoshiro_next(void) {
    uint32_t result = ((s1 * 5u) << 7 | (s1 * 5u) >> 25) * 9u;
    uint32_t t = s1 << 9;
    s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3; s2 ^= t;
    s3 = (s3 << 11) | (s3 >> 21);
    return result;
}

void rand_bytes(uint8_t *out, size_t n) {
    seed_once();
    /* Mix in a fresh SHA-256 hash of the state every 32 bytes so
     * the output isn't trivially the raw xoshiro stream. */
    while (n > 0) {
        uint8_t state[16];
        state[ 0] = (uint8_t)(s0      ); state[ 1] = (uint8_t)(s0 >> 8);
        state[ 2] = (uint8_t)(s0 >> 16); state[ 3] = (uint8_t)(s0 >> 24);
        state[ 4] = (uint8_t)(s1      ); state[ 5] = (uint8_t)(s1 >> 8);
        state[ 6] = (uint8_t)(s1 >> 16); state[ 7] = (uint8_t)(s1 >> 24);
        state[ 8] = (uint8_t)(s2      ); state[ 9] = (uint8_t)(s2 >> 8);
        state[10] = (uint8_t)(s2 >> 16); state[11] = (uint8_t)(s2 >> 24);
        state[12] = (uint8_t)(s3      ); state[13] = (uint8_t)(s3 >> 8);
        state[14] = (uint8_t)(s3 >> 16); state[15] = (uint8_t)(s3 >> 24);
        uint8_t hash[32];
        sha256(state, 16, hash);

        size_t take = (n < 32) ? n : 32;
        for (size_t i = 0; i < take; i++) out[i] = hash[i];
        out += take; n -= take;
        /* Advance state regardless of how much we used. */
        for (int i = 0; i < 4; i++) (void)xoshiro_next();
    }
}
