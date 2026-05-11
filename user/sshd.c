/*
 * sshd — TLS-over-TCP remote shell server for AdventOS.
 *
 * "SSH" in the AdventOS sense: not RFC-4253 SSH-2, but a remote-shell
 * service built from the same three primitives a real sshd uses —
 * (a) a transport-level cipher (TLS 1.3 with cert auth in our case,
 * vs. SSH-2's own KEX+AEAD); (b) user authentication against
 * /etc/passwd with salt+SHA-256 hashes; (c) per-connection shells
 * with the user's real uid/gid. Each command runs through `sh.elf
 * -c` so users get the full session-49 shell — pipelines, env vars,
 * builtins, the works — only with stdin/stdout piped from the TLS
 * connection instead of the local TTY.
 *
 *   client (ssh.elf)                       server (sshd)
 *   ────────────────                       ─────────────
 *   socket+connect ──TCP──────────────►    accept
 *                                          fork per connection
 *   tls_client_handshake_cert ──TLS───►    tls_server_handshake_cert
 *
 *                  user/pass over TLS
 *                  ◄────────────────────► verify against /etc/passwd
 *                                         setuid/setgid
 *
 *                  one command at a time
 *                  ◄────────────────────► fork+exec("sh.elf","-c",line)
 *                                         pipe-shuttle stdout back
 *
 * Wire framing inside the TLS stream: prompts and outputs are plain
 * bytes; the server signals "your turn to type" by sending a single
 * 0x01 byte (an ASCII SOH — guaranteed not to appear in normal
 * stdout). The client reads until it sees 0x01, then waits for the
 * user's line. This is the moral equivalent of SSH-2's window
 * advertisement + channel-eof, only collapsed to one byte because
 * we don't multiplex.
 *
 * One process per connection by way of fork(); state isolation is
 * the kernel's address-space isolation, not threads. `cd` is handled
 * inline (no fork) so cwd persists; everything else runs sh.elf -c.
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"
#include "../libcrypto/x509.h"

#define SSH_PORT     2222
#define LINE_MAX     256
#define USERS_MAX    16

/* Deterministic seed for the server's ECDSA-P256 keypair. Different
 * from httpsd's seed so the two services have distinct cert
 * fingerprints — makes wire captures easier to read and prevents
 * a curl --pinnedpubkey collision between the two demos. */
static const unsigned char SSHD_SEED[32] = {
    0x55, 0x41, 0x8E, 0xC2, 0x97, 0x33, 0xB2, 0x09,
    0x7B, 0xC1, 0xF5, 0x80, 0x4D, 0x6E, 0x21, 0xA8,
    0x18, 0xDD, 0x44, 0xE7, 0x52, 0x95, 0x06, 0x6B,
    0xFA, 0x21, 0x90, 0xC4, 0xAB, 0x77, 0x33, 0x18,
};

/* Filled at startup by main(); inherited COW by every forked child. */
static unsigned char g_pub[64];
static unsigned char g_priv[32];
static unsigned char g_cert[X509_MAX_CERT];
static int           g_cert_len;

/* ---- /etc/passwd parsing (copied from login.c) -------------------- */

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

