#pragma once

#include <stdint.h>

static constexpr uint32_t IOAPIC_REGSEL = 0x00;
static constexpr uint32_t IOAPIC_IOWIN = 0x10;
static constexpr uint32_t IOAPIC_REG_ID = 0x00;
static constexpr uint32_t IOAPIC_REG_VER = 0x01;
static constexpr uint32_t IOAPIC_REG_REDTBL = 0x10;

class IOAPIC {
public:
    IOAPIC() = default;
    void initialize(uint64_t physAddr, uint32_t gsiBase);
    
    void setRedirect(uint8_t irq, uint8_t vector, uint32_t lapicId, bool masked, uint16_t flags);
    void maskIRQ(uint8_t irq);
    void unmaskIRQ(uint8_t irq);
    
    uint32_t getGSIBase() const { return gsiBase; }
    uint32_t getMaxRedirect();
    
private:
    volatile uint32_t* base;
    uint32_t gsiBase;
    
    void write(uint8_t reg, uint32_t value);
    uint32_t read(uint8_t reg);
};