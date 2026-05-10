/*
 * AdventOS minimal TLS 1.3.
 *
 * Two server flows live here:
 *
 *   PSK flow  (tls_server_handshake_psk / tls_client_handshake_psk)
 *     Original session-36 demo. ClientHello body is just
 *     {random, psk_id_len, psk_id, x25519_pub} — no extensions,
 *     no certificate, single hardcoded ciphersuite. Speaks to
 *     httpsget over the OS↔OS demo path.
 *
 *   Cert flow (tls_server_handshake_cert)
 *     RFC 8446-faithful. Parses real ClientHello extensions
 *     (supported_versions, key_share, signature_algorithms),
 *     sends ServerHello + ChangeCipherSpec + EncryptedExtensions
 *     + Certificate + CertificateVerify + Finished.
 *     This is what `curl -k https://10.0.2.15:4433/` talks.
 *
 * Both flows share record I/O (send_record, send_encrypted,
 * recv_record), key schedule (HKDF-Extract/Expand-Label,
 * Derive-Secret), and AEAD nonce construction (sequence ^ iv).
 */
#include "tls.h"
#include "x509.h"     /* X509_MAX_CERT */

/* ---- I/O helpers ------------------------------------------------- */

static int read_n(int fd, void *buf, int n) {
    uint8_t *p = (uint8_t *)buf;
    int      got = 0;
    while (got < n) {
        int r = sys_read(fd, p + got, n - got);
        if (r <= 0) return -1;
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

static void build_nonce(uint8_t out[12], const uint8_t iv[12], uint64_t seq) {
    for (int i = 0; i < 12; i++) out[i] = iv[i];
    for (int i = 0; i < 8; i++) {
        out[11 - i] ^= (uint8_t)(seq >> (i * 8));
    }
}

static int send_record(int fd, uint8_t outer_type,
                       const uint8_t *frag, int frag_len) {
    if (frag_len < 0 || frag_len > TLS_MAX_FRAGMENT + 256) return -1;
    uint8_t hdr[5];
    hdr[0] = outer_type;
    hdr[1] = 0x03; hdr[2] = 0x03;
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

/* Receive a record. Transparently skips ChangeCipherSpec records
 * (TLS 1.3 §5: middlebox-compatibility, no semantic effect).
 *
 *   k != NULL    → decrypt; returns inner_type
 *   k == NULL    → plaintext; returns outer_type (record header)
 *
 * *out_len is the plaintext length (excluding inner_type byte for
 * encrypted records). */
static int recv_record(struct tls_conn *c, struct tls_keys *k,
                       uint8_t *out, int *out_len) {
    for (;;) {
        uint8_t hdr[5];
        if (read_n(c->fd, hdr, 5) < 0) return -1;
        int rec_len = (hdr[3] << 8) | hdr[4];
        if (rec_len < 0 || rec_len > TLS_MAX_FRAGMENT + 256) return -1;

        if (hdr[0] == TLS_REC_CHANGE_CIPHER) {
            /* Drain and ignore. Don't bump k->seq — CCS is plaintext
             * and not part of the AEAD record sequence. */
            uint8_t junk[8];
            int rem = rec_len;
            while (rem > 0) {
                int take = rem > (int)sizeof(junk) ? (int)sizeof(junk) : rem;
                if (read_n(c->fd, junk, take) < 0) return -1;
                rem -= take;
            }
            continue;
        }

        if (k == 0) {
            if (read_n(c->fd, out, rec_len) < 0) return -1;
            *out_len = rec_len;
            return hdr[0];
        }

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
        /* Skip TLS-level padding: trailing zeros before the inner type. */
        int pt_end = ct_len - 1;
        while (pt_end > 0 && out[pt_end] == 0) pt_end--;
        *out_len = pt_end;
        return out[pt_end];
    }
}

/* ---- Handshake message helpers ---------------------------------- */

static int hs_pack(uint8_t *dst, uint8_t msg_type,
                   const uint8_t *body, int body_len) {
    dst[0] = msg_type;
    dst[1] = (uint8_t)((body_len >> 16) & 0xFF);
    dst[2] = (uint8_t)((body_len >>  8) & 0xFF);
    dst[3] = (uint8_t)(body_len & 0xFF);
    for (int i = 0; i < body_len; i++) dst[4 + i] = body[i];
    return 4 + body_len;
}

/* Append a handshake message (built in `body`) into the running
 * encrypted-flight buffer at offset *off, also feeding it through
 * the transcript hasher. Returns 0 / -1 on overflow. */
static int hs_append(uint8_t *buf, int cap, int *off,
                     uint8_t msg_type,
                     const uint8_t *body, int body_len,
                     struct sha256 *transcript) {
    int need = 4 + body_len;
    if (*off + need > cap) return -1;
    int n = hs_pack(buf + *off, msg_type, body, body_len);
    sha256_update(transcript, buf + *off, n);
    *off += n;
    return 0;
}

/* ---- Key schedule ------------------------------------------------ */

static void derive_traffic_keys(const uint8_t secret[32], struct tls_keys *k) {
    tls13_hkdf_expand_label(secret, "key", 0, 0, k->key, TLS_AES_KEY_LEN);
    tls13_hkdf_expand_label(secret, "iv",  0, 0, k->iv,  TLS_AES_IV_LEN);
    k->seq = 0;
}

static void compute_handshake_keys_psk(struct tls_conn *c) {
    /* PSK flow: PSK becomes the early_secret IKM. */
    uint8_t zero[32] = {0};
    hkdf_extract(zero, 32, c->psk, c->psk_len, c->early_secret);

    uint8_t empty_hash[32];
    sha256("", 0, empty_hash);
    uint8_t derived[32];
    tls13_derive_secret(c->early_secret, "derived", empty_hash, 32, derived);

    hkdf_extract(derived, 32, c->ecdhe_shared, 32, c->handshake_secret);

    struct sha256 t = c->transcript;
    uint8_t th[32];
    sha256_final(&t, th);

    tls13_derive_secret(c->handshake_secret, "c hs traffic", th, 32,
                        c->c_hs_traffic_secret);
    tls13_derive_secret(c->handshake_secret, "s hs traffic", th, 32,
                        c->s_hs_traffic_secret);
    derive_traffic_keys(c->c_hs_traffic_secret, &c->c_hs_keys);
    derive_traffic_keys(c->s_hs_traffic_secret, &c->s_hs_keys);
}

static void compute_handshake_keys_cert(struct tls_conn *c) {
    /* Cert flow: no PSK, so early_secret IKM is 32 zero bytes. */
    uint8_t zero[32] = {0};
    hkdf_extract(zero, 32, zero, 32, c->early_secret);

    uint8_t empty_hash[32];
    sha256("", 0, empty_hash);
    uint8_t derived[32];
    tls13_derive_secret(c->early_secret, "derived", empty_hash, 32, derived);

    hkdf_extract(derived, 32, c->ecdhe_shared, 32, c->handshake_secret);

    struct sha256 t = c->transcript;
    uint8_t th[32];
    sha256_final(&t, th);

    tls13_derive_secret(c->handshake_secret, "c hs traffic", th, 32,
                        c->c_hs_traffic_secret);
    tls13_derive_secret(c->handshake_secret, "s hs traffic", th, 32,
                        c->s_hs_traffic_secret);
    derive_traffic_keys(c->c_hs_traffic_secret, &c->c_hs_keys);
    derive_traffic_keys(c->s_hs_traffic_secret, &c->s_hs_keys);
}

static void compute_application_keys(struct tls_conn *c) {
    uint8_t empty_hash[32];
    sha256("", 0, empty_hash);
    uint8_t derived[32];
    tls13_derive_secret(c->handshake_secret, "derived", empty_hash, 32, derived);

    uint8_t zero[32] = {0};
    hkdf_extract(derived, 32, zero, 32, c->master_secret);

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

/* ============================================================
 * PSK flow (kept for the OS↔OS httpsget demo)
 * ============================================================ */

int tls_server_handshake_psk(struct tls_conn *c, int fd,
                             const uint8_t *psk, size_t psk_len,
                             const char *psk_id) {
    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    c->fd = fd;
    c->is_server = 1;
    c->psk = psk;
    c->psk_len = psk_len;
    c->psk_id = psk_id;
    size_t pidn = 0; while (psk_id[pidn]) pidn++;
    c->psk_id_len = pidn;
    sha256_init(&c->transcript);

    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -1;
    if (rlen < 4) return -2;
    if (recbuf[0] != TLS_HS_CLIENT_HELLO) return -3;

    int body_len = (recbuf[1] << 16) | (recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen) return -4;
    if (body_len < 32 + 2 + 32) return -5;
    int o = 4;
    for (int i = 0; i < 32; i++) c->client_random[i] = recbuf[o++];
    int idlen = (recbuf[o] << 8) | recbuf[o+1]; o += 2;
    if (idlen > 64 || o + idlen + 32 > rlen) return -6;
    if ((size_t)idlen != c->psk_id_len) return -7;
    for (int i = 0; i < idlen; i++)
        if (recbuf[o + i] != (uint8_t)c->psk_id[i]) return -8;
    o += idlen;
    for (int i = 0; i < 32; i++) c->client_pub[i] = recbuf[o++];

    sha256_update(&c->transcript, recbuf, body_len + 4);

    rand_bytes(c->server_random, 32);
    rand_bytes(c->our_priv, 32);
    x25519(c->server_pub, c->our_priv, x25519_basepoint);

    uint8_t sh_body[32 + 32];
    for (int i = 0; i < 32; i++) sh_body[i]      = c->server_random[i];
    for (int i = 0; i < 32; i++) sh_body[32 + i] = c->server_pub[i];

    uint8_t sh_msg[4 + 64];
    int sh_len = hs_pack(sh_msg, TLS_HS_SERVER_HELLO, sh_body, sizeof(sh_body));
    if (send_record(fd, TLS_REC_HANDSHAKE, sh_msg, sh_len) < 0) return -10;
    sha256_update(&c->transcript, sh_msg, sh_len);

    x25519(c->ecdhe_shared, c->our_priv, c->client_pub);
    compute_handshake_keys_psk(c);

    uint8_t verify[32];
    compute_finished(c->s_hs_traffic_secret, &c->transcript, verify);
    uint8_t fin_msg[4 + 32];
    int fin_len = hs_pack(fin_msg, TLS_HS_FINISHED, verify, 32);
    if (send_encrypted(c, &c->s_hs_keys, TLS_REC_HANDSHAKE,
                       fin_msg, fin_len) < 0) return -11;
    sha256_update(&c->transcript, fin_msg, fin_len);

    uint8_t cfin[64];
    int cfin_len;
    int it = recv_record(c, &c->c_hs_keys, cfin, &cfin_len);
    if (it != TLS_REC_HANDSHAKE) return -12;
    if (cfin_len < 4 || cfin[0] != TLS_HS_FINISHED) return -13;

    uint8_t expect[32];
    compute_finished(c->c_hs_traffic_secret, &c->transcript, expect);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (cfin[4 + i] != expect[i]) ok = 0;
    if (!ok) return -14;
    sha256_update(&c->transcript, cfin, cfin_len);

    compute_application_keys(c);
    return 0;
}

int tls_client_handshake_psk(struct tls_conn *c, int fd,
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

    rand_bytes(c->client_random, 32);
    rand_bytes(c->our_priv, 32);
    x25519(c->client_pub, c->our_priv, x25519_basepoint);

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

    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -2;
    if (rlen < 4 || recbuf[0] != TLS_HS_SERVER_HELLO) return -3;
    int body_len = (recbuf[1] << 16) | (recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen || body_len < 64) return -4;
    int oo = 4;
    for (int i = 0; i < 32; i++) c->server_random[i] = recbuf[oo++];
    for (int i = 0; i < 32; i++) c->server_pub[i]    = recbuf[oo++];
    sha256_update(&c->transcript, recbuf, body_len + 4);

    x25519(c->ecdhe_shared, c->our_priv, c->server_pub);
    compute_handshake_keys_psk(c);

    uint8_t sfin[64];
    int sfin_len;
    int it = recv_record(c, &c->s_hs_keys, sfin, &sfin_len);
    if (it != TLS_REC_HANDSHAKE) return -5;
    if (sfin_len < 4 || sfin[0] != TLS_HS_FINISHED) return -6;
    uint8_t expect[32];
    compute_finished(c->s_hs_traffic_secret, &c->transcript, expect);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (sfin[4 + i] != expect[i]) ok = 0;
    if (!ok) return -7;
    sha256_update(&c->transcript, sfin, sfin_len);

    uint8_t verify[32];
    compute_finished(c->c_hs_traffic_secret, &c->transcript, verify);
    uint8_t fin_msg[4 + 32];
    int fin_len = hs_pack(fin_msg, TLS_HS_FINISHED, verify, 32);
    if (send_encrypted(c, &c->c_hs_keys, TLS_REC_HANDSHAKE,
                       fin_msg, fin_len) < 0) return -8;
    sha256_update(&c->transcript, fin_msg, fin_len);

    compute_application_keys(c);
    return 0;
}

/* ============================================================
 * Cert flow (RFC 8446 with X.509 + Ed25519 CertificateVerify)
 * ============================================================ */

/* Read a 16-bit big-endian field from buf[off..]. */
static int rd16(const uint8_t *buf, int off) {
    return ((int)buf[off] << 8) | (int)buf[off + 1];
}

/* Walk the ClientHello extensions block, looking for the
 * key_share extension's x25519 entry. Sets *out_pub to point
 * at the 32-byte client public key (within the buffer). Also
 * verifies that supported_versions advertises TLS 1.3 and
 * signature_algorithms advertises Ed25519.
 *
 * Returns 0 on success, negative on malformed / missing.  */
static int parse_clienthello_extensions(
    const uint8_t *ext, int ext_len,
    const uint8_t **out_pub,
    int server_wants_sig_alg,
    int *out_chosen_sig_alg)
{
    int seen_tls13 = 0, seen_match = 0, seen_x25519_share = 0;
    *out_chosen_sig_alg = 0;
    int o = 0;
    while (o + 4 <= ext_len) {
        int et = rd16(ext, o);
        int el = rd16(ext, o + 2);
        o += 4;
        if (o + el > ext_len) return -1;

        if (et == 0x002B) {
            /* supported_versions: 1B list_len + list of 2B versions */
            if (el < 1) return -2;
            int ll = ext[o];
            if (ll < 2 || ll + 1 > el) return -3;
            for (int j = 0; j + 1 < ll; j += 2) {
                if (rd16(ext, o + 1 + j) == 0x0304) seen_tls13 = 1;
            }
        } else if (et == 0x000D) {
            /* signature_algorithms: 2B list_len + list of 2B schemes.
             * Match server_wants_sig_alg against the client's offered
             * list. Server has only one cert kind on hand (whichever
             * the httpsd configured), so this is "does the client
             * support the alg of our cert?" — not actual negotiation. */
            if (el < 2) return -4;
            int ll = rd16(ext, o);
            if (ll + 2 > el) return -5;
            for (int j = 0; j + 1 < ll; j += 2) {
                int sa = rd16(ext, o + 2 + j);
                if (sa == server_wants_sig_alg) {
                    seen_match = 1;
                    *out_chosen_sig_alg = sa;
                }
            }
        } else if (et == 0x0033) {
            /* key_share: 2B client_shares_len + entries
             *   each entry: 2B group + 2B key_len + key */
            if (el < 2) return -6;
            int sl = rd16(ext, o);
            if (sl + 2 > el) return -7;
            int p = o + 2;
            int end = o + 2 + sl;
            while (p + 4 <= end) {
                int grp  = rd16(ext, p);
                int klen = rd16(ext, p + 2);
                if (p + 4 + klen > end) return -8;
                if (grp == 0x001D && klen == 32) {
                    *out_pub = ext + p + 4;
                    seen_x25519_share = 1;
                }
                p += 4 + klen;
            }
        }
        /* Other extensions (server_name, supported_groups, ALPN,
         * etc.) are silently ignored. */
        o += el;
    }
    if (!seen_tls13)        return -10;
    if (!seen_match)        return -11;
    if (!seen_x25519_share) return -12;
    return 0;
}

/* Build the ServerHello body. Returns body length. */
static int build_serverhello_body(struct tls_conn *c, uint8_t *body) {
    int o = 0;
    body[o++] = 0x03; body[o++] = 0x03;          /* legacy_version = TLS 1.2 */
    for (int i = 0; i < 32; i++) body[o++] = c->server_random[i];
    body[o++] = (uint8_t)c->session_id_len;
    for (int i = 0; i < c->session_id_len; i++) body[o++] = c->session_id[i];
    body[o++] = 0x13; body[o++] = 0x01;          /* TLS_AES_128_GCM_SHA256 */
    body[o++] = 0x00;                            /* legacy_compression_method = null */

    /* Extensions. We need supported_versions (selected = 0x0304)
     * and key_share (group = x25519, our 32-byte pub). */
    int ext_len_off = o;
    o += 2;     /* placeholder for ext_len */

    /* supported_versions extension */
    body[o++] = 0x00; body[o++] = 0x2B;          /* type = 43 */
    body[o++] = 0x00; body[o++] = 0x02;          /* len = 2 */
    body[o++] = 0x03; body[o++] = 0x04;          /* selected = TLS 1.3 */

    /* key_share extension: group + key_len + key */
    body[o++] = 0x00; body[o++] = 0x33;          /* type = 51 */
    body[o++] = 0x00; body[o++] = 36;            /* len = 36 */
    body[o++] = 0x00; body[o++] = 0x1D;          /* group = x25519 */
    body[o++] = 0x00; body[o++] = 32;            /* key len = 32 */
    for (int i = 0; i < 32; i++) body[o++] = c->server_pub[i];

    int ext_total = o - ext_len_off - 2;
    body[ext_len_off    ] = (uint8_t)(ext_total >> 8);
    body[ext_len_off + 1] = (uint8_t)ext_total;
    return o;
}

/* Build the CertificateVerify signature input per RFC 8446 §4.4.3:
 *   64 octets of 0x20  ||  "TLS 1.3, server CertificateVerify"
 *   ||  0x00  ||  transcript_hash[32]   = 130 bytes total. */
static void build_cv_sign_input(uint8_t out[130],
                                const uint8_t transcript_hash[32])
{
    static const char CTX[] = "TLS 1.3, server CertificateVerify";
    int o = 0;
    for (int i = 0; i < 64; i++) out[o++] = 0x20;
    for (int i = 0; CTX[i]; i++) out[o++] = (uint8_t)CTX[i];
    out[o++] = 0x00;
    for (int i = 0; i < 32; i++) out[o++] = transcript_hash[i];
}

int tls_server_handshake_cert(struct tls_conn *c, int fd) {
    /* Save inputs the caller stashed (cert_der, server_sk) so we
     * can zero the rest. */
    const uint8_t *cert_der    = c->cert_der;
    int            cert_len    = c->cert_der_len;
    const uint8_t *sk          = c->server_sk;

    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    c->fd = fd;
    c->is_server = 1;
    c->cert_der = cert_der;
    c->cert_der_len = cert_len;
    c->server_sk = sk;
    sha256_init(&c->transcript);

    /* === 1. Receive ClientHello === */
    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -1;
    if (rlen < 4 || recbuf[0] != TLS_HS_CLIENT_HELLO) return -2;

    int body_len = ((int)recbuf[1] << 16) | ((int)recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen) return -3;

    /* Parse fixed-position fields up to extensions. */
    int o = 4;
    if (o + 2 + 32 > rlen) return -4;
    /* legacy_version (2B) — TLS 1.3 wire shows 0x0303, ignore */
    o += 2;
    for (int i = 0; i < 32; i++) c->client_random[i] = recbuf[o++];

    /* legacy_session_id: 1B length + bytes */
    if (o + 1 > rlen) return -5;
    int sid_len = recbuf[o++];
    if (sid_len > 32 || o + sid_len > rlen) return -6;
    c->session_id_len = sid_len;
    for (int i = 0; i < sid_len; i++) c->session_id[i] = recbuf[o++];

    /* cipher_suites: 2B length + suites */
    if (o + 2 > rlen) return -7;
    int cs_len = rd16(recbuf, o); o += 2;
    if (cs_len < 2 || cs_len & 1 || o + cs_len > rlen) return -8;
    int found_aes_gcm = 0;
    for (int i = 0; i < cs_len; i += 2) {
        if (rd16(recbuf, o + i) == 0x1301) { found_aes_gcm = 1; break; }
    }
    o += cs_len;
    if (!found_aes_gcm) return -9;

    /* compression_methods: 1B length + bytes */
    if (o + 1 > rlen) return -10;
    int cm_len = recbuf[o++];
    if (o + cm_len > rlen) return -11;
    o += cm_len;

    /* extensions: 2B length + bytes */
    if (o + 2 > rlen) return -12;
    int ext_len = rd16(recbuf, o); o += 2;
    if (o + ext_len > rlen) return -13;
    const uint8_t *cli_pub = 0;
    int chosen_sig = 0;
    int want_sig = c->sig_alg ? c->sig_alg : 0x0807;   /* default ed25519 */
    int per = parse_clienthello_extensions(recbuf + o, ext_len, &cli_pub,
                                           want_sig, &chosen_sig);
    if (per < 0) return -1400 + per;     /* -1401..-1412 — see parser */
    c->sig_alg = chosen_sig;
    for (int i = 0; i < 32; i++) c->client_pub[i] = cli_pub[i];

    /* Update transcript with the full ClientHello message (header + body). */
    sha256_update(&c->transcript, recbuf, body_len + 4);

    /* === 2. Send ServerHello === */
    rand_bytes(c->server_random, 32);
    rand_bytes(c->our_priv, 32);
    x25519(c->server_pub, c->our_priv, x25519_basepoint);

    uint8_t sh_body[256];
    int sh_body_len = build_serverhello_body(c, sh_body);

    uint8_t sh_msg[4 + 256];
    int sh_msg_len = hs_pack(sh_msg, TLS_HS_SERVER_HELLO, sh_body, sh_body_len);
    if (send_record(fd, TLS_REC_HANDSHAKE, sh_msg, sh_msg_len) < 0) return -15;
    sha256_update(&c->transcript, sh_msg, sh_msg_len);

    /* === 3. Send ChangeCipherSpec (middlebox compatibility) === */
    uint8_t ccs = 0x01;
    if (send_record(fd, TLS_REC_CHANGE_CIPHER, &ccs, 1) < 0) return -16;

    /* === 4. Compute handshake keys === */
    x25519(c->ecdhe_shared, c->our_priv, c->client_pub);
    compute_handshake_keys_cert(c);

    /* === 5–8. Build encrypted server flight: EE || Cert || CV || Finished === */
    uint8_t flight[TLS_MAX_FRAGMENT];
    int foff = 0;

    /* EncryptedExtensions: empty extensions list (2B = 0) */
    {
        uint8_t ee_body[2] = { 0x00, 0x00 };
        if (hs_append(flight, sizeof(flight), &foff,
                      TLS_HS_ENCRYPTED_EXTS, ee_body, 2,
                      &c->transcript) < 0) return -17;
    }

    /* Certificate:
     *   1B  certificate_request_context_len = 0
     *   3B  certificate_list_len
     *   per cert: 3B cert_len + cert + 2B exts_len = 0 */
    {
        if (cert_len <= 0) return -18;
        int cert_entry_len = 3 + cert_len + 2;
        int list_len       = cert_entry_len;
        int body_total     = 1 + 3 + cert_entry_len;
        uint8_t cert_body[X509_MAX_CERT + 16];
        if (body_total > (int)sizeof(cert_body)) return -19;
        int p = 0;
        cert_body[p++] = 0x00;                                 /* ctx len */
        cert_body[p++] = (uint8_t)((list_len >> 16) & 0xFF);
        cert_body[p++] = (uint8_t)((list_len >>  8) & 0xFF);
        cert_body[p++] = (uint8_t) (list_len & 0xFF);
        cert_body[p++] = (uint8_t)((cert_len >> 16) & 0xFF);
        cert_body[p++] = (uint8_t)((cert_len >>  8) & 0xFF);
        cert_body[p++] = (uint8_t) (cert_len & 0xFF);
        for (int i = 0; i < cert_len; i++) cert_body[p++] = cert_der[i];
        cert_body[p++] = 0x00;
        cert_body[p++] = 0x00;
        if (hs_append(flight, sizeof(flight), &foff,
                      TLS_HS_CERTIFICATE, cert_body, body_total,
                      &c->transcript) < 0) return -20;
    }

    /* CertificateVerify: snapshot transcript, sign, append.
     *
     * Both supported signature schemes sign the same input — the
     * 130-byte CV-context construction from RFC 8446 §4.4.3.
     *
     *   Ed25519 (0x0807): raw signature is exactly 64 bytes.
     *   ECDSA-P256 (0x0403): first SHA-256 the 130-byte construction,
     *                        then sign the hash; the body carries a
     *                        DER-encoded (R, S) — variable length.
     */
    {
        struct sha256 t = c->transcript;
        uint8_t th[32];
        sha256_final(&t, th);
        uint8_t to_sign[130];
        build_cv_sign_input(to_sign, th);

        uint8_t cv_body[160];     /* enough for ed25519 (68) or ecdsa (~80) */
        int     cv_body_len = 0;

        if (c->sig_alg == 0x0403) {
            /* ECDSA-with-SHA256 */
            uint8_t hash[32];
            sha256(to_sign, 130, hash);
            uint8_t rs[64];
            if (p256_sign(rs, hash, c->server_sk) != 0) return -210;
            uint8_t der[72];
            int dlen = p256_sig_to_der(der, sizeof(der), rs);
            if (dlen < 0) return -211;

            cv_body[0] = 0x04; cv_body[1] = 0x03;            /* alg */
            cv_body[2] = (uint8_t)(dlen >> 8);               /* siglen hi */
            cv_body[3] = (uint8_t)dlen;                      /* siglen lo */
            for (int i = 0; i < dlen; i++) cv_body[4 + i] = der[i];
            cv_body_len = 4 + dlen;
        } else {
            /* Ed25519, the session-39 default. */
            uint8_t sig[64];
            ed25519_sign(sig, to_sign, 130, c->server_sk);
            cv_body[0] = 0x08; cv_body[1] = 0x07;            /* alg */
            cv_body[2] = 0x00; cv_body[3] = 0x40;            /* siglen = 64 */
            for (int i = 0; i < 64; i++) cv_body[4 + i] = sig[i];
            cv_body_len = 68;
        }
        if (hs_append(flight, sizeof(flight), &foff,
                      TLS_HS_CERT_VERIFY, cv_body, cv_body_len,
                      &c->transcript) < 0) return -21;
    }

    /* Server Finished. */
    {
        uint8_t verify[32];
        compute_finished(c->s_hs_traffic_secret, &c->transcript, verify);
        if (hs_append(flight, sizeof(flight), &foff,
                      TLS_HS_FINISHED, verify, 32,
                      &c->transcript) < 0) return -22;
    }

    /* Encrypt the whole flight in a single record (inner_type =
     * HANDSHAKE since all four messages are handshake). */
    if (send_encrypted(c, &c->s_hs_keys, TLS_REC_HANDSHAKE,
                       flight, foff) < 0) return -23;

    /* === 9. Pre-derive application keys (transcript = CH..ServerFinished). */
    compute_application_keys(c);

    /* === 10. Receive ClientFinished. (recv_record skips client's CCS.) === */
    uint8_t cfin[64];
    int cfin_len;
    int it = recv_record(c, &c->c_hs_keys, cfin, &cfin_len);
    if (it != TLS_REC_HANDSHAKE) return -24;
    if (cfin_len < 4 || cfin[0] != TLS_HS_FINISHED) return -25;

    uint8_t expect[32];
    compute_finished(c->c_hs_traffic_secret, &c->transcript, expect);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (cfin[4 + i] != expect[i]) ok = 0;
    if (!ok) return -26;
    sha256_update(&c->transcript, cfin, cfin_len);

    return 0;
}

/* ---- Cert-mode client ------------------------------------------ */

/* Build a client ClientHello body matching what curl/openssl send:
 * legacy_version + random + empty session_id + [0x1301] +
 * [null compression] + extensions(supported_versions, supported_groups,
 * signature_algorithms, key_share). */
static int build_clienthello_body(struct tls_conn *c, uint8_t *body) {
    int o = 0;
    body[o++] = 0x03; body[o++] = 0x03;          /* legacy_version = TLS 1.2 */
    for (int i = 0; i < 32; i++) body[o++] = c->client_random[i];
    body[o++] = 0x00;                            /* legacy_session_id_len = 0 */
    body[o++] = 0x00; body[o++] = 0x02;          /* cipher_suites_len = 2 */
    body[o++] = 0x13; body[o++] = 0x01;          /* TLS_AES_128_GCM_SHA256 */
    body[o++] = 0x01;                            /* compression_methods_len = 1 */
    body[o++] = 0x00;                            /* null compression */

    int ext_off = o;
    o += 2;     /* placeholder for ext_len */

    /* supported_versions (43): list of versions */
    body[o++] = 0x00; body[o++] = 0x2B;
    body[o++] = 0x00; body[o++] = 0x03;          /* ext data len */
    body[o++] = 0x02;                            /* list len */
    body[o++] = 0x03; body[o++] = 0x04;          /* TLS 1.3 */

    /* supported_groups (10): list of named groups */
    body[o++] = 0x00; body[o++] = 0x0A;
    body[o++] = 0x00; body[o++] = 0x04;
    body[o++] = 0x00; body[o++] = 0x02;          /* list len */
    body[o++] = 0x00; body[o++] = 0x1D;          /* x25519 */

    /* signature_algorithms (13): list of sig schemes the client
     * will accept on the server's cert. Offer both because the OS
     * ↔ OS demo's server may be configured either way (session 43
     * switched httpsd to ECDSA for Schannel-compat; the Ed25519
     * code path still exists for completeness). */
    body[o++] = 0x00; body[o++] = 0x0D;
    body[o++] = 0x00; body[o++] = 0x06;
    body[o++] = 0x00; body[o++] = 0x04;          /* list len = 4 */
    body[o++] = 0x04; body[o++] = 0x03;          /* ecdsa_secp256r1_sha256 */
    body[o++] = 0x08; body[o++] = 0x07;          /* ed25519 */

    /* key_share (51): one share — group + key.
     *   ext data = 2B shares_len + 36B share = 38 bytes
     *   share    = 2B group + 2B keylen + 32B key = 36 bytes */
    body[o++] = 0x00; body[o++] = 0x33;
    body[o++] = 0x00; body[o++] = 38;            /* ext data len */
    body[o++] = 0x00; body[o++] = 36;            /* shares_len = one share = 36 */
    body[o++] = 0x00; body[o++] = 0x1D;          /* x25519 */
    body[o++] = 0x00; body[o++] = 32;            /* key len */
    for (int i = 0; i < 32; i++) body[o++] = c->client_pub[i];

    int ext_total = o - ext_off - 2;
    body[ext_off    ] = (uint8_t)(ext_total >> 8);
    body[ext_off + 1] = (uint8_t)ext_total;
    return o;
}

/* Parse a ServerHello extensions block, looking for the server's
 * key_share (group + 32-byte key). Returns 0 / -1. */
static int parse_serverhello_extensions(
    const uint8_t *ext, int ext_len, uint8_t srv_pub[32])
{
    int o = 0;
    int seen = 0;
    while (o + 4 <= ext_len) {
        int et = rd16(ext, o);
        int el = rd16(ext, o + 2);
        o += 4;
        if (o + el > ext_len) return -1;
        if (et == 0x0033 && el >= 4 + 32) {
            /* server key_share is a single entry: group + key */
            int grp  = rd16(ext, o);
            int klen = rd16(ext, o + 2);
            if (grp == 0x001D && klen == 32) {
                for (int i = 0; i < 32; i++) srv_pub[i] = ext[o + 4 + i];
                seen = 1;
            }
        }
        o += el;
    }
    return seen ? 0 : -2;
}

int tls_client_handshake_cert(struct tls_conn *c, int fd) {
    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    c->fd = fd;
    c->is_server = 0;
    sha256_init(&c->transcript);

    /* === 1. Send ClientHello === */
    rand_bytes(c->client_random, 32);
    rand_bytes(c->our_priv, 32);
    x25519(c->client_pub, c->our_priv, x25519_basepoint);

    uint8_t ch_body[256];
    int ch_body_len = build_clienthello_body(c, ch_body);

    uint8_t ch_msg[4 + 256];
    int ch_msg_len = hs_pack(ch_msg, TLS_HS_CLIENT_HELLO, ch_body, ch_body_len);
    if (send_record(fd, TLS_REC_HANDSHAKE, ch_msg, ch_msg_len) < 0) return -1;
    sha256_update(&c->transcript, ch_msg, ch_msg_len);

    /* === 2. Send ChangeCipherSpec (compatibility) === */
    uint8_t ccs = 0x01;
    if (send_record(fd, TLS_REC_CHANGE_CIPHER, &ccs, 1) < 0) return -2;

    /* === 3. Receive ServerHello === */
    uint8_t recbuf[TLS_MAX_FRAGMENT + 16];
    int rlen;
    int rt = recv_record(c, 0, recbuf, &rlen);
    if (rt != TLS_REC_HANDSHAKE) return -3;
    if (rlen < 4 || recbuf[0] != TLS_HS_SERVER_HELLO) return -4;

    int body_len = ((int)recbuf[1] << 16) | ((int)recbuf[2] << 8) | recbuf[3];
    if (body_len + 4 > rlen) return -5;

    int o = 4 + 2;     /* skip msg_type/len + legacy_version */
    if (o + 32 > rlen) return -6;
    for (int i = 0; i < 32; i++) c->server_random[i] = recbuf[o++];
    int sid_len = recbuf[o++];
    if (sid_len > 32 || o + sid_len > rlen) return -7;
    o += sid_len;
    /* cipher_suite (2B) + compression (1B) */
    if (o + 3 > rlen) return -8;
    o += 3;
    /* extensions: 2B len + bytes */
    if (o + 2 > rlen) return -9;
    int ext_len = rd16(recbuf, o); o += 2;
    if (o + ext_len > rlen) return -10;
    if (parse_serverhello_extensions(recbuf + o, ext_len, c->server_pub) < 0) return -11;

    sha256_update(&c->transcript, recbuf, body_len + 4);

    /* === 4. Compute handshake keys === */
    x25519(c->ecdhe_shared, c->our_priv, c->server_pub);
    compute_handshake_keys_cert(c);

    /* === 5. Receive encrypted server flight (EE || Cert || CV || Finished).
     * One record may carry all four messages, or they may come in
     * separate records (TLS allows handshake fragmentation across
     * records). Either way, we keep reading until we see Finished
     * and have processed each prior message into the transcript. */
    int seen_finished = 0;
    uint8_t srv_finished_verify[32];
    int     srv_finished_verify_len = 0;
    /* Snapshot transcript-without-server-Finished for later verify. */
    struct sha256 transcript_at_cf_compute_point;

    /* Buffer for handshake message reassembly across records (rare
     * but allowed). For our minimal server flight everything fits
     * in one record though. */
    uint8_t hs_buf[TLS_MAX_FRAGMENT + 16];
    int     hs_off = 0;

    while (!seen_finished) {
        uint8_t recbuf2[TLS_MAX_FRAGMENT + 16];
        int     rlen2;
        int it = recv_record(c, &c->s_hs_keys, recbuf2, &rlen2);
        if (it != TLS_REC_HANDSHAKE) return -12;
        if (hs_off + rlen2 > (int)sizeof(hs_buf)) return -13;
        for (int i = 0; i < rlen2; i++) hs_buf[hs_off + i] = recbuf2[i];
        hs_off += rlen2;

        /* Walk handshake messages in hs_buf[0..hs_off). */
        int p = 0;
        while (p + 4 <= hs_off) {
            uint8_t mt   = hs_buf[p];
            int     mlen = ((int)hs_buf[p+1] << 16) | ((int)hs_buf[p+2] << 8) | hs_buf[p+3];
            if (p + 4 + mlen > hs_off) break;     /* incomplete — wait for more */

            if (mt == TLS_HS_FINISHED) {
                /* Snapshot the transcript BEFORE adding Finished — that's
                 * what server's verify_data was computed over. */
                transcript_at_cf_compute_point = c->transcript;
                if (mlen > (int)sizeof(srv_finished_verify)) return -14;
                for (int i = 0; i < mlen; i++) srv_finished_verify[i] = hs_buf[p + 4 + i];
                srv_finished_verify_len = mlen;
                seen_finished = 1;
            }

            sha256_update(&c->transcript, hs_buf + p, 4 + mlen);
            p += 4 + mlen;
        }
        /* Move any leftover bytes (incomplete message) to front. */
        if (p < hs_off) {
            for (int i = 0; i < hs_off - p; i++) hs_buf[i] = hs_buf[p + i];
            hs_off -= p;
        } else {
            hs_off = 0;
        }
    }

    /* === 6. Verify server Finished. */
    {
        uint8_t expect[32];
        compute_finished(c->s_hs_traffic_secret,
                         &transcript_at_cf_compute_point, expect);
        if (srv_finished_verify_len != 32) return -15;
        int ok = 1;
        for (int i = 0; i < 32; i++)
            if (srv_finished_verify[i] != expect[i]) ok = 0;
        if (!ok) return -16;
    }

    /* === 7. Pre-derive application keys (transcript = CH..ServerFinished). */
    compute_application_keys(c);

    /* === 8. Send our Finished (encrypted) === */
    {
        uint8_t verify[32];
        compute_finished(c->c_hs_traffic_secret, &c->transcript, verify);
        uint8_t fin_msg[4 + 32];
        int fin_len = hs_pack(fin_msg, TLS_HS_FINISHED, verify, 32);
        if (send_encrypted(c, &c->c_hs_keys, TLS_REC_HANDSHAKE,
                           fin_msg, fin_len) < 0) return -17;
        sha256_update(&c->transcript, fin_msg, fin_len);
    }

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
    /* Loop in case we get post-handshake messages (NewSessionTicket,
     * KeyUpdate, etc.) — we just discard them. */
    for (;;) {
        int len;
        int it = recv_record(c, k, buf, &len);
        if (it < 0) return -1;
        if (it == TLS_REC_HANDSHAKE) continue;     /* drop tickets, etc. */
        if (it != TLS_REC_APP_DATA) return -1;
        if (len > max_n) len = max_n;
        for (int i = 0; i < len; i++) ((uint8_t *)out)[i] = buf[i];
        return len;
    }
}
