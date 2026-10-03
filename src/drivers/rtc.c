/*
 * VibeCagOS — CMOS Real-Time Clock (RTC) Driver Implementation
 */

#include "rtc.h"
#include "kernel.h"
#include "klog.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool rtc_is_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val / 16) * 10);
}

void rtc_init(void) {
    KINFO("RTC", "CMOS Real-Time Clock initialized");
}

void rtc_get_time(struct rtc_time *t) {
    /* Wait if update in progress */
    while (rtc_is_updating()) ;

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);

    /* Read register B for format check */
    uint8_t reg_b = cmos_read(0x0B);

    /* Convert BCD to binary if needed */
    if (!(reg_b & 0x04)) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = ((hour & 0x0F) + (((hour & 0x70) / 16) * 10)) | (hour & 0x80);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    }

    /* Convert 12 hour to 24 hour if necessary */
    if (!(reg_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    t->sec   = sec;
    t->min   = min;
    t->hour  = hour;
    t->day   = day;
    t->month = month;
    t->year  = 2000 + (uint16_t)year;
}

void rtc_format_time(char *buf, size_t max_len) {
    if (!buf || max_len < 20) return;
    struct rtc_time t;
    rtc_get_time(&t);

    /* Format YYYY-MM-DD HH:MM:SS */
    buf[0] = '0' + (t.year / 1000);
    buf[1] = '0' + ((t.year / 100) % 10);
    buf[2] = '0' + ((t.year / 10) % 10);
    buf[3] = '0' + (t.year % 10);
    buf[4] = '-';
    buf[5] = '0' + (t.month / 10);
    buf[6] = '0' + (t.month % 10);
    buf[7] = '-';
    buf[8] = '0' + (t.day / 10);
    buf[9] = '0' + (t.day % 10);
    buf[10] = ' ';
    buf[11] = '0' + (t.hour / 10);
    buf[12] = '0' + (t.hour % 10);
    buf[13] = ':';
    buf[14] = '0' + (t.min / 10);
    buf[15] = '0' + (t.min % 10);
    buf[16] = ':';
    buf[17] = '0' + (t.sec / 10);
    buf[18] = '0' + (t.sec % 10);
    buf[19] = '\0';
}
