/*
 * ps — list running processes, sourced from /proc.
 *
 *   ps                -> human-readable table
 *   ps --json         -> {"processes":[{"pid":N,"name":"...","state":"..."}]}
 *
 * The procfs layer (kernel/procfs.c) exposes one directory per live
 * pid; each has a `status` file. We readdir /proc, filter out the
 * non-numeric entries (cpuinfo, meminfo, etc.), then read each
 * /proc/<pid>/status into a small buffer and extract Name + State.
 *
 * No flags besides --json today. No -aux / -ef / TTY column — the
 * JSON shape is the contract agents target; humans get a compact
 * three-column view as a courtesy.
 */
#include "libuser.h"
#include "../libjson/libjson.h"

static int my_atoi_positive(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        if (n > 100000) return -1;
        n = n * 10 + (*s - '0');
        s++;
    }
    if (*s != 0) return -1;
    return n;
}

/* Read /proc/<pid>/status into buf. Returns bytes read or -1.
 * status format (see kernel/procfs.c::gen_status):
 *
 *   Name:    sh
 *   Pid:     5
 *   State:   running
 *   Pgid:    5
 *   Sid:     5
 *
 * We only need Name + State for the listing. */
static int read_status(int pid, char *buf, int cap) {
    /* Build "/proc/<pid>/status" without printf. */
    char path[64];
    int  o = 0;
    const char *prefix = "/proc/";
    for (int i = 0; prefix[i] && o < (int)sizeof(path) - 1; i++)
        path[o++] = prefix[i];
    /* Append decimal pid (digits in correct order). */
    char tmp[12]; int ti = 0;
    int p = pid;
    if (p == 0) tmp[ti++] = '0';
    while (p) { tmp[ti++] = (char)('0' + p % 10); p /= 10; }
    while (ti > 0 && o < (int)sizeof(path) - 1) path[o++] = tmp[--ti];
    /* Append "/status". */
    const char *suf = "/status";
    for (int i = 0; suf[i] && o < (int)sizeof(path) - 1; i++)
        path[o++] = suf[i];
    path[o] = 0;

    int fd = sys_open(path);
    if (fd < 0) return -1;
    int total = 0;
    int n;
    while (total < cap - 1 &&
           (n = sys_read(fd, buf + total, cap - 1 - total)) > 0) {
        total += n;
    }
    sys_close(fd);
    buf[total] = 0;
    return total;
}

/* Locate the line beginning with `prefix` and copy the trimmed value
 * into out. Returns the length copied. */
static int extract_field(const char *status, const char *prefix,
                         char *out, int cap) {
    int o = 0;
    int plen = (int)strlen(prefix);
    for (int i = 0; status[i]; ) {
        /* Match `prefix` at start of a line. */
        int j;
        for (j = 0; j < plen && status[i + j]; j++) {
            if (status[i + j] != prefix[j]) break;
        }
        if (j == plen) {
            /* Skip whitespace. */
            i += plen;
            while (status[i] == ' ' || status[i] == '\t') i++;
            while (status[i] && status[i] != '\n' && o < cap - 1) {
                out[o++] = status[i++];
            }
            out[o] = 0;
            return o;
        }
        /* Advance to next line. */
        while (status[i] && status[i] != '\n') i++;
        if (status[i] == '\n') i++;
    }
    out[0] = 0;
    return 0;
}

struct proc_row {
    int  pid;
    char name [32];
    char state[32];
};

#define MAX_ROWS 32

/* Walk /proc, fill rows[]. Returns the number filled. */
static int gather(struct proc_row *rows, int max_rows) {
    int n = 0;
    int iter = 0;
    char name[17];
    for (;;) {
        for (int i = 0; i < 17; i++) name[i] = 0;
        int idx = sys_readdir("/proc", &iter, name);
        if (idx < 0) break;

        /* Only numeric entries are pids. */
        int pid = my_atoi_positive(name);
        if (pid <= 0) continue;
        if (n >= max_rows) break;

        char status_buf[256];
        int  sz = read_status(pid, status_buf, sizeof(status_buf));
        if (sz <= 0) continue;

        rows[n].pid = pid;
        extract_field(status_buf, "Name:",  rows[n].name,  sizeof(rows[n].name));
        extract_field(status_buf, "State:", rows[n].state, sizeof(rows[n].state));
        n++;
    }
    return n;
}

/* Write decimal `n` right-justified into `out[0..width)`, space-padded.
 * Pre-condition: width small enough for the line buffer. */
static void emit_dec_padded(char *out, int width, int n) {
    char tmp[8];
    int  ti = 0;
    if (n == 0) tmp[ti++] = '0';
    while (n > 0) { tmp[ti++] = (char)('0' + n % 10); n /= 10; }
    /* Right-justify. */
    for (int i = 0; i < width; i++) out[i] = ' ';
    int k = width - 1;
    for (int i = 0; i < ti && k >= 0; i++) out[k--] = tmp[i];
}

static void print_human(const struct proc_row *rows, int n) {
    /* Three-column listing: PID (5), STATE (10), NAME. Hand-padded
     * because libuser's printf doesn't grok width specifiers. */
    sys_write(1, "  PID  STATE      NAME\n", 23);
    for (int i = 0; i < n; i++) {
        char line[80];
        int  o = 0;
        emit_dec_padded(line, 5, rows[i].pid);   o = 5;
        line[o++] = ' '; line[o++] = ' ';
        int sl = (int)strlen(rows[i].state);
        for (int k = 0; k < sl && k < 10; k++) line[o++] = rows[i].state[k];
        for (int k = sl; k < 10; k++)          line[o++] = ' ';
        line[o++] = ' ';
        int nl = (int)strlen(rows[i].name);
        for (int k = 0; k < nl && o < (int)sizeof(line) - 1; k++)
            line[o++] = rows[i].name[k];
        line[o++] = '\n';
        sys_write(1, line, o);
    }
}

static void print_json(const struct proc_row *rows, int n) {
    char buf[2048];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "processes");
      json_arr_begin(&w);
      for (int i = 0; i < n; i++) {
          json_obj_begin(&w);
            json_key(&w, "pid");   json_int(&w, rows[i].pid);
            json_key(&w, "name");  json_str(&w, rows[i].name);
            json_key(&w, "state"); json_str(&w, rows[i].state);
          json_obj_end(&w);
      }
      json_arr_end(&w);
    json_obj_end(&w);
    if (json_w_ok(&w)) {
        sys_write(1, buf, json_w_len(&w));
        sys_write(1, "\n", 1);
    } else {
        sys_write(2, "ps: JSON buffer overflow\n", 25);
    }
}

int main(int argc, char **argv) {
    int json_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json_mode = 1;
        else {
            sys_write(2, "ps: unknown flag\n", 17);
            return 2;
        }
    }

    static struct proc_row rows[MAX_ROWS];
    int n = gather(rows, MAX_ROWS);
    if (json_mode) print_json(rows, n);
    else           print_human(rows, n);
    return 0;
}
