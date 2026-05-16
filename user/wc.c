/*
 * wc — count lines, words, and bytes.
 *
 *   echo hello world | wc      ->  1 2 12  (the 12th byte is the \n)
 *   wc /etc/inittab            ->  10 28 301 /etc/inittab
 *   wc --json /etc/inittab     ->  {"files":[{"path":"...","lines":N,...}]}
 *
 * Flags: -l (lines), -w (words), -c (bytes), --json (structured output).
 * Without -l/-w/-c the human output prints all three. --json always
 * emits all three counts per file regardless of which flags are set;
 * agents that want a subset can pick the keys they need.
 *
 * Word counting follows POSIX: a "word" is a maximal run of
 * non-whitespace characters. Whitespace is space / tab / newline.
 */

#include "libuser.h"
#include "../libjson/libjson.h"

static void count_fd(int fd, uint32_t *l, uint32_t *w, uint32_t *b) {
    char     buf[256];
    int      in_word = 0;
    int      n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            (*b)++;
            if (c == '\n')                                    (*l)++;
            if (c == ' ' || c == '\t' || c == '\n')           in_word = 0;
            else if (!in_word)                                { in_word = 1; (*w)++; }
        }
    }
}

static void emit(uint32_t l, uint32_t w, uint32_t b,
                 int show_l, int show_w, int show_c, const char *name) {
    int first = 1;
    if (show_l) { if (!first) putchar(' '); printf("%u", l); first = 0; }
    if (show_w) { if (!first) putchar(' '); printf("%u", w); first = 0; }
    if (show_c) { if (!first) putchar(' '); printf("%u", b); first = 0; }
    if (name && *name) { putchar(' '); puts(name); }
    else putchar('\n');
}

/* Session 82: JSONL emitter. One record per file. Multi-file
 * invocations append a `{"file":"TOTAL", ...}` aggregate matching
 * text-mode wc's `total` line.
 *
 * Field omission: when -l / -w / -c are passed, only the requested
 * counts populate the record. Un-requested fields are OMITTED
 * (not zeroed), so an agent doing `wc -l |> pluck lines` never
 * accidentally gets 0 from a `wc -w` invocation. This is the
 * "predictable failure mode" principle from the agent learning-
 * surface design: missing fields are diagnosable; silent zeros are
 * not. */
static void emit_wc_record_jsonl(const char *file_label,
                                 uint32_t l, uint32_t w, uint32_t b,
                                 int show_l, int show_w, int show_c) {
    char obuf[256];
    struct json_w jw;
    json_w_init(&jw, obuf, sizeof(obuf));
    json_obj_begin(&jw);
      json_key(&jw, "file"); json_str(&jw, file_label);
      if (show_l) { json_key(&jw, "lines"); json_uint(&jw, l); }
      if (show_w) { json_key(&jw, "words"); json_uint(&jw, w); }
      if (show_c) { json_key(&jw, "bytes"); json_uint(&jw, b); }
    json_obj_end(&jw);
    if (json_w_ok(&jw)) json_emit_line(&jw, 1);
}

