#ifndef ADVENTOS_SHA512_H
#define ADVENTOS_SHA512_H

#include "crypto.h"

struct sha512 {
    uint64_t state[8];
    uint64_t count[2];   /* 128-bit byte count: [low, high] */
    uint8_t  buf[128];
    uint32_t buf_len;
};
void sha512_init  (struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t n);
void sha512_final (struct sha512 *s, uint8_t out[64]);
void sha512       (const void *data, size_t n, uint8_t out[64]);

#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

#endif
