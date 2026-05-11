#include "dns.h"
#include "udp.h"
#include "pit.h"
#include "rtc.h"
#include "fs.h"
#include "string.h"
#include "kprintf.h"

#define DNS_PORT     53

#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1

#define DNS_RESPONSE_TIMEOUT_MS 2000

/* Session 60 — multiple DNS servers parsed from /etc/resolv.conf.
 * DHCP option 6 populates the FIRST slot; resolv.conf entries fill the
 * rest. dns_resolve walks the list and falls over to the next on
 * timeout. */
#define DNS_SERVERS_MAX  4
static struct ip_addr g_dns_servers[DNS_SERVERS_MAX];
static int            g_n_dns_servers;

/* TTL cache.  Tiny hand-rolled hashless table — for a single-user
 * hobby OS the workload is dozens of names not thousands.
 *
 * Invalidation: scan expired entries on every lookup miss.  No LRU —
 * full table just drops the next insert. */
#define DNS_CACHE_MAX  16
struct dns_cache_entry {
    int            in_use;
    char           name[64];
    struct ip_addr ip;
    uint32_t       expires_at;       /* sys_time epoch */
};
static struct dns_cache_entry g_dns_cache[DNS_CACHE_MAX];
static uint32_t g_dns_lookups, g_dns_hits, g_dns_misses;

struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

/* Volatile state for the synchronous wait. The UDP listener fills
 * g_resp_buf[] in IRQ context, then sets g_have_resp; the polling
 * caller spins on the flag. */
static volatile int g_have_resp;
static uint8_t      g_resp_buf[1500];
static int          g_resp_len;
static volatile uint16_t g_expecting_id;

void dns_init(void) {
    g_have_resp = 0;
    g_n_dns_servers = 0;
    /* DHCP fills g_dns_server (the legacy single global); promote it
     * into slot 0 of our new server array. */
    if (g_dns_server.b[0] || g_dns_server.b[1] ||
        g_dns_server.b[2] || g_dns_server.b[3]) {
        g_dns_servers[g_n_dns_servers++] = g_dns_server;
    }
    /* Listener runs on a high port — pick something in the ephemeral
     * range. (We don't have a port allocator yet.) */
    udp_listen(53000, NULL);     /* placeholder; real registration in resolve() */
}

/* Parse one line "nameserver A.B.C.D"; returns 1 if it added a server. */
static int parse_resolv_line(const char *line, int len) {
    /* Trim leading whitespace + skip comments. */
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] == '#') return 0;
    const char *kw = "nameserver";
    int kn = 10;
    if (len - i < kn) return 0;
    for (int j = 0; j < kn; j++) if (line[i + j] != kw[j]) return 0;
    i += kn;
    if (i >= len || (line[i] != ' ' && line[i] != '\t')) return 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    /* Parse dotted-quad. */
    struct ip_addr addr = {0};
    for (int oct = 0; oct < 4; oct++) {
        int v = 0;
        int saw = 0;
        while (i < len && line[i] >= '0' && line[i] <= '9') {
            v = v * 10 + (line[i] - '0');
            i++; saw = 1;
        }
        if (!saw || v > 255) return 0;
        addr.b[oct] = (uint8_t)v;
        if (oct < 3) {
            if (i >= len || line[i] != '.') return 0;
            i++;
        }
    }
    if (g_n_dns_servers >= DNS_SERVERS_MAX) return 0;
    /* Skip duplicates (DHCP may have already added the same address). */
    for (int s = 0; s < g_n_dns_servers; s++) {
        const struct ip_addr *e = &g_dns_servers[s];
        if (e->b[0] == addr.b[0] && e->b[1] == addr.b[1] &&
            e->b[2] == addr.b[2] && e->b[3] == addr.b[3]) return 0;
    }
    g_dns_servers[g_n_dns_servers++] = addr;
    return 1;
}

void dns_load_resolv_conf(void) {
    int fd = fs_open("/etc/resolv.conf");
    if (fd < 0) return;
    char buf[512];
    int n = fs_read(fd, 0, buf, (uint32_t)sizeof(buf));
    if (n <= 0) return;
    int added = 0;
    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            if (i > line_start) added += parse_resolv_line(buf + line_start, i - line_start);
            line_start = i + 1;
        }
    }
    if (added > 0) {
        kprintf("dns: loaded %d nameserver(s) from /etc/resolv.conf "
                "(total %d configured)\n", added, g_n_dns_servers);
    }
}

