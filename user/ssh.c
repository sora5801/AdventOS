/*
 * ssh — RFC 4253 SSH-2 client for AdventOS.
 *
 * Symmetric counterpart to sshd.c. Same algorithm set:
 *   curve25519-sha256 / ssh-ed25519 / aes128-gcm@openssh.com / none.
 *
 * Usage:
 *   ssh <ip>[:port] <user> <password> <command>
 *
 * Yes — the password and command are command-line args. This is a demo
 * client meant to drive the loopback selftest; on a real Unix you'd
 * never type passwords on argv. The deep dive calls out that gap.
 *
 * Host key verification: we trust the server's ed25519 signature
 * binding K_S to H. We do NOT pin the host key fingerprint (no
 * known_hosts) — same `-k` posture as session-50's TLS client.
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/ssh.h"

/* ---- TCP I/O ----------------------------------------------------- */

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

/* ---- SSH conn ---------------------------------------------------- */

struct ssh_conn {
    int     fd;
    char    v_c[64];  int v_c_len;
    char    v_s[256]; int v_s_len;
    uint8_t i_c[1024]; int i_c_len;
    uint8_t i_s[4096]; int i_s_len;
    uint8_t q_c[32];
    uint8_t q_s[32];
    uint8_t d_c[32];
    uint8_t host_pk[32];
    uint8_t k_mpint[40]; int k_mpint_len;
    uint8_t h[32];
    uint8_t session_id[32];

    int     enc_in, enc_out;
    uint8_t key_c2s[16];
    uint8_t iv_c2s [12];
    uint8_t key_s2c[16];
    uint8_t iv_s2c [12];
};

/* ---- packet layer ------------------------------------------------ */

static int send_packet_clear(struct ssh_conn *c, const uint8_t *p, int n) {
    /* RFC 4253 §6: total (4 + 1 + payload + pad) multiple of 8, pad >= 4. */
    int pad_n = 8 - ((4 + 1 + n) % 8);
    if (pad_n < 4) pad_n += 8;
    int enc_n = 1 + n + pad_n;
    static uint8_t out[8192];
    if (4 + enc_n > (int)sizeof(out)) return -1;
    out[0]=(uint8_t)(enc_n>>24); out[1]=(uint8_t)(enc_n>>16);
    out[2]=(uint8_t)(enc_n>>8 ); out[3]=(uint8_t)(enc_n);
    out[4] = (uint8_t)pad_n;
    for (int i = 0; i < n; i++) out[5+i] = p[i];
    rand_bytes(out + 5 + n, pad_n);
    return write_all(c->fd, out, 4 + enc_n);
}

static int recv_packet_clear(struct ssh_conn *c, uint8_t *p, int max) {
    uint8_t h[5];
    if (read_exact(c->fd, h, 5) < 0) return -1;
    uint32_t enc_n = ((uint32_t)h[0]<<24)|((uint32_t)h[1]<<16)|
                     ((uint32_t)h[2]<< 8)|(uint32_t)h[3];
    int pad_n = h[4];
    if (enc_n < 8 || enc_n > 8000) return -1;
    static uint8_t buf[8192];
    if (read_exact(c->fd, buf, (int)enc_n - 1) < 0) return -1;
    int pl = (int)enc_n - 1 - pad_n;
    if (pl < 0 || pl > max) return -1;
    for (int i = 0; i < pl; i++) p[i] = buf[i];
    return pl;
}

static int send_packet_aead(struct ssh_conn *c, const uint8_t *p, int n) {
    static uint8_t out[SSH_MAX_PACKET];
    size_t out_len;
    if (ssh_packet_seal(c->key_c2s, c->iv_c2s, p, n, out, &out_len) < 0) return -1;
    return write_all(c->fd, out, (int)out_len);
}

static int recv_packet_aead(struct ssh_conn *c, uint8_t *p, int max) {
    uint8_t hdr[4];
    if (read_exact(c->fd, hdr, 4) < 0) return -1;
    uint32_t enc_n = ((uint32_t)hdr[0]<<24)|((uint32_t)hdr[1]<<16)|
                     ((uint32_t)hdr[2]<< 8)|(uint32_t)hdr[3];
    if (enc_n < 16 || enc_n > SSH_MAX_PACKET - 4 - 16) return -1;
    static uint8_t pkt[SSH_MAX_PACKET];
    for (int i = 0; i < 4; i++) pkt[i] = hdr[i];
    if (read_exact(c->fd, pkt + 4, (int)enc_n + 16) < 0) return -1;
    size_t pl;
    if (ssh_packet_open(c->key_s2c, c->iv_s2c, pkt, 4 + enc_n + 16,
                        p, (size_t)max, &pl) < 0) return -1;
    return (int)pl;
}

