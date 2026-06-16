#include <cpu/acpi/acpi.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <memory/vmm.hpp>
#include <stddef.h>

struct Rsdp {
    char signature[8];
    uint8_t checksum;
    char oemId[6];
    uint8_t revision;
    uint32_t rsdtAddress;
} __attribute__((packed));

struct Rsdp20 {
    Rsdp firstPart;
    uint32_t length;
    uint64_t xsdtAddress;
    uint8_t extendedChecksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct AcpiHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemId[6];
    char oemTableId[8];
    uint32_t oemRevision;
    uint32_t creatorId;
    uint32_t creatorRevision;
} __attribute__((packed));

struct Rsdt {
    AcpiHeader header;
    uint32_t pointers[];
} __attribute__((packed));

struct Xsdt {
    AcpiHeader header;
    uint64_t pointers[];
} __attribute__((packed));

struct GenericAddressStructure {
    uint8_t addressSpace;
    uint8_t bitWidth;
    uint8_t bitOffset;
    uint8_t accessSize;
    uint64_t address;
} __attribute__((packed));

// MCFG: PCI Express Memory Mapped Configuration Space Base Address Description
// Table (ACPI 6.x, "Static Resource Allocation Structures"). The fixed header
// is followed by one allocation entry per (segment, bus-range) ECAM window.
struct McfgAllocation {
    uint64_t baseAddress;
    uint16_t segmentGroup;
    uint8_t startBus;
    uint8_t endBus;
    uint32_t reserved;
} __attribute__((packed));

struct Mcfg {
    AcpiHeader header;
    uint64_t reserved;
    McfgAllocation allocations[];
} __attribute__((packed));

static_assert(sizeof(McfgAllocation) == 16);
static_assert(offsetof(Mcfg, allocations) == 44);

struct Fadt {
    AcpiHeader header;
    uint32_t firmwareCtrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferredPowerManagementProfile;
    uint16_t sciInterrupt;
    uint32_t smiCommandPort;
    uint8_t acpiEnable;
    uint8_t acpiDisable;
    uint8_t s4BiosReq;
    uint8_t pstateControl;
    uint32_t pm1aEventBlock;
    uint32_t pm1bEventBlock;
    uint32_t pm1aControlBlock;
    uint32_t pm1bControlBlock;
    uint32_t pm2ControlBlock;
    uint32_t pmTimerBlock;
    uint32_t gpe0Block;
    uint32_t gpe1Block;
    uint8_t pm1EventLength;
    uint8_t pm1ControlLength;
    uint8_t pm2ControlLength;
    uint8_t pmTimerLength;
    uint8_t gpe0Length;
    uint8_t gpe1Length;
    uint8_t gpe1Base;
    uint8_t cStateControl;
    uint16_t worstC2Latency;
    uint16_t worstC3Latency;
    uint16_t flushSize;
    uint16_t flushStride;
    uint8_t dutyOffset;
    uint8_t dutyWidth;
    uint8_t dayAlarm;
    uint8_t monthAlarm;
    uint8_t century;
    uint16_t iaPcBootArchitectureFlags;
    uint8_t reserved2;
    uint32_t flags;
    GenericAddressStructure resetReg;
    uint8_t resetValue;
    uint16_t armBootArchitectureFlags;
    uint8_t fadtMinorVersion;
    uint64_t xFirmwareControl;
    uint64_t xDsdt;
    GenericAddressStructure xPm1aEventBlock;
    GenericAddressStructure xPm1bEventBlock;
    GenericAddressStructure xPm1aControlBlock;
    GenericAddressStructure xPm1bControlBlock;
    GenericAddressStructure xPm2ControlBlock;
    GenericAddressStructure xPmTimerBlock;
    GenericAddressStructure xGpe0Block;
    GenericAddressStructure xGpe1Block;
    GenericAddressStructure sleepControlReg;
    GenericAddressStructure sleepStatusReg;
    uint64_t hypervisorVendorIdentity;
} __attribute__((packed));

static_assert(offsetof(Fadt, resetReg) == 116);
static_assert(offsetof(Fadt, xFirmwareControl) == 132);
static_assert(offsetof(Fadt, xDsdt) == 140);
static_assert(offsetof(Fadt, sleepControlReg) == 244);
static_assert(offsetof(Fadt, hypervisorVendorIdentity) == 268);
static_assert(sizeof(Fadt) == 276);

