/*
 * sshd — RFC 4253 SSH-2 server for AdventOS.
 *
 * Wire-level compatible with OpenSSH clients. The protocol stack:
 *
 *   transport (RFC 4253)
 *     V_S = "SSH-2.0-AdventOS_1.0\r\n"
 *     KEXINIT exchange — we advertise exactly one algorithm per slot
 *     curve25519-sha256 ECDH key exchange (RFC 8731)
 *     ssh-ed25519 host-key signature (RFC 8709)
 *     aes128-gcm@openssh.com AEAD record layer (RFC 5647 framing)
 *
 *   userauth (RFC 4252)
 *     "none" → USERAUTH_FAILURE with "password" in the methods list
 *     "password" → check against /etc/passwd (salt + SHA-256)
 *
 *   connection (RFC 4254)
 *     CHANNEL_OPEN "session"  → CHANNEL_OPEN_CONFIRMATION
 *     CHANNEL_REQUEST "pty-req" → CHANNEL_FAILURE (no pty pairs yet)
 *     CHANNEL_REQUEST "env"     → CHANNEL_SUCCESS (ignored)
 *     CHANNEL_REQUEST "exec"    → fork+exec sh.elf -c <cmd>, pipe back
 *     CHANNEL_REQUEST "shell"   → CHANNEL_SUCCESS, but only sends a
 *                                 hint and closes — see deep dive
 *
 * Each accepted connection forks; the child does the whole handshake +
 * exec, then exits. Host keypair is deterministic (seeded from a fixed
 * 32-byte constant) so the public-key fingerprint is stable across
 * reboots — handy for OpenSSH's known_hosts.
 *
 * Test from the host (one command):
 *
 *   sshpass -p guest \
 *     ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
 *         -o KexAlgorithms=curve25519-sha256 \
 *         -o HostKeyAlgorithms=ssh-ed25519 \
 *         -o Ciphers=aes128-gcm@openssh.com \
 *         -p 2222 guest@127.0.0.1 id
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/ssh.h"

#define SSH_PORT     2222
#define USERS_MAX    16

/* ---- host keypair (loaded from / written to /etc/ssh_host_key) ----
 *
 * Real Unix sshd writes its host private key to
 * /etc/ssh/ssh_host_ed25519_key on first boot and reads it back
 * thereafter, so the fingerprint a client sees in `known_hosts`
 * stays stable across reboots — but is unique per install. We do
 * the same, only with a flat 32-byte seed file (the ed25519 seed)
 * because our libcrypto's keypair_from_seed re-derives the
 * private key bytes deterministically.
 *
 * File: /etc/ssh_host_key, mode 0600, owner root.
 *   exists  →  read 32-byte seed, derive keypair
 *   missing →  rand_bytes(32), write file, derive keypair
 *
 * The previous session 51 design used a baked-in HOSTKEY_SEED
 * constant — stable but with the same fingerprint on every install
 * of AdventOS, which is what real sshd's first-boot generation
 * exists to avoid. */
#define HOSTKEY_PATH "/etc/ssh_host_key"

static uint8_t g_host_pk[32];
static uint8_t g_host_sk[64];

static void hex_byte(uint8_t b, char out[2]) {
    static const char *d = "0123456789abcdef";
    out[0] = d[b >> 4];
    out[1] = d[b & 0xF];
}

/* Print SHA-256(public-key) as 64 lowercase-hex chars — the OpenSSH
 * "SHA256:<base64>" fingerprint without the base64 (libuser doesn't
 * have a base64 encoder and a hex print is just as good for the
 * across-reboots-stability sanity check this is for). */
static void print_host_fingerprint(void) {
    uint8_t digest[32];
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, g_host_pk, 32);
    sha256_final(&s, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) hex_byte(digest[i], hex + i*2);
    hex[64] = 0;
    printf("sshd: host key sha256: %s\n", hex);
}

/* Either load the existing seed from /etc/ssh_host_key or generate
 * a fresh one and persist it. Either way, fills g_host_pk + g_host_sk.
 *
 * Returns 0 on success. If both read AND write fail (degenerate
 * disk-full case), we still get a working (in-memory) keypair from
 * rand_bytes — the warning to the operator says the fingerprint
 * will reset on next boot. */
static int load_or_generate_host_key(void) {
    uint8_t seed[32];
    int fd = sys_open(HOSTKEY_PATH);
    if (fd >= 0) {
        int n = sys_read(fd, seed, 32);
        sys_close(fd);
        if (n == 32) {
            ed25519_keypair_from_seed(g_host_pk, g_host_sk, seed);
            printf("sshd: loaded host key from %s\n", HOSTKEY_PATH);
            return 0;
        }
        printf("sshd: %s present but %d bytes (need 32); regenerating\n",
               HOSTKEY_PATH, n);
    }

    /* No usable file — generate a fresh seed and try to persist. */
    rand_bytes(seed, 32);
    ed25519_keypair_from_seed(g_host_pk, g_host_sk, seed);

    if (sys_fs_write(HOSTKEY_PATH, seed, 32) < 0) {
        puts("sshd: WARNING: could not write host key file; "
             "fingerprint will reset on next boot\n");
        return 0;
    }
    /* Tighten perms — only root may read the private seed. We're
     * still uid 0 at this point (setuid happens per-connection,
     * after this), and we're the owner since we just wrote it. */
    if (sys_chmod(HOSTKEY_PATH, 0600) < 0) {
        puts("sshd: warning: chmod 0600 failed on host key file\n");
    }
    /* Force the bcache out to disk NOW. Without this, a non-clean
     * shutdown (qemu killed before the syncer's periodic flush)
     * would lose the file, defeating the whole "stable across
     * reboots" point. Writeback alone doesn't survive `kill -9`. */
    int synced = (int)sys_bcache_sync();
    printf("sshd: generated fresh host key (saved to %s, mode 0600, "
           "%d block(s) synced)\n", HOSTKEY_PATH, synced);
    return 0;
}

/* ---- pubkey-auth (session 53, RFC 4252 §7) ----------------------- */

/* Demo client keypair — seeded so the same pubkey lands in the
 * authorized list every boot. ssh.elf derives the matching private
 * key from the same constant via `@key` mode, so the in-OS loopback
 * selftest can verify the auth flow end-to-end without filesystem
 * setup. Real OpenSSH clients use their own keys — those go in
 * /etc/ssh_keys (parsed below). */
