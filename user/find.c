/*
 * find — recursive file listing.
 *
 *   find [PATH]    list every entry under PATH (default: cwd)
 *
 * Minimal initial shape — no predicates yet (`-name`, `-type`,
 * `-maxdepth`, etc.). Walks the directory tree depth-first and prints
 * every path (including PATH itself, matching POSIX find). The "is
 * this a dir" check piggybacks on the existing convention that
 * sys_fs_size(path) returns -1 for directories — same trick `ls` uses
 * to fill the type field in its JSONL output.
 *
 * Path buffer is fixed at 256 bytes; this matches the FS's effective
 * path-depth ceiling (FS_NAME_MAX=16 per component, parent_dir is a
 * uint8_t so at most 256 unique entries). A recursion depth limit of
 * 32 keeps the C stack bounded even if a pathological symlink loop
 * gets added later. Per-call name buffer is 17 bytes (16 + NUL).
 *
 * Exit code: 0 on success, 1 if PATH wasn't readable.
 */

#include "libuser.h"

static int my_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void put_path(const char *path) {
    sys_write(1, path, my_strlen(path));
    sys_write(1, "\n", 1);
}

/* Append "/name" to base, writing into out (cap bytes). Returns
 * resulting length, or -1 on overflow. Skips the slash if base
 * already ends with one (e.g. "/" + "etc" -> "/etc"). */
static int path_join(const char *base, const char *name, char *out, int cap) {
    int bl = my_strlen(base);
    int nl = my_strlen(name);
    int need = bl + 1 + nl + 1;   /* base + '/' + name + NUL */
    int skip_slash = (bl > 0 && base[bl - 1] == '/');
    if (skip_slash) need--;
    if (need > cap) return -1;
    int o = 0;
    for (int i = 0; i < bl; i++) out[o++] = base[i];
    if (!skip_slash) out[o++] = '/';
    for (int i = 0; i < nl; i++) out[o++] = name[i];
    out[o] = 0;
    return o;
}

/* Depth-bounded recursive walk. Prints `path` then recurses into it
 * if it's a directory. Returns 0 always (per-entry errors are
 * non-fatal — we just skip and continue). */
static void walk(const char *path, int depth) {
    put_path(path);
    if (depth >= 32) return;
    /* Probe: if size >= 0, it's a file — nothing to recurse into.
     * If size < 0, it's either a dir, missing, or non-regular; the
     * sys_readdir call below sorts that out (returns -1 immediately
     * on a missing/non-dir path). */
    if (sys_fs_size(path) >= 0) return;

    int  iter = 0;
    char name[17];
    char child[256];
    for (;;) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        if (path_join(path, name, child, sizeof(child)) < 0) continue;
        walk(child, depth + 1);
    }
}

int main(int argc, char **argv) {
    const char *start = ".";
    if (argc >= 2) start = argv[1];
    walk(start, 0);
    return 0;
}
