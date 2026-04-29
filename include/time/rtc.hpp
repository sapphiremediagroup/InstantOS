#pragma once

#include <stdint.h>

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

uint8_t bcd_to_bin(uint8_t value);
bool rtc_update_in_progress();
uint8_t rtc_read(uint8_t reg);
bool rtc_is_bcd();
bool rtc_is_12_hour();
void rtc_read_time(struct rtc_time* time);
uint64_t rtc_to_unix(const struct rtc_time* time);
