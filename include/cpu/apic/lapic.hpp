#pragma once

#include <stdint.h>

static constexpr uint32_t LAPIC_ID = 0x20;
static constexpr uint32_t LAPIC_EOI = 0xB0;
static constexpr uint32_t LAPIC_SPURIOUS = 0xF0;
static constexpr uint32_t LAPIC_TIMER = 0x320;
static constexpr uint32_t LAPIC_TIMER_INITCNT = 0x380;
static constexpr uint32_t LAPIC_TIMER_CURCNT = 0x390;
static constexpr uint32_t LAPIC_TIMER_DIV = 0x3E0;
static constexpr uint32_t LAPIC_ICR_LOW = 0x300;
static constexpr uint32_t LAPIC_ICR_HIGH = 0x310;

class LAPIC {
public:
    static LAPIC& get();
    
    bool initialize();
    void enable();
    void sendEOI();
    
    void sendIPI(uint32_t lapicId, uint32_t vector, uint32_t deliveryMode, uint32_t level = 1, uint32_t trigger = 0);
    void sendInitIPI(uint32_t lapicId);
    void sendStartupIPI(uint32_t lapicId, uint8_t vector);

    uint32_t getId();
    void setTimerDivide(uint8_t divide);
    void startTimer(uint32_t initialCount, uint8_t vector, bool periodic);
    uint32_t read(uint32_t reg);
    void write(uint32_t reg, uint32_t value);
    
private:
    LAPIC() : base(nullptr), initialized(false) {}
    
    volatile uint32_t* base;
    bool initialized;
    
};