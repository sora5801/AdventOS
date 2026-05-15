/*
 * ls — list directory entries.
 *
 *   ls                -> contents of cwd, one per line
 *   ls /etc           -> contents of /etc
 *   ls --json         -> {"path":"...","entries":[{...}]}
 *   ls /etc | wc -l   -> count entries via pipeline
 *
 * --json adds mode + (uid, gid) per entry; humans only see the name.
 * AdventFS doesn't store a creation/size that's cheap to read without
 * an open()+stat(), so size is intentionally omitted.
 *
 * Why a binary AND a shell builtin? Shell builtins can't appear in
 * pipelines (they'd run inline in the shell instead of in a forked
 * child). The builtin is for snappy interactive use; this binary is
 * what `ls /etc | wc -l` actually exec()s into.
 */

#include "libuser.h"
#include "../libjson/libjson.h"

/* Build "<dir>/<name>" into out. Handles dir=="/", dir=="", trailing
 * slash. Returns 0 on success, -1 on overflow. */
static int join_path(const char *dir, const char *name, char *out, int cap) {
    int o = 0;
    if (dir && dir[0] && !(dir[0] == '.' && dir[1] == 0)) {
        for (int i = 0; dir[i]; i++) {
            if (o >= cap - 2) return -1;
            out[o++] = dir[i];
        }
        if (o > 0 && out[o - 1] != '/') {
            if (o >= cap - 1) return -1;
            out[o++] = '/';
        }
    }
    for (int i = 0; name[i]; i++) {
        if (o >= cap - 1) return -1;
        out[o++] = name[i];
    }
    out[o] = 0;
    return 0;
}

static int emit_human(const char *path) {
    int  iter = 0;
    char name[17];
    int  shown = 0;
    for (;;) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;
        sys_write(1, name, (int)strlen(name));
        sys_write(1, "\n", 1);
        shown++;
    }
    if (shown == 0) {
        sys_write(2, "ls: empty or no such directory\n", 31);
        return 1;
    }
    return 0;
}

static int emit_json(const char *path) {
    char buf[2048];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));

    json_obj_begin(&w);
      json_key(&w, "path"); json_str(&w, path);
      json_key(&w, "entries");
      json_arr_begin(&w);

        int  iter = 0;
        char name[17];
        int  shown = 0;
        for (;;) {
            for (int i = 0; i < 17; i++) name[i] = 0;
            int idx = sys_readdir(path, &iter, name);
            if (idx < 0) break;

            /* Look up mode + owner via the full path (the kernel
             * needs an absolute or cwd-relative path; we built `path`
             * to be absolute or "."). */
            char full[80];
            int  has_meta = 0, mode = 0, uid = 0, gid = 0;
            if (join_path(path, name, full, sizeof(full)) == 0) {
                mode = sys_fs_mode(full);
                int owner = sys_fs_owner(full);
                if (mode >= 0 && owner >= 0) {
                    has_meta = 1;
                    uid = (owner >> 16) & 0xFFFF;
                    gid = owner & 0xFFFF;
                }
            }

            json_obj_begin(&w);
              json_key(&w, "name"); json_str(&w, name);
              if (has_meta) {
                  json_key(&w, "mode"); json_int(&w, mode);
                  json_key(&w, "uid");  json_int(&w, uid);
                  json_key(&w, "gid");  json_int(&w, gid);
              }
            json_obj_end(&w);
            shown++;
        }
        if (shown == 0 && !json_w_ok(&w)) {
            /* Even an empty directory is a successful listing; only
             * a truly bad path produces no result and no entries. The
             * agent can distinguish by reading the array length. */
        }
      json_arr_end(&w);
    json_obj_end(&w);

    if (!json_w_ok(&w)) {
        sys_write(2, "ls: JSON buffer overflow\n", 25);
        return 1;
    }
    sys_write(1, buf, json_w_len(&w));
    sys_write(1, "\n", 1);
    return 0;
}

/* Session 81: JSONL emitter — one record per entry, each a self-
 * contained `{"name":..., "type":..., "size":..., "perm":..., ...}`
 * line. Used by `|>` pipelines. Distinct from --json (the legacy
 * mode that wraps everything in a single {path, entries[...]}
 * object) because line-oriented JSONL streams through tools like
 * pluck/where/count without buffering. */
static int emit_jsonl(const char *path) {
    int  iter = 0;
    char name[17];
    for (;;) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir(path, &iter, name);
        if (idx < 0) break;

        char full[80];
        int  mode = -1, owner = -1, size = -1;
        if (join_path(path, name, full, sizeof(full)) == 0) {
            mode  = sys_fs_mode(full);
            owner = sys_fs_owner(full);
            size  = sys_fs_size(full);
        }

        char buf[256];
        struct json_w w;
        json_w_init(&w, buf, sizeof(buf));
        json_obj_begin(&w);
          json_key(&w, "name"); json_str(&w, name);
          /* `type`: FILE / DIR. sys_fs_size returns -1 for dirs;
           * we treat that as the directory marker. (mode's high
           * bits could carry the type if the FS exposed it, but
           * AdventFS's mode is permission bits only today.) */
          json_key(&w, "type"); json_str(&w, size < 0 ? "DIR" : "FILE");
          json_key(&w, "size"); json_int(&w, size < 0 ? 0 : size);
          if (mode >= 0)  { json_key(&w, "perm"); json_int(&w, mode & 0777); }
          if (owner >= 0) {
              json_key(&w, "uid"); json_int(&w, (owner >> 16) & 0xFFFF);
              json_key(&w, "gid"); json_int(&w, owner & 0xFFFF);
          }
        json_obj_end(&w);
        if (!json_w_ok(&w)) {
            sys_write(2, "ls: JSONL record overflow\n", 26);
            return 1;
        }
        if (json_emit_line(&w, 1) < 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int json_mode = 0;      /* legacy single-object --json */
    int advjson   = 0;      /* session 81: JSONL stream */
    const char *path = ".";
    char  cwd_buf[128];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json")    == 0) json_mode = 1;
        else if (strcmp(argv[i], "--advjson") == 0) advjson = 1;
        else path = argv[i];
    }

    if (path[0] == '.' && path[1] == 0) {
        if (sys_getcwd(cwd_buf, sizeof(cwd_buf)) < 0) {
            sys_write(2, "ls: getcwd failed\n", 18);
            return 1;
        }
        path = cwd_buf;
    }

    if (advjson)   return emit_jsonl(path);
    if (json_mode) return emit_json(path);
    return emit_human(path);
}
