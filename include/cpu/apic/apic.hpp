#pragma once

#include <stdint.h>
#include <cpu/apic/lapic.hpp>
#include <cpu/apic/ioapic.hpp>

class APICManager {
public:
    static APICManager& get();
    
    bool initialize();
    void mapIRQ(uint8_t irq, uint8_t vector, uint32_t dest = 0);
    uint16_t getFlags(uint8_t irq);
    
    void startupAPs();
    uint8_t getCPUCount() const { return cpuCount; }
    uint8_t getAPICId(uint8_t index) const { return apicIds[index]; }
    
private:
    APICManager() : initialized(false), ioapicCount(0), overrideCount(0), cpuCount(0) {}
    
    struct InterruptOverride {
        uint8_t source;
        uint32_t gsi;
        uint16_t flags;
    };
    
    bool initialized;
    IOAPIC ioapics[16];
    uint8_t ioapicCount;
    InterruptOverride overrides[16];
    uint8_t overrideCount;
    uint8_t apicIds[16];
    uint8_t cpuCount;
    
    IOAPIC* getIOAPICForGSI(uint32_t gsi);
    uint32_t resolveIRQ(uint8_t irq);
};
