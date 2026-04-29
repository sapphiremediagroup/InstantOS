#pragma once

#include <stdint.h>
#include <cpu/idt/interrupt.hpp>

extern "C" void exceptionHandler(InterruptFrame* frame);
extern "C" void irqHandler(InterruptFrame* frame);

class ISR {
public:
    static void registerIRQ(uint8_t vector, Interrupt* handler);
};