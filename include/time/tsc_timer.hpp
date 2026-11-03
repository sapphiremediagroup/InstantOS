#pragma once

#include <stdint.h>

extern volatile uint64_t uptime_ticks;
extern volatile uint64_t uptime_ms;

extern uint64_t tsc_frequency;
extern uint64_t cycles_per_ms;
extern uint64_t cycles_per_us;

bool cpu_has_tsc_deadline();
// True when this CPU's microcode predates the fix for the TSC-deadline erratum
// (the timer may stop firing); reports the model and microcode revision.
bool tsc_deadline_has_errata(uint32_t* modelOut, uint32_t* revisionOut);
// Whether the boot CPU's tick is the TSC-deadline timer (else the PIT).
bool time_tick_is_tsc_deadline();
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
// The tick's duties (input polling, key repeat, waking sleepers, the network
// timers) normally run on the boot CPU. When it has not run them for
// kTickDutiesOverdueMs (it is stuck with interrupts off, e.g. polling a disk
// that is spinning up), an AP's tick runs them instead: under the big kernel
// lock, only when it interrupted user mode or idle (as the boot CPU does).
constexpr uint64_t kTickDutiesOverdueMs = 4;
bool timer_duties_overdue();
void timer_run_overdue_duties();
// How many times an AP has stood in for the boot CPU's tick since boot.
uint64_t timer_failover_count();
void pit_enable_timer_mode(uint32_t frequency_hz = 1000);
// After pit_enable_timer_mode: keep the uptime with the TSC if it runs at a
// constant rate (calibrating it if needed). False if the PIT ticks must do.
bool tsc_enable_clocksource();

uint64_t time_get_uptime_ticks();
uint64_t time_get_uptime_ms();
uint64_t time_get_uptime_ns();