static const uint8_t DEMO_USER_SEED[32] = {
    0x42, 0x18, 0xE4, 0x77, 0xB5, 0x29, 0xAA, 0x06,
    0x3C, 0x1F, 0x55, 0x91, 0x2E, 0x4D, 0xC3, 0x88,
    0x6B, 0xAA, 0x07, 0x14, 0xE5, 0xD9, 0x60, 0x21,
    0xCC, 0x8E, 0xFB, 0x32, 0x70, 0xA5, 0x16, 0x4F,
};

#define AUTH_KEYS_MAX 16
struct auth_key {
    char    user[32];
    uint8_t pubkey[32];
};
static struct auth_key g_auth_keys[AUTH_KEYS_MAX];
static int             g_n_auth_keys;

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void add_auth_key(const char *user, const uint8_t pk[32]) {
    if (g_n_auth_keys >= AUTH_KEYS_MAX) return;
    int i = 0;
    while (user[i] && i < 31) { g_auth_keys[g_n_auth_keys].user[i] = user[i]; i++; }
    g_auth_keys[g_n_auth_keys].user[i] = 0;
    for (int j = 0; j < 32; j++) g_auth_keys[g_n_auth_keys].pubkey[j] = pk[j];
    g_n_auth_keys++;
}

static int pubkey_authorized(const char *user, const uint8_t pk[32]) {
    int ulen = my_strlen(user);
    for (int i = 0; i < g_n_auth_keys; i++) {
        if (my_strlen(g_auth_keys[i].user) != ulen) continue;
        int match = 1;
        for (int j = 0; j < ulen; j++) {
            if (g_auth_keys[i].user[j] != user[j]) { match = 0; break; }
        }
        if (!match) continue;
        int kmatch = 1;
        for (int j = 0; j < 32; j++) {
            if (g_auth_keys[i].pubkey[j] != pk[j]) { kmatch = 0; break; }
        }
        if (kmatch) return 1;
    }
    return 0;
}

/* Standard base64 alphabet. Decode `in[0..len-1]`, skipping whitespace
 * and `=` padding, into `out`. Returns bytes written, or -1 on a
 * non-base64 char. */
static int b64decode(const char *in, int len, uint8_t *out) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0, bits = 0, val = 0;
    for (int i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        int v = -1;
        for (int j = 0; j < 64; j++) if (alpha[j] == c) { v = j; break; }
        if (v < 0) return -1;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((val >> bits) & 0xff);
        }
    }
    return o;
}

/* Parse one /etc/ssh_keys line:
 *
 *   <user> ssh-ed25519 <base64-blob> [comment]
 *
 * `blob` is the SSH-format public key blob:
 *   string "ssh-ed25519" || string pubkey(32)
 * which base64-decodes to 51 bytes total. */
static void parse_auth_line(const char *line, int len) {
    int p = 0;
    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p == len || line[p] == '#') return;

    /* Field 1: user */
    int us = p;
    while (p < len && line[p] != ' ' && line[p] != '\t') p++;
    int ulen = p - us;
    if (ulen <= 0 || ulen > 31) return;
    char user[32];
    for (int i = 0; i < ulen; i++) user[i] = line[us + i];
    user[ulen] = 0;

    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;

    /* Field 2: algo — only "ssh-ed25519" supported. */
    int as = p;
    while (p < len && line[p] != ' ' && line[p] != '\t') p++;
    int alen = p - as;
    if (alen != 11) return;
    for (int i = 0; i < 11; i++) if (line[as + i] != "ssh-ed25519"[i]) return;

    while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;

    /* Field 3: base64 blob (up to next whitespace). */
    int bs = p;
    while (p < len && line[p] != ' ' && line[p] != '\t') p++;
    int blen = p - bs;
    if (blen <= 0) return;

    uint8_t decoded[128];
    int dlen = b64decode(line + bs, blen, decoded);
    if (dlen < 51) return;
    /* Decoded layout: u32(11) || "ssh-ed25519" || u32(32) || pubkey */
    if (!(decoded[0] == 0 && decoded[1] == 0 && decoded[2] == 0 && decoded[3] == 11)) return;
    for (int i = 0; i < 11; i++) if (decoded[4 + i] != "ssh-ed25519"[i]) return;
    if (!(decoded[15] == 0 && decoded[16] == 0 && decoded[17] == 0 && decoded[18] == 32)) return;
    add_auth_key(user, decoded + 19);
}

static int load_auth_keys_file(const char *path) {
    int fd = sys_open(path);
    if (fd < 0) return -1;
    static char buf[4096];
    int n = sys_read(fd, buf, sizeof(buf));
    sys_close(fd);
    if (n <= 0) return -1;
    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            if (i > line_start) parse_auth_line(buf + line_start, i - line_start);
            line_start = i + 1;
        }
    }
    return 0;
}

/* ---- /etc/passwd (same parser as login.c / session-47 sshd) ------- */

struct user_entry {
    char name[32];
    char salt[16];
    char hash[68];
    int  uid;
    int  gid;
    char home[32];
    char shell[32];
};

static struct user_entry g_users[USERS_MAX];
static int               g_n_users;

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int parse_int(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void hex_lower(const uint8_t *in, int n, char *out) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2*i  ] = d[in[i] >> 4];
        out[2*i+1] = d[in[i] & 0xF];
    }
    out[2*n] = 0;
}

static int parse_passwd_line(const char *line, int len) {
    if (g_n_users >= USERS_MAX) return -1;
    struct user_entry *u = &g_users[g_n_users];
    int starts[6] = {0}, ends[6] = {0};
    int field = 0;
    int s = 0;
    for (int i = 0; i <= len && field < 6; i++) {
        if (i == len || line[i] == ':') {
            starts[field] = s;
            ends[field]   = i;
            field++;
            s = i + 1;
        }
    }
    if (field < 6) return -1;
    int nl = ends[0] - starts[0];
    if (nl <= 0 || nl >= (int)sizeof(u->name)) return -1;
    for (int i = 0; i < nl; i++) u->name[i] = line[starts[0] + i];
    u->name[nl] = 0;
    int dollar = -1;
    for (int i = starts[1]; i < ends[1]; i++) {
        if (line[i] == '$') { dollar = i; break; }
    }
    if (dollar < 0) return -1;
    int sl = dollar - starts[1];
    int hl = ends[1] - dollar - 1;
    if (sl <= 0 || sl >= (int)sizeof(u->salt) || hl != 64) return -1;
    for (int i = 0; i < sl; i++) u->salt[i] = line[starts[1] + i];
    u->salt[sl] = 0;
    for (int i = 0; i < hl; i++) u->hash[i] = line[dollar + 1 + i];
    u->hash[hl] = 0;
    u->uid = parse_int(line + starts[2], ends[2] - starts[2]);
    u->gid = parse_int(line + starts[3], ends[3] - starts[3]);
    int shl = ends[5] - starts[5];
    if (shl >= (int)sizeof(u->shell)) shl = (int)sizeof(u->shell) - 1;
    for (int i = 0; i < shl; i++) u->shell[i] = line[starts[5] + i];
    u->shell[shl] = 0;
    g_n_users++;
    return 0;
}