namespace {

constexpr uint8_t kAcpiAddressSpaceSystemMemory = 0;
constexpr uint8_t kAcpiAddressSpaceSystemIo = 1;
constexpr uint32_t kFadtFlagHardwareReducedAcpi = 1U << 20;

struct AcpiRegister {
    uint8_t addressSpace = 0;
    uint8_t bitWidth = 0;
    uint64_t address = 0;
    bool valid = false;
};

}

static bool acpiSignatureEquals(const char* a, const char* b) {
    return strncmp(a, b, 4) == 0;
}

static uint8_t acpiChecksum(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return sum;
}

static bool acpiChecksumValid(const void* data, size_t length) {
    return data && length > 0 && acpiChecksum(data, length) == 0;
}

static bool acpiRsdpValid(const Rsdp* rsdp) {
    if (!rsdp || !acpiSignatureEquals(rsdp->signature, "RSD ")) {
        return false;
    }
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        return false;
    }
    if (!acpiChecksumValid(rsdp, sizeof(Rsdp))) {
        return false;
    }
    if (rsdp->revision < 2) {
        return true;
    }

    const Rsdp20* ext = reinterpret_cast<const Rsdp20*>(rsdp);
    if (ext->length < sizeof(Rsdp20) || ext->length > 4096) {
        return false;
    }
    return acpiChecksumValid(ext, ext->length);
}

static bool acpiTableHeaderLooksValid(const AcpiHeader* header) {
    if (!header || header->length < sizeof(AcpiHeader)) {
        return false;
    }
    return true;
}

static bool acpiTableValid(const void* table, const char* expectedSignature = nullptr) {
    const AcpiHeader* header = static_cast<const AcpiHeader*>(table);
    if (!acpiTableHeaderLooksValid(header)) {
        return false;
    }
    if (expectedSignature && !acpiSignatureEquals(header->signature, expectedSignature)) {
        return false;
    }
    return acpiChecksumValid(table, header->length);
}

static bool acpiRootTableValid(const void* table, bool useXsdt) {
    return acpiTableValid(table, useXsdt ? "XSDT" : "RSDT");
}

static size_t acpiRootEntryCount(const AcpiHeader* header, bool useXsdt) {
    if (!acpiTableHeaderLooksValid(header)) {
        return 0;
    }
    const size_t entrySize = useXsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    return (header->length - sizeof(AcpiHeader)) / entrySize;
}

// A root-table entry must point at something that can plausibly hold an ACPI
// header. Reject the obvious sentinels (null and the all-ones value used by
// unmapped firmware regions) before any dereference; full structural and
// checksum validation still happens in acpiTableValid().
static bool acpiEntryPointerPlausible(uint64_t physAddr) {
    if (physAddr == 0 || physAddr == UINT32_MAX || physAddr == UINT64_MAX) {
        return false;
    }
    return true;
}

static bool fadtHasField(const Fadt* fadt, size_t offset, size_t length) {
    if (!fadt || !acpiTableHeaderLooksValid(&fadt->header)) {
        return false;
    }
    if (offset > UINT32_MAX - length) {
        return false;
    }
    return fadt->header.length >= offset + length;
}

static bool fadtHasDsdt(const Fadt* fadt) {
    return fadtHasField(fadt, offsetof(Fadt, dsdt), sizeof(fadt->dsdt));
}

static bool fadtHasXDsdt(const Fadt* fadt) {
    return fadtHasField(fadt, offsetof(Fadt, xDsdt), sizeof(fadt->xDsdt));
}

static uint64_t fadtDsdtAddress(const Fadt* fadt) {
    if (!fadt) {
        return 0;
    }
    if (fadtHasXDsdt(fadt) && fadt->xDsdt) {
        return fadt->xDsdt;
    }
    if (fadtHasDsdt(fadt)) {
        return fadt->dsdt;
    }
    return 0;
}

static bool acpiGasSupported(const GenericAddressStructure& gas) {
    return gas.address != 0 &&
           gas.bitOffset == 0 &&
           (gas.addressSpace == kAcpiAddressSpaceSystemMemory ||
            gas.addressSpace == kAcpiAddressSpaceSystemIo);
}