static int send_packet(struct ssh_conn *c, const uint8_t *p, int n) {
    return c->enc_out ? send_packet_aead(c, p, n) : send_packet_clear(c, p, n);
}

static int recv_packet(struct ssh_conn *c, uint8_t *p, int max) {
    for (;;) {
        int n = c->enc_in ? recv_packet_aead(c, p, max) : recv_packet_clear(c, p, max);
        if (n < 1) return n;
        if (p[0] == SSH_MSG_IGNORE || p[0] == SSH_MSG_DEBUG) continue;
        return n;
    }
}

/* ---- banner ------------------------------------------------------ */

static int do_banner(struct ssh_conn *c) {
    static const char ours[] = "SSH-2.0-AdventOS_1.0-client";
    int i = 0;
    while (ours[i]) { c->v_c[i] = ours[i]; i++; }
    c->v_c_len = i;

    char buf[64];
    for (int j = 0; j < c->v_c_len; j++) buf[j] = c->v_c[j];
    buf[c->v_c_len] = '\r';
    buf[c->v_c_len + 1] = '\n';
    if (write_all(c->fd, buf, c->v_c_len + 2) < 0) return -1;

    int len = 0;
    while (len < 255) {
        char ch;
        if (read_exact(c->fd, &ch, 1) < 0) return -1;
        if (ch == '\r') continue;
        if (ch == '\n') {
            c->v_s[len] = 0;
            if (len >= 4 && c->v_s[0]=='S'&&c->v_s[1]=='S'&&c->v_s[2]=='H'&&c->v_s[3]=='-') {
                break;
            }
            len = 0;
            continue;
        }
        c->v_s[len++] = ch;
    }
    c->v_s_len = len;
    return 0;
}

/* ---- KEX --------------------------------------------------------- */

static int send_kexinit(struct ssh_conn *c) {
    uint8_t *p = c->i_c;
    ssh_put_u8(&p, SSH_MSG_KEXINIT);
    uint8_t cookie[16]; rand_bytes(cookie, 16);
    ssh_put_bytes(&p, cookie, 16);
    ssh_put_cstring(&p, "curve25519-sha256");
    ssh_put_cstring(&p, "ssh-ed25519");
    ssh_put_cstring(&p, "aes128-gcm@openssh.com");
    ssh_put_cstring(&p, "aes128-gcm@openssh.com");
    ssh_put_cstring(&p, "");
    ssh_put_cstring(&p, "");
    ssh_put_cstring(&p, "none");
    ssh_put_cstring(&p, "none");
    ssh_put_cstring(&p, "");
    ssh_put_cstring(&p, "");
    ssh_put_bool(&p, 0);
    ssh_put_u32(&p, 0);
    c->i_c_len = (int)(p - c->i_c);
    return send_packet(c, c->i_c, c->i_c_len);
}

static int recv_kexinit(struct ssh_conn *c) {
    int n = recv_packet(c, c->i_s, sizeof(c->i_s));
    if (n < 1 || c->i_s[0] != SSH_MSG_KEXINIT) return -1;
    c->i_s_len = n;
    return 0;
}

static void hash_string(struct sha256 *s, const void *d, int n) {
    uint8_t lb[4];
    lb[0]=(uint8_t)(n>>24); lb[1]=(uint8_t)(n>>16);
    lb[2]=(uint8_t)(n>>8 ); lb[3]=(uint8_t)n;
    sha256_update(s, lb, 4);
    sha256_update(s, d, n);
}

/* Send SSH_MSG_KEX_ECDH_INIT (our Q_C); receive REPLY (K_S, Q_S, sig).
 * Verify the host signature and compute the keys via the exchange hash. */
