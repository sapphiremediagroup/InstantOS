#pragma once

#include <stdint.h>
#include <cpu/idt/interrupt.hpp>

// Lock-up watchdog for real hardware, where there is no serial port and a hung
// boot CPU also stops the log from being saved.
//
// The boot CPU's timer tick drives the clock, sleepers and input. The APs check
// on their own timer tick (before taking any lock) that it still advances. When
// it has not for kStallMs, the first AP to notice sends the boot CPU an NMI,
// which records where it was (onNmi), and writes a report straight onto the
// screen without taking any lock: the boot CPU's RIP/RSP and the function
// there, which CPU holds the big kernel lock and from which entry site. It is
// put in the kernel log then (the hung CPU may hold the log's lock), but once
// the tick comes back it is, and the watchdog re-arms for the next stall.
namespace Watchdog {

constexpr uint64_t kStallMs = 2000;

// On every AP timer interrupt, before the big kernel lock.
void apTick();

// On every interrupt (irqHandler), before any lock: counts the boot CPU's
// vectors for the report.
void noteIrq(uint8_t vector);

// On an NMI (vector 2), before any lock; returns true if it was the
// watchdog's own NMI.
bool onNmi(InterruptFrame* frame);

} // namespace Watchdog
