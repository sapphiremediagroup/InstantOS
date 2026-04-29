#include <cpu/acpi/pci.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/idt/isr.hpp>
#include <common/ports.hpp>

namespace {
constexpr uint16_t kInterruptLineOffset = 0x3C;
constexpr uint16_t kInterruptPinOffset = 0x3D;
constexpr uint8_t kMaxLegacyIrq = 15;
constexpr uint8_t kMaxHandlersPerIrq = 8;

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
            if (handlers[i]) {
                handlers[i]->Run(frame);
            }
        }
        LAPIC::get().sendEOI();
    }

private:
    Interrupt* handlers[kMaxHandlersPerIrq] = {};
    uint8_t handlerCount;
};

PCILegacyIRQDispatcher g_pciDispatchers[16];
bool g_pciDispatcherMapped[16] = {};
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
    auto& dispatcher = g_pciDispatchers[irq];
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