static bool acpiGasSupported8(const GenericAddressStructure& gas) {
    return acpiGasSupported(gas) && gas.bitWidth == 8;
}

static bool acpiGasSupported16(const GenericAddressStructure& gas) {
    return acpiGasSupported(gas) && (gas.bitWidth == 0 || gas.bitWidth >= 16);
}

static AcpiRegister acpiRegisterFromGas(const GenericAddressStructure& gas) {
    AcpiRegister reg;
    reg.addressSpace = gas.addressSpace;
    reg.bitWidth = gas.bitWidth;
    reg.address = gas.address;
    reg.valid = true;
    return reg;
}

static AcpiRegister acpiIoRegister(uint32_t port, uint8_t bitWidth) {
    AcpiRegister reg;
    if (!port) {
        return reg;
    }
    reg.addressSpace = kAcpiAddressSpaceSystemIo;
    reg.bitWidth = bitWidth;
    reg.address = port;
    reg.valid = true;
    return reg;
}

static AcpiRegister fadtPm1aControlRegister(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, xPm1aControlBlock), sizeof(fadt->xPm1aControlBlock)) &&
        acpiGasSupported16(fadt->xPm1aControlBlock)) {
        return acpiRegisterFromGas(fadt->xPm1aControlBlock);
    }
    if (fadtHasField(fadt, offsetof(Fadt, pm1aControlBlock), sizeof(fadt->pm1aControlBlock))) {
        return acpiIoRegister(fadt->pm1aControlBlock, 16);
    }
    return {};
}

static AcpiRegister fadtPm1bControlRegister(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, xPm1bControlBlock), sizeof(fadt->xPm1bControlBlock)) &&
        acpiGasSupported16(fadt->xPm1bControlBlock)) {
        return acpiRegisterFromGas(fadt->xPm1bControlBlock);
    }
    if (fadtHasField(fadt, offsetof(Fadt, pm1bControlBlock), sizeof(fadt->pm1bControlBlock))) {
        return acpiIoRegister(fadt->pm1bControlBlock, 16);
    }
    return {};
}

static AcpiRegister fadtSleepControlRegister(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, sleepControlReg), sizeof(fadt->sleepControlReg)) &&
        acpiGasSupported8(fadt->sleepControlReg)) {
        return acpiRegisterFromGas(fadt->sleepControlReg);
    }
    return {};
}

static AcpiRegister fadtResetRegister(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, resetReg), sizeof(fadt->resetReg)) &&
        acpiGasSupported8(fadt->resetReg)) {
        return acpiRegisterFromGas(fadt->resetReg);
    }
    return {};
}

// Prefer the 64-bit Generic Address Structure form of the ACPI PM timer when
// the FADT is long enough to contain it and it describes a usable block;
// otherwise fall back to the legacy 32-bit I/O port field. The PM timer is a
// 24/32-bit counter, so request a 32-bit-wide register.
static AcpiRegister fadtPmTimerRegister(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, xPmTimerBlock), sizeof(fadt->xPmTimerBlock)) &&
        acpiGasSupported(fadt->xPmTimerBlock) &&
        (fadt->xPmTimerBlock.bitWidth == 0 || fadt->xPmTimerBlock.bitWidth >= 24)) {
        return acpiRegisterFromGas(fadt->xPmTimerBlock);
    }
    if (fadtHasField(fadt, offsetof(Fadt, pmTimerBlock), sizeof(fadt->pmTimerBlock))) {
        return acpiIoRegister(fadt->pmTimerBlock, 32);
    }
    return {};
}

// GPE (General Purpose Event) blocks drive wake/runtime events for devices such
// as USB controllers and GPIO-attached peripherals, so resolving them via the
// 64-bit GAS where available matters for future power-management work.
static AcpiRegister fadtGpe0Register(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, xGpe0Block), sizeof(fadt->xGpe0Block)) &&
        acpiGasSupported(fadt->xGpe0Block)) {
        return acpiRegisterFromGas(fadt->xGpe0Block);
    }
    if (fadtHasField(fadt, offsetof(Fadt, gpe0Block), sizeof(fadt->gpe0Block)) &&
        fadt->gpe0Block) {
        return acpiIoRegister(fadt->gpe0Block, 8);
    }
    return {};
}

