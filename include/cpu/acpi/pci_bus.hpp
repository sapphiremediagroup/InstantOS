#pragma once

#include <stdint.h>
#include <stddef.h>

// Reusable PCI/PCIe bus enumeration.
//
// Unlike the ad-hoc per-driver scans that hardcode segment 0 and the legacy
// 0xCF8/0xCFC mechanism, PciBus performs a proper recursive enumeration that
// follows PCI-to-PCI bridges, honors every PCIe segment described by the ACPI
// MCFG (ECAM) table, and resolves legacy INTx interrupt routing through the
// ACPI _PRT of each host bridge. Drivers query the resulting device registry
// instead of re-walking config space.

struct PciDeviceInfo {
    uint16_t segment = 0;
    uint8_t bus = 0;
    uint8_t device = 0;
    uint8_t function = 0;

    uint16_t vendorId = 0xFFFF;
    uint16_t deviceId = 0xFFFF;
    uint8_t classCode = 0;
    uint8_t subclass = 0;
    uint8_t progIf = 0;
    uint8_t revision = 0;
    uint8_t headerType = 0;

    uint8_t interruptPin = 0;   // 0 = none, 1..4 = INTA..INTD
    uint8_t interruptLine = 0;  // legacy 8259 line as programmed by firmware

    bool hasEcam = false;       // config space reachable via ECAM MMIO
};

class PciBus {
public:
    static constexpr size_t kMaxDevices = 64;

    using DeviceCallback = bool (*)(const PciDeviceInfo& info, void* context);

    static PciBus& get();

    // Enumerate all PCI segments/buses. Safe to call more than once; a second
    // call rescans from scratch. Returns the number of devices discovered.
    size_t scan();

    size_t deviceCount() const { return count; }
    const PciDeviceInfo* devices() const { return entries; }

    // Iterate discovered devices; returning false from the callback stops.
    void forEachDevice(DeviceCallback callback, void* context) const;

    // Find the first device matching a class/subclass/prog-IF triple. Pass 0xFF
    // for any field to wildcard it. Returns nullptr when no match is found.
    const PciDeviceInfo* findByClass(uint8_t classCode, uint8_t subclass,
                                     uint8_t progIf) const;

    // Resolve the interrupt for a function. Prefers the ACPI _PRT routing of the
    // owning host bridge (returning a Global System Interrupt and IOAPIC trigger
    // flags). Returns false when no _PRT entry applies (caller may then fall
    // back to MSI or the firmware-programmed interrupt line).
    bool resolveInterruptGsi(const PciDeviceInfo& info, uint32_t* outGsi,
                             uint16_t* outFlags) const;

private:
    PciBus() = default;

    struct PrtEntry {
        uint16_t segment;
        uint8_t bus;
        uint8_t device;   // 0..31 (the PCI device, address >> 16)
        uint8_t pin;      // 0..3 -> INTA..INTD
        uint32_t gsi;     // resolved Global System Interrupt
        uint16_t flags;   // IOAPIC polarity/trigger flags
        bool valid;
    };

    void scanSegment(uint16_t segment, uint8_t startBus);
    void scanBus(uint16_t segment, uint8_t bus);
    void scanFunction(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function);
    void buildRoutingTables();

    PciDeviceInfo entries[kMaxDevices];
    size_t count = 0;

    PrtEntry routes[256];
    size_t routeCount = 0;
};
