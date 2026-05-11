#include "rtc.h"
#include "../include/io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define CMOS_REG_SECONDS  0x00
#define CMOS_REG_MINUTES  0x02
#define CMOS_REG_HOURS    0x04
#define CMOS_REG_DAY      0x07
#define CMOS_REG_MONTH    0x08
#define CMOS_REG_YEAR     0x09
#define CMOS_REG_CENTURY  0x32
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

#define STATUS_A_UIP     0x80
#define STATUS_B_BIN     0x04   /* 1 = binary, 0 = BCD */
#define STATUS_B_24H     0x02
#define HOUR_PM          0x80

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int rtc_updating(void) {
    return cmos_read(CMOS_REG_STATUS_A) & STATUS_A_UIP;
}

static uint8_t bcd2bin(uint8_t v) {
    return (uint8_t)((v >> 4) * 10u + (v & 0x0F));
}

void rtc_read(struct rtc_time *t) {
    /* Read everything twice and only accept a result when two reads in
     * a row agree — guards against the RTC ticking mid-read. */
    uint8_t s, m, h, d, mo, y, c, status_b;
    uint8_t s2, m2, h2, d2, mo2, y2, c2;

    do {
        while (rtc_updating()) {}
        s   = cmos_read(CMOS_REG_SECONDS);
        m   = cmos_read(CMOS_REG_MINUTES);
        h   = cmos_read(CMOS_REG_HOURS);
        d   = cmos_read(CMOS_REG_DAY);
        mo  = cmos_read(CMOS_REG_MONTH);
        y   = cmos_read(CMOS_REG_YEAR);
        c   = cmos_read(CMOS_REG_CENTURY);
        status_b = cmos_read(CMOS_REG_STATUS_B);

        while (rtc_updating()) {}
        s2  = cmos_read(CMOS_REG_SECONDS);
        m2  = cmos_read(CMOS_REG_MINUTES);
        h2  = cmos_read(CMOS_REG_HOURS);
        d2  = cmos_read(CMOS_REG_DAY);
        mo2 = cmos_read(CMOS_REG_MONTH);
        y2  = cmos_read(CMOS_REG_YEAR);
        c2  = cmos_read(CMOS_REG_CENTURY);
    } while (s != s2 || m != m2 || h != h2 ||
             d != d2 || mo != mo2 || y != y2 || c != c2);

    if (!(status_b & STATUS_B_BIN)) {
        s  = bcd2bin(s);
        m  = bcd2bin(m);
        /* Hour: PM bit (0x80) must be preserved through the conversion */
        h  = (uint8_t)((h & HOUR_PM) | bcd2bin(h & 0x7F));
        d  = bcd2bin(d);
        mo = bcd2bin(mo);
        y  = bcd2bin(y);
        c  = bcd2bin(c);
    }

    /* 12-hour mode: convert PM-flagged hours to 24-hour. */
    if (!(status_b & STATUS_B_24H) && (h & HOUR_PM)) {
        h = (uint8_t)(((h & 0x7F) + 12) % 24);
    }

    /* Some BIOSes don't populate the century byte. If it looks bogus,
     * fall back to assuming 21st century. */
    if (c < 19 || c > 22) c = 20;

    t->sec   = s;
    t->min   = m;
    t->hour  = h;
    t->day   = d;
    t->month = mo;
    t->year  = (int)c * 100 + (int)y;
}

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int days_per_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

uint32_t rtc_to_epoch(const struct rtc_time *t) {
    uint32_t days = 0;
    for (int y = 1970; y < t->year; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 1; m < t->month; m++) {
        days += days_per_month[m - 1];
        if (m == 2 && is_leap(t->year)) days += 1;
    }
    days += (uint32_t)t->day - 1;
    return days * 86400u + (uint32_t)t->hour * 3600u
                         + (uint32_t)t->min  * 60u
                         + (uint32_t)t->sec;
}

/* Session 60: an NTP-driven correction additively applied to every
 * SYS_TIME / rtc_epoch_corrected call.  Signed so we can step the
 * clock backwards (the RTC can be ahead of UTC, e.g. set to local time).
 * Persists for the life of the boot — we don't push it back into the
 * CMOS chip. */
static int32_t g_clock_correction;

uint32_t rtc_epoch_corrected(void) {
    struct rtc_time t;
    rtc_read(&t);
    uint32_t base = rtc_to_epoch(&t);
    /* int32_t cast then add — handles negative corrections via wrap. */
    return base + (uint32_t)g_clock_correction;
}

void rtc_apply_correction(int32_t delta_seconds) {
    g_clock_correction += delta_seconds;
}
