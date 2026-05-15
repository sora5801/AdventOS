/*
 * date — print the current time as YYYY-MM-DD HH:MM:SS UTC.
 *
 *   date                  -> 2026-05-09 07:48:02 UTC
 *   date --json           -> {"epoch":N,"iso":"YYYY-MM-DDTHH:MM:SSZ","utc":"..."}
 *
 * Reads the kernel's UNIX epoch via SYS_TIME (seconds since
 * 1970-01-01 00:00:00 UTC) and converts to a broken-down calendar
 * date. No timezones, no locales — UTC always. No `+FORMAT` arg.
 *
 * The conversion is hand-rolled because libuser doesn't have a
 * gmtime equivalent. The algorithm is the same one Howard Hinnant
 * documented for civil_from_days — works for any year >= 1970 and
 * has no leap-second / leap-year corner cases beyond the standard
 * Gregorian rules.
 */

#include "libuser.h"
#include "../libjson/libjson.h"

static const int days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

struct civil {
    int year, mon, dom;
    int hh, mm, ss;
};

static struct civil break_down(uint32_t t) {
    struct civil c = {0};
    uint32_t day = t / 86400;
    uint32_t sec = t % 86400;

    c.year = 1970;
    while (1) {
        int yd = is_leap(c.year) ? 366 : 365;
        if (day < (uint32_t)yd) break;
        day -= yd;
        c.year++;
    }
    c.mon = 0;
    while (1) {
        int md = days_in_month[c.mon];
        if (c.mon == 1 && is_leap(c.year)) md = 29;
        if (day < (uint32_t)md) break;
        day -= md;
        c.mon++;
    }
    c.dom = (int)day + 1;
    c.hh  = (int)(sec / 3600);
    c.mm  = (int)((sec % 3600) / 60);
    c.ss  = (int)(sec % 60);
    return c;
}

/* Build "YYYY-MM-DD<sep>HH:MM:SS<tail>" into buf. tail may be empty.
 * Returns bytes written. */
static int format_civil(const struct civil *c, char sep, const char *tail,
                        char *buf, int cap) {
    int o = 0;
    int yy = c->year;
    char ys[6]; int yi = 0;
    if (yy == 0) ys[yi++] = '0';
    while (yy) { ys[yi++] = (char)('0' + yy % 10); yy /= 10; }
    while (yi--) if (o < cap) buf[o++] = ys[yi];
    if (o < cap) buf[o++] = '-';
    if (o + 2 <= cap) {
        int v = c->mon + 1;
        buf[o++] = (char)('0' + v / 10); buf[o++] = (char)('0' + v % 10);
    }
    if (o < cap) buf[o++] = '-';
    if (o + 2 <= cap) {
        buf[o++] = (char)('0' + c->dom / 10); buf[o++] = (char)('0' + c->dom % 10);
    }
    if (o < cap) buf[o++] = sep;
    if (o + 2 <= cap) { buf[o++] = (char)('0' + c->hh / 10); buf[o++] = (char)('0' + c->hh % 10); }
    if (o < cap) buf[o++] = ':';
    if (o + 2 <= cap) { buf[o++] = (char)('0' + c->mm / 10); buf[o++] = (char)('0' + c->mm % 10); }
    if (o < cap) buf[o++] = ':';
    if (o + 2 <= cap) { buf[o++] = (char)('0' + c->ss / 10); buf[o++] = (char)('0' + c->ss % 10); }
    if (tail) {
        for (int i = 0; tail[i] && o < cap; i++) buf[o++] = tail[i];
    }
    return o;
}

/* Session 81: JSONL emitter. date emits a single record per
 * invocation — there's only one current time. The pipeline-spec
 * schema is {iso, unix, year, month, day, hour, min, sec, tz}. */
static int emit_jsonl(uint32_t t, const struct civil *c) {
    char iso[24];
    int  iso_n = format_civil(c, 'T', "Z", iso, sizeof(iso));
    char buf[256];
    struct json_w w;
    json_w_init(&w, buf, sizeof(buf));
    json_obj_begin(&w);
      json_key(&w, "iso");   json_str_n(&w, iso, iso_n);
      json_key(&w, "unix");  json_uint(&w, t);
      json_key(&w, "year");  json_int(&w, c->year);
      json_key(&w, "month"); json_int(&w, c->mon + 1);
      json_key(&w, "day");   json_int(&w, c->dom);
      json_key(&w, "hour");  json_int(&w, c->hh);
      json_key(&w, "min");   json_int(&w, c->mm);
      json_key(&w, "sec");   json_int(&w, c->ss);
      json_key(&w, "tz");    json_str(&w, "UTC");
    json_obj_end(&w);
    if (!json_w_ok(&w)) {
        sys_write(2, "date: JSONL overflow\n", 21);
        return 1;
    }
    json_emit_line(&w, 1);
    return 0;
}

int main(int argc, char **argv) {
    int json_mode = 0;
    int advjson   = 0;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--json")    == 0) json_mode = 1;
        else if (strcmp(argv[i], "--advjson") == 0) advjson = 1;
    }

    uint32_t t = sys_time();
    struct civil c = break_down(t);

    if (advjson) return emit_jsonl(t, &c);

    if (json_mode) {
        char iso[24], utc[32];
        int  iso_n = format_civil(&c, 'T', "Z",      iso, sizeof(iso));
        int  utc_n = format_civil(&c, ' ', " UTC",   utc, sizeof(utc));
        char buf[160];
        struct json_w w;
        json_w_init(&w, buf, sizeof(buf));
        json_obj_begin(&w);
          json_key(&w, "epoch"); json_uint(&w, t);
          json_key(&w, "iso");   json_str_n(&w, iso, iso_n);
          json_key(&w, "utc");   json_str_n(&w, utc, utc_n);
        json_obj_end(&w);
        if (!json_w_ok(&w)) {
            sys_write(2, "date: JSON overflow\n", 20);
            return 1;
        }
        sys_write(1, buf, json_w_len(&w));
        sys_write(1, "\n", 1);
        return 0;
    }

    char out[32];
    int  n = format_civil(&c, ' ', " UTC\n", out, sizeof(out));
    sys_write(1, out, n);
    return 0;
}
