/*
 * kvctl — command-line interface to the session-73 KV store.
 *
 * Lets you exercise the agentd-level kv.* tools without needing to
 * pipe long JSON-RPC requests through nc. Uses the same libuser
 * kv_* helpers that agentd uses internally, so the syscall path is
 * identical — this is a thin CLI veneer for human-friendly testing.
 *
 * Usage:
 *   kvctl get  NAMESPACE KEY
 *   kvctl put  NAMESPACE KEY VALUE
 *   kvctl del  NAMESPACE KEY
 *   kvctl list NAMESPACE [PREFIX]
 *   kvctl stat NAMESPACE KEY
 *
 * NAMESPACE / KEY constraints (enforced by libuser):
 *   namespace  1..32 chars, [A-Za-z0-9_-]
 *   key        1..64 chars, [A-Za-z0-9_.-], no leading dot, no '/'
 *   value      bytes; if you need spaces or shell metacharacters,
 *              quote the whole VALUE argument
 *
 * Exit codes:
 *   0  success (kv.get printed the value, kv.put / kv.del / etc.
 *      reported ok)
 *   1  validation / usage error (bad subcommand, missing args)
 *   2  KV operation failed (returned -1 from the underlying call)
 *
 * Session 73 followup: this binary exists specifically because the
 * paste-into-shell path is unreliable under -serial stdio on
 * Windows/MSYS2, so the host-side nc-into-agentd path is the only
 * way agents have to reach the KV store remotely. From the in-guest
 * shell, kvctl is the path that doesn't require pasting JSON.
 */

#include "libuser.h"

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int slen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void usage(void) {
    puts("usage:\n"
         "  kvctl get  NAMESPACE KEY\n"
         "  kvctl put  NAMESPACE KEY VALUE\n"
         "  kvctl del  NAMESPACE KEY\n"
         "  kvctl list NAMESPACE [PREFIX]\n"
         "  kvctl stat NAMESPACE KEY\n");
}

#define VBUF_MAX 4096

static int cmd_get(int argc, char **argv) {
    if (argc != 4) { usage(); return 1; }
    static char vbuf[VBUF_MAX];
    int n = kv_get(argv[2], argv[3], vbuf, sizeof(vbuf));
    if (n < 0) {
        printf("kvctl: get failed (no such key, or validation error)\n");
        return 2;
    }
    /* Write raw bytes — no implicit newline. Mirrors `cat` semantics. */
    sys_write(1, vbuf, n);
    sys_write(1, "\n", 1);
    return 0;
}

static int cmd_put(int argc, char **argv) {
    if (argc != 5) { usage(); return 1; }
    int vlen = slen(argv[4]);
    int rc = kv_put(argv[2], argv[3], argv[4], vlen);
    if (rc < 0) {
        printf("kvctl: put failed (validation error or FS write failure)\n");
        printf("       check: namespace and key only contain "
               "[A-Za-z0-9_.-] (no slashes or leading dots)\n");
        return 2;
    }
    printf("ok\n");
    return 0;
}

static int cmd_del(int argc, char **argv) {
    if (argc != 4) { usage(); return 1; }
    int rc = kv_del(argv[2], argv[3]);
    if (rc < 0) {
        printf("kvctl: del failed (no such key, or validation error)\n");
        return 2;
    }
    printf("ok\n");
    return 0;
}

static int cmd_list(int argc, char **argv) {
    if (argc < 3 || argc > 4) { usage(); return 1; }
    const char *prefix = (argc == 4) ? argv[3] : 0;
    int  iter = 0;
    char name[16];
    int  count = 0;
    for (int safety = 0; safety < 256; safety++) {
        int r = kv_list(argv[2], prefix, &iter, name);
        if (r < 0) break;
        printf("%s\n", name);
        count++;
    }
    if (count == 0) printf("(no keys)\n");
    return 0;
}

static int cmd_stat(int argc, char **argv) {
    if (argc != 4) { usage(); return 1; }
    int size = 0;
    int ok   = (kv_stat(argv[2], argv[3], &size) == 0);
    printf("exists: %s\n", ok ? "true" : "false");
    printf("size:   %d\n", size);
    return ok ? 0 : 2;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (streq(argv[1], "get"))   return cmd_get (argc, argv);
    if (streq(argv[1], "put"))   return cmd_put (argc, argv);
    if (streq(argv[1], "del"))   return cmd_del (argc, argv);
    if (streq(argv[1], "list"))  return cmd_list(argc, argv);
    if (streq(argv[1], "stat"))  return cmd_stat(argc, argv);
    printf("kvctl: unknown subcommand '%s'\n", argv[1]);
    usage();
    return 1;
}
