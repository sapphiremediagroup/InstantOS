#include <cpu/acpi/acpi.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>
#include <stddef.h>
#include <cpu/acpi/acpi_tables.hpp>

using namespace acpi;

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

bool ACPI::forEachDevice(AML::Interpreter::DeviceCallback callback, void* context) {
    if (!callback) {
        return false;
    }
    enumerate(); // ensure DSDT + SSDTs are loaded into the namespace
    if (!amlInitialized) {
        return false;
    }
    amlInterpreter.forEachDevice(callback, context);
    return true;
}

AML::NamespaceNode* ACPI::findDeviceByHid(const char* hid) {
    if (!hid) {
        return nullptr;
    }
    enumerate();
    if (!amlInitialized) {
        return nullptr;
    }
    return amlInterpreter.findDeviceByHid(hid);
}

size_t ACPI::readDeviceResources(AML::NamespaceNode* node, AML::AcpiResource* outResources,
                                 size_t maxResources) {
    if (!node || !outResources || maxResources == 0 || !amlInitialized) {
        return 0;
    }
    return amlInterpreter.readDeviceResources(node, outResources, maxResources);
}

void ACPI::shutdown() {
    rsdp = nullptr;
    rsdt = nullptr;
    rootUsesXsdt = false;
    initialized = false;
    amlInitialized = false;
    ecamCount = 0;
}

namespace {
// Gives a reset method time to take effect before the next is tried.
void waitBeforeNextResetMethod() {
    const uint64_t deadline = time_get_uptime_ms() + 100;
    while (time_get_uptime_ms() < deadline) {
        asm volatile("pause");
    }
}
}

// Resets the machine, trying in turn what Linux's reboot tries on x86: the
// FADT's reset register, the keyboard controller's pulse, then the chipset's
// reset control register (0xCF9), and as a last resort a triple fault. A
// machine without a PS/2 controller reads 0xFF from port 0x64, so its wait
// is bounded.
void ACPI::reboot() {
    if (initialized) {
        Fadt* fadt = static_cast<Fadt*>(findTable("FACP"));
        if (fadt && fadtHasField(fadt, offsetof(Fadt, resetValue), sizeof(fadt->resetValue))) {
            acpiWriteRegister8(fadtResetRegister(fadt), fadt->resetValue);
            waitBeforeNextResetMethod();
        }
    }

    for (int tries = 0; tries < 100000 && (inb(0x64) & 0x02); ++tries) {
        asm volatile("pause");
    }
    outb(0x64, 0xFE);
    waitBeforeNextResetMethod();

    outb(0xCF9, 0x02);   // prepare a hard reset...
    outb(0xCF9, 0x06);   // ...and do it (Linux's BOOT_CF9_FORCE)
    waitBeforeNextResetMethod();

    struct [[gnu::packed]] {
        uint16_t limit;
        uint64_t base;
    } noIdt = {0, 0};
    asm volatile("lidt %0; int3" : : "m"(noIdt));
    while (true) {
        asm volatile("cli; hlt");
    }
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
