#pragma once

#include <stdint.h>
#include <cpu/idt/isr.hpp>

struct IDTEntry {
    uint16_t offsetLow;
    uint16_t selector;
    uint8_t ist;
    uint8_t typeAttribute;
    uint16_t offsetMiddle;
    uint32_t offsetHigh;
    uint32_t zero;
} __attribute__((packed));

struct IDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class IDT {
private:
    IDTEntry idt[256];
    IDTPointer idtp;
public:
    IDT();

    void setEntry(uint8_t target, uint64_t offset, uint16_t selector, uint8_t ist, uint8_t typeAttributes);
    void load();

    static IDT& get();
};