static int load_passwd(void) {
    int fd = sys_open("/etc/passwd");
    if (fd < 0) return -1;
    static char buf[2048];
    int n = sys_read(fd, buf, (int)sizeof(buf));
    sys_close(fd);
    if (n <= 0) return -1;
    int line_start = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n' || i == n - 1) {
            int line_end = (buf[i] == '\n') ? i : i + 1;
            if (line_end > line_start) {
                parse_passwd_line(buf + line_start, line_end - line_start);
            }
            line_start = i + 1;
        }
    }
    return 0;
}

static struct user_entry *find_user(const char *name) {
    for (int i = 0; i < g_n_users; i++) {
        if (my_strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return 0;
}

static int verify_password(struct user_entry *u, const char *password) {
    struct sha256 s;
    int sl = 0; while (u->salt[sl])  sl++;
    int pl = 0; while (password[pl]) pl++;
    sha256_init(&s);
    sha256_update(&s, u->salt, sl);
    sha256_update(&s, password, pl);
    uint8_t digest[32];
    sha256_final(&s, digest);
    char got[68];
    hex_lower(digest, 32, got);
    return my_strcmp(got, u->hash) == 0;
}

/* ---- TCP I/O helpers --------------------------------------------- */

static int read_exact(int fd, void *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = sys_read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, int n) {
    int put = 0;
    while (put < n) {
        int w = sys_write(fd, (const char *)buf + put, n - put);
        if (w <= 0) return -1;
        put += w;
    }
    return 0;
}

/* ---- SSH connection state ---------------------------------------- */

struct ssh_conn {
    int     fd;

    /* Banner strings (without trailing \r\n). */
    char    v_c[256]; int v_c_len;
    char    v_s[64];  int v_s_len;

    /* KEXINIT payloads (starting from the SSH_MSG_KEXINIT byte). */
    uint8_t i_c[4096]; int i_c_len;
    uint8_t i_s[1024]; int i_s_len;

    /* KEX state. */
    uint8_t q_c[32];           /* client ephemeral pub */
    uint8_t q_s[32];           /* server ephemeral pub */
    uint8_t d_s[32];           /* server ephemeral priv */
    uint8_t k_mpint[40];       /* shared secret encoded as mpint */
    int     k_mpint_len;
    uint8_t h[32];             /* exchange hash */
    uint8_t session_id[32];    /* = h from first handshake */

    /* AEAD keys + IVs after NEWKEYS. */
    int     enc_in, enc_out;
    uint8_t key_c2s[16];
    uint8_t iv_c2s [12];
    uint8_t key_s2c[16];
    uint8_t iv_s2c [12];
};

/* ---- packet layer ------------------------------------------------- */

/* Pre-NEWKEYS framing (no MAC, padding to 8-byte boundary).
 *
 * RFC 4253 §6: the TOTAL on-wire bytes (4-byte length field + pad_len
 * byte + payload + padding) must be a multiple of max(cipher_block, 8).
 * For the "none" cipher block_size is 8, so we pad (4+1+N+P) to a
 * multiple of 8, with at least 4 bytes of padding. */
static int send_packet_clear(struct ssh_conn *c,
                             const uint8_t *payload, int payload_len) {
    int min_pad = 4, block = 8;
    int pad_n = block - ((4 + 1 + payload_len) % block);
    if (pad_n < min_pad) pad_n += block;
    int enc_n = 1 + payload_len + pad_n;
    int total = 4 + enc_n;
    static uint8_t out[8192];
    if (total > (int)sizeof(out)) return -1;
    out[0] = (uint8_t)(enc_n >> 24);
    out[1] = (uint8_t)(enc_n >> 16);
    out[2] = (uint8_t)(enc_n >>  8);
    out[3] = (uint8_t)(enc_n);
    out[4] = (uint8_t)pad_n;
    for (int i = 0; i < payload_len; i++) out[5 + i] = payload[i];
    rand_bytes(out + 5 + payload_len, pad_n);
    return write_all(c->fd, out, total);
}

static int recv_packet_clear(struct ssh_conn *c,
                             uint8_t *payload, int max_payload) {
    uint8_t hdr[5];
    if (read_exact(c->fd, hdr, 5) < 0) return -1;
    uint32_t enc_n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] <<  8) | (uint32_t)hdr[3];
    int pad_n = hdr[4];
    if (enc_n < 8 || enc_n > 8000) return -1;
    int body = (int)enc_n - 1;       /* already consumed pad_len byte */
    static uint8_t buf[8192];
    if (read_exact(c->fd, buf, body) < 0) return -1;
    int payload_len = body - pad_n;
    if (payload_len < 0 || payload_len > max_payload) return -1;
    for (int i = 0; i < payload_len; i++) payload[i] = buf[i];
    return payload_len;
}

/* AEAD framing post-NEWKEYS. */
static int send_packet_aead(struct ssh_conn *c,
                            const uint8_t *payload, int payload_len) {
    static uint8_t out[SSH_MAX_PACKET];
    size_t out_len;
    if (ssh_packet_seal(c->key_s2c, c->iv_s2c,
                        payload, payload_len, out, &out_len) < 0) return -1;
    return write_all(c->fd, out, (int)out_len);
}

static int recv_packet_aead(struct ssh_conn *c,
                            uint8_t *payload, int max_payload) {
    uint8_t hdr[4];
    if (read_exact(c->fd, hdr, 4) < 0) return -1;
    uint32_t enc_n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] <<  8) | (uint32_t)hdr[3];
    if (enc_n < 16 || enc_n > SSH_MAX_PACKET - 4 - 16) return -1;
    static uint8_t pkt[SSH_MAX_PACKET];
    for (int i = 0; i < 4; i++) pkt[i] = hdr[i];
    if (read_exact(c->fd, pkt + 4, (int)enc_n + 16) < 0) return -1;
    size_t pl;
    if (ssh_packet_open(c->key_c2s, c->iv_c2s, pkt, 4 + enc_n + 16,
                        payload, (size_t)max_payload, &pl) < 0) return -1;
    return (int)pl;
}

/* Dispatch to the right framing. Auto-skips IGNORE/DEBUG messages so
 * callers can just expect the next semantic packet. */
static int send_packet(struct ssh_conn *c, const uint8_t *p, int n) {
    return c->enc_out ? send_packet_aead(c, p, n) : send_packet_clear(c, p, n);
}

