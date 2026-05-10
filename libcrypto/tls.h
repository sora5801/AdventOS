/*
 * AdventOS minimal TLS 1.3, server + client.
 *
 * Cipher suite (only one): TLS_AES_128_GCM_SHA256
 * Key exchange (only one): X25519 ECDHE
 * Authentication (only one): pre-shared key (PSK) — no X.509
 * Versions (only one):       TLS 1.3
 *
 * Wire format mostly mirrors RFC 8446 §5 (record layer), §4.1.2/3
 * (ClientHello/ServerHello sub-message structure), §4.4.4 (Finished),
 * §7.1 (key schedule). Simplifications relative to real TLS 1.3:
 *
 *   - No certificate / CertificateVerify (PSK only).
 *   - Single hardcoded cipher suite, version, and named group:
 *     no extensions for negotiation, no HelloRetryRequest.
 *   - ClientHello / ServerHello don't include a session_id, the
 *     legacy compression byte, or any extensions — body is purely
 *     {random, psk_id_len, psk_id, x25519_public}.
 *   - PSK binder is NOT verified (real TLS 1.3 binds the PSK to
 *     the ClientHello transcript via HMAC; we skip — the ECDHE
 *     shared secret already provides confidentiality and the
 *     transcript hash protection comes from Finished).
 *   - Single connection, no resumption, no 0-RTT, no post-handshake.
 *
 * What IS RFC-faithful: the cryptography. SHA-256, HMAC, HKDF,
 * AES-128-GCM, X25519, the TLS 1.3 key schedule (Early/Handshake/
 * Master secrets, the "tls13 " HKDF label prefix, the c_hs_traffic
 * / s_hs_traffic / c_ap_traffic / s_ap_traffic secrets), the
 * AEAD nonce construction (sequence ^ write_iv), the additional-
 * data layout (5-byte record header).
 */
#ifndef ADVENTOS_TLS_H
#define ADVENTOS_TLS_H

#include "crypto.h"

#define TLS_REC_HANDSHAKE     22
#define TLS_REC_APP_DATA      23
#define TLS_REC_ALERT         21

#define TLS_HS_CLIENT_HELLO    1
#define TLS_HS_SERVER_HELLO    2
#define TLS_HS_FINISHED       20

#define TLS_AES_KEY_LEN       16
#define TLS_AES_IV_LEN        12
#define TLS_TAG_LEN           16
#define TLS_HASH_LEN          32

/* Maximum TLS record fragment. Real TLS 1.3 caps at 2^14 + 256;
 * we run on a 16-task system with a 16 KiB user stack — and the
 * record buffers live ON the stack of the handshake/send/recv
 * functions. 1.5 KiB record + handshake-message buffers + AES
 * round keys all need to fit. The HTTPS demo's largest message
 * (the static reply body) is ~600 bytes; 1024 leaves headroom. */
#define TLS_MAX_FRAGMENT      1024

struct tls_keys {
    uint8_t key[TLS_AES_KEY_LEN];
    uint8_t iv [TLS_AES_IV_LEN];
    uint64_t seq;       /* incremented per record sent/received */
};

struct tls_conn {
    int      fd;                /* underlying TCP socket fd */
    int      is_server;

    /* Handshake state */
    uint8_t  client_random[32];
    uint8_t  server_random[32];
    uint8_t  client_pub[32];
    uint8_t  server_pub[32];
    uint8_t  our_priv[32];
    uint8_t  ecdhe_shared[32];

    /* PSK */
    const uint8_t *psk;
    size_t         psk_len;
    const char    *psk_id;
    size_t         psk_id_len;

    /* Key schedule outputs */
    uint8_t  early_secret      [32];
    uint8_t  handshake_secret  [32];
    uint8_t  master_secret     [32];
    uint8_t  c_hs_traffic_secret[32];
    uint8_t  s_hs_traffic_secret[32];
    uint8_t  c_ap_traffic_secret[32];
    uint8_t  s_ap_traffic_secret[32];

    /* Derived keys+ivs+sequences for AEAD. */
    struct tls_keys c_hs_keys, s_hs_keys;
    struct tls_keys c_ap_keys, s_ap_keys;

    /* Running transcript hash. */
    struct sha256 transcript;

    /* Receive buffer (for reassembling a record from the TCP stream). */
    uint8_t  rxbuf[TLS_MAX_FRAGMENT + 64];
    size_t   rxlen;
};

/* Returns 0 on success. fd is a connected TCP socket (server-side
 * comes from accept(); client-side from connect()). */
int tls_server_handshake(struct tls_conn *c, int fd,
                         const uint8_t *psk, size_t psk_len,
                         const char *psk_id);
int tls_client_handshake(struct tls_conn *c, int fd,
                         const uint8_t *psk, size_t psk_len,
                         const char *psk_id);

/* Send/receive application data. tls_send returns bytes sent or
 * -1; tls_recv returns bytes received, 0 on close, -1 on error
 * (incl. AEAD tag mismatch). */
int tls_send(struct tls_conn *c, const void *data, int n);
int tls_recv(struct tls_conn *c, void *out, int max_n);

#endif
