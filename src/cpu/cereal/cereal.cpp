#include <cpu/cereal/cereal.hpp>
#include <common/ports.hpp>

Cereal& Cereal::get() {
    static Cereal instance;
    return instance;
}

void Cereal::initialize() {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    
    initialized = true;
}

bool Cereal::isTransmitEmpty() {
    uint8_t status = inb(COM1 + 5);
    return status & 0x20;
}

bool Cereal::hasInput() {
    if (!initialized) return false;

    uint8_t status = inb(COM1 + 5);
    return (status & 0x01) != 0;
}

char Cereal::read() {
    if (!hasInput()) return 0;
    return static_cast<char>(inb(COM1));
}

uint64_t Cereal::lock() {
    uint64_t flags = 0;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    // Bounded spin: a fault/panic path that logs while this CPU already holds
    // the lock must not deadlock, so after the cap we write anyway and accept
    // a garbled line over a hung kernel.
    for (uint32_t spins = 0; __atomic_test_and_set(&lockFlag, __ATOMIC_ACQUIRE); ++spins) {
        if (spins >= 50000000U) break;
        asm volatile("pause");
    }
    return flags;
}

void Cereal::unlock(uint64_t flags) {
    __atomic_clear(&lockFlag, __ATOMIC_RELEASE);
    asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

void Cereal::putRaw(char c) {
    while (!isTransmitEmpty());
    outb(COM1, c);
}

void Cereal::write(char c) {
    uint64_t flags = lock();
    record(c);
    if (initialized) putRaw(c);
    unlock(flags);
}

void Cereal::write(const char* str) {
    if (!str) return;

    uint64_t flags = lock();
    for (; *str; ++str) {
        record(*str);
        if (initialized) putRaw(*str);
    }
    unlock(flags);
}

size_t Cereal::copyLog(char* out, size_t capacity) {
    uint64_t flags = lock();
    const uint64_t held = logWritten < kLogSize ? logWritten : kLogSize;
    const uint64_t count = held < capacity ? held : capacity;
    for (uint64_t i = 0; i < count; ++i) {
        out[i] = logRing[(logWritten - count + i) % kLogSize];
    }
    unlock(flags);
    return count;
}
