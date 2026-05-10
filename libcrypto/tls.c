/*
 * AdventOS minimal TLS 1.3.
 *
 * Server flow:
 *   1. Read ClientHello (record type 22, hs sub-type 1)
 *      Parse: client_random[32], psk_id, client_x25519_pub[32]
 *      Update transcript hash with the message body.
 *      Verify psk_id matches our configured PSK identity.
 *   2. Generate server_random[32] and ephemeral X25519 keypair.
 *      Compute ECDHE shared = X25519(server_priv, client_pub).
 *      Send ServerHello (record type 22, hs sub-type 2).
 *      Update transcript hash.
 *   3. Run TLS 1.3 key schedule:
 *        early_secret      = HKDF-Extract(0, PSK)
 *        derived           = Derive-Secret(early, "derived", "")
 *        handshake_secret  = HKDF-Extract(derived, ECDHE shared)
 *        c_hs_traffic      = Derive-Secret(handshake, "c hs traffic", transcript)
 *        s_hs_traffic      = Derive-Secret(handshake, "s hs traffic", transcript)
 *      Then derive AES key + GCM IV from each handshake traffic
 *      secret via HKDF-Expand-Label("key" / "iv").
 *   4. Send ServerFinished (encrypted with s_hs_keys):
 *        finished_key = HKDF-Expand-Label(s_hs_traffic, "finished", "")
 *        verify_data  = HMAC-SHA256(finished_key, transcript_hash)
 *      Update transcript with the Finished plaintext.
 *   5. Receive ClientFinished (encrypted with c_hs_keys), verify.
 *   6. Derive application traffic keys from master_secret:
 *        derived         = Derive-Secret(handshake, "derived", "")
 *        master_secret   = HKDF-Extract(derived, 0)
 *        c_ap_traffic    = Derive-Secret(master, "c ap traffic", transcript)
 *        s_ap_traffic    = Derive-Secret(master, "s ap traffic", transcript)
 *      App-data records use these.
 *
 * Client flow is symmetric.
 */
#include "tls.h"

/* ---- I/O helpers ------------------------------------------------- */

/* read_n: read exactly n bytes from a socket fd, or -1 on EOF/error.
 * sys_read on a socket can return short — keep looping until we have
 * the full requested length or hit EOF. */
static int read_n(int fd, void *buf, int n) {
    uint8_t *p = (uint8_t *)buf;
    int      got = 0;
    while (got < n) {
        int r = sys_read(fd, p + got, n - got);
        if (r <= 0) return -1;        /* 0 = EOF, <0 = error */
        got += r;
    }
    return got;
}

