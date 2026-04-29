#include <cpu/percpu.hpp>
#include <common/ports.hpp>

constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

static PerCPUData bspPerCPU;

void initPerCPU(uint64_t kernelStack) {
    bspPerCPU.kernelStack = kernelStack;
    bspPerCPU.userRSP = 0;
    wrmsr(MSR_KERNEL_GS_BASE, reinterpret_cast<uint64_t>(&bspPerCPU));
}

PerCPUData* getPerCPU() {
    return &bspPerCPU;
}
