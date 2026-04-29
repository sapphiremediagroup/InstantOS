#include <common/ports.hpp>

uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile ("inb %1, %0"
        : "=a"(ret)
        : "Nd"(port));
    return ret;
}

uint16_t inw(uint16_t port)
{
    uint16_t ret;
    asm volatile ("inw %1, %0"
        : "=a"(ret)
        : "Nd"(port));
    return ret;
}

uint32_t inl(uint16_t port)
{
    uint32_t ret;
    asm volatile ("inl %1, %0"
        : "=a"(ret)
        : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t value)
{
    asm volatile ("outb %0, %1"
        :
        : "a"(value), "Nd"(port));
}

void outw(uint16_t port, uint16_t value)
{
    asm volatile ("outw %0, %1"
        :
        : "a"(value), "Nd"(port));
}

void outl(uint16_t port, uint32_t value)
{
    asm volatile ("outl %0, %1"
        :
        : "a"(value), "Nd"(port));
}

void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
  asm volatile("cpuid \n"
               : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
               : "a"(*eax), "c"(*ecx)
               : "memory");
}

void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = value & 0xFFFFFFFF;
    uint32_t hi = value >> 32;
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t rdtsc() {
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
