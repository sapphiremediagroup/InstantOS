#pragma once

#include <cpu/idt/interrupt.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/apic/irqs.hpp>
#include <time/tsc_timer.hpp>

namespace {
constexpr uint64_t kUserCodeSelector = 0x23;
}

class Timer : public Interrupt {
public:
    void initialize() override;
    
    void Run(InterruptFrame* frame) override;
    
    static Timer& get() {
        static Timer instance;
        return instance;
    }
    
    uint64_t getMilliseconds() const {
        return time_get_uptime_ms();
    }
    
    uint64_t getTicks() const {
        return time_get_uptime_ticks();
    }
};
