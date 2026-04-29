#pragma once

#include <fs/ahci/ahci.hpp>
#include <stdint.h>

#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_CLASS 0x0B
#define PCI_SUBCLASS 0x0A
#define PCI_PROG_IF 0x09
#define PCI_BAR0 0x10
#define PCI_BAR5 0x24

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA 0x06
#define PCI_PROG_IF_AHCI 0x01

class AHCIDetector {
public:
    static AHCIController* detectAndInitialize();
    
private:
    static uint32_t pciConfigReadDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    static void pciConfigWriteDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
    static uint16_t pciConfigReadWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    static uint8_t pciConfigReadByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
    static bool checkDevice(uint8_t bus, uint8_t slot, uint8_t func, uint64_t* abar);
};
