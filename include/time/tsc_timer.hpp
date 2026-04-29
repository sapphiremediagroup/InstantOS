#pragma once

#include <stdint.h>

extern volatile uint64_t uptime_ticks;
extern volatile uint64_t uptime_ms;

extern uint64_t tsc_frequency;
extern uint64_t cycles_per_ms;
extern uint64_t cycles_per_us;

bool cpu_has_tsc_deadline();
uint64_t detect_tsc_frequency();
void lapic_enable_tsc_deadline_mode();
void tsc_deadline_set(uint64_t tsc_target);
void timer_schedule_next_tick();
void timer_interrupt_handler();
void pit_enable_timer_mode(uint32_t frequency_hz = 1000);

uint64_t time_get_uptime_ticks();
uint64_t time_get_uptime_ms();
