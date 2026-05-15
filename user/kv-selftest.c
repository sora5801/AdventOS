/*
 * kv-selftest — verifies the session-73 KV store at the libuser
 * boundary.  Talks to /var/kv/<ns>/<key> via the kv_* wrappers, so
 * it exercises the same code path agentd's kv.* tools use.
 *
 * Checks:
 *   1. kv_put -> kv_get round-trip
 *   2. kv_list enumerates a fresh namespace
 *   3. kv_del removes the entry
 *   4. kv_get after del returns -1 (not found)
 *   5. kv_put with a key containing "/" rejected (path traversal)
 *   6. kv_put with a namespace containing "." rejected (traversal)
 *   7. kv_put with len > 65536 rejected (max-size enforcement)
 *   8. Empty value (len=0) round-trips: kv_get returns 0 (found,
 *      empty) — distinguishable from -1 (missing)
 *   9. kv_stat on missing key returns -1 with size=0
 *
 * Uses namespace "selftest" so it doesn't collide with anything an
 * agent might have put in production namespaces. Each run starts by
 * deleting any prior state in "selftest" before running.
 */

#include "libuser.h"

static int g_fail;

static void expect(int cond, const char *what) {
    if (cond) printf("  PASS  %s\n", what);
    else      { printf("  FAIL  %s\n", what); g_fail++; }
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int streq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[kv] selftest — session 73 + 75\n");

    /* Clean slate for the "selftest" namespace. Ignore errors. */
    (void)kv_del("selftest", "hello");
    (void)kv_del("selftest", "empty");

    /* === 1. put/get round-trip === */
    int rc = kv_put("selftest", "hello", "world", 5);
    expect(rc == 0, "kv_put(selftest, hello, world) returns 0");

    char buf[32];
    int n = kv_get("selftest", "hello", buf, sizeof(buf));
    expect(n == 5, "kv_get returns 5 bytes");
    expect(n == 5 && streq(buf, "world", 5), "kv_get content == \"world\"");

    /* === 2. kv_list enumerates the namespace === */
    int  iter = 0;
    char name[16];
    int  found_hello = 0;
    for (int safety = 0; safety < 32; safety++) {
        int r = kv_list("selftest", 0, &iter, name);
        if (r < 0) break;
        int nl = slen(name);
        if (nl == 5 && streq(name, "hello", 5)) { found_hello = 1; break; }
    }
    expect(found_hello, "kv_list enumerates \"hello\"");

    /* === 3. delete === */
    rc = kv_del("selftest", "hello");
    expect(rc == 0, "kv_del returns 0");

    /* === 4. get after delete returns -1 === */
    n = kv_get("selftest", "hello", buf, sizeof(buf));
    expect(n < 0, "kv_get after del returns -1 (missing)");

    /* === 5. key with '/' rejected === */
    rc = kv_put("selftest", "../escape", "x", 1);
    expect(rc < 0, "kv_put(key=\"../escape\") rejected");

    /* === 6. namespace with '.' rejected === */
    rc = kv_put("..", "key", "x", 1);
    expect(rc < 0, "kv_put(ns=\"..\") rejected");

    /* === 7. oversize value rejected === */
    {
        /* Build a placeholder pointer + huge size — libuser's validator
         * inspects len BEFORE touching the buffer, so we can pass a
         * small buffer with a lying len and the call returns -1
         * cleanly without ever reading past the end. */
        char tiny[8] = "abcdefg";
        rc = kv_put("selftest", "big", tiny, 65537);
        expect(rc < 0, "kv_put(len=65537) rejected (max 65536)");
    }

    /* === 8. empty-value round-trip === */
    rc = kv_put("selftest", "empty", "", 0);
    expect(rc == 0, "kv_put(len=0) accepted");
    n = kv_get("selftest", "empty", buf, sizeof(buf));
    expect(n == 0, "kv_get(empty) returns 0 (found, empty) — not -1");
    /* clean up */
    (void)kv_del("selftest", "empty");

    /* === 9. stat on missing key === */
    int sz = 999;     /* sentinel — should be overwritten to 0 */
    rc = kv_stat("selftest", "nope", &sz);
    expect(rc < 0, "kv_stat(missing) returns -1");
    expect(sz == 0,  "kv_stat(missing) writes size=0");

    if (g_fail == 0) { printf("[kv] selftest: all checks PASS\n"); return 0; }
    printf("[kv] selftest: %d FAILURE(S)\n", g_fail);
    return 1;
}
