/*
 * AdventOS libcrypto — SSH-2 protocol helpers (RFC 4253 wire format).
 *
 * Algorithm choices made for the AdventOS implementation:
 *
 *   KEX:                curve25519-sha256 (RFC 8731)
 *   server host key:    ssh-ed25519        (RFC 8709)
 *   encryption + MAC:   aes128-gcm@openssh.com (RFC 5647 framing)
 *   MAC:                AEAD_AES_128_GCM   (implicit; no separate HMAC)
 *   compression:        none
 *
 * All of these are mandatory-to-implement (or at least the most-common
 * picks) in modern openssh, so an unmodified client just works.
 *
 * Functions here are byte-pushing and AEAD wrappers — the actual state
 * machine for handshake / userauth / channels lives in user/sshd.c
 * and user/ssh.c.
 */
#ifndef ADVENTOS_SSH_H
#define ADVENTOS_SSH_H

#include "crypto.h"

/* ---- SSH-2 message numbers (RFC 4250 §4.1) ---- */

#define SSH_MSG_DISCONNECT                  1
#define SSH_MSG_IGNORE                      2
#define SSH_MSG_UNIMPLEMENTED               3
#define SSH_MSG_DEBUG                       4
#define SSH_MSG_SERVICE_REQUEST             5
#define SSH_MSG_SERVICE_ACCEPT              6
#define SSH_MSG_EXT_INFO                    7   /* RFC 8308 */
#define SSH_MSG_NEWCOMPRESS                 8
#define SSH_MSG_KEXINIT                    20
#define SSH_MSG_NEWKEYS                    21
#define SSH_MSG_KEX_ECDH_INIT              30   /* RFC 5656 / 8731 */
#define SSH_MSG_KEX_ECDH_REPLY             31
#define SSH_MSG_USERAUTH_REQUEST           50
#define SSH_MSG_USERAUTH_FAILURE           51
#define SSH_MSG_USERAUTH_SUCCESS           52
#define SSH_MSG_USERAUTH_BANNER            53
#define SSH_MSG_USERAUTH_PK_OK             60   /* "publickey" probe reply */
#define SSH_MSG_GLOBAL_REQUEST             80
#define SSH_MSG_REQUEST_SUCCESS            81
#define SSH_MSG_REQUEST_FAILURE            82
#define SSH_MSG_CHANNEL_OPEN               90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION  91
#define SSH_MSG_CHANNEL_OPEN_FAILURE       92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST      93
#define SSH_MSG_CHANNEL_DATA               94
#define SSH_MSG_CHANNEL_EXTENDED_DATA      95
#define SSH_MSG_CHANNEL_EOF                96
#define SSH_MSG_CHANNEL_CLOSE              97
#define SSH_MSG_CHANNEL_REQUEST            98
#define SSH_MSG_CHANNEL_SUCCESS            99
#define SSH_MSG_CHANNEL_FAILURE           100

/* Disconnect reasons (RFC 4253 §11.1). */
#define SSH_DISCONNECT_PROTOCOL_ERROR              2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED         3
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER     13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE 14

/* Channel-open failure reasons. */
#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED       1
#define SSH_OPEN_CONNECT_FAILED                    2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE              3

/* Limits — generous but bounded to keep static buffers sane. AdventOS
 * isn't talking to mega-corp deployments. */
#define SSH_MAX_PACKET    32768
#define SSH_BLOCK_SIZE    16          /* AES block size; padding multiple */

/* ---- Wire-format byte-pushers ---- */

void     ssh_put_u8     (uint8_t **out, uint8_t v);
void     ssh_put_u32    (uint8_t **out, uint32_t v);
void     ssh_put_bytes  (uint8_t **out, const void *data, size_t n);
void     ssh_put_string (uint8_t **out, const void *data, size_t n);
void     ssh_put_cstring(uint8_t **out, const char *s);
void     ssh_put_bool   (uint8_t **out, int v);
void     ssh_put_mpint  (uint8_t **out, const uint8_t *be, size_t n);

uint8_t  ssh_get_u8     (const uint8_t **p);
uint32_t ssh_get_u32    (const uint8_t **p);
/* On success returns pointer to in-buffer data, fills *len, advances *p.
 * On length overflow returns NULL. Caller verifies the data lies within
 * an outer buffer they own. */
const uint8_t *ssh_get_string(const uint8_t **p, const uint8_t *end,
                              uint32_t *out_len);

/* ---- AES-128-GCM packet seal/open (RFC 5647 framing) ----
 *
 * On the wire:
 *
 *   uint32  packet_length        cleartext, but AAD for GCM
 *   byte    padding_length       \
 *   bytes   payload               | encrypted under AES-128-GCM
 *   bytes   random padding       /
 *   bytes   mac (16)             AEAD tag
 *
 * (packet_length covers padding_length + payload + padding, NOT mac.)
 *
 * Padding makes the encrypted region a multiple of 16 octets per
 * RFC 5647 §6 — different from base RFC 4253 which says cipher block
 * size; for GCM the block size doesn't apply so we always pad to 16.
 * Padding is random per RFC 4253 §6.
 *
 * IV scheme (RFC 5647 §7.1):
 *   - 12-byte nonce
 *   - last 8 bytes form a big-endian 64-bit counter
 *   - counter increments by 1 mod 2^64 after each packet
 *
 * Return 0 on success, -1 on tag failure / size error / etc. */
int  ssh_packet_seal(const uint8_t key[16], uint8_t iv[12],
                     const uint8_t *plaintext, size_t plaintext_len,
                     uint8_t *out, size_t *out_len);
/* packet is decrypted IN PLACE — caller's buffer is mutated.
 * Returns -1 (without touching `plaintext`) if the decrypted payload
 * would exceed max_plaintext. */
int  ssh_packet_open(const uint8_t key[16], uint8_t iv[12],
                     uint8_t *packet, size_t packet_len,
                     uint8_t *plaintext, size_t max_plaintext,
                     size_t *plaintext_len);
void ssh_iv_increment(uint8_t iv[12]);

/* ---- Key derivation (RFC 4253 §7.2) ----
 *
 *   K_x = HASH(K || H || letter || session_id)   ... and chain
 *         K_y = HASH(K || H || K_x)
 *
 * For our cipher (aes128-gcm) we need a 16-byte key and 12-byte IV
 * per direction; one SHA-256 invocation (32 bytes) covers either with
 * room to spare, so we never need the chain extension.
 *
 *   letter:
 *     'A' = IV  client -> server
 *     'B' = IV  server -> client
 *     'C' = key client -> server
 *     'D' = key server -> client
 *     'E' = integrity client -> server  (unused: AEAD)
 *     'F' = integrity server -> client  (unused: AEAD)
 *
 * K is the mpint-encoded shared secret; we take the SSH-style encoded
 * form so callers can pass the bytes-on-wire directly.
 *
 * Output is always 32 bytes (one SHA-256 block) — caller trims to
 * 16 (key) or 12 (IV) as needed. */
void ssh_kdf(uint8_t out[32],
             const uint8_t *K_mpint, size_t K_mpint_len,
             const uint8_t H[32], char letter,
             const uint8_t session_id[32]);

#endif
