#include <time/rtc.hpp>

#include <common/ports.hpp>

namespace {
constexpr uint16_t kCmosIndexPort = 0x70;
constexpr uint16_t kCmosDataPort = 0x71;

constexpr uint8_t kRtcSeconds = 0x00;
constexpr uint8_t kRtcMinutes = 0x02;
constexpr uint8_t kRtcHours = 0x04;
constexpr uint8_t kRtcDay = 0x07;
constexpr uint8_t kRtcMonth = 0x08;
constexpr uint8_t kRtcYear = 0x09;
constexpr uint8_t kRtcStatusA = 0x0A;
constexpr uint8_t kRtcStatusB = 0x0B;
constexpr uint8_t kRtcUpdateInProgress = 1U << 7;
constexpr uint8_t kRtc24HourMode = 1U << 1;
constexpr uint8_t kRtcBinaryMode = 1U << 2;

uint8_t rtc_read_raw(uint8_t reg) {
    outb(kCmosIndexPort, static_cast<uint8_t>(reg | 0x80));
    return inb(kCmosDataPort);
}

bool rtc_times_equal(const rtc_time& lhs, const rtc_time& rhs) {
    return lhs.second == rhs.second &&
           lhs.minute == rhs.minute &&
           lhs.hour == rhs.hour &&
           lhs.day == rhs.day &&
           lhs.month == rhs.month &&
           lhs.year == rhs.year;
}

bool is_leap_year(uint16_t year) {
    if ((year % 4) != 0) {
        return false;
    }

    if ((year % 100) != 0) {
        return true;
    }

    return (year % 400) == 0;
}
}

uint8_t bcd_to_bin(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

bool rtc_update_in_progress() {
    return (rtc_read_raw(kRtcStatusA) & kRtcUpdateInProgress) != 0;
}

uint8_t rtc_read(uint8_t reg) {
    return rtc_read_raw(reg);
}

bool rtc_is_bcd() {
    return (rtc_read_raw(kRtcStatusB) & kRtcBinaryMode) == 0;
}

bool rtc_is_12_hour() {
    return (rtc_read_raw(kRtcStatusB) & kRtc24HourMode) == 0;
}

void rtc_read_time(struct rtc_time* time) {
    if (!time) {
        return;
    }

    rtc_time first = {};
    rtc_time second = {};

    do {
        while (rtc_update_in_progress()) {
            asm volatile("pause");
        }

        first.second = rtc_read_raw(kRtcSeconds);
        first.minute = rtc_read_raw(kRtcMinutes);
        first.hour = rtc_read_raw(kRtcHours);
        first.day = rtc_read_raw(kRtcDay);
        first.month = rtc_read_raw(kRtcMonth);
        first.year = rtc_read_raw(kRtcYear);

        while (rtc_update_in_progress()) {
            asm volatile("pause");
        }

        second.second = rtc_read_raw(kRtcSeconds);
        second.minute = rtc_read_raw(kRtcMinutes);
        second.hour = rtc_read_raw(kRtcHours);
        second.day = rtc_read_raw(kRtcDay);
        second.month = rtc_read_raw(kRtcMonth);
        second.year = rtc_read_raw(kRtcYear);
    } while (!rtc_times_equal(first, second));

    const bool isBcd = rtc_is_bcd();
    const bool is12Hour = rtc_is_12_hour();
    const bool pm = (second.hour & 0x80U) != 0;

    if (isBcd) {
        second.second = bcd_to_bin(second.second);
        second.minute = bcd_to_bin(second.minute);
        second.hour = bcd_to_bin(static_cast<uint8_t>(second.hour & 0x7FU));
        second.day = bcd_to_bin(second.day);
        second.month = bcd_to_bin(second.month);
        second.year = bcd_to_bin(second.year);
    } else {
        second.hour &= 0x7FU;
    }

    if (is12Hour) {
        second.hour %= 12;
        if (pm) {
            second.hour = static_cast<uint8_t>(second.hour + 12);
        }
    }

    second.year = static_cast<uint16_t>(2000U + second.year);
    *time = second;
}

uint64_t rtc_to_unix(const struct rtc_time* time) {
    if (!time) {
        return 0;
    }

    static const uint8_t kMonthDays[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    uint64_t days = 0;
    for (uint16_t year = 1970; year < time->year; ++year) {
        days += is_leap_year(year) ? 366U : 365U;
    }

    for (uint8_t month = 1; month < time->month; ++month) {
        days += kMonthDays[month - 1];
        if (month == 2 && is_leap_year(time->year)) {
            ++days;
        }
    }

    days += static_cast<uint64_t>(time->day - 1U);

    return (days * 86400ULL) +
           (static_cast<uint64_t>(time->hour) * 3600ULL) +
           (static_cast<uint64_t>(time->minute) * 60ULL) +
           static_cast<uint64_t>(time->second);
}
