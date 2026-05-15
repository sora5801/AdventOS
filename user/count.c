/*
 * count — consume all JSONL records from stdin, emit a single
 * record `{"count": N}` at EOF.
 *
 *   ls / |> count
 *   ps   |> where state=running |> count
 *
 * Counts lines, not characters or records — but since the JSONL
 * convention is one record per line, the two are equivalent. We
 * don't parse the records (no need); we just count newlines and
 * any non-empty trailing partial line.
 *
 * Output is itself a single JSONL record, so chaining still works:
 *   ls / |> count |> pluck count -> bare integer per "category".
 *
 * Session 81 — this replaces the old demo `count` (a 5-iteration
 * timer the original session-3 onboarding used). The demo's role
 * is filled by hello.elf / count is now structured-pipeline first
 * class.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

int main(int argc, char **argv) {
    /* --advjson is a no-op for us — we always emit JSONL. */
    (void)argc; (void)argv;

    long n = 0;
    char buf[1024];
    int  r;
    int  in_line = 0;     /* a partial trailing line (no \n) still counts */
    while ((r = sys_read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                if (in_line) n++;
                in_line = 0;
            } else {
                in_line = 1;
            }
        }
    }
    if (in_line) n++;

    char out[64];
    struct json_w w;
    json_w_init(&w, out, sizeof(out));
    json_obj_begin(&w);
      json_key(&w, "count"); json_int(&w, (int)n);
    json_obj_end(&w);
    json_emit_line(&w, 1);
    return 0;
}
