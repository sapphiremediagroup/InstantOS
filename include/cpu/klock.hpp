#pragma once

#include <stdint.h>

// Big kernel lock (BKL).
//
// On one CPU the kernel relied on "interrupts off => nothing else runs": every
// syscall, IRQ and exception runs with IF=0, so kernel code never overlapped.
// With several CPUs that no longer holds, so all kernel code (everything except
// the entry/exit stubs, user mode and an idle CPU's hlt) now runs holding this
// one lock. It is recursive per CPU (a page fault taken inside a syscall nests)
// and must be taken with interrupts disabled.
//
// Taken: syscall entry (saveSyscallState), irqHandler/exceptionHandler, the
//        boot CPU's idle polling, and the boot path before its first yield.
// Dropped: on the way back to user mode (syscall exit, IRQ exit, the user-entry
//        trampolines) and when a CPU goes idle (idleLoop).
// Across a context switch the lock stays held by the switching CPU and passes
// to the context that resumes; Scheduler::schedule() saves its nesting depth
// before switchContext() and restores it when it is resumed.
namespace KernelLock {

// Entry-site tags for the optional contention profile (KLOCK_PROFILE in
// klock.cpp): a syscall number, or one of these bases plus a vector.
constexpr uint32_t kSiteIrq = 256;
constexpr uint32_t kSiteException = 512;
constexpr uint32_t kSiteIdlePoll = 768;
constexpr uint32_t kSiteOther = 769;
constexpr uint32_t kSiteResume = 770;
constexpr uint32_t kSiteCount = 771;

void enter(uint32_t site = kSiteOther);
// enter() if the lock is free or already held by this CPU; false otherwise.
// For periodic work that can just as well run on the next tick.
bool tryEnter(uint32_t site = kSiteOther);
void leave();

// Drops the lock entirely if this CPU holds it (entering user mode or idle).
void releaseAll();

// This CPU's nesting depth (0 when it does not hold the lock).
uint32_t depth();
// Restores the nesting depth of a context resumed by a context switch.
void setDepth(uint32_t depth);

bool heldByThisCpu();

// Drops the lock for a stretch of code that does not need it (storage I/O, see
// fs/storage/storage_lock.hpp) and returns the nesting depth to hand back to
// resume(). Returns 0, and does nothing, if this CPU does not hold the lock.
uint32_t suspend();
void resume(uint32_t depth);

} // namespace KernelLock

// For the asm user-entry trampolines.
extern "C" void kernelLockExitToUser();
