#pragma once

#include <stdint.h>
#include <cpu/percpu.hpp>

// Multiprocessor bring-up. The boot CPU is index 0; application processors
// (APs) listed in the MADT get indices 1.. in the order they come online.
namespace SMP {

constexpr uint32_t kMaxCPUs = 16;

// Claims real-mode memory for the AP startup trampoline. Call right after the
// PMM is initialized, before low frames are handed out to anything else.
void reserveLowMemory();

// Starts every enabled AP from the MADT and parks it (cli; hlt) once it has
// its own GDT/TSS, GS, IDT, CPU features, syscall MSRs and LAPIC set up.
// Needs the boot CPU's GDT/IDT/CPU/LAPIC/syscall setup and the heap.
void startAPs();

// Lets the parked APs into the scheduler (each starts its LAPIC timer and runs
// its idle process). Call once boot is done, right before the boot CPU's
// first yield: boot code runs without the big kernel lock until then.
void startScheduling();
bool schedulingActive();

// Reschedule IPI to `cpu` (it runs the scheduler if idle or in user mode).
void sendReschedule(PerCPUData* cpu);

// CPUs that are up, including the boot CPU.
uint32_t onlineCount();

// PerCPUData of CPU `index`, or nullptr.
PerCPUData* cpu(uint32_t index);

} // namespace SMP
