#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cpu/acpi/aml.hpp>

// One PCIe Enhanced Configuration Access Mechanism (ECAM) region, as described
// by an MCFG allocation entry. Each entry covers a PCI segment group and a
// contiguous range of bus numbers whose 4 KiB-per-function config space is
// memory mapped starting at `base`.
struct AcpiEcamRegion {
    uint64_t base = 0;       // physical base of the ECAM window
    uint16_t segment = 0;    // PCI segment group number
    uint8_t startBus = 0;    // first bus number covered
    uint8_t endBus = 0;      // last bus number covered (inclusive)
};

class ACPI {
public:
    using TableCallback = void (*)(const char* signature, void* table, void* context);

    static constexpr size_t kMaxEcamRegions = 16;

    static ACPI& get();
    
    bool initialize(uint64_t rsdpAddr);
    void shutdown();
    
    void* findTable(const char* signature);
    void forEachTable(TableCallback callback, void* context);
    void* findDsdt();
    AML::Interpreter& aml();
    bool evaluateAml(const char* path, AML::Object* result);

    // PCIe ECAM (MCFG) accessors. Returns the MMIO base for a given
    // segment/bus/device/function, or 0 if no ECAM region covers it (callers
    // should then fall back to legacy 0xCF8/0xCFC config access on segment 0).
    bool hasEcam() const { return ecamCount > 0; }
    size_t ecamRegionCount() const { return ecamCount; }
    const AcpiEcamRegion* ecamRegions() const { return ecam; }
    uint64_t ecamAddress(uint16_t segment, uint8_t bus, uint8_t device,
                         uint8_t function, uint16_t offset) const;

    // Power-management register accessors. These prefer the 64-bit Generic
    // Address Structure form from the FADT and fall back to the legacy I/O
    // ports, and are intended for wake/runtime-event handling (USB, GPIO, etc.).
    // Each returns false when the FADT does not describe a usable register.
    bool readPmTimer(uint32_t* outValue) const;
    bool gpe0Address(uint64_t* outAddress, uint8_t* outAddressSpace) const;
    bool gpe1Address(uint64_t* outAddress, uint8_t* outAddressSpace) const;

    void enumerate();
    void reboot();
    void sysShutdown();
private:
    ACPI() = default;

    void parseMcfg();

    void* rsdp = nullptr;
    void* rsdt = nullptr;
    bool rootUsesXsdt = false;
    bool initialized = false;
    bool amlInitialized = false;
    AML::Interpreter amlInterpreter;

    AcpiEcamRegion ecam[kMaxEcamRegions];
    size_t ecamCount = 0;
};
