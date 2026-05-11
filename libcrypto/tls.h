/*
 * AdventOS minimal TLS 1.3, server + client.
 *
 * Cipher suite (only one): TLS_AES_128_GCM_SHA256
 * Key exchange (only one): X25519 ECDHE
 * Signature scheme:        Ed25519 (RFC 8410 — and the only thing
 *                          we have a working sign/verify for).
 * Versions (only one):     TLS 1.3
 *
 * Two server modes:
 *
 *   1. tls_server_handshake_psk  — original PSK flow from session 36.
 *      ClientHello carries PSK identity inline; no extensions, no
 *      certificates. Used by httpsget for the OS↔OS demo.
 *
 *   2. tls_server_handshake_cert — full RFC 8446 cert flow with
 *      real ClientHello extensions, EncryptedExtensions, Certificate,
 *      CertificateVerify, Finished. This is what curl talks.
 *
 * Both share record I/O (send_record, send_encrypted, recv_record),
 * key schedule (HKDF-Extract/Expand-Label, Derive-Secret), and
 * AEAD nonce construction (sequence ^ write_iv).
 *
 * The cert flow handles middlebox-compatibility ChangeCipherSpec
 * records (RFC 8446 Appendix D.4): server sends one after its
 * ServerHello, ignores any received from the client.
 */
#ifndef ADVENTOS_TLS_H
#define ADVENTOS_TLS_H

#include "crypto.h"

#define TLS_REC_CHANGE_CIPHER 20
#define TLS_REC_ALERT         21
#define TLS_REC_HANDSHAKE     22
#define TLS_REC_APP_DATA      23

#define TLS_HS_CLIENT_HELLO    1
#define TLS_HS_SERVER_HELLO    2
#define TLS_HS_ENCRYPTED_EXTS  8
#define TLS_HS_CERTIFICATE    11
#define TLS_HS_CERT_VERIFY    15
#define TLS_HS_FINISHED       20

#define TLS_AES_KEY_LEN       16
#define TLS_AES_IV_LEN        12
#define TLS_TAG_LEN           16
#define TLS_HASH_LEN          32

/* Bumped from 1024 to 2048: a real ClientHello from curl can hit
 * ~500 bytes (ALPN, SNI, lots of sig schemes, lots of groups), and
 * our server-side encrypted flight (EncryptedExtensions + Certificate
 * + CertificateVerify + Finished) is ~440 bytes plaintext. 2048
 * leaves headroom and keeps everything in one record per direction. */
/* TLS 1.3 caps records at 2^14 plaintext (RFC 8446 §5.1). Real-world
 * server flights (cert chain + CertificateVerify + Finished) can hit
 * 3-5 KiB. Old value was 2048; bumped to 6144 to comfortably fit a
 * single-cert Cloudflare flight (~2.5 KiB) while keeping the static
 * BSS for TLS recv buffers under control across user programs. */
#define TLS_MAX_FRAGMENT      6144

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

    /* Echoed legacy_session_id from ClientHello (cert flow only). */
    uint8_t  session_id[32];
    int      session_id_len;

    /* PSK (psk-flow only — ignored in cert flow) */
    const uint8_t *psk;
    size_t         psk_len;
    const char    *psk_id;
    size_t         psk_id_len;

    /* Certificate (cert-flow only — set by caller before handshake).
     * sig_alg controls which signature scheme is used in
     * CertificateVerify and which interpretation of server_sk applies:
     *
     *   0x0807 (ed25519)               server_sk = 64-byte expanded sk
     *   0x0403 (ecdsa_secp256r1_sha256) server_sk = 32-byte P-256 scalar
     */
    const uint8_t *cert_der;
    int            cert_der_len;
    const uint8_t *server_sk;
    int            sig_alg;            /* 0x0807 (default) or 0x0403 */

    /* Client-side SNI (session 45). Set by the caller before
     * tls_client_handshake_cert when talking to a real-world server
     * that hosts multiple domains on the same IP — without SNI most
     * public HTTPS servers either refuse the connection or serve
     * the wrong vhost's cert. NULL = no SNI extension emitted. */
    const char    *server_name;

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
};

/* PSK-mode handshake (session 36 flow, used by httpsget demo). */
int tls_server_handshake_psk(struct tls_conn *c, int fd,
                             const uint8_t *psk, size_t psk_len,
                             const char *psk_id);
int tls_client_handshake_psk(struct tls_conn *c, int fd,
                             const uint8_t *psk, size_t psk_len,
                             const char *psk_id);

/* Cert-mode handshake (RFC 8446 with X.509 + CertificateVerify).
 * Caller must populate c->cert_der / cert_der_len / server_sk
 * before calling — these are typically static state in the
 * server program (e.g., httpsd). */
int tls_server_handshake_cert(struct tls_conn *c, int fd);

/* Cert-mode CLIENT — speaks the same wire format curl does, so
 * httpsget can talk to httpsd over the cert flow. Accepts any
 * server cert without validation (i.e., -k semantics) — this is
 * a demo TLS client, not a security-critical CA-validating one. */
int tls_client_handshake_cert(struct tls_conn *c, int fd);

/* Send/receive application data. tls_send returns bytes sent or
 * -1; tls_recv returns bytes received, 0 on close, -1 on error
 * (including AEAD tag mismatch). */
int tls_send(struct tls_conn *c, const void *data, int n);
int tls_recv(struct tls_conn *c, void *out, int max_n);

/* Send a close_notify alert (RFC 8446 §6.1) before the underlying TCP
 * close. Avoids "unexpected eof" warnings from OpenSSL / Schannel. */
int tls_close_notify(struct tls_conn *c);

#endif
