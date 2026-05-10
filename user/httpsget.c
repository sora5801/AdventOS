/*
 * httpsget — HTTPS client. Connects to TCP host:port (default
 * 10.0.2.15:4433), runs a TLS 1.3 cert-mode handshake, sends a
 * basic GET request, prints the response.
 *
 * Now uses the cert-mode handshake (the same flow curl talks),
 * not the original session-36 PSK demo. The client doesn't
 * validate the server's certificate — equivalent to `curl -k`.
 *
 * Usage:
 *   httpsget                — connects to 10.0.2.15:4433
 *   httpsget <ip4> <port>   — explicit host
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"

static const char REQUEST[] =
    "GET / HTTP/1.0\r\n"
    "Host: localhost\r\n"
    "User-Agent: AdventOS-httpsget/1.0\r\n"
    "\r\n";

int main(int argc, char **argv) {
    unsigned char ip[4] = { 10, 0, 2, 15 };
    int port = 4433;
    if (argc >= 2) {
        const char *p = argv[1]; int idx = 0; int v = 0;
        while (*p && idx < 4) {
            if (*p == '.') { ip[idx++] = (unsigned char)v; v = 0; }
            else if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0');
            p++;
        }
        if (idx == 3) ip[3] = (unsigned char)v;
    }
    if (argc >= 3) port = atoi(argv[2]);

    int sk = sys_socket();
    if (sk < 0) { puts("httpsget: socket failed\n"); return 1; }
    printf("httpsget: connecting to %d.%d.%d.%d:%d\n",
           ip[0], ip[1], ip[2], ip[3], port);
    if (sys_connect(sk, ip, port) < 0) {
        puts("httpsget: connect failed\n");
        sys_close(sk);
        return 1;
    }

    struct tls_conn t;
    int hs = tls_client_handshake_cert(&t, sk);
    if (hs != 0) {
        printf("httpsget: handshake failed (rc=%d)\n", hs);
        sys_close(sk);
        return 1;
    }
    puts("httpsget: TLS handshake OK\n");

    int sent = tls_send(&t, REQUEST, sizeof(REQUEST) - 1);
    printf("httpsget: sent %d-byte request encrypted\n", sent);

    char resp[2048];
    int n = tls_recv(&t, resp, sizeof(resp) - 1);
    if (n <= 0) {
        printf("httpsget: tls_recv returned %d\n", n);
        sys_close(sk);
        return 1;
    }
    resp[n] = 0;
    printf("httpsget: got %d-byte plaintext reply:\n----\n%s----\n", n, resp);

    sys_close(sk);
    return 0;
}