static int recv_packet(struct ssh_conn *c, uint8_t *p, int max) {
    for (;;) {
        int n = c->enc_in ? recv_packet_aead(c, p, max)
                          : recv_packet_clear(c, p, max);
        if (n < 1) return n;
        if (p[0] == SSH_MSG_IGNORE || p[0] == SSH_MSG_DEBUG) continue;
        return n;
    }
}

/* ---- banner exchange (RFC 4253 §4.2) ----------------------------- */

static int do_banner(struct ssh_conn *c) {
    static const char ours[] = "SSH-2.0-AdventOS_1.0";
    int i = 0;
    while (ours[i]) { c->v_s[i] = ours[i]; i++; }
    c->v_s_len = i;
    char buf[64];
    for (int j = 0; j < c->v_s_len; j++) buf[j] = c->v_s[j];
    buf[c->v_s_len] = '\r';
    buf[c->v_s_len + 1] = '\n';
    if (write_all(c->fd, buf, c->v_s_len + 2) < 0) return -1;

    /* Read up to 255 bytes ending in \n. Per RFC, lines before the
     * SSH-x.y line are "for human consumption" and can be ignored. We
     * keep only the SSH- line. */
    int len = 0;
    int saw_ssh = 0;
    while (len < 255) {
        char ch;
        if (read_exact(c->fd, &ch, 1) < 0) return -1;
        if (ch == '\r') continue;
        if (ch == '\n') {
            c->v_c[len] = 0;
            if (len >= 4 && c->v_c[0] == 'S' && c->v_c[1] == 'S' &&
                c->v_c[2] == 'H' && c->v_c[3] == '-') {
                saw_ssh = 1;
                break;
            }
            len = 0;
            continue;
        }
        c->v_c[len++] = ch;
    }
    if (!saw_ssh) return -1;
    c->v_c_len = len;
    return 0;
}

/* ---- KEXINIT ----------------------------------------------------- */

/* Build and send our KEXINIT. Returns 0 on success. */
static int send_kexinit(struct ssh_conn *c) {
    uint8_t *p = c->i_s;
    ssh_put_u8(&p, SSH_MSG_KEXINIT);
    uint8_t cookie[16];
    rand_bytes(cookie, 16);
    ssh_put_bytes(&p, cookie, 16);
    /* 10 name-lists — we offer exactly one algo per slot. */
    ssh_put_cstring(&p, "curve25519-sha256");           /* kex */
    ssh_put_cstring(&p, "ssh-ed25519");                  /* host key */
    ssh_put_cstring(&p, "aes128-gcm@openssh.com");       /* enc c->s */
    ssh_put_cstring(&p, "aes128-gcm@openssh.com");       /* enc s->c */
    ssh_put_cstring(&p, "");                              /* mac c->s (AEAD) */
    ssh_put_cstring(&p, "");                              /* mac s->c (AEAD) */
    ssh_put_cstring(&p, "none");                          /* comp c->s */
    ssh_put_cstring(&p, "none");                          /* comp s->c */
    ssh_put_cstring(&p, "");                              /* lang c->s */
    ssh_put_cstring(&p, "");                              /* lang s->c */
    ssh_put_bool(&p, 0);                                  /* first_kex_packet_follows */
    ssh_put_u32(&p, 0);                                   /* reserved */
    c->i_s_len = (int)(p - c->i_s);
    return send_packet(c, c->i_s, c->i_s_len);
}

static int recv_kexinit(struct ssh_conn *c) {
    int n = recv_packet(c, c->i_c, sizeof(c->i_c));
    if (n < 1 || c->i_c[0] != SSH_MSG_KEXINIT) return -1;
    c->i_c_len = n;
    /* We don't verify intersection — if the client picks an algo we
     * can't do, KEX will fail downstream and we'll send DISCONNECT. */
    return 0;
}

/* ---- KEX_ECDH ---------------------------------------------------- */

static void hash_string(struct sha256 *s, const void *data, int n) {
    uint8_t lb[4];
    lb[0] = (uint8_t)(n >> 24);
    lb[1] = (uint8_t)(n >> 16);
    lb[2] = (uint8_t)(n >>  8);
    lb[3] = (uint8_t)n;
    sha256_update(s, lb, 4);
    sha256_update(s, data, n);
}

/* Receive SSH_MSG_KEX_ECDH_INIT (Q_C), do the ECDH, build and send
 * SSH_MSG_KEX_ECDH_REPLY with K_S + Q_S + signature(H). */
static int do_kex_ecdh(struct ssh_conn *c) {
    uint8_t buf[256];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_KEX_ECDH_INIT) return -1;
    const uint8_t *p = buf + 1, *end = buf + n;
    uint32_t q_len;
    const uint8_t *q = ssh_get_string(&p, end, &q_len);
    if (!q || q_len != 32) return -1;
    for (int i = 0; i < 32; i++) c->q_c[i] = q[i];

    /* Ephemeral keypair + shared secret. */
    rand_bytes(c->d_s, 32);
    x25519(c->q_s, c->d_s, x25519_basepoint);
    uint8_t k_raw[32];
    x25519(k_raw, c->d_s, c->q_c);

    /* mpint-encode the shared secret. */
    uint8_t *kp = c->k_mpint;
    ssh_put_mpint(&kp, k_raw, 32);
    c->k_mpint_len = (int)(kp - c->k_mpint);

    /* Build K_S — the SSH-encoded ssh-ed25519 host key blob:
     *   string "ssh-ed25519" || string pubkey(32). */
    uint8_t k_s[64];
    uint8_t *ksp = k_s;
    ssh_put_cstring(&ksp, "ssh-ed25519");
    ssh_put_string(&ksp, g_host_pk, 32);
    int k_s_len = (int)(ksp - k_s);

    /* H = SHA-256(string(V_C) || string(V_S) ||
     *             string(I_C) || string(I_S) ||
     *             string(K_S) || string(Q_C) || string(Q_S) ||
     *             mpint(K)). */
    struct sha256 sh;
    sha256_init(&sh);
    hash_string(&sh, c->v_c, c->v_c_len);
    hash_string(&sh, c->v_s, c->v_s_len);
    hash_string(&sh, c->i_c, c->i_c_len);
    hash_string(&sh, c->i_s, c->i_s_len);
    hash_string(&sh, k_s,    k_s_len);
    hash_string(&sh, c->q_c, 32);
    hash_string(&sh, c->q_s, 32);
    sha256_update(&sh, c->k_mpint, c->k_mpint_len);
    sha256_final(&sh, c->h);

    /* First handshake — session_id is "frozen" to H. We never rekey,
     * so this is also the final value. */
    for (int i = 0; i < 32; i++) c->session_id[i] = c->h[i];

    /* Sign H with the host key. */
    uint8_t sig_raw[64];
    ed25519_sign(sig_raw, c->h, 32, g_host_sk);

    /* Wrap signature as SSH-format blob: string "ssh-ed25519" || string sig. */
    uint8_t sig_blob[128];
    uint8_t *sbp = sig_blob;
    ssh_put_cstring(&sbp, "ssh-ed25519");
    ssh_put_string(&sbp, sig_raw, 64);
    int sig_blob_len = (int)(sbp - sig_blob);

    /* Send SSH_MSG_KEX_ECDH_REPLY:
     *   byte 31 || string K_S || string Q_S || string sig_blob. */
    uint8_t reply[256];
    uint8_t *rp = reply;
    ssh_put_u8(&rp, SSH_MSG_KEX_ECDH_REPLY);
    ssh_put_string(&rp, k_s, k_s_len);
    ssh_put_string(&rp, c->q_s, 32);
    ssh_put_string(&rp, sig_blob, sig_blob_len);
    return send_packet(c, reply, (int)(rp - reply));
}

