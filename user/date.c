/*
 * date — print the current time as YYYY-MM-DD HH:MM:SS UTC.
 *
 * Reads the kernel's UNIX epoch via SYS_TIME (seconds since
 * 1970-01-01 00:00:00 UTC) and converts to a broken-down calendar
 * date. No timezones, no locales — UTC always. No `+FORMAT` arg.
 *
 *   date                  -> 2026-05-09 07:48:02 UTC
 *
 * The conversion is hand-rolled because libuser doesn't have a
 * gmtime equivalent. The algorithm is the same one Howard Hinnant
 * documented for civil_from_days — works for any year >= 1970 and
 * has no leap-second / leap-year corner cases beyond the standard
 * Gregorian rules.
 */

#include "libuser.h"

static const int days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint32_t t   = sys_time();
    uint32_t day = t / 86400;
    uint32_t sec = t % 86400;

    int year = 1970;
    while (1) {
        int yd = is_leap(year) ? 366 : 365;
        if (day < (uint32_t)yd) break;
        day -= yd;
        year++;
    }

    int mon = 0;
    while (1) {
        int md = days_in_month[mon];
        if (mon == 1 && is_leap(year)) md = 29;
        if (day < (uint32_t)md) break;
        day -= md;
        mon++;
    }

    int dom = (int)day + 1;
    int hh  = (int)(sec / 3600);
    int mm  = (int)((sec % 3600) / 60);
    int ss  = (int)(sec % 60);

    /* libuser printf doesn't grok %02d; pad by hand. */
    char buf[32];
    int  o = 0;
    int  yy = year;
    char ys[6];
    int  yi = 0;
    if (yy == 0) ys[yi++] = '0';
    while (yy) { ys[yi++] = (char)('0' + yy % 10); yy /= 10; }
    while (yi--) buf[o++] = ys[yi];
    buf[o++] = '-';
    int v = mon + 1;
    buf[o++] = (char)('0' + v / 10); buf[o++] = (char)('0' + v % 10);
    buf[o++] = '-';
    buf[o++] = (char)('0' + dom / 10); buf[o++] = (char)('0' + dom % 10);
    buf[o++] = ' ';
    buf[o++] = (char)('0' + hh / 10);  buf[o++] = (char)('0' + hh  % 10);
    buf[o++] = ':';
    buf[o++] = (char)('0' + mm / 10);  buf[o++] = (char)('0' + mm  % 10);
    buf[o++] = ':';
    buf[o++] = (char)('0' + ss / 10);  buf[o++] = (char)('0' + ss  % 10);
    buf[o++] = ' '; buf[o++] = 'U'; buf[o++] = 'T'; buf[o++] = 'C'; buf[o++] = '\n';
    sys_write(1, buf, o);
    return 0;
}
