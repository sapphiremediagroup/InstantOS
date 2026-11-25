#pragma once

#include <stdint.h>
#include <cpu/apic/lapic.hpp>

// What isr.asm pushes: every general register (a signal frame or a switch to
// another process needs all of them), then the vector, the error code and the
// CPU's frame.
struct InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8, rbp, rsi, rdi, rax, rcx, rdx, rbx;
    
    uint64_t interrupt, errCode;
    
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

class Interrupt {
public:
    Interrupt() = default;
    virtual ~Interrupt();

    virtual void initialize() = 0;

    virtual bool shouldDispatch() {
        return true;
    }

    virtual void Run(InterruptFrame* frame) = 0;
    void sendEOI(){
        LAPIC::get().sendEOI();
    }
};