/* ---- NEWKEYS ----------------------------------------------------- */

static int do_newkeys(struct ssh_conn *c) {
    uint8_t nk = SSH_MSG_NEWKEYS;
    if (send_packet(c, &nk, 1) < 0) return -1;

    /* Outbound (server → client) keys go live now. */
    uint8_t tmp[32];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'B', c->session_id);
    for (int i = 0; i < 12; i++) c->iv_s2c[i] = tmp[i];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'D', c->session_id);
    for (int i = 0; i < 16; i++) c->key_s2c[i] = tmp[i];
    c->enc_out = 1;

    /* Wait for client NEWKEYS (still in cleartext mode at this point). */
    uint8_t buf[16];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_NEWKEYS) return -1;

    /* Inbound (client → server) keys go live next. */
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'A', c->session_id);
    for (int i = 0; i < 12; i++) c->iv_c2s[i] = tmp[i];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'C', c->session_id);
    for (int i = 0; i < 16; i++) c->key_c2s[i] = tmp[i];
    c->enc_in = 1;
    return 0;
}

/* ---- service request + userauth ---------------------------------- */

static int do_service_request(struct ssh_conn *c) {
    uint8_t buf[256];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_SERVICE_REQUEST) return -1;
    const uint8_t *p = buf + 1, *end = buf + n;
    uint32_t svc_len;
    const uint8_t *svc = ssh_get_string(&p, end, &svc_len);
    if (!svc || svc_len != 12) return -1;
    for (int i = 0; i < 12; i++) {
        if (svc[i] != "ssh-userauth"[i]) return -1;
    }
    uint8_t reply[32];
    uint8_t *rp = reply;
    ssh_put_u8(&rp, SSH_MSG_SERVICE_ACCEPT);
    ssh_put_cstring(&rp, "ssh-userauth");
    return send_packet(c, reply, (int)(rp - reply));
}

/* Process userauth requests until one succeeds (or we give up).
 * On success, returns 0 and fills *out_user. */
static int do_userauth(struct ssh_conn *c, struct user_entry **out_user) {
    static uint8_t buf[1024];
    *out_user = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        int n = recv_packet(c, buf, sizeof(buf));
        if (n < 1 || buf[0] != SSH_MSG_USERAUTH_REQUEST) return -1;
        const uint8_t *p = buf + 1, *end = buf + n;
        uint32_t ul, sl, ml;
        const uint8_t *us = ssh_get_string(&p, end, &ul);
        const uint8_t *ss = ssh_get_string(&p, end, &sl);
        const uint8_t *ms = ssh_get_string(&p, end, &ml);
        (void)ss;
        if (!us || !ss || !ms) return -1;

        char username[64];
        if (ul >= sizeof(username)) ul = sizeof(username) - 1;
        for (uint32_t i = 0; i < ul; i++) username[i] = (char)us[i];
        username[ul] = 0;

        if (ml == 8 && ms[0] == 'p' && ms[1] == 'a' && ms[2] == 's' &&
            ms[3] == 's' && ms[4] == 'w' && ms[5] == 'o' &&
            ms[6] == 'r' && ms[7] == 'd') {
            ssh_get_u8(&p);   /* FALSE — change-password flag */
            uint32_t pl;
            const uint8_t *ps = ssh_get_string(&p, end, &pl);
            if (!ps) return -1;
            char pass[64];
            if (pl >= sizeof(pass)) pl = sizeof(pass) - 1;
            for (uint32_t i = 0; i < pl; i++) pass[i] = (char)ps[i];
            pass[pl] = 0;

            struct user_entry *u = find_user(username);
            if (u && verify_password(u, pass)) {
                uint8_t s = SSH_MSG_USERAUTH_SUCCESS;
                if (send_packet(c, &s, 1) < 0) return -1;
                *out_user = u;
                return 0;
            }
            printf("sshd: password auth failed user='%s'\n", username);
            sys_sleep_ms(200);
        }
        else if (ml == 9 && ms[0]=='p'&&ms[1]=='u'&&ms[2]=='b'&&
                            ms[3]=='l'&&ms[4]=='i'&&ms[5]=='c'&&
                            ms[6]=='k'&&ms[7]=='e'&&ms[8]=='y') {
            /* publickey method (RFC 4252 §7). Two-step:
             *   has_sig=FALSE → probe; reply PK_OK if pubkey is in the
             *                  user's authorized list, else FAILURE
             *   has_sig=TRUE  → verify signature over the canonical
             *                  auth-blob, reply SUCCESS or FAILURE */
            int has_sig = ssh_get_u8(&p);
            uint32_t algo_len, pk_blob_len;
            const uint8_t *algo    = ssh_get_string(&p, end, &algo_len);
            const uint8_t *pk_blob = ssh_get_string(&p, end, &pk_blob_len);
            if (!algo || !pk_blob) return -1;

            /* Only ssh-ed25519 supported. */
            int is_ed = (algo_len == 11);
            for (uint32_t i = 0; i < 11 && is_ed; i++)
                if (algo[i] != "ssh-ed25519"[i]) is_ed = 0;
            if (!is_ed) goto pk_fail;

            /* Parse blob inner = string("ssh-ed25519") || string(pubkey). */
            const uint8_t *bp = pk_blob, *bend = pk_blob + pk_blob_len;
            uint32_t tl, pkl;
            const uint8_t *tn = ssh_get_string(&bp, bend, &tl);
            if (!tn || tl != 11) goto pk_fail;
            for (uint32_t i = 0; i < 11; i++)
                if (tn[i] != "ssh-ed25519"[i]) goto pk_fail;
            const uint8_t *pk_raw = ssh_get_string(&bp, bend, &pkl);
            if (!pk_raw || pkl != 32) goto pk_fail;

            struct user_entry *u = find_user(username);
            if (!u || !pubkey_authorized(username, pk_raw)) goto pk_fail;

            if (!has_sig) {
                /* Probe: PK_OK echoes the same algo + blob — tells
                 * client "this key is acceptable, send a sig". */
                uint8_t ok[256];
                uint8_t *op = ok;
                ssh_put_u8(&op, SSH_MSG_USERAUTH_PK_OK);
                ssh_put_string(&op, algo, algo_len);
                ssh_put_string(&op, pk_blob, pk_blob_len);
                if (send_packet(c, ok, (int)(op - ok)) < 0) return -1;
                continue;     /* Loop back: expect another USERAUTH_REQUEST */
            }

            /* has_sig=TRUE: parse the signature blob. */
            uint32_t sig_blob_len;
            const uint8_t *sig_blob = ssh_get_string(&p, end, &sig_blob_len);
            if (!sig_blob) goto pk_fail;
            const uint8_t *sbp = sig_blob, *sbend = sig_blob + sig_blob_len;
            const uint8_t *stn = ssh_get_string(&sbp, sbend, &tl);
            if (!stn || tl != 11) goto pk_fail;
            for (uint32_t i = 0; i < 11; i++)
                if (stn[i] != "ssh-ed25519"[i]) goto pk_fail;
            uint32_t slen;
            const uint8_t *sig = ssh_get_string(&sbp, sbend, &slen);
            if (!sig || slen != 64) goto pk_fail;

            /* Build the canonical auth-blob (RFC 4252 §7). The string()
             * wrappers + the byte(50) + booleans are spec-exact. */
            uint8_t auth_blob[1024];
            uint8_t *abp = auth_blob;
            ssh_put_string(&abp, c->session_id, 32);
            ssh_put_u8(&abp, SSH_MSG_USERAUTH_REQUEST);
            ssh_put_string(&abp, username, my_strlen(username));
            ssh_put_cstring(&abp, "ssh-connection");
            ssh_put_cstring(&abp, "publickey");
            ssh_put_bool(&abp, 1);
            ssh_put_string(&abp, algo, algo_len);
            ssh_put_string(&abp, pk_blob, pk_blob_len);
            int auth_blob_len = (int)(abp - auth_blob);

            if (ed25519_verify(sig, auth_blob, auth_blob_len, pk_raw) == 0) {
                uint8_t s = SSH_MSG_USERAUTH_SUCCESS;
                if (send_packet(c, &s, 1) < 0) return -1;
                printf("sshd: pubkey auth ok user='%s'\n", username);
                *out_user = u;
                return 0;
            }
            printf("sshd: pubkey sig verify FAILED user='%s'\n", username);
        }
pk_fail:

        /* Reject: send USERAUTH_FAILURE listing what we DO support. */
        {
            uint8_t fail[64];
            uint8_t *fp = fail;
            ssh_put_u8(&fp, SSH_MSG_USERAUTH_FAILURE);
            ssh_put_cstring(&fp, "publickey,password");
            ssh_put_bool(&fp, 0);
            if (send_packet(c, fail, (int)(fp - fail)) < 0) return -1;
        }
    }
    return -1;
}