static int do_kex_ecdh(struct ssh_conn *c) {
    /* Ephemeral keypair + send. */
    rand_bytes(c->d_c, 32);
    x25519(c->q_c, c->d_c, x25519_basepoint);

    uint8_t pkt[64];
    uint8_t *pp = pkt;
    ssh_put_u8(&pp, SSH_MSG_KEX_ECDH_INIT);
    ssh_put_string(&pp, c->q_c, 32);
    if (send_packet(c, pkt, (int)(pp - pkt)) < 0) return -1;

    /* Receive REPLY. */
    uint8_t buf[1024];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_KEX_ECDH_REPLY) return -1;
    const uint8_t *p = buf + 1, *end = buf + n;

    /* K_S = string "ssh-ed25519" || string pubkey(32) */
    uint32_t ks_len;
    const uint8_t *ks = ssh_get_string(&p, end, &ks_len);
    if (!ks) return -1;
    {
        const uint8_t *kp = ks, *ke = ks + ks_len;
        uint32_t tl;
        const uint8_t *tp = ssh_get_string(&kp, ke, &tl);
        if (!tp || tl != 11) return -1;
        for (int i = 0; i < 11; i++) {
            if (tp[i] != "ssh-ed25519"[i]) return -1;
        }
        uint32_t pkl;
        const uint8_t *pkp = ssh_get_string(&kp, ke, &pkl);
        if (!pkp || pkl != 32) return -1;
        for (int i = 0; i < 32; i++) c->host_pk[i] = pkp[i];
    }

    /* Q_S */
    uint32_t qs_len;
    const uint8_t *qs = ssh_get_string(&p, end, &qs_len);
    if (!qs || qs_len != 32) return -1;
    for (int i = 0; i < 32; i++) c->q_s[i] = qs[i];

    /* Signature blob = string "ssh-ed25519" || string sig(64) */
    uint32_t sig_len;
    const uint8_t *sig = ssh_get_string(&p, end, &sig_len);
    if (!sig) return -1;
    uint8_t sig_raw[64];
    {
        const uint8_t *kp = sig, *ke = sig + sig_len;
        uint32_t tl;
        const uint8_t *tp = ssh_get_string(&kp, ke, &tl);
        if (!tp || tl != 11) return -1;
        uint32_t sl;
        const uint8_t *sp2 = ssh_get_string(&kp, ke, &sl);
        if (!sp2 || sl != 64) return -1;
        for (int i = 0; i < 64; i++) sig_raw[i] = sp2[i];
    }

    /* Shared secret + mpint. */
    uint8_t k_raw[32];
    x25519(k_raw, c->d_c, c->q_s);
    uint8_t *kp = c->k_mpint;
    ssh_put_mpint(&kp, k_raw, 32);
    c->k_mpint_len = (int)(kp - c->k_mpint);

    /* H = SHA-256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K). */
    struct sha256 sh;
    sha256_init(&sh);
    hash_string(&sh, c->v_c, c->v_c_len);
    hash_string(&sh, c->v_s, c->v_s_len);
    hash_string(&sh, c->i_c, c->i_c_len);
    hash_string(&sh, c->i_s, c->i_s_len);
    hash_string(&sh, ks,     (int)ks_len);
    hash_string(&sh, c->q_c, 32);
    hash_string(&sh, c->q_s, 32);
    sha256_update(&sh, c->k_mpint, c->k_mpint_len);
    sha256_final(&sh, c->h);

    /* Verify the server's signature over H with the advertised pubkey. */
    if (ed25519_verify(sig_raw, c->h, 32, c->host_pk) != 0) {
        puts("ssh: ed25519 host-key signature FAILED\n");
        return -1;
    }

    for (int i = 0; i < 32; i++) c->session_id[i] = c->h[i];
    return 0;
}

static int do_newkeys(struct ssh_conn *c) {
    uint8_t nk = SSH_MSG_NEWKEYS;
    if (send_packet(c, &nk, 1) < 0) return -1;
    uint8_t tmp[32];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'A', c->session_id);
    for (int i = 0; i < 12; i++) c->iv_c2s[i] = tmp[i];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'C', c->session_id);
    for (int i = 0; i < 16; i++) c->key_c2s[i] = tmp[i];
    c->enc_out = 1;

    uint8_t buf[16];
    int n = recv_packet(c, buf, sizeof(buf));
    if (n < 1 || buf[0] != SSH_MSG_NEWKEYS) return -1;
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'B', c->session_id);
    for (int i = 0; i < 12; i++) c->iv_s2c[i] = tmp[i];
    ssh_kdf(tmp, c->k_mpint, c->k_mpint_len, c->h, 'D', c->session_id);
    for (int i = 0; i < 16; i++) c->key_s2c[i] = tmp[i];
    c->enc_in = 1;
    return 0;
}

/* ---- userauth + channel + exec --------------------------------- */

static int do_service_request(struct ssh_conn *c) {
    uint8_t p[32];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_SERVICE_REQUEST);
    ssh_put_cstring(&pp, "ssh-userauth");
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1 || r[0] != SSH_MSG_SERVICE_ACCEPT) return -1;
    return 0;
}

