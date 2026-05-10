/*
 * httpsget — HTTPS client. Connects to TCP host:port (default
 * 127.0.0.1:4433), runs a TLS 1.3 handshake using the same PSK
 * as httpsd, sends a basic GET request, prints the response.
 *
 * Usage:
 *   httpsget                     — connects to 127.0.0.1:4433
 *   httpsget <ip4> <port>        — explicit host
 */
#include "libuser.h"
#include "../libcrypto/crypto.h"
#include "../libcrypto/tls.h"

static const unsigned char DEMO_PSK[32] = {
    0xA5, 0x3F, 0x91, 0x77, 0xB2, 0xCE, 0x40, 0x06,
    0x88, 0xDD, 0x21, 0x55, 0x9C, 0x4E, 0xFA, 0x33,
    0x71, 0x82, 0x49, 0xBE, 0x0F, 0x6A, 0x1D, 0x55,
    0xC8, 0x37, 0x12, 0xE6, 0xAB, 0x90, 0x44, 0x68,
};
static const char DEMO_PSK_ID[] = "adventos-demo-psk-v1";

static const char REQUEST[] =
    "GET / HTTP/1.0\r\n"
    "Host: localhost\r\n"
    "User-Agent: AdventOS-httpsget/1.0\r\n"
    "\r\n";

int main(int argc, char **argv) {
    /* Default to the guest's own DHCP-assigned IP. AdventOS's TCP
     * stack loopbacks only when destination matches g_my_ip (no
     * 127.0.0.0/8 special-case). SLIRP gives us 10.0.2.15. */
    unsigned char ip[4] = { 10, 0, 2, 15 };
    int port = 4433;
    if (argc >= 2) {
        /* Parse a.b.c.d. */
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
    int hs = tls_client_handshake(&t, sk,
                                  DEMO_PSK, sizeof(DEMO_PSK),
                                  DEMO_PSK_ID);
    if (hs != 0) {
        printf("httpsget: handshake failed (rc=%d)\n", hs);
        sys_close(sk);
        return 1;
    }
    puts("httpsget: TLS handshake OK\n");

    int sent = tls_send(&t, REQUEST, sizeof(REQUEST) - 1);
    printf("httpsget: sent %d-byte request encrypted\n", sent);

    /* Read response (single record max for now). */
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