/* ---- channels ---------------------------------------------------- */

struct channel {
    uint32_t their_id;
    uint32_t our_id;
    uint32_t their_window;
    uint32_t our_window;
    uint32_t max_packet;
};

static int do_channel_open(struct ssh_conn *c, struct channel *ch) {
    uint8_t buf[256];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_CHANNEL_OPEN) return -1;
    const uint8_t *p = buf + 1, *end = buf + n;
    uint32_t type_len;
    const uint8_t *type = ssh_get_string(&p, end, &type_len);
    uint32_t their_id     = ssh_get_u32(&p);
    uint32_t their_window = ssh_get_u32(&p);
    uint32_t their_max    = ssh_get_u32(&p);
    if (!type || type_len != 7 ||
        type[0]!='s'||type[1]!='e'||type[2]!='s'||type[3]!='s'||
        type[4]!='i'||type[5]!='o'||type[6]!='n') {
        /* Reject anything other than "session". */
        uint8_t r[64];
        uint8_t *rp = r;
        ssh_put_u8(&rp, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&rp, their_id);
        ssh_put_u32(&rp, SSH_OPEN_UNKNOWN_CHANNEL_TYPE);
        ssh_put_cstring(&rp, "only session channels supported");
        ssh_put_cstring(&rp, "");
        send_packet(c, r, (int)(rp - r));
        return -1;
    }
    ch->their_id     = their_id;
    ch->our_id       = 0;
    ch->their_window = their_window;
    ch->our_window   = 0x100000;
    ch->max_packet   = their_max < 16384 ? their_max : 16384;

    uint8_t reply[32];
    uint8_t *rp = reply;
    ssh_put_u8(&rp, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    ssh_put_u32(&rp, their_id);
    ssh_put_u32(&rp, ch->our_id);
    ssh_put_u32(&rp, ch->our_window);
    ssh_put_u32(&rp, ch->max_packet);
    return send_packet(c, reply, (int)(rp - reply));
}

/* Pump channel-request messages until we see "exec" or "shell", then
 * return that command. Returns 1 = exec (cmd filled), 2 = shell, -1 on
 * error. Side-effect: replies to pty-req / env / etc.
 *
 * `buf` is static (not stack) because the user stack is only 16 KiB
 * total and a 4 KiB stack array, combined with the rest of the call
 * chain, was overflowing into the guard page after auth. */