void dns_cache_stats(uint32_t out[4]) {
    out[0] = g_dns_lookups;
    out[1] = g_dns_hits;
    out[2] = g_dns_misses;
    /* Live entries: walk + count. */
    uint32_t now = rtc_epoch_corrected();
    int live = 0;
    for (int i = 0; i < DNS_CACHE_MAX; i++) {
        if (g_dns_cache[i].in_use && g_dns_cache[i].expires_at > now) live++;
    }
    out[3] = (uint32_t)live;
}

int dns_get_servers(struct ip_addr *out, int max) {
    int n = g_n_dns_servers < max ? g_n_dns_servers : max;
    for (int i = 0; i < n; i++) out[i] = g_dns_servers[i];
    return n;
}

static int name_eq(const char *a, const char *b) {
    /* Case-sensitive — DNS names are case-insensitive but real-world
     * lookups normalize to lowercase. We don't, accepting the
     * occasional cache miss across case variants. */
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == b[i];        /* both NUL → match */
}

static int cache_lookup(const char *name, struct ip_addr *out) {
    uint32_t now = rtc_epoch_corrected();
    for (int i = 0; i < DNS_CACHE_MAX; i++) {
        if (!g_dns_cache[i].in_use)            continue;
        if (g_dns_cache[i].expires_at <= now)  continue;     /* expired */
        if (!name_eq(g_dns_cache[i].name, name)) continue;
        *out = g_dns_cache[i].ip;
        return 1;
    }
    return 0;
}

static void cache_store(const char *name, const struct ip_addr *ip, uint32_t ttl) {
    /* Find a free slot or an expired one. */
    uint32_t now = rtc_epoch_corrected();
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_MAX; i++) {
        if (!g_dns_cache[i].in_use ||
             g_dns_cache[i].expires_at <= now) { slot = i; break; }
    }
    if (slot < 0) return;       /* table full — give up, no LRU */
    int j = 0;
    while (name[j] && j < (int)sizeof(g_dns_cache[slot].name) - 1) {
        g_dns_cache[slot].name[j] = name[j]; j++;
    }
    g_dns_cache[slot].name[j] = 0;
    g_dns_cache[slot].ip       = *ip;
    /* Cap TTL at 1 hour so a misbehaving auth server can't pin us
     * forever; floor at 30s so we don't thrash. */
    if (ttl < 30)    ttl = 30;
    if (ttl > 3600)  ttl = 3600;
    g_dns_cache[slot].expires_at = now + ttl;
    g_dns_cache[slot].in_use     = 1;
}

static void on_dns_reply(const struct ip_addr *src, uint16_t src_port,
                         const void *data, int len) {
    (void)src; (void)src_port;
    if (len < (int)sizeof(struct dns_hdr)) return;
    const struct dns_hdr *h = data;
    if (ntohs(h->id) != g_expecting_id)    return;
    if (len > (int)sizeof(g_resp_buf))     return;

    memcpy(g_resp_buf, data, (size_t)len);
    g_resp_len  = len;
    g_have_resp = 1;
}

/* Encode a hostname into the DNS wire format: each dot-separated
 * label is preceded by its 1-byte length; trailing zero terminator.
 * "google.com" -> [6]google[3]com[0]. Returns the encoded length, or
 * -1 if `dst` is too small. */
static int encode_name(const char *name, uint8_t *dst, int cap) {
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *start = p;
        while (*p && *p != '.') p++;
        int len = (int)(p - start);
        if (len == 0 || len > 63)         return -1;
        if (o + 1 + len + 1 > cap)        return -1;
        dst[o++] = (uint8_t)len;
        for (int i = 0; i < len; i++) dst[o++] = (uint8_t)start[i];
        if (*p == '.') p++;
    }
    if (o + 1 > cap) return -1;
    dst[o++] = 0;
    return o;
}

/* Skip past a name in a DNS message (handles 0xC0xx pointer
 * compression). Returns the number of bytes consumed at `at`, or -1
 * on parse error. */
