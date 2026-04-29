#include <cpu/acpi/pci.hpp>
#include <common/ports.hpp>

PCI& PCI::get() {
    static PCI instance;
    return instance;
}

uint32_t PCI::makeAddress(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    return (1U << 31) |                           // Enable bit
           (static_cast<uint32_t>(bus) << 16) |   // Bus number
           (static_cast<uint32_t>(device & 0x1F) << 11) | // Device number (5 bits)
           (static_cast<uint32_t>(function & 0x07) << 8) | // Function number (3 bits)
           (offset & 0xFC);                       // Register offset (aligned to 4 bytes)
}

uint8_t PCI::readConfig8(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    if (segment != 0) {
        return 0xFF;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);
    
    uint32_t data = inl(CONFIG_DATA);
    
    return (data >> ((offset & 3) * 8)) & 0xFF;
}

uint16_t PCI::readConfig16(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    if (segment != 0) {
        return 0xFFFF;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);
    
    uint32_t data = inl(CONFIG_DATA);
    
    return (data >> ((offset & 2) * 8)) & 0xFFFF;
}

uint32_t PCI::readConfig32(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    if (segment != 0) {
        return 0xFFFFFFFF;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);
    
    uint32_t data = inl(CONFIG_DATA);
    
    return data;
}

void PCI::writeConfig8(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value) {
    if (segment != 0) {
        return;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);

    uint32_t data = inl(CONFIG_DATA);

    uint8_t shift = (offset & 3) * 8;
    data = (data & ~(0xFF << shift)) | (static_cast<uint32_t>(value) << shift);
    
    outl(CONFIG_DATA, data);
}

void PCI::writeConfig16(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value) {
    if (segment != 0) {
        return;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);
    
    uint32_t data = inl(CONFIG_DATA);
    
    uint8_t shift = (offset & 2) * 8;
    data = (data & ~(0xFFFF << shift)) | (static_cast<uint32_t>(value) << shift);
    
    outl(CONFIG_DATA, data);
}

void PCI::writeConfig32(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value) {
    if (segment != 0) {
        return;
    }
    
    uint32_t address = makeAddress(bus, device, function, offset);
    
    outl(CONFIG_ADDRESS, address);
    outl(CONFIG_DATA, value);
}
