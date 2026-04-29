#pragma once

#include <stdint.h>

struct PerCPUData {
    uint64_t kernelStack;
    uint64_t userRSP;
} __attribute__((packed));

void initPerCPU(uint64_t kernelStack);
PerCPUData* getPerCPU();
