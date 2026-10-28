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
// The boot CPU's tick in two parts. timer_account_tick() advances the uptime
// and arms the next deadline; it runs on every tick, without the big kernel
// lock (only the boot CPU writes these). timer_interrupt_handler() is the rest
// (input polling, waking sleepers, EOI) and needs the lock.
void timer_account_tick();
void timer_interrupt_handler();
void pit_enable_timer_mode(uint32_t frequency_hz = 1000);
// After pit_enable_timer_mode: keep the uptime with the TSC if it runs at a
// constant rate (calibrating it if needed). False if the PIT ticks must do.
bool tsc_enable_clocksource();

uint64_t time_get_uptime_ticks();
uint64_t time_get_uptime_ms();
uint64_t time_get_uptime_ns();