static void hex_lower(const unsigned char *in, int n, char *out) {
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
    int hml = ends[4] - starts[4];
    if (hml >= (int)sizeof(u->home)) hml = (int)sizeof(u->home) - 1;
    for (int i = 0; i < hml; i++) u->home[i] = line[starts[4] + i];
    u->home[hml] = 0;
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

/* ---- TLS line I/O -------------------------------------------------- */

/* Userspace receive buffer.
 *
 * tls_recv() decrypts one whole TLS record and copies AT MOST max_n
 * plaintext bytes into the caller's buffer — the rest is DISCARDED
 * (libcrypto/tls.c:1050). Reading a line byte-by-byte directly from
 * tls_recv would therefore lose 5 of 6 bytes from a "guest\n" record
 * and hang on the next call waiting for a record that never comes.
 *
 * Fix: read one record's worth into a static buffer, hand out from
 * there. Each forked sshd child has its own copy of g_rx (forked
 * via COW), so there's no inter-connection bleed. */
static struct rx_state {
    char buf[TLS_MAX_FRAGMENT];
    int  pos;
    int  end;
} g_rx;

static int tls_read_byte(struct tls_conn *t, char *out) {
    if (g_rx.pos >= g_rx.end) {
        int n = tls_recv(t, g_rx.buf, (int)sizeof(g_rx.buf));
        if (n <= 0) return -1;
        g_rx.end = n;
        g_rx.pos = 0;
    }
    *out = g_rx.buf[g_rx.pos++];
    return 1;
}

/* Read a logical line (terminated by '\n', or EOF) from the TLS
 * connection. Strips trailing CR if present so Windows clients work.
 * Returns the line length (>= 0), or -1 if the connection dropped. */
static int tls_read_line(struct tls_conn *t, char *out, int cap) {
    int len = 0;
    for (;;) {
        char c;
        if (tls_read_byte(t, &c) < 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len < cap - 1) out[len++] = c;
    }
    out[len] = 0;
    return len;
}

/* Send a NUL-terminated string over TLS in one record. */
static int tls_send_str(struct tls_conn *t, const char *s) {
    int n = 0; while (s[n]) n++;
    return tls_send(t, s, n);
}

/* The "your turn" sentinel: a lone 0x01 byte (SOH). The client reads
 * TLS bytes to stdout until it sees one of these, then prompts the
 * user for the next command. We emit it after every server-side
 * output block — login prompts, the post-auth banner, and after each
 * command's output finishes draining. */
static int tls_send_ready(struct tls_conn *t) {
    char b = 0x01;
    return tls_send(t, &b, 1);
}

/* ---- Per-connection command loop ----------------------------------- */

/* Run one command line: fork a child that execs `sh.elf -c <line>`
 * with stdout/stderr captured into a pipe, then shuttle the pipe
 * contents back over TLS. Returns 0 if the connection should
 * continue, -1 if the parent should tear it down. */
static int run_remote_command(struct tls_conn *t, const char *line) {
    int outp[2];
    if (sys_pipe(outp) < 0) {
        tls_send_str(t, "sshd: pipe() failed\n");
        return 0;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(outp[0]); sys_close(outp[1]);
        tls_send_str(t, "sshd: fork() failed\n");
        return 0;
    }

    if (pid == 0) {
        /* Child: redirect stdout + stderr to the pipe, drop the read
         * end, then exec the shell. Stdin keeps pointing at whatever
         * we inherited (the TLS connection's TCP socket, which has no
         * controlling TTY) — sh.elf -c never reads stdin. */
        sys_dup2(outp[1], 1);
        sys_dup2(outp[1], 2);
        sys_close(outp[0]);
        sys_close(outp[1]);
        const char *argv[] = { "sh.elf", "-c", line, 0 };
        sys_exec("sh.elf", argv);
        /* exec failed — write something the parent will forward back */
        const char err[] = "sshd: exec sh.elf failed\n";
        sys_write(1, err, (int)sizeof(err) - 1);
        sys_exit(127);
    }

    /* Parent: close the write end so reads see EOF when the child
     * (and any of its grandchildren that inherited fd 1) all exit. */
    sys_close(outp[1]);

    char buf[256];
    for (;;) {
        int n = sys_read(outp[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (tls_send(t, buf, n) < 0) {
            /* Client dropped — reap the child and bail. */
            sys_close(outp[0]);
            int code; sys_wait(&code);
            return -1;
        }
    }
    sys_close(outp[0]);
    int code; sys_wait(&code);
    return 0;
}

/* The post-auth interactive loop. Builtins handled inline (so they
 * mutate THIS process's state); everything else fork+execs sh.elf -c.
 * Loops until the client sends "exit" or the connection drops. */
static void shell_loop(struct tls_conn *t, struct user_entry *u) {
    char welcome[128];
    int w = 0;
    const char *banner = "\nWelcome to AdventOS over TLS.  user=";
    while (*banner) welcome[w++] = *banner++;
    int nl = 0; while (u->name[nl]) welcome[w++] = u->name[nl++];
    const char *rest = "  shell=sh.elf -c\nType 'exit' to disconnect.\n\n";
    while (*rest) welcome[w++] = *rest++;
    welcome[w] = 0;
    tls_send(t, welcome, w);

    for (;;) {
        tls_send_str(t, "advent-ssh$ ");
        tls_send_ready(t);

        char line[LINE_MAX];
        int len = tls_read_line(t, line, sizeof(line));
        if (len < 0) return;

        /* Trim leading whitespace. */
        int s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (!line[s]) continue;
        char *cmd = &line[s];

        if (my_strcmp(cmd, "exit") == 0) {
            tls_send_str(t, "bye\n");
            return;
        }

        /* `cd` handled inline so the cwd change persists for the next
         * command. Anything else (including pipelines, redirects, env
         * exports) goes through sh.elf -c. */
        if (cmd[0] == 'c' && cmd[1] == 'd' && (cmd[2] == 0 || cmd[2] == ' ')) {
            const char *path = "/";
            if (cmd[2] == ' ') {
                int p = 3;
                while (cmd[p] == ' ') p++;
                if (cmd[p]) path = &cmd[p];
            }
            if (sys_chdir(path) < 0) {
                tls_send_str(t, "cd: ");
                tls_send_str(t, path);
                tls_send_str(t, ": no such directory\n");
            }
            continue;
        }

        if (run_remote_command(t, cmd) < 0) return;
    }
}

/* Per-connection driver. Handshake → log in → shell loop. */
static void serve_one(int conn) {
    struct tls_conn t;
    t.cert_der     = g_cert;
    t.cert_der_len = g_cert_len;
    t.server_sk    = g_priv;
    t.sig_alg      = 0x0403;     /* ecdsa_secp256r1_sha256 */

    int hs = tls_server_handshake_cert(&t, conn);
    if (hs != 0) {
        printf("sshd: handshake failed rc=%d\n", hs);
        sys_close(conn);
        return;
    }
    puts("sshd: TLS handshake OK; entering auth\n");

    /* Two tries before we hang up. Reading 0 bytes for username or
     * password means the client closed mid-prompt — treat as drop. */
    for (int attempt = 0; attempt < 2; attempt++) {
        tls_send_str(&t, "AdventOS sshd over TLS 1.3 (ECDSA-P256)\n");
        tls_send_str(&t, "login: ");
        tls_send_ready(&t);

        char user[64];
        if (tls_read_line(&t, user, sizeof(user)) <= 0) {
            sys_close(conn); return;
        }

        tls_send_str(&t, "password: ");
        tls_send_ready(&t);

        char pass[64];
        if (tls_read_line(&t, pass, sizeof(pass)) < 0) {
            sys_close(conn); return;
        }

        struct user_entry *u = find_user(user);
        if (!u || !verify_password(u, pass)) {
            tls_send_str(&t, "Login incorrect\n");
            printf("sshd: auth failed for user='%s'\n", user);
            sys_sleep_ms(300);
            continue;
        }

        printf("sshd: authenticated user=%s uid=%d\n", u->name, u->uid);
        sys_setgid(u->gid);
        if (sys_setuid(u->uid) < 0) {
            tls_send_str(&t, "sshd: setuid failed\n");
            sys_close(conn); return;
        }

        shell_loop(&t, u);
        sys_close(conn);
        return;
    }

    tls_send_str(&t, "Too many failed attempts. Bye.\n");
    sys_close(conn);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (load_passwd() < 0) {
        puts("sshd: cannot read /etc/passwd — refusing to start\n");
        return 1;
    }
    printf("sshd: loaded %d users from /etc/passwd\n", g_n_users);

    /* Lazy keypair + cert build, same pattern as httpsd. */
    p256_keypair_from_seed(g_pub, g_priv, SSHD_SEED);
    g_cert_len = x509_build_self_signed_p256(
        g_pub, g_priv, "AdventOS sshd",
        g_cert, sizeof(g_cert));
    if (g_cert_len < 0) {
        puts("sshd: x509 build failed\n");
        return 1;
    }
    printf("sshd: built self-signed ECDSA-P256 cert (%d bytes DER)\n", g_cert_len);

    int srv = sys_socket();
    if (srv < 0) { puts("sshd: socket failed\n"); return 1; }
    if (sys_bind(srv, SSH_PORT) < 0) {
        puts("sshd: bind failed\n");
        sys_close(srv);
        return 1;
    }
    if (sys_listen(srv, 4) < 0) {
        puts("sshd: listen failed\n");
        sys_close(srv);
        return 1;
    }
    printf("sshd: listening on TLS port %d\n", SSH_PORT);

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
        while (sys_wait_nb(&code) > 0) {}    /* reap zombies */
    }
    return 0;
}
