#include <cpu/percpu.hpp>
#include <cpu/gdt/gdt.hpp>
#include <common/ports.hpp>

constexpr uint32_t MSR_GS_BASE = 0xC0000101;
constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

static PerCPUData bspPerCPU;

// False until GS points at a PerCPUData. Early boot code (console, heap locks)
// runs before that and must not read through GS.
static bool perCpuReady = false;

void initBootCPU() {
    bspPerCPU.kernelStack = 0;
    bspPerCPU.userRSP = 0;
    bspPerCPU.self = &bspPerCPU;
    bspPerCPU.cpuIndex = 0;
    bspPerCPU.lapicId = 0;
    bspPerCPU.currentProcess = nullptr;
    bspPerCPU.idleProcess = nullptr;
    bspPerCPU.gdt = &GDT::boot();
    bspPerCPU.deferredProcess = nullptr;
    bspPerCPU.tlbPending = 0;

    wrmsr(MSR_GS_BASE, reinterpret_cast<uint64_t>(&bspPerCPU));
    // The user GS base; swapped in by the first return to user mode.
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    perCpuReady = true;
}

PerCPUData* getPerCPU() {
    if (!perCpuReady) {
        return &bspPerCPU;
    }
    PerCPUData* cpu = nullptr;
    asm volatile("mov %%gs:16, %0" : "=r"(cpu));
    return cpu;
}

uint32_t currentCpuIndex() {
    if (!perCpuReady) {
        return 0;
    }
    uint32_t index = 0;
    asm volatile("movl %%gs:24, %0" : "=r"(index));
    return index;
}
