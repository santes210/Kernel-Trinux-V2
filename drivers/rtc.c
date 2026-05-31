/* drivers/rtc.c  -  CMOS Real-Time Clock reader. */
#include "rtc.h"
#include "../cpu/ports.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t val)
{
    return (uint8_t)((val & 0x0F) + ((val >> 4) * 10));
}

void rtc_read_datetime(datetime_t *dt)
{
    /* Wait for any in-progress update to finish. */
    while (update_in_progress())
        ;

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);

    uint8_t status_b = cmos_read(0x0B);

    /* Convert from BCD if needed. */
    if (!(status_b & 0x04)) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    }

    /* 12-hour to 24-hour. */
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    dt->second = sec;
    dt->minute = min;
    dt->hour   = hour;
    dt->day    = day;
    dt->month  = month;
    dt->year   = (uint16_t)(2000 + year);
}
