#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include "../lib/types.h"

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} datetime_t;

void rtc_read_datetime(datetime_t *dt);

#endif /* DRIVERS_RTC_H */