static AcpiRegister fadtGpe1Register(const Fadt* fadt) {
    if (!fadt) {
        return {};
    }
    if (fadtHasField(fadt, offsetof(Fadt, xGpe1Block), sizeof(fadt->xGpe1Block)) &&
        acpiGasSupported(fadt->xGpe1Block)) {
        return acpiRegisterFromGas(fadt->xGpe1Block);
    }
    if (fadtHasField(fadt, offsetof(Fadt, gpe1Block), sizeof(fadt->gpe1Block)) &&
        fadt->gpe1Block) {
        return acpiIoRegister(fadt->gpe1Block, 8);
    }
    return {};
}

static bool acpiWriteRegister8(const AcpiRegister& reg, uint8_t value) {
    if (!reg.valid) {
        return false;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemIo) {
        outb(static_cast<uint16_t>(reg.address), value);
        return true;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemMemory) {
        *reinterpret_cast<volatile uint8_t*>(reg.address) = value;
        return true;
    }
    return false;
}

static bool acpiWriteRegister16(const AcpiRegister& reg, uint16_t value) {
    if (!reg.valid) {
        return false;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemIo) {
        outw(static_cast<uint16_t>(reg.address), value);
        return true;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemMemory) {
        *reinterpret_cast<volatile uint16_t*>(reg.address) = value;
        return true;
    }
    return false;
}

static bool acpiReadRegister32(const AcpiRegister& reg, uint32_t* value) {
    if (!reg.valid || !value) {
        return false;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemIo) {
        *value = inl(static_cast<uint16_t>(reg.address));
        return true;
    }
    if (reg.addressSpace == kAcpiAddressSpaceSystemMemory) {
        *value = *reinterpret_cast<volatile uint32_t*>(reg.address);
        return true;
    }
    return false;
}

static bool fadtHardwareReduced(const Fadt* fadt) {
    return fadt &&
           fadtHasField(fadt, offsetof(Fadt, flags), sizeof(fadt->flags)) &&
           (fadt->flags & kFadtFlagHardwareReducedAcpi) != 0;
}

static void fadtEnableAcpi(const Fadt* fadt) {
    if (!fadt ||
        !fadtHasField(fadt, offsetof(Fadt, smiCommandPort), sizeof(fadt->smiCommandPort)) ||
        !fadtHasField(fadt, offsetof(Fadt, acpiEnable), sizeof(fadt->acpiEnable))) {
        return;
    }

    if (fadt->smiCommandPort && fadt->acpiEnable) {
        outb(static_cast<uint16_t>(fadt->smiCommandPort), fadt->acpiEnable);
        for (int i = 0; i < 3000; i++) asm volatile("pause");
    }
}

static bool fadtEnterFixedSleep(const Fadt* fadt, uint16_t slpTypA, uint16_t slpTypB) {
    const AcpiRegister pm1a = fadtPm1aControlRegister(fadt);
    const AcpiRegister pm1b = fadtPm1bControlRegister(fadt);
    bool wrote = false;

    if (pm1a.valid) {
        wrote |= acpiWriteRegister16(pm1a, static_cast<uint16_t>((slpTypA << 10) | (1 << 13)));
    }
    if (pm1b.valid) {
        wrote |= acpiWriteRegister16(pm1b, static_cast<uint16_t>((slpTypB << 10) | (1 << 13)));
    }

    return wrote;
}

static bool fadtEnterHardwareReducedSleep(const Fadt* fadt, uint16_t slpTyp) {
    const AcpiRegister sleepControl = fadtSleepControlRegister(fadt);
    if (!sleepControl.valid) {
        return false;
    }

    return acpiWriteRegister8(sleepControl, static_cast<uint8_t>(((slpTyp & 0x7) << 2) | (1 << 5)));
}

static void loadAmlTableCallback(const char* signature, void* table, void* context) {
    if (!context || !table || !signature) {
        return;
    }
    if (!acpiSignatureEquals(signature, "SSDT")) {
        return;
    }

    ACPI* acpi = static_cast<ACPI*>(context);
    acpi->aml().loadTable(table);
}

ACPI& ACPI::get() {
    static ACPI instance;
    return instance;
}

