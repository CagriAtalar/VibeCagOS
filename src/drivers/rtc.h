/*
 * VibeCagOS — CMOS Real-Time Clock (RTC) Driver
 *
 * Reads real calendar date and wall-clock time from x86 CMOS RTC.
 */

#pragma once
#include "common.h"

struct rtc_time {
    uint8_t  sec;
    uint8_t  min;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
};

/* Initialize RTC driver */
void rtc_init(void);

/* Read current date and time */
void rtc_get_time(struct rtc_time *t);

/* Format time as "YYYY-MM-DD HH:MM:SS" */
void rtc_format_time(char *buf, size_t max_len);
