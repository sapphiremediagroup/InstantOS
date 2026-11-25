#pragma once

/*
 * What every InstantOS driver module includes: the ABI (idrv.h) plus helpers
 * that need no kernel (port I/O, barriers, MMIO). A module is freestanding: it
 * has its own memcpy/memset/memmove/memcmp (idrv_runtime.cpp, linked into
 * every module by add_idrv_module) and reaches the kernel only through the
 * IdrvKernelApi its init and probe are given.
 */

#include <idrv/idrv.h>

static inline uint8_t idrv_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t idrv_inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint32_t idrv_inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void idrv_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void idrv_outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline void idrv_outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t idrv_mmio_read32(uint64_t base, uint32_t offset) {
    return *(volatile uint32_t*)(base + offset);
}
static inline void idrv_mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(base + offset) = value;
}

static inline void idrv_barrier(void) {
    __asm__ volatile("" ::: "memory");
}
static inline void idrv_pause(void) {
    __asm__ volatile("pause");
}

#ifdef __cplusplus
extern "C" {
#endif
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
#ifdef __cplusplus
}
#endif

/* PCI configuration registers drivers commonly touch. */
#define IDRV_PCI_COMMAND 0x04
#define IDRV_PCI_COMMAND_IO (1u << 0)
#define IDRV_PCI_COMMAND_MEMORY (1u << 1)
#define IDRV_PCI_COMMAND_BUS_MASTER (1u << 2)
#define IDRV_PCI_BAR0 0x10