static int do_userauth(struct ssh_conn *c, const char *user, const char *pass) {
    uint8_t p[256];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_USERAUTH_REQUEST);
    ssh_put_cstring(&pp, user);
    ssh_put_cstring(&pp, "ssh-connection");
    ssh_put_cstring(&pp, "password");
    ssh_put_bool(&pp, 0);
    ssh_put_cstring(&pp, pass);
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1) return -1;
    if (r[0] == SSH_MSG_USERAUTH_SUCCESS) return 0;
    return -1;
}

static int do_open_session(struct ssh_conn *c, uint32_t *server_chan) {
    uint8_t p[64];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_CHANNEL_OPEN);
    ssh_put_cstring(&pp, "session");
    ssh_put_u32(&pp, 0);            /* our chan id */
    ssh_put_u32(&pp, 0x100000);     /* initial window */
    ssh_put_u32(&pp, 16384);        /* max packet */
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1 || r[0] != SSH_MSG_CHANNEL_OPEN_CONFIRMATION) return -1;
    const uint8_t *q = r + 1;
    ssh_get_u32(&q);                /* our chan id (echoed) */
    *server_chan = ssh_get_u32(&q); /* server's chan id */
    return 0;
}

static int do_exec_request(struct ssh_conn *c, uint32_t server_chan, const char *cmd) {
    uint8_t p[1024];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&pp, server_chan);
    ssh_put_cstring(&pp, "exec");
    ssh_put_bool(&pp, 1);             /* want_reply */
    ssh_put_cstring(&pp, cmd);
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1 || r[0] != SSH_MSG_CHANNEL_SUCCESS) return -1;
    return 0;
}

/* `pty-req` — minimal payload, server ignores the terminal modes
 * because our pty has no line discipline (raw passthrough). */
static int do_pty_request(struct ssh_conn *c, uint32_t server_chan) {
    uint8_t p[256];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&pp, server_chan);
    ssh_put_cstring(&pp, "pty-req");
    ssh_put_bool(&pp, 1);                   /* want_reply */
    ssh_put_cstring(&pp, "xterm-256color"); /* TERM */
    ssh_put_u32(&pp, 80);                   /* width chars */
    ssh_put_u32(&pp, 24);                   /* height rows */
    ssh_put_u32(&pp, 0);                    /* width pixels */
    ssh_put_u32(&pp, 0);                    /* height pixels */
    ssh_put_string(&pp, "", 0);             /* termios modes (empty) */
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1 || r[0] != SSH_MSG_CHANNEL_SUCCESS) return -1;
    return 0;
}

static int do_shell_request(struct ssh_conn *c, uint32_t server_chan) {
    uint8_t p[64];
    uint8_t *pp = p;
    ssh_put_u8(&pp, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&pp, server_chan);
    ssh_put_cstring(&pp, "shell");
    ssh_put_bool(&pp, 1);
    if (send_packet(c, p, (int)(pp - p)) < 0) return -1;
    uint8_t r[64];
    int n = recv_packet(c, r, sizeof(r));
    if (n < 1 || r[0] != SSH_MSG_CHANNEL_SUCCESS) return -1;
    return 0;
}

/* Bidirectional shuttle for shell mode (session 52). Fork a TX helper
 * that reads local stdin and forwards it as CHANNEL_DATA; parent
 * becomes RX, prints incoming CHANNEL_DATA to stdout, exits on CLOSE.
 *
 * Same architectural shape as sshd's run_shell — two unidirectional
 * helpers, each using its own AEAD direction key, no shared state
 * between them after fork. */
