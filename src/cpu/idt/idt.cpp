#include <cpu/idt/idt.hpp>
#include <common/string.hpp>

extern "C" void* isrTable[];
extern "C" void* irqTable[];
extern "C" void syscallEntry();

IDT::IDT(){
    idtp = IDTPointer {
        (uint16_t)sizeof(idt) - 1,
        (uintptr_t)&idt[0]
    };
    memset(idt, 0, sizeof(idt));

    for (uint8_t i = 0; i < 32; i++) {
        if (isrTable[i] != nullptr) {
            setEntry(i, (uint64_t)isrTable[i], 0x08, 0, 0x8E);
        }
    }
    setEntry(8, (uint64_t)isrTable[8], 0x08, 1, 0x8E);

    for (unsigned int i = 32; i < 256; i++) {
        if (irqTable[i-32] != nullptr) {
            setEntry(i, (uint64_t)irqTable[i-32], 0x08, 0, 0x8E);
        }
    }
    
    setEntry(0x80, (uint64_t)&syscallEntry, 0x08, 0, 0xEE);

    asm volatile("lidt %0" : : "m"(idtp));
}

void IDT::load() {
    asm volatile("lidt %0" : : "m"(idtp));
}

IDT& IDT::get() {
    static IDT instance;
    return instance;
}

void IDT::setEntry(uint8_t target, uint64_t offset, uint16_t selector, uint8_t ist, uint8_t typeAttributes) { 
    auto targetIDT = &idt[target];

    targetIDT->offsetLow = offset & 0xFFFF;
    targetIDT->selector = selector;
    targetIDT->ist = ist;
    targetIDT->typeAttribute = typeAttributes;
    targetIDT->offsetMiddle = (offset >> 16) & 0xFFFF;
    targetIDT->offsetHigh = (offset >> 32) & 0xFFFFFFFF;
    targetIDT->zero = 0;
}
