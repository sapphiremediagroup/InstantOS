#include "graphics/console.hpp"
#include "graphics/framebuffer.hpp"
#include <fs/ahci/detect.hpp>
#include <fs/ahci/ahci.hpp>
#include <common/ports.hpp>
#include <cpu/acpi/pci.hpp>

uint32_t AHCIDetector::pciConfigReadDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return PCI::get().readConfig32(0, bus, slot, func, offset);
}

void AHCIDetector::pciConfigWriteDword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    PCI::get().writeConfig32(0, bus, slot, func, offset, value);
}

uint16_t AHCIDetector::pciConfigReadWord(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return PCI::get().readConfig16(0, bus, slot, func, offset);
}

uint8_t AHCIDetector::pciConfigReadByte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return PCI::get().readConfig8(0, bus, slot, func, offset);
}

bool AHCIDetector::checkDevice(uint8_t bus, uint8_t slot, uint8_t func, uint64_t* abar) {
    uint16_t vendorID = pciConfigReadWord(bus, slot, func, PCI_VENDOR_ID);
    if (vendorID == 0xFFFF) return false;
    
    uint8_t classCode = pciConfigReadByte(bus, slot, func, PCI_CLASS);
    uint8_t subclass = pciConfigReadByte(bus, slot, func, PCI_SUBCLASS);
    uint8_t progIF = pciConfigReadByte(bus, slot, func, PCI_PROG_IF);
    
    if (classCode == PCI_CLASS_MASS_STORAGE && subclass == PCI_SUBCLASS_SATA && progIF == PCI_PROG_IF_AHCI) {
        uint32_t bar5 = pciConfigReadDword(bus, slot, func, PCI_BAR5);
        Console::get().drawText("[AHCI] BAR5 raw: 0x");
        Console::get().drawHex(bar5);
        Console::get().drawText("\n");
        if ((bar5 & 0x6) == 0x4) {
            uint32_t bar5_high = pciConfigReadDword(bus, slot, func, PCI_BAR5 + 4);
            Console::get().drawText("[AHCI] BAR5 high: 0x");
            Console::get().drawHex(bar5_high);
            Console::get().drawText("\n");
            *abar = ((uint64_t)bar5_high << 32) | (bar5 & 0xFFFFFFF0);
        } else {
            *abar = bar5 & 0xFFFFFFF0;
        }
        
        uint16_t command = pciConfigReadWord(bus, slot, func, PCI_COMMAND);
        command |= 0x06;
        PCI::get().writeConfig16(0, bus, slot, func, PCI_COMMAND, command);
        
        return true;
    }
    
    return false;
}

AHCIController* AHCIDetector::detectAndInitialize() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint64_t abar;
                if (checkDevice(bus, slot, func, &abar)) {
                    Console::get().drawText("AHCI: [ ");
                    Console::get().setTextColor(0x49ceee);
                    Console::get().drawText("...");
                    Console::get().setTextColor(0xFFFFFF);
                    Console::get().drawText(" ] ");
                    Console::get().drawHex(abar);
                    Console::get().drawText(" ");
                    Console::get().drawNumber(slot);
                    Console::get().drawText(":");
                    Console::get().drawNumber(func);
                    Console::get().drawText("\n");
                    
                    AHCIController* controller = new AHCIController(abar);
                    if (controller->initialize()) {
                        return controller;
                    }
                    delete controller;
                }
            }
        }
    }
    
    return nullptr;
}