static int do_channel_requests(struct ssh_conn *c, struct channel *ch,
                                char *cmd_out, int cmd_cap) {
    static uint8_t buf[4096];
    for (;;) {
        int n = recv_packet(c, buf, sizeof(buf));
        if (n < 1) return -1;
        uint8_t m = buf[0];

        if (m == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            const uint8_t *p = buf + 1;
            ssh_get_u32(&p);     /* our chan */
            ch->their_window += ssh_get_u32(&p);
            continue;
        }

        if (m != SSH_MSG_CHANNEL_REQUEST) return -1;

        const uint8_t *p = buf + 1, *end = buf + n;
        ssh_get_u32(&p);                  /* our chan id */
        uint32_t tl;
        const uint8_t *t = ssh_get_string(&p, end, &tl);
        int want_reply = ssh_get_u8(&p);

        int handled_ok = 0;
        int is_exec = 0, is_shell = 0;

        if (tl == 7 && t[0]=='p'&&t[1]=='t'&&t[2]=='y'&&
                       t[3]=='-'&&t[4]=='r'&&t[5]=='e'&&t[6]=='q') {
            /* Accept. Terminal modes (echo, line discipline) come in
             * the request payload but we don't apply them — the pty
             * we allocate is always raw passthrough and the userspace
             * shell handles line editing. Good enough for openssh. */
            handled_ok = 1;
        }
        else if (tl == 3 && t[0]=='e'&&t[1]=='n'&&t[2]=='v') {
            handled_ok = 1;
        }
        else if (tl == 4 && t[0]=='e'&&t[1]=='x'&&t[2]=='e'&&t[3]=='c') {
            uint32_t cl;
            const uint8_t *cs = ssh_get_string(&p, end, &cl);
            if (cs) {
                if ((int)cl >= cmd_cap) cl = cmd_cap - 1;
                for (uint32_t i = 0; i < cl; i++) cmd_out[i] = (char)cs[i];
                cmd_out[cl] = 0;
                handled_ok = 1;
                is_exec = 1;
            }
        }
        else if (tl == 5 && t[0]=='s'&&t[1]=='h'&&t[2]=='e'&&t[3]=='l'&&t[4]=='l') {
            cmd_out[0] = 0;
            handled_ok = 1;
            is_shell = 1;
        }

        if (want_reply) {
            uint8_t reply[8];
            uint8_t *rp = reply;
            ssh_put_u8(&rp, handled_ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
            ssh_put_u32(&rp, ch->their_id);
            if (send_packet(c, reply, (int)(rp - reply)) < 0) return -1;
        }

        if (is_exec)  return 1;
        if (is_shell) return 2;
    }
}

/* Send a CHANNEL_DATA packet. */
static int send_channel_data(struct ssh_conn *c, struct channel *ch,
                              const void *data, int n) {
    static uint8_t pkt[SSH_MAX_PACKET];
    uint8_t *p = pkt;
    ssh_put_u8(&p, SSH_MSG_CHANNEL_DATA);
    ssh_put_u32(&p, ch->their_id);
    ssh_put_string(&p, data, n);
    return send_packet(c, pkt, (int)(p - pkt));
}

/* Run sh.elf -c "<cmd>" and stream its stdout/stderr as CHANNEL_DATA
 * frames. exit-status is delivered after EOF. */
static int run_exec(struct ssh_conn *c, struct channel *ch, const char *cmd) {
    int outp[2];
    if (sys_pipe(outp) < 0) return -1;
    int pid = sys_fork();
    if (pid == 0) {
        sys_dup2(outp[1], 1);
        sys_dup2(outp[1], 2);
        sys_close(outp[0]);
        sys_close(outp[1]);
        const char *argv[] = { "sh.elf", "-c", cmd, 0 };
        sys_exec("sh.elf", argv);
        sys_exit(127);
    }
    sys_close(outp[1]);

    static char buf[2048];
    for (;;) {
        int n = sys_read(outp[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (send_channel_data(c, ch, buf, n) < 0) {
            sys_close(outp[0]);
            int code; sys_wait(&code);
            return -1;
        }
    }
    sys_close(outp[0]);
    int code = 0;
    sys_wait(&code);

    /* exit-status (RFC 4254 §6.10). */
    {
        uint8_t p[64];
        uint8_t *q = p;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_REQUEST);
        ssh_put_u32(&q, ch->their_id);
        ssh_put_cstring(&q, "exit-status");
        ssh_put_bool(&q, 0);            /* want_reply = false */
        ssh_put_u32(&q, (uint32_t)(code & 0xff));
        send_packet(c, p, (int)(q - p));
    }

    /* CHANNEL_EOF + CHANNEL_CLOSE. */
    {
        uint8_t p[8]; uint8_t *q = p;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_EOF); ssh_put_u32(&q, ch->their_id);
        send_packet(c, p, (int)(q - p));
    }
    {
        uint8_t p[8]; uint8_t *q = p;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_CLOSE); ssh_put_u32(&q, ch->their_id);
        send_packet(c, p, (int)(q - p));
    }

    /* Drain a few packets so we see the client's CHANNEL_CLOSE / EOF
     * before tearing down the TCP socket — keeps the wire clean. */
    for (int i = 0; i < 8; i++) {
        uint8_t buf[256];
        int n = recv_packet(c, buf, sizeof(buf));
        if (n <= 0) break;
        if (buf[0] == SSH_MSG_CHANNEL_CLOSE) break;
    }
    return 0;
}

/* Persistent shell over a pty (session 52).
 *
 * Architecture is a 3-way split with no shared state past fork():
 *
 *                  ┌──────────────┐
 *   SSH client ──► │  RX helper   │ → pty master write → slave reads → shell stdin
 *                  │  (this fn)   │
 *                  └──────────────┘
 *                  ┌──────────────┐
 *   SSH client ◄── │  TX helper   │ ← pty master read  ← slave writes ← shell stdout
 *                  │  (forked)    │
 *                  └──────────────┘
 *
 * The shell itself is a third fork — gets the pty slave as fd 0/1/2,
 * execs sh.elf with no args (interactive mode, session-49 line editor
 * runs). TX uses `key_s2c`+`iv_s2c`; RX uses `key_c2s`+`iv_c2s`.
 * Because each direction has its OWN AEAD keys and IV counter, the
 * COW-divergence between the two helpers after fork is fine — they
 * each only touch their own state.
 *
 * No concurrent writes on the TCP socket: TX writes ciphertext, RX
 * only reads. No concurrent writes on the pty either: RX writes the
 * master, TX only reads it. Two independent unidirectional shuttles,
 * which is what makes the no-poll/no-select model work cleanly. */