bool ACPI::initialize(uint64_t rsdpAddr) {
    if (initialized) {
        return true;
    }
    
    if (!rsdpAddr) return false;

    this->rsdp = reinterpret_cast<void*>(rsdpAddr);
    Rsdp* base = reinterpret_cast<Rsdp*>(rsdpAddr);
    if (!acpiRsdpValid(base)) {
        this->rsdp = nullptr;
        return false;
    }

    this->rsdt = nullptr;
    rootUsesXsdt = false;

    if (base->revision >= 2) {
        Rsdp20* ext = reinterpret_cast<Rsdp20*>(rsdpAddr);
        void* xsdt = reinterpret_cast<void*>(ext->xsdtAddress);
        if (ext->xsdtAddress && acpiRootTableValid(xsdt, true)) {
            this->rsdt = xsdt;
            rootUsesXsdt = true;
        }
    }

    if (!this->rsdt && base->rsdtAddress) {
        void* rsdt = reinterpret_cast<void*>((uint64_t)base->rsdtAddress);
        if (acpiRootTableValid(rsdt, false)) {
            this->rsdt = rsdt;
            rootUsesXsdt = false;
        }
    }

    if (!this->rsdt) {
        this->rsdp = nullptr;
        return false;
    }

    initialized = true;

    parseMcfg();

    if (!amlInitialized && amlInterpreter.initialize()) {
        void* dsdt = findDsdt();
        if (dsdt) {
            amlInterpreter.loadTable(dsdt);
        }
        forEachTable(loadAmlTableCallback, this);
        amlInitialized = true;
    }

    return true;
}

void* ACPI::findTable(const char* signature) {
    if (!initialized || !rsdt || !signature) return nullptr;
    if (!acpiRootTableValid(rsdt, rootUsesXsdt)) return nullptr;

    AcpiHeader* header = reinterpret_cast<AcpiHeader*>(rsdt);
    size_t entries = acpiRootEntryCount(header, rootUsesXsdt);

    if (rootUsesXsdt) {
        Xsdt* xsdtPtr = reinterpret_cast<Xsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            if (!acpiEntryPointerPlausible(xsdtPtr->pointers[i])) {
                continue;
            }
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>(xsdtPtr->pointers[i]);
            if (acpiTableValid(h, signature)) {
                return h;
            }
        }
    } else {
        Rsdt* rsdtPtr = reinterpret_cast<Rsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            if (!acpiEntryPointerPlausible(rsdtPtr->pointers[i])) {
                continue;
            }
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>((uint64_t)rsdtPtr->pointers[i]);
            if (acpiTableValid(h, signature)) {
                return h;
            }
        }
    }

    return nullptr;
}

void ACPI::forEachTable(TableCallback callback, void* context) {
    if (!initialized || !rsdt || !callback) return;
    if (!acpiRootTableValid(rsdt, rootUsesXsdt)) return;

    AcpiHeader* header = reinterpret_cast<AcpiHeader*>(rsdt);
    size_t entries = acpiRootEntryCount(header, rootUsesXsdt);

    // Some firmware lists the same table pointer more than once. De-duplicate so
    // a callback (e.g. AML SSDT loading) does not process a table twice.
    constexpr size_t kMaxSeen = 64;
    uint64_t seen[kMaxSeen];
    size_t seenCount = 0;
    auto alreadySeen = [&](uint64_t addr) -> bool {
        for (size_t j = 0; j < seenCount; ++j) {
            if (seen[j] == addr) {
                return true;
            }
        }
        if (seenCount < kMaxSeen) {
            seen[seenCount++] = addr;
        }
        return false;
    };

    if (rootUsesXsdt) {
        Xsdt* xsdtPtr = reinterpret_cast<Xsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            const uint64_t addr = xsdtPtr->pointers[i];
            if (!acpiEntryPointerPlausible(addr) || alreadySeen(addr)) {
                continue;
            }
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>(addr);
            if (acpiTableValid(h)) {
                callback(h->signature, h, context);
            }
        }
    } else {
        Rsdt* rsdtPtr = reinterpret_cast<Rsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            const uint64_t addr = static_cast<uint64_t>(rsdtPtr->pointers[i]);
            if (!acpiEntryPointerPlausible(addr) || alreadySeen(addr)) {
                continue;
            }
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>(addr);
            if (acpiTableValid(h)) {
                callback(h->signature, h, context);
            }
        }
    }
}

