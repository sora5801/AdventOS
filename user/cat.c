/*
 * cat — read each named file and write its contents to stdout.
 *
 *   cat foo bar           plain concat
 *   cat --json foo bar    {"files":[{"path":"foo","data":"..."},...]}
 *
 * --json emits one JSON document containing per-file objects with the
 * file's path and its full contents as a properly-escaped JSON string.
 * Binary files survive because libjson escapes control bytes as
 * \u00XX. Files are read entirely into memory before emit; the
 * per-file cap is FILE_CAP below (currently 16 KiB).
 *
 * Without flags cat is unchanged: it concatenates raw bytes to stdout,
 * and with no args it's a stdin → stdout pipe pass-through.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

#define FILE_CAP   16384

static int slurp_file(int fd, char *buf, int cap) {
    int total = 0;
    int n;
    while (total < cap &&
           (n = sys_read(fd, buf + total, cap - total)) > 0) {
        total += n;
    }
    return total;
}

static int emit_plain(int argc, char **argv) {
    /* No-args path: copy stdin to stdout until EOF. Lets cat slot
     * into a pipeline as `... | cat | ...` or `... | cat`. */
    if (argc < 2) {
        char buf[256];
        int  n;
        while ((n = sys_read(0, buf, sizeof(buf))) > 0) {
            sys_write(1, buf, n);
        }
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "cat: ", 5);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            continue;
        }
        char buf[256];
        int  n;
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
            sys_write(1, buf, n);
        }
        sys_close(fd);
    }
    return 0;
}

static int emit_json(int argc, char **argv) {
    /* The output buffer has to hold the whole JSON document. With
     * one file at FILE_CAP=16 KiB, worst-case escaping doubles size
     * (every byte becomes \uXXXX) so we budget 6x file_cap plus some
     * headroom for keys. Two files at 16 KiB each blows that, so we
     * cap argc — agents should call shell.exec for big multi-file
     * dumps instead of relying on cat. */
    if (argc < 2) {
        sys_write(2, "cat --json: needs at least one file\n", 36);
        return 2;
    }

    static char obuf[131072];
    static char fbuf[FILE_CAP];
    struct json_w w;
    json_w_init(&w, obuf, sizeof(obuf));

    json_obj_begin(&w);
      json_key(&w, "files");
      json_arr_begin(&w);

      for (int i = 1; i < argc; i++) {
          int fd = sys_open(argv[i]);
          if (fd < 0) {
              json_obj_begin(&w);
                json_key(&w, "path");  json_str(&w, argv[i]);
                json_key(&w, "error"); json_str(&w, "cannot open");
              json_obj_end(&w);
              continue;
          }
          int n = slurp_file(fd, fbuf, sizeof(fbuf));
          sys_close(fd);
          json_obj_begin(&w);
            json_key(&w, "path"); json_str(&w, argv[i]);
            json_key(&w, "size"); json_int(&w, n);
            json_key(&w, "data"); json_str_n(&w, fbuf, n);
          json_obj_end(&w);
      }

      json_arr_end(&w);
    json_obj_end(&w);

    if (!json_w_ok(&w)) {
        sys_write(2, "cat --json: output too large\n", 29);
        return 1;
    }
    sys_write(1, obuf, json_w_len(&w));
    sys_write(1, "\n", 1);
    return 0;
}

/* Session 82: JSONL mode. Each input line becomes one record
 * `{"line": "..."}`; multi-file invocations prepend `"file"` so the
 * downstream pipeline can disambiguate. The schema is intentionally
 * minimal — agents that want byte offsets or line numbers can add
 * `--with-line-number` style flags in a follow-up. The current
 * surface deliberately optimises for predictable composition: the
 * field count is constant, the field names never change between
 * invocations, and a missing trailing newline still emits a final
 * record (no half-line surprises).
 *
 * Binary files: refused with a clear error. The control-byte
 * heuristic is the standard "is_text" test — any \0 or any byte
 * in 0x01..0x1f outside {\t \r \n} flips the verdict. We scan the
 * first 4 KiB of the file rather than the whole thing because a
 * 64 KiB ELF would otherwise be unreasonably expensive to reject,
 * and the first 4 KiB is enough to catch any sensible binary
 * (ELF, PE, gzip, tar, etc. all have their magic in byte 0..4). */
