/*
 * httpsd — HTTPS server backed by AdventOS minimal TLS 1.3.
 *
 * On startup:
 *   1. Generate an Ed25519 keypair (deterministic from a fixed
 *      seed so the cert hash is stable across reboots — useful
 *      for browser SPKI pinning and for `curl --pinnedpubkey`).
 *   2. Build a self-signed X.509 v3 certificate over that keypair.
 *   3. Bind TCP port 4433 and accept-loop.
 *
 * For each accept: fork, run a TLS 1.3 cert-mode handshake (real
 * extensions, CertificateVerify, the lot — see libcrypto/tls.c),
 * read the HTTP request, send a static reply, close.
 *
 * Test from the host (with QEMU's hostfwd=tcp::4433-:4433):
 *   curl -k https://localhost:4433/
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"
#include "../libcrypto/x509.h"

#define HTTPS_PORT  4433

/* Deterministic Ed25519 seed. Re-rolled per release would be the
 * "right" thing in production; baked-in here so anyone running
 * `curl --pinnedpubkey sha256//...` against a fresh boot sees the
 * same fingerprint. */
static const unsigned char DEMO_SEED[32] = {
    0x21, 0x6E, 0x9F, 0x42, 0x07, 0xA8, 0x10, 0xC3,
    0xBE, 0x55, 0xDD, 0x32, 0x88, 0x4F, 0x91, 0x07,
    0xA0, 0xC4, 0xFE, 0x12, 0x6B, 0x99, 0x57, 0x73,
    0x10, 0x2D, 0xCC, 0xB1, 0x44, 0x80, 0xEB, 0x65,
};

/* Filled at startup by main(). */
static unsigned char g_pk[32];
static unsigned char g_sk[64];
static unsigned char g_cert[X509_MAX_CERT];
static int           g_cert_len;

static const char REPLY[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from a TLS 1.3 + HTTPS server in AdventOS!\n"
    "\n"
    "What just happened on the wire:\n"
    "  - Real RFC 8446 ClientHello with extensions\n"
    "  - X25519 ECDHE for forward secrecy\n"
    "  - Self-signed Ed25519 X.509 certificate (RFC 8410)\n"
    "  - CertificateVerify Ed25519 signature over the transcript\n"
    "  - AES-128-GCM record-layer encryption\n"
    "  - 1500 lines of crypto, served from ring 3, no libssl\n"
    "\n"
    "Magic header: ADLC. Cipher suite: TLS_AES_128_GCM_SHA256.\n";

static void serve_one(int conn) {
    struct tls_conn t;
    /* The cert + sk live as static state in this program; the
     * handshake just reads them via the conn struct. */
    t.cert_der     = g_cert;
    t.cert_der_len = g_cert_len;
    t.server_sk    = g_sk;

    int hs = tls_server_handshake_cert(&t, conn);
    if (hs != 0) {
        printf("httpsd: handshake failed (rc=%d)\n", hs);
        sys_close(conn);
        return;
    }
    puts("httpsd: TLS handshake OK\n");

    /* Drain the request (we ignore the contents but log the
     * request line for diagnostics). */
    char req[1024];
    int n = tls_recv(&t, req, sizeof(req) - 1);
    if (n > 0) {
        req[n] = 0;
        for (int i = 0; i < n && i < 128 && req[i] != '\r'; i++) putchar(req[i]);
        putchar('\n');
    }

    int sent = tls_send(&t, REPLY, sizeof(REPLY) - 1);
    printf("httpsd: sent %d encrypted bytes\n", sent);
    sys_close(conn);
}

int main(void) {
    /* Build the keypair + cert once, before binding. Both stay in
     * .data and the fork()d child handlers inherit them via COW. */
    ed25519_keypair_from_seed(g_pk, g_sk, DEMO_SEED);
    g_cert_len = x509_build_self_signed_ed25519(
        g_pk, g_sk, "AdventOS demo cert",
        g_cert, sizeof(g_cert));
    if (g_cert_len < 0) {
        puts("httpsd: x509 build failed\n");
        return 1;
    }
    printf("httpsd: built self-signed Ed25519 cert (%d bytes DER)\n", g_cert_len);

    int srv = sys_socket();
    if (srv < 0) { puts("httpsd: socket failed\n"); return 1; }
    if (sys_bind(srv, HTTPS_PORT) < 0) {
        puts("httpsd: bind failed\n");
        sys_close(srv);
        return 1;
    }
    if (sys_listen(srv, 4) < 0) {
        puts("httpsd: listen failed\n");
        sys_close(srv);
        return 1;
    }
    printf("httpsd: listening on TLS port %d\n", HTTPS_PORT);

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
    return 0;
}