static int run_shell(struct ssh_conn *c, struct channel *ch) {
    int pty[2];
    if (sys_openpty(pty) < 0) {
        const char *m = "sshd: pty table full\r\n";
        int n = 0; while (m[n]) n++;
        send_channel_data(c, ch, m, n);
        return -1;
    }
    int master = pty[0];
    int slave  = pty[1];
    printf("sshd: pty allocated master=%d slave=%d\n", master, slave);

    /* Fork order matters: TX FIRST, then shell.
     *
     * Forking the shell first and then TX hits a reproducible kernel
     * issue where the second fork (TX) appears to stall its child after
     * the first syscall — the task is created but doesn't make progress
     * past one sys_write. Forking TX first avoids whatever the shell's
     * exec/destroy-PD path leaves behind. Filed as a follow-up. */
    int tx_pid = sys_fork();
    if (tx_pid == 0) {
        /* Inherit `c` via COW (eager copy actually) — uses c->key_s2c
         * + c->iv_s2c. */
        char buf[256];
        for (;;) {
            int n = sys_read(master, buf, sizeof(buf));
            if (n <= 0) break;        /* slave closed = shell exited */
            if (send_channel_data(c, ch, buf, n) < 0) break;
        }
        /* Signal channel teardown to the client. */
        uint8_t pkt[8]; uint8_t *q = pkt;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_EOF); ssh_put_u32(&q, ch->their_id);
        send_packet(c, pkt, (int)(q - pkt));
        q = pkt;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_CLOSE); ssh_put_u32(&q, ch->their_id);
        send_packet(c, pkt, (int)(q - pkt));
        sys_exit(0);
    }

    /* Now fork the shell. */
    int shell_pid = sys_fork();
    if (shell_pid == 0) {
        sys_dup2(slave, 0);
        sys_dup2(slave, 1);
        sys_dup2(slave, 2);
        sys_close(master);
        sys_close(slave);
        const char *argv[] = { "sh.elf", 0 };
        sys_exec("sh.elf", argv);
        sys_exit(127);
    }
    /* Parent doesn't need the slave handle once the shell has it. */
    sys_close(slave);

    /* Parent becomes the RX helper. Loops on incoming SSH packets,
     * forwarding CHANNEL_DATA bytes to the pty master (which appear
     * on shell's stdin). Exit on CHANNEL_CLOSE or read error. */
    static uint8_t rxbuf[SSH_MAX_PACKET];
    for (;;) {
        int n = recv_packet(c, rxbuf, sizeof(rxbuf));
        if (n < 1) break;
        uint8_t m = rxbuf[0];
        if (m == SSH_MSG_CHANNEL_DATA) {
            const uint8_t *p = rxbuf + 1, *end = rxbuf + n;
            ssh_get_u32(&p);            /* our chan id */
            uint32_t dl;
            const uint8_t *d = ssh_get_string(&p, end, &dl);
            if (d) {
                /* Write all bytes to master — pty_master_write blocks
                 * on full buffer, which is exactly what we want
                 * (back-pressure from a slow slave). */
                int w = 0;
                while (w < (int)dl) {
                    int k = sys_write(master, d + w, (int)dl - w);
                    if (k <= 0) break;
                    w += k;
                }
            }
            continue;
        }
        if (m == SSH_MSG_CHANNEL_WINDOW_ADJUST) continue;
        if (m == SSH_MSG_CHANNEL_EOF) continue;
        if (m == SSH_MSG_CHANNEL_CLOSE || m == SSH_MSG_DISCONNECT) break;
    }

    /* RX exit path. Closing the master makes the slave's reads return
     * EOF, which the shell's read_line_interactive interprets as "no
     * more input" and falls out of its loop, exiting. Then the TX
     * helper's master_read returns 0, it sends CHANNEL_EOF + CLOSE,
     * and exits. */
    sys_close(master);
    int code;
    while (sys_wait(&code) >= 0) {}     /* reap shell + TX */
    return 0;
}

/* ---- top-level connection driver --------------------------------- */

static void serve_one(int conn) {
    static struct ssh_conn c;
    /* Zero out the on-fork state — child got the parent's zeroed copy
     * but a previous child of THIS parent (after wait()) would have
     * written into it. We use a static to keep it off the user stack
     * (16 KiB total). */
    for (size_t i = 0; i < sizeof(c); i++) ((uint8_t *)&c)[i] = 0;
    c.fd = conn;

    if (do_banner(&c)        < 0) goto bye;
    printf("sshd: client %s\n", c.v_c);
    if (send_kexinit(&c)      < 0) goto bye;
    if (recv_kexinit(&c)      < 0) goto bye;
    if (do_kex_ecdh(&c)       < 0) goto bye;
    if (do_newkeys(&c)        < 0) goto bye;
    puts("sshd: KEX complete, transport secured\n");

    if (do_service_request(&c) < 0) goto bye;
    struct user_entry *u;
    if (do_userauth(&c, &u)    < 0) goto bye;
    printf("sshd: authenticated user=%s uid=%d\n", u->name, u->uid);

    sys_setgid(u->gid);
    if (sys_setuid(u->uid) < 0) goto bye;

    struct channel ch;
    if (do_channel_open(&c, &ch) < 0) goto bye;

    char cmd[1024];
    int mode = do_channel_requests(&c, &ch, cmd, sizeof(cmd));
    if (mode == 1)       run_exec(&c, &ch, cmd);
    else if (mode == 2)  run_shell(&c, &ch);

bye:
    sys_close(conn);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (load_passwd() < 0) {
        puts("sshd: cannot read /etc/passwd — refusing to start\n");
        return 1;
    }
    printf("sshd: loaded %d users from /etc/passwd\n", g_n_users);

    load_or_generate_host_key();
    print_host_fingerprint();

    /* Register the built-in demo pubkey for `guest`. The selftest's
     * ssh.elf @key mode derives the matching private key from the
     * same seed and can authenticate without filesystem setup. */
    {
        uint8_t demo_pk[32], demo_sk[64];
        ed25519_keypair_from_seed(demo_pk, demo_sk, DEMO_USER_SEED);
        add_auth_key("guest", demo_pk);
    }
    /* Also pull in any keys the user has dropped in /etc/ssh_keys.
     * Format per line: "<user> ssh-ed25519 <base64-blob> [comment]". */
    if (load_auth_keys_file("/etc/ssh_keys") == 0) {
        printf("sshd: loaded /etc/ssh_keys, total %d authorized key(s)\n",
               g_n_auth_keys);
    } else {
        printf("sshd: no /etc/ssh_keys file; %d authorized key(s) (demo only)\n",
               g_n_auth_keys);
    }

    int srv = sys_socket();
    if (srv < 0)                              { puts("sshd: socket failed\n");  return 1; }
    if (sys_bind  (srv, SSH_PORT) < 0)        { puts("sshd: bind failed\n");    return 1; }
    if (sys_listen(srv, 4)        < 0)        { puts("sshd: listen failed\n");  return 1; }
    printf("sshd: listening on SSH-2 port %d\n", SSH_PORT);

    for (;;) {
        int conn = sys_accept(srv);
        if (conn < 0) { sys_sleep_ms(100); continue; }
        int pid = sys_fork();
        if (pid == 0) {
            sys_close(srv);
            serve_one(conn);
            sys_exit(0);
        }
        sys_close(conn);
        int code;
        while (sys_wait_nb(&code) > 0) {}
    }
}
