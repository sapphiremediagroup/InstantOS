#pragma once

#include <stdint.h>

// Spreads device interrupts over the CPUs. Drivers route to the boot CPU while
// it boots alone; the routing code records every route here, and
// SMP::startScheduling() calls distribute() to move them round-robin onto the
// application processors. Routes made later go straight to the next AP.
// The boot CPU keeps the PIT tick (VECTOR_TIMER), which never moves.
namespace IRQBalance {

// Record an IOAPIC route (APICManager::mapGSI), an MSI route
// (PCI::registerMSIInterrupt) or an MSI-X one. Each returns the LAPIC ID to
// program now:
// `lapicId` before distribute(), the next AP after it.
uint32_t addGsiRoute(uint32_t gsi, uint8_t vector, uint32_t lapicId);
uint32_t addMsiRoute(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                     uint16_t cap, uint8_t vector, uint32_t lapicId);
// An MSI-X table entry (PCI::registerMSIXInterrupt), by its mapped address.
uint32_t addMsixRoute(uint64_t entryAddress, uint8_t vector, uint32_t lapicId);

// Moves every recorded route onto the online APs. No-op with one CPU.
void distribute();

} // namespace IRQBalance