void* ACPI::findDsdt() {
    Fadt* fadt = static_cast<Fadt*>(findTable("FACP"));
    if (!fadt) {
        return nullptr;
    }

    uint64_t dsdtAddr = fadtDsdtAddress(fadt);
    if (!dsdtAddr) {
        return nullptr;
    }

    void* dsdt = reinterpret_cast<void*>(dsdtAddr);
    return acpiTableValid(dsdt, "DSDT") ? dsdt : nullptr;
}

AML::Interpreter& ACPI::aml() {
    return amlInterpreter;
}

void ACPI::parseMcfg() {
    ecamCount = 0;

    Mcfg* mcfg = static_cast<Mcfg*>(findTable("MCFG"));
    if (!mcfg) {
        return;
    }

    const uint32_t headerLength = mcfg->header.length;
    if (headerLength < offsetof(Mcfg, allocations)) {
        return;
    }

    const size_t allocBytes = headerLength - offsetof(Mcfg, allocations);
    const size_t allocations = allocBytes / sizeof(McfgAllocation);

    for (size_t i = 0; i < allocations && ecamCount < kMaxEcamRegions; ++i) {
        const McfgAllocation& entry = mcfg->allocations[i];
        if (!entry.baseAddress || entry.endBus < entry.startBus) {
            continue;
        }

        AcpiEcamRegion& region = ecam[ecamCount];
        region.base = entry.baseAddress;
        region.segment = entry.segmentGroup;
        region.startBus = entry.startBus;
        region.endBus = entry.endBus;

        // Map the ECAM window as uncached MMIO so config reads/writes bypass the
        // CPU cache. Each (bus, device, function) occupies a 4 KiB config page,
        // i.e. 1 MiB per bus.
        const uint32_t busCount = static_cast<uint32_t>(entry.endBus - entry.startBus) + 1;
        const uint64_t bytes = static_cast<uint64_t>(busCount) << 20; // busCount * 1 MiB
        const uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        VMM::MapRange(region.base, region.base, pages,
                      Present | ReadWrite | CacheDisab | WriteThru);

        ++ecamCount;
    }
}

uint64_t ACPI::ecamAddress(uint16_t segment, uint8_t bus, uint8_t device,
                           uint8_t function, uint16_t offset) const {
    if (device >= 32 || function >= 8 || offset >= 4096) {
        return 0;
    }

    for (size_t i = 0; i < ecamCount; ++i) {
        const AcpiEcamRegion& region = ecam[i];
        if (region.segment != segment || bus < region.startBus || bus > region.endBus) {
            continue;
        }

        const uint64_t busOffset = static_cast<uint64_t>(bus - region.startBus);
        return region.base +
            ((busOffset << 20) |
             (static_cast<uint64_t>(device) << 15) |
             (static_cast<uint64_t>(function) << 12) |
             offset);
    }

    return 0;
}

bool ACPI::readPmTimer(uint32_t* outValue) const {
    if (!initialized || !outValue) {
        return false;
    }
    Fadt* fadt = static_cast<Fadt*>(const_cast<ACPI*>(this)->findTable("FACP"));
    return acpiReadRegister32(fadtPmTimerRegister(fadt), outValue);
}

bool ACPI::gpe0Address(uint64_t* outAddress, uint8_t* outAddressSpace) const {
    if (!initialized || !outAddress) {
        return false;
    }
    Fadt* fadt = static_cast<Fadt*>(const_cast<ACPI*>(this)->findTable("FACP"));
    const AcpiRegister reg = fadtGpe0Register(fadt);
    if (!reg.valid) {
        return false;
    }
    *outAddress = reg.address;
    if (outAddressSpace) {
        *outAddressSpace = reg.addressSpace;
    }
    return true;
}

bool ACPI::gpe1Address(uint64_t* outAddress, uint8_t* outAddressSpace) const {
    if (!initialized || !outAddress) {
        return false;
    }
    Fadt* fadt = static_cast<Fadt*>(const_cast<ACPI*>(this)->findTable("FACP"));
    const AcpiRegister reg = fadtGpe1Register(fadt);
    if (!reg.valid) {
        return false;
    }
    *outAddress = reg.address;
    if (outAddressSpace) {
        *outAddressSpace = reg.addressSpace;
    }
    return true;
}