static int run_shell_shuttle(struct ssh_conn *c, uint32_t server_chan) {
    int tx_pid = sys_fork();
    if (tx_pid == 0) {
        /* TX child: read stdin, send as CHANNEL_DATA. Uses key_c2s
         * (this side is "client to server" for sends). */
        static char ibuf[1024];
        for (;;) {
            int n = sys_read(0, ibuf, sizeof(ibuf));
            if (n <= 0) break;
            uint8_t pkt[1024 + 32];
            uint8_t *p = pkt;
            ssh_put_u8(&p, SSH_MSG_CHANNEL_DATA);
            ssh_put_u32(&p, server_chan);
            ssh_put_string(&p, ibuf, n);
            if (send_packet(c, pkt, (int)(p - pkt)) < 0) break;
        }
        /* Local stdin hit EOF or send failed — tell the peer. */
        uint8_t pkt[8]; uint8_t *q = pkt;
        ssh_put_u8(&q, SSH_MSG_CHANNEL_EOF); ssh_put_u32(&q, server_chan);
        send_packet(c, pkt, (int)(q - pkt));
        sys_exit(0);
    }

    /* Parent: RX side. Same packet handling as drain_session but with
     * an extra side-effect that on CHANNEL_CLOSE we kill the TX helper
     * so the parent's sys_wait below isn't left holding it open. */
    int exit_status = -1;
    int saw_close = 0;
    for (;;) {
        static uint8_t buf[SSH_MAX_PACKET];
        int n = recv_packet(c, buf, sizeof(buf));
        if (n < 1) break;
        uint8_t m = buf[0];
        if (m == SSH_MSG_CHANNEL_DATA || m == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            const uint8_t *p = buf + 1, *end = buf + n;
            ssh_get_u32(&p);
            if (m == SSH_MSG_CHANNEL_EXTENDED_DATA) ssh_get_u32(&p);
            uint32_t dl;
            const uint8_t *d = ssh_get_string(&p, end, &dl);
            if (d) sys_write(1, d, (int)dl);
            continue;
        }
        if (m == SSH_MSG_CHANNEL_REQUEST) {
            const uint8_t *p = buf + 1, *end = buf + n;
            ssh_get_u32(&p);
            uint32_t tl;
            const uint8_t *t = ssh_get_string(&p, end, &tl);
            ssh_get_u8(&p);
            if (t && tl == 11) {
                int is_status = 1;
                for (int i = 0; i < 11; i++) if (t[i] != "exit-status"[i]) { is_status = 0; break; }
                if (is_status) exit_status = (int)ssh_get_u32(&p);
            }
            continue;
        }
        if (m == SSH_MSG_CHANNEL_WINDOW_ADJUST) continue;
        if (m == SSH_MSG_CHANNEL_EOF)           continue;
        if (m == SSH_MSG_CHANNEL_CLOSE) {
            if (!saw_close) {
                uint8_t cl[8]; uint8_t *q = cl;
                ssh_put_u8(&q, SSH_MSG_CHANNEL_CLOSE);
                ssh_put_u32(&q, server_chan);
                send_packet(c, cl, (int)(q - cl));
                saw_close = 1;
            }
            break;
        }
        if (m == SSH_MSG_DISCONNECT) break;
    }

    /* Kill the TX helper so we can reap it cleanly. SIGTERM tears it
     * out of its blocking sys_read on stdin. */
    sys_kill(tx_pid, SIGTERM);
    int code;
    sys_wait(&code);
    return exit_status;
}

/* Drain CHANNEL_DATA / EOF / CLOSE / etc. until the connection ends.
 * Returns the exit-status reported by the server, or -1. */
static int drain_session(struct ssh_conn *c, uint32_t server_chan) {
    int exit_status = -1;
    int saw_close = 0;
    for (;;) {
        uint8_t buf[SSH_MAX_PACKET];
        int n = recv_packet(c, buf, sizeof(buf));
        if (n < 1) break;
        uint8_t m = buf[0];

        if (m == SSH_MSG_CHANNEL_DATA || m == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            const uint8_t *p = buf + 1, *end = buf + n;
            ssh_get_u32(&p);                 /* our chan */
            if (m == SSH_MSG_CHANNEL_EXTENDED_DATA) ssh_get_u32(&p);   /* type */
            uint32_t dl;
            const uint8_t *d = ssh_get_string(&p, end, &dl);
            if (d) sys_write(1, d, (int)dl);
            continue;
        }
        if (m == SSH_MSG_CHANNEL_REQUEST) {
            const uint8_t *p = buf + 1, *end = buf + n;
            ssh_get_u32(&p);
            uint32_t tl;
            const uint8_t *t = ssh_get_string(&p, end, &tl);
            ssh_get_u8(&p);                   /* want_reply */
            if (t && tl == 11) {
                int is_status = 1;
                for (int i = 0; i < 11; i++) if (t[i] != "exit-status"[i]) { is_status = 0; break; }
                if (is_status) exit_status = (int)ssh_get_u32(&p);
            }
            continue;
        }
        if (m == SSH_MSG_CHANNEL_WINDOW_ADJUST) continue;
        if (m == SSH_MSG_CHANNEL_EOF)           continue;
        if (m == SSH_MSG_CHANNEL_CLOSE) {
            if (!saw_close) {
                /* Echo back so the server can tear down cleanly. */
                uint8_t cl[8]; uint8_t *q = cl;
                ssh_put_u8(&q, SSH_MSG_CHANNEL_CLOSE);
                ssh_put_u32(&q, server_chan);
                send_packet(c, cl, (int)(q - cl));
                saw_close = 1;
            }
            break;
        }
        if (m == SSH_MSG_DISCONNECT) break;
    }
    return exit_status;
}

