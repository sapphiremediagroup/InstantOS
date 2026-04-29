#pragma once

#include <stdint.h>

uint8_t inb(uint16_t port);
uint16_t inw(uint16_t port);
uint32_t inl(uint16_t port);

void outb(uint16_t port, uint8_t value);
void outw(uint16_t port, uint16_t value);
void outl(uint16_t port, uint32_t value);

void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);

void wrmsr(uint32_t msr, uint64_t value);
uint64_t rdmsr(uint32_t msr);
uint64_t rdtsc();