int main(int argc, char **argv) {
    int show_l = 0, show_w = 0, show_c = 0;
    int json_mode = 0;
    int advjson   = 0;
    int argi   = 1;

    /* Parse flags. --json is a long flag, handled before short flags
     * (so `wc --json -l file` still works). Short flags may be
     * grouped (`-lw`). */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--json") == 0) {
            json_mode = 1;
            argi++;
            continue;
        }
        if (strcmp(argv[argi], "--advjson") == 0) {
            advjson = 1;
            argi++;
            continue;
        }
        if (argv[argi][1] == 0) break;     /* lone "-" is a filename (stdin) */
        for (int k = 1; argv[argi][k]; k++) {
            switch (argv[argi][k]) {
                case 'l': show_l = 1; break;
                case 'w': show_w = 1; break;
                case 'c': case 'm': show_c = 1; break;
                default:
                    sys_write(2, "wc: bad flag\n", 13);
                    return 2;
            }
        }
        argi++;
    }
    if (!show_l && !show_w && !show_c) {
        show_l = show_w = show_c = 1;
    }

    if (advjson) {
        if (argi >= argc) {
            uint32_t l = 0, w = 0, b = 0;
            count_fd(0, &l, &w, &b);
            emit_wc_record_jsonl("<stdin>", l, w, b, show_l, show_w, show_c);
            return 0;
        }
        uint32_t tl = 0, tw = 0, tb = 0;
        int nfiles = 0;
        for (int i = argi; i < argc; i++) {
            int fd = sys_open(argv[i]);
            if (fd < 0) {
                sys_write(2, "wc: ", 4);
                sys_write(2, argv[i], (int)strlen(argv[i]));
                sys_write(2, ": cannot open\n", 14);
                continue;
            }
            uint32_t l = 0, w = 0, b = 0;
            count_fd(fd, &l, &w, &b);
            sys_close(fd);
            emit_wc_record_jsonl(argv[i], l, w, b, show_l, show_w, show_c);
            tl += l; tw += w; tb += b;
            nfiles++;
        }
        if (nfiles > 1) {
            emit_wc_record_jsonl("TOTAL", tl, tw, tb, show_l, show_w, show_c);
        }
        return 0;
    }

    if (json_mode) {
        char obuf[2048];
        struct json_w jw;
        json_w_init(&jw, obuf, sizeof(obuf));
        json_obj_begin(&jw);
          json_key(&jw, "files");
          json_arr_begin(&jw);
          if (argi >= argc) {
              uint32_t l = 0, w = 0, b = 0;
              count_fd(0, &l, &w, &b);
              json_obj_begin(&jw);
                json_key(&jw, "path");  json_str(&jw, "-");
                json_key(&jw, "lines"); json_uint(&jw, l);
                json_key(&jw, "words"); json_uint(&jw, w);
                json_key(&jw, "bytes"); json_uint(&jw, b);
              json_obj_end(&jw);
          } else {
              uint32_t tl = 0, tw = 0, tb = 0; int nfiles = 0;
              for (int i = argi; i < argc; i++) {
                  int fd = sys_open(argv[i]);
                  if (fd < 0) {
                      json_obj_begin(&jw);
                        json_key(&jw, "path");  json_str(&jw, argv[i]);
                        json_key(&jw, "error"); json_str(&jw, "cannot open");
                      json_obj_end(&jw);
                      continue;
                  }
                  uint32_t l = 0, w = 0, b = 0;
                  count_fd(fd, &l, &w, &b);
                  sys_close(fd);
                  tl += l; tw += w; tb += b; nfiles++;
                  json_obj_begin(&jw);
                    json_key(&jw, "path");  json_str(&jw, argv[i]);
                    json_key(&jw, "lines"); json_uint(&jw, l);
                    json_key(&jw, "words"); json_uint(&jw, w);
                    json_key(&jw, "bytes"); json_uint(&jw, b);
                  json_obj_end(&jw);
              }
              if (nfiles > 1) {
                  /* Emit a total entry with path="total" so agents
                   * can find it positionally OR by name. */
                  json_obj_begin(&jw);
                    json_key(&jw, "path");  json_str(&jw, "total");
                    json_key(&jw, "lines"); json_uint(&jw, tl);
                    json_key(&jw, "words"); json_uint(&jw, tw);
                    json_key(&jw, "bytes"); json_uint(&jw, tb);
                  json_obj_end(&jw);
              }
          }
          json_arr_end(&jw);
        json_obj_end(&jw);
        if (!json_w_ok(&jw)) {
            sys_write(2, "wc: JSON buffer overflow\n", 25);
            return 1;
        }
        sys_write(1, obuf, json_w_len(&jw));
        sys_write(1, "\n", 1);
        return 0;
    }

    /* Human-readable path (original logic). */
    if (argi >= argc) {
        uint32_t l = 0, w = 0, b = 0;
        count_fd(0, &l, &w, &b);
        emit(l, w, b, show_l, show_w, show_c, "");
        return 0;
    }

    uint32_t tl = 0, tw = 0, tb = 0;
    int      nfiles = 0;
    for (int i = argi; i < argc; i++) {
        int fd = sys_open(argv[i]);
        if (fd < 0) {
            sys_write(2, "wc: ", 4);
            sys_write(2, argv[i], (int)strlen(argv[i]));
            sys_write(2, ": cannot open\n", 14);
            continue;
        }
        uint32_t l = 0, w = 0, b = 0;
        count_fd(fd, &l, &w, &b);
        sys_close(fd);
        emit(l, w, b, show_l, show_w, show_c, argv[i]);
        tl += l; tw += w; tb += b;
        nfiles++;
    }
    if (nfiles > 1) emit(tl, tw, tb, show_l, show_w, show_c, "total");
    return 0;
}
