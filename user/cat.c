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

int main(int argc, char **argv) {
    /* Scan for --json. Filter it out of argv (compact in place) so
     * the plain path stays exactly as it was: cat treats all non-
     * flag args as filenames. */
    int json_mode = 0;
    int w = 1;
    for (int r = 1; r < argc; r++) {
        if (strcmp(argv[r], "--json") == 0) json_mode = 1;
        else argv[w++] = argv[r];
    }
    argc = w;
    return json_mode ? emit_json(argc, argv) : emit_plain(argc, argv);
}