void ACPI::enumerate() {
    if (!initialized) {
        return;
    }
    if (!amlInitialized && amlInterpreter.initialize()) {
        void* dsdt = findDsdt();
        if (dsdt) {
            amlInterpreter.loadTable(dsdt);
        }
        forEachTable(loadAmlTableCallback, this);
        amlInitialized = true;
    }
}

bool ACPI::evaluateAml(const char* path, AML::Object* result) {
    if (!amlInitialized || !path || !result) {
        return false;
    }
    return amlInterpreter.evaluate(path, result);
}

void ACPI::shutdown() {
    rsdp = nullptr;
    rsdt = nullptr;
    rootUsesXsdt = false;
    initialized = false;
    amlInitialized = false;
    ecamCount = 0;
}

void ACPI::reboot() {
    if (!initialized) return;

    Fadt* fadt = static_cast<Fadt*>(findTable("FACP"));
    if (fadt && fadtHasField(fadt, offsetof(Fadt, resetValue), sizeof(fadt->resetValue))) {
        acpiWriteRegister8(fadtResetRegister(fadt), fadt->resetValue);
    }

    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE);

    asm volatile("cli; hlt");
}

void ACPI::sysShutdown() {
    if (!initialized) return;

    Fadt* fadt = static_cast<Fadt*>(findTable("FACP"));
    if (!fadt) {
        outw(0xB004, 0x2000);
        outw(0x604, 0x2000);
        asm volatile("cli; hlt");
        return;
    }

    uint16_t interpretedSlpTypA = 0;
    uint16_t interpretedSlpTypB = 0;
    if (amlInitialized && amlInterpreter.getS5SleepTypes(&interpretedSlpTypA, &interpretedSlpTypB)) {
        fadtEnableAcpi(fadt);

        if (fadtHardwareReduced(fadt)) {
            fadtEnterHardwareReducedSleep(fadt, interpretedSlpTypA);
        } else {
            fadtEnterFixedSleep(fadt, interpretedSlpTypA, interpretedSlpTypB);
        }
    }

    uint64_t dsdtAddr = fadtDsdtAddress(fadt);
    if (dsdtAddr) {
        AcpiHeader* dsdt = reinterpret_cast<AcpiHeader*>(dsdtAddr);
        if (!acpiTableValid(dsdt, "DSDT")) {
            dsdt = nullptr;
        }
        if (!dsdt) {
            outw(0xB004, 0x2000);
            outw(0x604, 0x2000);
            asm volatile("cli; hlt");
            return;
        }

        const uint8_t* body = reinterpret_cast<const uint8_t*>(dsdt) + sizeof(AcpiHeader);
        const uint8_t* end = reinterpret_cast<const uint8_t*>(dsdt) + dsdt->length;

        for (const uint8_t* s5 = body; static_cast<size_t>(end - s5) >= 6; ++s5) {
            if (memcmp(s5, "_S5_", 4) != 0) {
                continue;
            }

            const bool hasNamePrefix =
                (s5 > body && *(s5 - 1) == 0x08) ||
                (s5 >= body + 2 && *(s5 - 2) == 0x08);
            if (!hasNamePrefix || *(s5 + 4) != 0x12) {
                continue;
            }

            const uint8_t* cursor = s5 + 5;
            const size_t pkgLengthBytes = ((*cursor & 0xC0) >> 6) + 1;
            if (pkgLengthBytes > static_cast<size_t>(end - cursor)) {
                continue;
            }
            cursor += pkgLengthBytes;
            if (cursor >= end) {
                continue;
            }
            cursor++;

            if (cursor >= end) {
                continue;
            }
            if (*cursor == 0x0A) {
                cursor++;
            }
            if (cursor >= end) {
                continue;
            }
            uint16_t SLP_TYPa = *cursor & 0x7;
            cursor++;

            if (cursor >= end) {
                continue;
            }
            if (*cursor == 0x0A) {
                cursor++;
            }
            if (cursor >= end) {
                continue;
            }
            uint16_t SLP_TYPb = *cursor & 0x7;

            fadtEnableAcpi(fadt);

            if (fadtHardwareReduced(fadt)) {
                fadtEnterHardwareReducedSleep(fadt, SLP_TYPa);
            } else {
                fadtEnterFixedSleep(fadt, SLP_TYPa, SLP_TYPb);
            }
            break;
        }
    }

    outw(0xB004, 0x2000);
    outw(0x604, 0x2000);
    asm volatile("cli; hlt");
}