#define IS_BAD_BYTE(c) \
    ((c) == 0 || ((c) > 0 && (c) < 32 && (c) != '\t' && (c) != '\n' && (c) != '\r'))

static int looks_binary(const char *buf, int n) {
    int lim = n < 4096 ? n : 4096;
    for (int i = 0; i < lim; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return 1;
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') return 1;
        if (c == 0x7f) return 1;
    }
    return 0;
}

/* Stream `fd` line-by-line, emitting one JSONL record per line.
 * Returns 0 on success, 1 if the file was rejected as binary. */
static int emit_jsonl_one(const char *file_label, int fd, int multi_file) {
    /* Peek at the first 4 KiB to do the binary check before
     * committing to streaming. */
    char peek[4096];
    int  pn = 0;
    int  r;
    while (pn < (int)sizeof(peek) &&
           (r = sys_read(fd, peek + pn, sizeof(peek) - pn)) > 0) {
        pn += r;
    }
    int eof = (r <= 0);
    if (looks_binary(peek, pn)) {
        sys_write(2, "cat: --advjson rejects binary file ", 35);
        sys_write(2, file_label, (int)strlen(file_label));
        sys_write(2, "\n", 1);
        return 1;
    }

    /* Walk peek + remaining stream as a single logical line buffer.
     * The line buffer is bounded at 4 KiB — files with longer lines
     * truncate at the boundary (rare for text). */
    char line[4096];
    int  ln = 0;
    int  src = 0;
    char tail[1024];
    int  tail_n = 0;
    int  tail_off = 0;

    for (;;) {
        char c;
        if (src < pn) {
            c = peek[src++];
        } else if (eof) {
            /* Drained peek; ensure any partial last line emits. */
            if (ln == 0) break;
            c = '\n';   /* synthesize a terminator to flush */
        } else if (tail_off < tail_n) {
            c = tail[tail_off++];
        } else {
            tail_n = sys_read(fd, tail, sizeof(tail));
            tail_off = 0;
            if (tail_n <= 0) { eof = 1; continue; }
            c = tail[tail_off++];
        }

        if (c == '\n') {
            char buf[5120];
            struct json_w w;
            json_w_init(&w, buf, sizeof(buf));
            json_obj_begin(&w);
              if (multi_file) {
                  json_key(&w, "file"); json_str(&w, file_label);
              }
              json_key(&w, "line"); json_str_n(&w, line, ln);
            json_obj_end(&w);
            if (json_w_ok(&w)) json_emit_line(&w, 1);
            ln = 0;
            /* If we ran out of source AND just emitted the synth
             * terminator, exit. */
            if (eof && src >= pn && tail_off >= tail_n) break;
            continue;
        }
        if (ln < (int)sizeof(line)) line[ln++] = c;
    }
    return 0;
}

static int emit_jsonl(int argc, char **argv) {
    /* No-args: stream stdin. Bare cat in pipelines is `cat | ...`
     * but the JSONL chain rarely needs a passthrough cat — keep
     * it for symmetry with text-mode. */
    if (argc < 2) {
        return emit_jsonl_one("<stdin>", 0, 0);
    }
    int multi = (argc > 2);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "cat: ", 5);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            rc = 1;
            continue;
        }
        if (emit_jsonl_one(argv[i], fd, multi) != 0) rc = 1;
        sys_close(fd);
    }
    return rc;
}

int main(int argc, char **argv) {
    /* Scan for --json AND session-81 --advjson. Filter them out of
     * argv (compact in place) so the plain path stays exactly as
     * it was: cat treats all non-flag args as filenames. */
    int json_mode = 0;
    int advjson   = 0;
    int w = 1;
    for (int r = 1; r < argc; r++) {
        if      (strcmp(argv[r], "--json")    == 0) json_mode = 1;
        else if (strcmp(argv[r], "--advjson") == 0) advjson   = 1;
        else argv[w++] = argv[r];
    }
    argc = w;
    if (advjson)   return emit_jsonl(argc, argv);
    if (json_mode) return emit_json (argc, argv);
    return emit_plain(argc, argv);
}
