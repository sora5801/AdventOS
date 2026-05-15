/*
 * smp-hammer — stress the SMP=2 loopback-TCP path.
 *
 * Opens a fresh TCP conn to agentd (127.0.0.1:7000) for each
 * request, fires N back-to-back `time` JSON-RPC calls, counts
 * responses, prints a one-line summary. If response count trails
 * request count by `STALL_S` seconds, emits DEADLOCK-OBSERVED and
 * dumps per-CPU tick counts from sys_smp_stats — a frozen CPU's
 * tick count names the stuck core.
 *
 * Reproducer for the session-66/80 loopback hang. Run from the
 * in-guest shell:
 *   advent$ smp-hammer 1000     # fire 1000 requests
 *
 * Under -smp 1 every run completes in seconds. Under -smp 2 the
 * historical pattern was: works for the first few iterations,
 * then hangs partway through. With session-80's fix in place,
 * any N up to several thousand should complete cleanly.
 */
#include "libuser.h"

#define PORT          7000
#define STALL_S       5     /* seconds without progress = call it hung */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int connect_local(void) {
    int sk = sys_socket();
    if (sk < 0) return -1;
    unsigned char ip[4] = {127, 0, 0, 1};
    if (sys_connect(sk, ip, PORT) < 0) {
        sys_close(sk);
        return -1;
    }
    return sk;
}

/* Send "{...time...}\n", read one line back, return bytes read or
 * -1 on failure. Caller times each call externally. */
static int one_time_request(void) {
    int sk = connect_local();
    if (sk < 0) return -1;
    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"time\",\"params\":{}}\n";
    int n = slen(req);
    int sent = 0;
    while (sent < n) {
        int w = sys_write(sk, req + sent, n - sent);
        if (w <= 0) { sys_close(sk); return -1; }
        sent += w;
    }
    /* Read one line. Bound the buffer; agentd's `time` response is
     * small (~50 bytes). */
    char buf[256];
    int got = 0;
    while (got < (int)sizeof(buf) - 1) {
        char c;
        int r = sys_read(sk, &c, 1);
        if (r <= 0) break;
        if (c == '\n') { got++; break; }
        buf[got++] = c;
    }
    sys_close(sk);
    return got;
}

/* Print per-CPU tick counts. A frozen CPU's count won't advance
 * between two consecutive reads — that's the deadlocked core. */
static void dump_smp_stats(const char *label) {
    unsigned int out[8] = {0};
    sys_smp_stats(out);
    int n = (int)out[0];
    if (n > 7) n = 7;
    printf("[smp-stats %s] nr_cpus=%d ticks=", label, n);
    for (int i = 0; i < n; i++) {
        printf("%u%s", out[1 + i], i + 1 < n ? "," : "");
    }
    printf("\n");
}

int main(int argc, char **argv) {
    int total = 1000;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0) total = v;
    }
    if (total > 100000) total = 100000;

    printf("[smp-hammer] firing %d `time` requests to 127.0.0.1:7000\n",
           total);
    dump_smp_stats("start");

    uint32_t t0       = sys_time();
    uint32_t last_progress = t0;
    int      ok       = 0;
    int      fail     = 0;
    int      last_logged = 0;

    for (int i = 0; i < total; i++) {
        int n = one_time_request();
        if (n > 0) {
            ok++;
            last_progress = sys_time();
        } else {
            fail++;
            /* Stall detection: if `STALL_S` seconds pass with NO
             * successful request, we treat it as a hang.        */
            uint32_t now = sys_time();
            if (now - last_progress >= STALL_S) {
                printf("[smp-hammer] DEADLOCK-OBSERVED at request %d/%d\n",
                       i, total);
                printf("[smp-hammer] %d ok, %d fail, %u s elapsed, "
                       "%u s since last response\n",
                       ok, fail, now - t0, now - last_progress);
                dump_smp_stats("hang");
                /* Re-sample after a small pause: a frozen-CPU diagnosis
                 * needs two readings showing the same tick value. */
                sys_sleep_ms(500);
                dump_smp_stats("hang+500ms");
                return 2;
            }
        }
        /* Progress line every 200 requests for human visibility. */
        if (i - last_logged >= 200) {
            uint32_t now = sys_time();
            printf("[smp-hammer] %d/%d ok=%d fail=%d (%u s elapsed)\n",
                   i + 1, total, ok, fail, now - t0);
            last_logged = i;
        }
    }

    uint32_t t1 = sys_time();
    printf("[smp-hammer] done: %d/%d ok, %d fail, %u s wall\n",
           ok, total, fail, t1 - t0);
    dump_smp_stats("end");
    return (fail == 0) ? 0 : 1;
}