static int skip_name(const uint8_t *msg, int msg_len, int at) {
    int p = at;
    while (p < msg_len) {
        uint8_t b = msg[p];
        if (b == 0)             { return p - at + 1; }
        if ((b & 0xC0) == 0xC0) { return p - at + 2; }
        if (p + 1 + b >= msg_len) return -1;
        p += 1 + b;
    }
    return -1;
}

int dns_resolve(const char *name, struct ip_addr *out) {
    if (!name || !out) return -1;
    g_dns_lookups++;

    /* Session 60 — cache lookup first.  Cache hits skip the entire
     * UDP round-trip. */
    if (cache_lookup(name, out)) {
        g_dns_hits++;
        return 0;
    }
    g_dns_misses++;

    /* If neither DHCP nor /etc/resolv.conf populated a server, give up. */
    if (g_n_dns_servers == 0) return -1;

    /* Build the query packet — same for every server. */
    uint8_t pkt[512];
    struct dns_hdr *h = (struct dns_hdr *)pkt;
    g_expecting_id = (uint16_t)(pit_ticks() & 0xFFFFu);
    if (g_expecting_id == 0) g_expecting_id = 1;
    h->id      = htons(g_expecting_id);
    h->flags   = htons(0x0100);          /* standard query, recursion desired */
    h->qdcount = htons(1);
    h->ancount = 0;
    h->nscount = 0;
    h->arcount = 0;

    int off = sizeof(*h);
    int n = encode_name(name, pkt + off, (int)sizeof(pkt) - off - 4);
    if (n < 0) return -1;
    off += n;

    /* QTYPE = A, QCLASS = IN. */
    pkt[off++] = 0; pkt[off++] = DNS_TYPE_A;
    pkt[off++] = 0; pkt[off++] = DNS_CLASS_IN;

    /* Walk each configured server in order until one responds.  This
     * is what /etc/resolv.conf's secondary nameserver line was always
     * for — DNS at the resolver level isn't redundant otherwise. */
    int got_response = 0;
    for (int sx = 0; sx < g_n_dns_servers && !got_response; sx++) {
        g_have_resp = 0;
        g_resp_len  = 0;
        udp_listen(53000, on_dns_reply);
        if (udp_send(&g_dns_servers[sx], 53000, DNS_PORT, pkt, off) < 0)
            continue;
        int waited = 0;
        while (!g_have_resp && waited < DNS_RESPONSE_TIMEOUT_MS) {
            pit_sleep(20);
            waited += 20;
        }
        if (g_have_resp) got_response = 1;
    }
    if (!got_response) return -1;

    /* Parse the response. Skip the header + question, then walk the
     * answer section looking for the first A record. */
    if (g_resp_len < (int)sizeof(struct dns_hdr)) return -1;
    const uint8_t *m  = g_resp_buf;
    int            mn = g_resp_len;
    int            p  = sizeof(struct dns_hdr);

    int qdcount = ntohs(h->qdcount);
    int ancount = ntohs(((const struct dns_hdr *)m)->ancount);
    if (qdcount != 1 || ancount < 1) return -1;

    /* Skip the question. */
    int sn = skip_name(m, mn, p);
    if (sn < 0) return -1;
    p += sn + 4;                           /* +qtype +qclass */
    if (p > mn) return -1;

    /* Walk answers. */
    for (int a = 0; a < ancount; a++) {
        sn = skip_name(m, mn, p);
        if (sn < 0)            return -1;
        p += sn;
        if (p + 10 > mn)       return -1;
        uint16_t type   = (uint16_t)((m[p] << 8) | m[p+1]);
        uint32_t ttl    = ((uint32_t)m[p+4] << 24) | ((uint32_t)m[p+5] << 16) |
                          ((uint32_t)m[p+6] <<  8) | (uint32_t)m[p+7];
        /* class @ p+2, ttl @ p+4..p+7, rdlength @ p+8..p+9 */
        uint16_t rdlen  = (uint16_t)((m[p+8] << 8) | m[p+9]);
        p += 10;
        if (p + rdlen > mn)    return -1;

        if (type == DNS_TYPE_A && rdlen == 4) {
            out->b[0] = m[p+0]; out->b[1] = m[p+1];
            out->b[2] = m[p+2]; out->b[3] = m[p+3];
            cache_store(name, out, ttl);
            return 0;
        }
        p += rdlen;
    }
    return -1;
}
