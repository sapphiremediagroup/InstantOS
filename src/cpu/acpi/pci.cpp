#include <cpu/acpi/pci.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/idt/isr.hpp>
#include <common/ports.hpp>

namespace {
constexpr uint16_t kInterruptLineOffset = 0x3C;
constexpr uint16_t kInterruptPinOffset = 0x3D;
constexpr uint16_t kStatusOffset = 0x06;
constexpr uint16_t kCommandOffset = 0x04;
constexpr uint16_t kCapabilityListOffset = 0x34;
constexpr uint16_t kCommandInterruptDisable = 1 << 10;
constexpr uint16_t kStatusCapabilityList = 1 << 4;
constexpr uint8_t kCapabilityMsi = 0x05;
constexpr uint8_t kMaxLegacyIrq = 15;
constexpr uint8_t kMaxHandlersPerIrq = 8;
uint8_t g_nextMsiVector = VECTOR_MSI_BASE;

class PCILegacyIRQDispatcher : public Interrupt {
public:
    PCILegacyIRQDispatcher() : handlerCount(0) {}

    void initialize() override {
    }

    bool addHandler(Interrupt* handler) {
        if (!handler) {
            return false;
        }

        for (uint8_t i = 0; i < handlerCount; ++i) {
            if (handlers[i] == handler) {
                return true;
            }
        }

        if (handlerCount >= kMaxHandlersPerIrq) {
            return false;
        }

        handlers[handlerCount++] = handler;
        return true;
    }

    void Run(InterruptFrame* frame) override {
        for (uint8_t i = 0; i < handlerCount; ++i) {
            if (handlers[i] && handlers[i]->shouldDispatch()) {
                handlers[i]->Run(frame);
            }
        }
        LAPIC::get().sendEOI();
    }

private:
    Interrupt* handlers[kMaxHandlersPerIrq] = {};
    uint8_t handlerCount;
};

bool g_pciDispatcherMapped[16] = {};

PCILegacyIRQDispatcher* getLegacyDispatchers() {
    static PCILegacyIRQDispatcher dispatchers[16];
    return dispatchers;
}
}

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

uint16_t PCI::findCapability(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t capabilityId) {
    const uint16_t status = readConfig16(segment, bus, device, function, kStatusOffset);
    if ((status & kStatusCapabilityList) == 0) {
        return 0;
    }

    uint8_t capPtr = readConfig8(segment, bus, device, function, kCapabilityListOffset) & 0xFC;
    for (uint32_t guard = 0; capPtr != 0 && guard < 64; ++guard) {
        if (readConfig8(segment, bus, device, function, capPtr) == capabilityId) {
            return capPtr;
        }

        capPtr = readConfig8(segment, bus, device, function, capPtr + 1) & 0xFC;
    }

    return 0;
}

bool PCI::registerMSIInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                               Interrupt* handler, uint8_t* outVector) {
    if (!handler) {
        return false;
    }

    const uint16_t cap = findCapability(segment, bus, device, function, kCapabilityMsi);
    if (cap == 0 || g_nextMsiVector > VECTOR_MSI_LIMIT) {
        return false;
    }

    const uint16_t control = readConfig16(segment, bus, device, function, cap + 2);
    const bool is64Bit = (control & (1 << 7)) != 0;
    const bool maskCapable = (control & (1 << 8)) != 0;
    (void)maskCapable;

    const uint8_t vector = g_nextMsiVector++;
    const uint32_t destination = LAPIC::get().getId() & 0xFF;
    const uint32_t messageAddress = 0xFEE00000U | (destination << 12);
    const uint16_t messageData = vector;

    writeConfig32(segment, bus, device, function, cap + 4, messageAddress);
    if (is64Bit) {
        writeConfig32(segment, bus, device, function, cap + 8, 0);
        writeConfig16(segment, bus, device, function, cap + 12, messageData);
    } else {
        writeConfig16(segment, bus, device, function, cap + 8, messageData);
    }

    const uint16_t updatedControl = static_cast<uint16_t>((control & ~0x0070U) | 0x0001U);
    writeConfig16(segment, bus, device, function, cap + 2, updatedControl);

    uint16_t command = readConfig16(segment, bus, device, function, kCommandOffset);
    command |= kCommandInterruptDisable;
    writeConfig16(segment, bus, device, function, kCommandOffset, command);

    ISR::registerIRQ(vector, handler);
    if (outVector) {
        *outVector = vector;
    }
    return true;
}

bool PCI::registerLegacyInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                                  Interrupt* handler, uint8_t* outIrq, uint8_t* outVector) {
    if (!handler) {
        return false;
    }

    const uint8_t pin = readConfig8(segment, bus, device, function, kInterruptPinOffset);
    if (pin == 0 || pin > 4) {
        return false;
    }

    const uint8_t irq = readConfig8(segment, bus, device, function, kInterruptLineOffset);
    if (irq == 0xFF || irq > kMaxLegacyIrq) {
        return false;
    }

    const uint8_t vector = static_cast<uint8_t>(VECTOR_PCI_BASE + irq);
    auto& dispatcher = getLegacyDispatchers()[irq];
    if (!dispatcher.addHandler(handler)) {
        return false;
    }

    if (!g_pciDispatcherMapped[irq]) {
        APICManager& apic = APICManager::get();
        const uint8_t targetCore = static_cast<uint8_t>(LAPIC::get().getId());
        apic.mapIRQ(irq, vector, targetCore);
        ISR::registerIRQ(vector, &dispatcher);
        g_pciDispatcherMapped[irq] = true;
    }

    if (outIrq) {
        *outIrq = irq;
    }
    if (outVector) {
        *outVector = vector;
    }

    return true;
}