static int write_all(int fd, const void *buf, int n) {
    const uint8_t *p = (const uint8_t *)buf;
    int sent = 0;
    while (sent < n) {
        int r = sys_write(fd, p + sent, n - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

/* ---- AEAD record I/O -------------------------------------------- */

/* Build the 12-byte AEAD nonce: 12-byte iv XOR (8-byte BE seq right-aligned). */
static void build_nonce(uint8_t out[12], const uint8_t iv[12], uint64_t seq) {
    for (int i = 0; i < 12; i++) out[i] = iv[i];
    for (int i = 0; i < 8; i++) {
        out[11 - i] ^= (uint8_t)(seq >> (i * 8));
    }
}

/* Send a TLS 1.3 record. type is the OUTER record type. For
 * encrypted records (handshake-after-handshake-keys, app data) the
 * outer type is always APP_DATA (23) and the INNER type (which
 * tells the receiver "this was actually a handshake") is appended
 * to the plaintext before AEAD encryption. */
static int send_record(int fd, uint8_t outer_type,
                       const uint8_t *frag, int frag_len) {
    if (frag_len < 0 || frag_len > TLS_MAX_FRAGMENT + 256) return -1;
    uint8_t hdr[5];
    hdr[0] = outer_type;
    hdr[1] = 0x03; hdr[2] = 0x03;       /* legacy_record_version: TLS 1.2 wire value */
    hdr[3] = (uint8_t)(frag_len >> 8);
    hdr[4] = (uint8_t)frag_len;
    if (write_all(fd, hdr, 5) < 0) return -1;
    if (frag_len && write_all(fd, frag, frag_len) < 0) return -1;
    return 0;
}

static int send_encrypted(struct tls_conn *c, struct tls_keys *k,
                          uint8_t inner_type,
                          const uint8_t *plaintext, int pt_len) {
    if (pt_len + 1 + TLS_TAG_LEN > TLS_MAX_FRAGMENT + 256) return -1;

    /* Single buffer reused for plaintext-then-ciphertext. AES-GCM
     * supports in-place encryption (ct[i] = pt[i] ^ ks[i]) so we
     * pass the same pointer for in and out. Halves stack usage. */
    uint8_t buf[TLS_MAX_FRAGMENT + 1];
    for (int i = 0; i < pt_len; i++) buf[i] = plaintext[i];
    buf[pt_len] = inner_type;
    int inner_len = pt_len + 1;

    int rec_len = inner_len + TLS_TAG_LEN;
    uint8_t hdr[5];
    hdr[0] = TLS_REC_APP_DATA;
    hdr[1] = 0x03; hdr[2] = 0x03;
    hdr[3] = (uint8_t)(rec_len >> 8);
    hdr[4] = (uint8_t)rec_len;

    uint8_t nonce[12], tag[TLS_TAG_LEN];
    build_nonce(nonce, k->iv, k->seq);
    aes_gcm_encrypt(k->key, nonce, hdr, 5, buf, inner_len, buf, tag);
    k->seq++;

    if (write_all(c->fd, hdr, 5) < 0) return -1;
    if (write_all(c->fd, buf, inner_len) < 0) return -1;
    if (write_all(c->fd, tag, TLS_TAG_LEN) < 0) return -1;
    return 0;
}

/* Receive a record. Returns the inner content type (1B; the byte
 * after the decrypted plaintext) for encrypted records, or the outer
 * type for plaintext records. *frag_len is set to the plaintext
 * length (excluding the inner_type byte for encrypted records). */
static int recv_record(struct tls_conn *c, struct tls_keys *k,
                       int decrypt,
                       uint8_t *out, int *out_len) {
    uint8_t hdr[5];
    if (read_n(c->fd, hdr, 5) < 0) return -1;
    int rec_len = (hdr[3] << 8) | hdr[4];
    if (rec_len < 0 || rec_len > TLS_MAX_FRAGMENT + 256) return -1;

    if (!decrypt) {
        if (read_n(c->fd, out, rec_len) < 0) return -1;
        *out_len = rec_len;
        return hdr[0];
    }

    /* Encrypted record: rec_len = ct_len + tag. Read ciphertext
     * directly into the caller's `out` buffer; aes_gcm_decrypt
     * supports in-place (pt and ct can be same pointer). */
    if (rec_len < TLS_TAG_LEN) return -1;
    int ct_len = rec_len - TLS_TAG_LEN;
    uint8_t tag[TLS_TAG_LEN];
    if (read_n(c->fd, out, ct_len) < 0) return -1;
    if (read_n(c->fd, tag, TLS_TAG_LEN) < 0) return -1;

    uint8_t nonce[12];
    build_nonce(nonce, k->iv, k->seq);
    if (aes_gcm_decrypt(k->key, nonce, hdr, 5, out, ct_len, tag, out) < 0) {
        return -1;
    }
    k->seq++;
    if (ct_len < 1) return -1;
    *out_len = ct_len - 1;
    return out[ct_len - 1];      /* inner_type */
}

/* ---- Handshake message helpers ---------------------------------- */

/* Build a handshake-message body and prefix with the 4-byte
 * msg_type+length header, then run the result through the
 * transcript hasher. Returns the total length (header included). */
static int hs_pack(uint8_t *dst, uint8_t msg_type,
                   const uint8_t *body, int body_len) {
    dst[0] = msg_type;
    dst[1] = (uint8_t)((body_len >> 16) & 0xFF);
    dst[2] = (uint8_t)((body_len >>  8) & 0xFF);
    dst[3] = (uint8_t)(body_len & 0xFF);
    for (int i = 0; i < body_len; i++) dst[4 + i] = body[i];
    return 4 + body_len;
}

/* ---- Key schedule ------------------------------------------------ */

static void derive_traffic_keys(const uint8_t secret[32], struct tls_keys *k) {
    tls13_hkdf_expand_label(secret, "key", 0, 0, k->key, TLS_AES_KEY_LEN);
    tls13_hkdf_expand_label(secret, "iv",  0, 0, k->iv,  TLS_AES_IV_LEN);
    k->seq = 0;
}

static void compute_handshake_keys(struct tls_conn *c) {
    /* early_secret = HKDF-Extract(salt=0, ikm=PSK) */
    uint8_t zero[32] = {0};
    hkdf_extract(zero, 32, c->psk, c->psk_len, c->early_secret);

    /* derived_for_handshake = Derive-Secret(early, "derived", H("")) */
    uint8_t empty_hash[32];
    sha256("", 0, empty_hash);
    uint8_t derived[32];
    tls13_derive_secret(c->early_secret, "derived", empty_hash, 32, derived);

    /* handshake_secret = HKDF-Extract(salt=derived, ikm=ECDHE shared) */
    hkdf_extract(derived, 32, c->ecdhe_shared, 32, c->handshake_secret);

    /* Snapshot transcript = SHA256(ClientHello || ServerHello) */
    struct sha256 t = c->transcript;
    uint8_t th[32];
    sha256_final(&t, th);

    /* c_hs_traffic = Derive-Secret(handshake, "c hs traffic", th) */
    tls13_derive_secret(c->handshake_secret, "c hs traffic", th, 32,
                        c->c_hs_traffic_secret);
    tls13_derive_secret(c->handshake_secret, "s hs traffic", th, 32,
                        c->s_hs_traffic_secret);
    derive_traffic_keys(c->c_hs_traffic_secret, &c->c_hs_keys);
    derive_traffic_keys(c->s_hs_traffic_secret, &c->s_hs_keys);
}

static void compute_application_keys(struct tls_conn *c) {
    /* derived_for_master = Derive-Secret(handshake, "derived", H("")) */
    uint8_t empty_hash[32];
    sha256("", 0, empty_hash);
    uint8_t derived[32];
    tls13_derive_secret(c->handshake_secret, "derived", empty_hash, 32, derived);

    uint8_t zero[32] = {0};
    hkdf_extract(derived, 32, zero, 32, c->master_secret);

    /* Transcript = ClientHello || ServerHello || ServerFinished
     *              || ClientFinished. */
    struct sha256 t = c->transcript;
    uint8_t th[32];
    sha256_final(&t, th);

    tls13_derive_secret(c->master_secret, "c ap traffic", th, 32,
                        c->c_ap_traffic_secret);
    tls13_derive_secret(c->master_secret, "s ap traffic", th, 32,
                        c->s_ap_traffic_secret);
    derive_traffic_keys(c->c_ap_traffic_secret, &c->c_ap_keys);
    derive_traffic_keys(c->s_ap_traffic_secret, &c->s_ap_keys);
}

/* Compute Finished verify_data = HMAC(finished_key, transcript_hash)
 * where finished_key = HKDF-Expand-Label(traffic_secret, "finished", "") */
static void compute_finished(const uint8_t traffic[32],
                             const struct sha256 *transcript,
                             uint8_t out[32]) {
    uint8_t fkey[32];
    tls13_hkdf_expand_label(traffic, "finished", 0, 0, fkey, 32);
    struct sha256 t = *transcript;
    uint8_t th[32];
    sha256_final(&t, th);
    hmac_sha256(fkey, 32, th, 32, out);
}

/* ---- Server handshake ------------------------------------------ */

int tls_server_handshake(struct tls_conn *c, int fd,
                         const uint8_t *psk, size_t psk_len,
                         const char *psk_id) {
    /* Initialize. */
    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    c->fd = fd;
    c->is_server = 1;
    c->psk = psk;
    c->psk_len = psk_len;
    c->psk_id = psk_id;
    size_t pidn = 0; while (psk_id[pidn]) pidn++;
    c->psk_id_len = pidn;
    sha256_init(&c->transcript);

    /* === 1. Receive ClientHello === */
    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -1;
    if (rlen < 4) return -2;
    if (recbuf[0] != TLS_HS_CLIENT_HELLO) return -3;

    int body_len = (recbuf[1] << 16) | (recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen) return -4;
    /* Body: random(32) || psk_id_len(2) || psk_id || client_pub(32) */
    if (body_len < 32 + 2 + 32) return -5;
    int o = 4;
    for (int i = 0; i < 32; i++) c->client_random[i] = recbuf[o++];
    int idlen = (recbuf[o] << 8) | recbuf[o+1]; o += 2;
    if (idlen > 64 || o + idlen + 32 > rlen) return -6;
    /* Verify PSK identity. */
    if ((size_t)idlen != c->psk_id_len) return -7;
    for (int i = 0; i < idlen; i++)
        if (recbuf[o + i] != (uint8_t)c->psk_id[i]) return -8;
    o += idlen;
    for (int i = 0; i < 32; i++) c->client_pub[i] = recbuf[o++];

    /* Add ClientHello to transcript. */
    sha256_update(&c->transcript, recbuf, body_len + 4);

    /* === 2. Send ServerHello === */
    rand_bytes(c->server_random, 32);
    rand_bytes(c->our_priv, 32);
    /* Clamp inside x25519, but compute pub from our priv now. */
    x25519(c->server_pub, c->our_priv, x25519_basepoint);

    /* Body: random(32) || server_pub(32) */
    uint8_t sh_body[32 + 32];
    for (int i = 0; i < 32; i++) sh_body[i]      = c->server_random[i];
    for (int i = 0; i < 32; i++) sh_body[32 + i] = c->server_pub[i];

    uint8_t sh_msg[4 + 64];
    int sh_len = hs_pack(sh_msg, TLS_HS_SERVER_HELLO, sh_body, sizeof(sh_body));
    if (send_record(fd, TLS_REC_HANDSHAKE, sh_msg, sh_len) < 0) return -10;
    sha256_update(&c->transcript, sh_msg, sh_len);

    /* === 3. Compute ECDHE shared + handshake keys === */
    x25519(c->ecdhe_shared, c->our_priv, c->client_pub);
    compute_handshake_keys(c);

    /* === 4. Send ServerFinished (encrypted) === */
    uint8_t verify[32];
    compute_finished(c->s_hs_traffic_secret, &c->transcript, verify);
    uint8_t fin_body[32];
    for (int i = 0; i < 32; i++) fin_body[i] = verify[i];
    uint8_t fin_msg[4 + 32];
    int fin_len = hs_pack(fin_msg, TLS_HS_FINISHED, fin_body, 32);
    if (send_encrypted(c, &c->s_hs_keys, TLS_REC_HANDSHAKE,
                       fin_msg, fin_len) < 0) return -11;
    sha256_update(&c->transcript, fin_msg, fin_len);

    /* === 5. Receive ClientFinished (encrypted) === */
    uint8_t cfin[64];
    int cfin_len;
    int it = recv_record(c, &c->c_hs_keys, 1, cfin, &cfin_len);
    if (it != TLS_REC_HANDSHAKE) return -12;
    if (cfin_len < 4 || cfin[0] != TLS_HS_FINISHED) return -13;

    /* Verify against expected (computed with current transcript = CH || SH || SF). */
    uint8_t expect[32];
    compute_finished(c->c_hs_traffic_secret, &c->transcript, expect);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (cfin[4 + i] != expect[i]) ok = 0;
    if (!ok) return -14;
    sha256_update(&c->transcript, cfin, cfin_len);

    /* === 6. Derive application traffic keys === */
    compute_application_keys(c);
    return 0;
}

/* ---- Client handshake ------------------------------------------- */

int tls_client_handshake(struct tls_conn *c, int fd,
                         const uint8_t *psk, size_t psk_len,
                         const char *psk_id) {
    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    c->fd = fd;
    c->is_server = 0;
    c->psk = psk;
    c->psk_len = psk_len;
    c->psk_id = psk_id;
    size_t pidn = 0; while (psk_id[pidn]) pidn++;
    c->psk_id_len = pidn;
    sha256_init(&c->transcript);

    /* === 1. Send ClientHello === */
    rand_bytes(c->client_random, 32);
    rand_bytes(c->our_priv, 32);
    x25519(c->client_pub, c->our_priv, x25519_basepoint);

    /* Body: random(32) || psk_id_len(2) || psk_id || client_pub(32) */
    uint8_t ch_body[32 + 2 + 64 + 32];
    int o = 0;
    for (int i = 0; i < 32; i++) ch_body[o++] = c->client_random[i];
    ch_body[o++] = (uint8_t)(c->psk_id_len >> 8);
    ch_body[o++] = (uint8_t)c->psk_id_len;
    for (size_t i = 0; i < c->psk_id_len; i++) ch_body[o++] = (uint8_t)psk_id[i];
    for (int i = 0; i < 32; i++) ch_body[o++] = c->client_pub[i];

    uint8_t ch_msg[4 + 32 + 2 + 64 + 32];
    int ch_len = hs_pack(ch_msg, TLS_HS_CLIENT_HELLO, ch_body, o);
    if (send_record(fd, TLS_REC_HANDSHAKE, ch_msg, ch_len) < 0) return -1;
    sha256_update(&c->transcript, ch_msg, ch_len);

    /* === 2. Receive ServerHello === */
    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -2;
    if (rlen < 4 || recbuf[0] != TLS_HS_SERVER_HELLO) return -3;
    int body_len = (recbuf[1] << 16) | (recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen || body_len < 64) return -4;
    int oo = 4;
    for (int i = 0; i < 32; i++) c->server_random[i] = recbuf[oo++];
    for (int i = 0; i < 32; i++) c->server_pub[i]    = recbuf[oo++];
    sha256_update(&c->transcript, recbuf, body_len + 4);

    /* === 3. ECDHE + handshake keys === */
    x25519(c->ecdhe_shared, c->our_priv, c->server_pub);
    compute_handshake_keys(c);

    /* === 4. Receive ServerFinished (encrypted with server's hs key) === */
    uint8_t sfin[64];
    int sfin_len;
    int it = recv_record(c, &c->s_hs_keys, 1, sfin, &sfin_len);
    if (it != TLS_REC_HANDSHAKE) return -5;
    if (sfin_len < 4 || sfin[0] != TLS_HS_FINISHED) return -6;
    uint8_t expect[32];
    compute_finished(c->s_hs_traffic_secret, &c->transcript, expect);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (sfin[4 + i] != expect[i]) ok = 0;
    if (!ok) return -7;
    sha256_update(&c->transcript, sfin, sfin_len);

    /* === 5. Send ClientFinished (encrypted with client's hs key) === */
    uint8_t verify[32];
    compute_finished(c->c_hs_traffic_secret, &c->transcript, verify);
    uint8_t fin_msg[4 + 32];
    int fin_len = hs_pack(fin_msg, TLS_HS_FINISHED, verify, 32);
    if (send_encrypted(c, &c->c_hs_keys, TLS_REC_HANDSHAKE,
                       fin_msg, fin_len) < 0) return -8;
    sha256_update(&c->transcript, fin_msg, fin_len);

    /* === 6. Derive application traffic keys === */
    compute_application_keys(c);
    return 0;
}

/* ---- Application data ------------------------------------------- */

int tls_send(struct tls_conn *c, const void *data, int n) {
    struct tls_keys *k = c->is_server ? &c->s_ap_keys : &c->c_ap_keys;
    if (send_encrypted(c, k, TLS_REC_APP_DATA,
                       (const uint8_t *)data, n) < 0) return -1;
    return n;
}

int tls_recv(struct tls_conn *c, void *out, int max_n) {
    struct tls_keys *k = c->is_server ? &c->c_ap_keys : &c->s_ap_keys;
    uint8_t buf[TLS_MAX_FRAGMENT + 16];
    int len;
    int it = recv_record(c, k, 1, buf, &len);
    if (it < 0) return -1;
    if (it != TLS_REC_APP_DATA) return -1;
    if (len > max_n) len = max_n;
    for (int i = 0; i < len; i++) ((uint8_t *)out)[i] = buf[i];
    return len;
}
