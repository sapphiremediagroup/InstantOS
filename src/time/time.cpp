#include <time/time.hpp>

#include <graphics/console.hpp>
#include <time/rtc.hpp>
#include <time/tsc_timer.hpp>

uint64_t boot_unix_time = 0;

bool time_init() {
    if (!cpu_has_tsc_deadline()) {
        Console::get().drawText("[TIME] CPU does not support TSC deadline mode; using PIT\n");
        pit_enable_timer_mode();
    } else {
        tsc_frequency = detect_tsc_frequency();
        if (tsc_frequency == 0) {
            Console::get().drawText("[TIME] Failed to detect TSC frequency; using PIT\n");
            pit_enable_timer_mode();
        } else {
            cycles_per_ms = tsc_frequency / 1000ULL;
            cycles_per_us = tsc_frequency / 1000000ULL;
            if (cycles_per_ms == 0 || cycles_per_us == 0) {
                Console::get().drawText("[TIME] TSC frequency too low; using PIT\n");
                pit_enable_timer_mode();
            } else {
                lapic_enable_tsc_deadline_mode();
                timer_schedule_next_tick();
            }
        }
    }

    rtc_time rtc = {};
    rtc_read_time(&rtc);
    boot_unix_time = rtc_to_unix(&rtc);

    return true;
}

uint64_t time_get_unix() {
    return boot_unix_time + (time_get_uptime_ms() / 1000ULL);
}

uint64_t time_get_boot_unix() {
    return boot_unix_time;
}
