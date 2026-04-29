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

void Cereal::write(char c) {
    if (!initialized) return;
    
    while (!isTransmitEmpty());
    outb(COM1, c);
}

void Cereal::write(const char* str) {
    if (!initialized || !str) return;
    
    while (*str) {
        write(*str++);
    }
}
