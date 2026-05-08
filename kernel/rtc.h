#ifndef ADVENTOS_RTC_H
#define ADVENTOS_RTC_H

#include "../include/types.h"

struct rtc_time {
    int year;     /* 4-digit                 */
    int month;    /* 1-12                    */
    int day;      /* 1-31                    */
    int hour;     /* 0-23                    */
    int min;      /* 0-59                    */
    int sec;      /* 0-59                    */
};

void rtc_read(struct rtc_time *t);

/* Days since 1970-01-01 00:00:00 UTC, as a UNIX timestamp. */
uint32_t rtc_to_epoch(const struct rtc_time *t);

#endif