/* ---- arg parsing ------------------------------------------------- */

static int parse_ip_port(const char *s, unsigned char ip[4], int *port_out) {
    int dots = 0, val = 0, seen = 0, idx = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') { val = val*10 + (*s - '0'); seen = 1; }
        else if (*s == '.') {
            if (!seen || val > 255 || idx >= 3) return -1;
            ip[idx++] = (unsigned char)val; val = 0; seen = 0; dots++;
        } else if (*s == ':') break;
        else return -1;
        s++;
    }
    if (dots != 3 || !seen || val > 255) return -1;
    ip[3] = (unsigned char)val;
    if (*s == ':') {
        s++; int p = 0;
        while (*s >= '0' && *s <= '9') { p = p*10 + (*s - '0'); s++; }
        if (p <= 0 || p > 65535) return -1;
        *port_out = p;
    } else {
        *port_out = 2222;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        puts("usage: ssh <ip>[:port] <user> <password> [command]\n");
        puts("  no command  -> request pty + shell, bidirectional shuttle\n");
        puts("  command     -> one-shot exec, no pty\n");
        return 1;
    }
    const char *host = argv[1];
    const char *user = argv[2];
    const char *pass = argv[3];
    const char *cmd  = argc >= 5 ? argv[4] : 0;

    unsigned char ip[4]; int port;
    if (parse_ip_port(host, ip, &port) < 0) {
        puts("ssh: bad ip\n"); return 1;
    }
    printf("ssh: connecting to %d.%d.%d.%d:%d as %s\n",
           ip[0], ip[1], ip[2], ip[3], port, user);

    int sk = sys_socket();
    if (sk < 0) { puts("ssh: socket() failed\n"); return 1; }
    if (sys_connect(sk, ip, port) < 0) {
        puts("ssh: connect failed\n"); sys_close(sk); return 1;
    }
    puts("ssh: TCP connected; starting SSH-2 handshake\n");

    static struct ssh_conn c;
    for (size_t i = 0; i < sizeof(c); i++) ((uint8_t *)&c)[i] = 0;
    c.fd = sk;

    if (do_banner(&c)      < 0) { puts("ssh: banner failed\n"); return 1; }
    printf("ssh: server banner: %s\n", c.v_s);
    if (send_kexinit(&c)   < 0) { puts("ssh: send KEXINIT failed\n"); return 1; }
    if (recv_kexinit(&c)   < 0) { puts("ssh: recv KEXINIT failed\n"); return 1; }
    if (do_kex_ecdh(&c)    < 0) { puts("ssh: ECDH or sig verify failed\n"); return 1; }
    puts("ssh: KEX done, host key verified\n");
    if (do_newkeys(&c)     < 0) { puts("ssh: NEWKEYS failed\n"); return 1; }
    puts("ssh: transport encrypted (aes128-gcm)\n");

    if (do_service_request(&c) < 0) { puts("ssh: SERVICE_REQUEST failed\n"); return 1; }
    if (do_userauth(&c, user, pass) < 0) {
        puts("ssh: authentication failed\n");
        return 1;
    }
    puts("ssh: authenticated\n");

    uint32_t scid;
    if (do_open_session(&c, &scid) < 0) {
        puts("ssh: CHANNEL_OPEN failed\n"); return 1;
    }

    int rc;
    if (cmd) {
        if (do_exec_request(&c, scid, cmd) < 0) {
            puts("ssh: exec request failed\n"); return 1;
        }
        rc = drain_session(&c, scid);
    } else {
        if (do_pty_request(&c, scid) < 0) {
            puts("ssh: pty-req rejected\n"); return 1;
        }
        if (do_shell_request(&c, scid) < 0) {
            puts("ssh: shell request rejected\n"); return 1;
        }
        puts("ssh: pty + shell allocated; entering interactive shuttle\n");
        rc = run_shell_shuttle(&c, scid);
    }
    sys_close(sk);
    printf("ssh: remote exit-status = %d\n", rc);
    return rc < 0 ? 0 : rc;
}
