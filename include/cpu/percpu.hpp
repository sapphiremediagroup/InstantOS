#pragma once

#include <stdint.h>

class Process;
class GDT;

// One per CPU. While executing in the kernel the GS base always points at the
// executing CPU's PerCPUData; in user mode the user's GS base is live and this
// pointer sits in IA32_KERNEL_GS_BASE. Every kernel entry/exit from/to user
// mode (syscallEntry, the ISR/IRQ stubs, the user-entry trampolines) does a
// swapgs to keep that invariant.
struct PerCPUData {
    uint64_t kernelStack;      // 0: loaded by syscallEntry (gs:[0])
    uint64_t userRSP;          // 8: user rsp saved by syscallEntry (gs:[8])
    PerCPUData* self;          // 16: lets getPerCPU() read its own address via GS
    uint32_t cpuIndex;         // 24: 0 = boot CPU
    uint32_t lapicId;          // 28
    Process* currentProcess;   // 32
    Process* idleProcess;      // 40
    GDT* gdt;                  // 48: this CPU's GDT + TSS
    // A process whose kernel stack this CPU may still be using: set when the
    // scheduler resumes another process by rewriting the IRQ frame (the CPU
    // stays on the old process's stack until iretq, after the BKL is dropped).
    // Released at this CPU's next kernel entry (Scheduler::onKernelEntry).
    Process* deferredProcess;  // 56
    // TLB shootdown mailbox (src/cpu/tlb.cpp): a sender fills tlbAddr/tlbPages,
    // sets tlbPending and waits until this CPU has flushed and cleared it.
    volatile uint32_t tlbPending;  // 64
    uint64_t tlbAddr;              // 72
    uint64_t tlbPages;             // 80: 0 = flush everything
    // Device interrupts this CPU served (not timer, scheduler or TLB vectors).
    uint64_t deviceIrqs;           // 88
    // The syscall this CPU is in, ~0 when none (for the lock-up watchdog).
    volatile uint64_t currentSyscall = ~0ULL;   // 96
    // Scheduler::kickIdleCpu sent this CPU a reschedule IPI it has not taken
    // yet (set by the sender, cleared by the IPI): the next kick goes to another.
    volatile uint32_t idleKicked = 0;           // 104
    // syscallEntry's return by iretq (after sigreturn): the user's rip and
    // rflags (rsp is userRSP).
    uint64_t iretRip = 0;                       // 112
    uint64_t iretRflags = 0;                    // 120
};

static_assert(__builtin_offsetof(PerCPUData, kernelStack) == 0, "syscallEntry reads gs:[0]");
static_assert(__builtin_offsetof(PerCPUData, userRSP) == 8, "syscallEntry uses gs:[8]");
static_assert(__builtin_offsetof(PerCPUData, self) == 16, "getPerCPU reads gs:[16]");
static_assert(__builtin_offsetof(PerCPUData, cpuIndex) == 24, "currentCpuIndex reads gs:[24]");
static_assert(__builtin_offsetof(PerCPUData, iretRip) == 112, "syscallEntry reads gs:[112]");
static_assert(__builtin_offsetof(PerCPUData, iretRflags) == 120, "syscallEntry reads gs:[120]");

// Sets up the boot CPU's PerCPUData and points GS at it. Call after the GDT is
// loaded (reloading GS clears the GS base).
void initBootCPU();

// The executing CPU's data. Before initBootCPU() this is the boot CPU's block.
PerCPUData* getPerCPU();

// Index of the executing CPU (0 = boot CPU).
uint32_t currentCpuIndex();
