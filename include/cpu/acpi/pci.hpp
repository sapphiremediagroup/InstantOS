#pragma once

#include <stdint.h>
#include <cpu/idt/interrupt.hpp>

class PCI {
public:
    static PCI& get();
    
    uint8_t readConfig8(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    uint16_t readConfig16(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    uint32_t readConfig32(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    
    void writeConfig8(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value);
    void writeConfig16(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value);
    void writeConfig32(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);

    bool registerMSIInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                              Interrupt* handler, uint8_t* outVector = nullptr);
    // Points an MSI capability (registered above) at another LAPIC.
    void setMSIDestination(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                           uint16_t cap, uint32_t lapicId);
    // MSI-X: programs table entry `entry` with a fresh vector, enables MSI-X
    // and disables INTx. The device decides which of its events use the entry
    // (virtio: queue_msix_vector).
    bool registerMSIXInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                               uint16_t entry, Interrupt* handler, uint8_t* outVector = nullptr);
    // Points an MSI-X table entry (its mapped address) at another LAPIC.
    void setMSIXDestination(uint64_t entryAddress, uint32_t lapicId);
    bool registerLegacyInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                                 Interrupt* handler, uint8_t* outIrq = nullptr, uint8_t* outVector = nullptr);

    // Physical base of memory BAR `bar` (64-bit aware), 0 if it is I/O or unset.
    uint64_t barAddress(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t bar);
    // The same, after making sure its first `bytes` are mapped (identity,
    // uncached): the firmware's map usually covers the BARs, but not always one
    // placed high above 4 GiB.
    uint64_t mapMemoryBar(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t bar, uint64_t bytes);
    
private:
    PCI() = default;
    
    static constexpr uint16_t CONFIG_ADDRESS = 0xCF8;
    static constexpr uint16_t CONFIG_DATA = 0xCFC;
    
    uint32_t makeAddress(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
    uint16_t findCapability(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t capabilityId);
};
