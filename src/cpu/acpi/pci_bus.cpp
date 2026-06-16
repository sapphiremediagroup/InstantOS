#include <cpu/acpi/pci_bus.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpu/acpi/aml.hpp>
#include <cpu/cereal/cereal.hpp>

namespace {

constexpr uint16_t PCI_VENDOR_ID = 0x00;
constexpr uint16_t PCI_DEVICE_ID = 0x02;
constexpr uint16_t PCI_REVISION = 0x08;
constexpr uint16_t PCI_PROG_IF = 0x09;
constexpr uint16_t PCI_SUBCLASS = 0x0A;
constexpr uint16_t PCI_CLASS = 0x0B;
constexpr uint16_t PCI_HEADER_TYPE = 0x0E;
constexpr uint16_t PCI_SECONDARY_BUS = 0x19;
constexpr uint16_t PCI_INTERRUPT_LINE = 0x3C;
constexpr uint16_t PCI_INTERRUPT_PIN = 0x3D;

constexpr uint8_t PCI_HEADER_MULTIFUNCTION = 0x80;
constexpr uint8_t PCI_HEADER_TYPE_MASK = 0x7F;
constexpr uint8_t PCI_HEADER_PCI_BRIDGE = 0x01;

constexpr uint8_t PCI_CLASS_BRIDGE = 0x06;
constexpr uint8_t PCI_SUBCLASS_PCI_BRIDGE = 0x04;

// ACPI EISA IDs for PCI/PCIe host bridges that own a _PRT routing table.
constexpr const char* kHostBridgeHidPci = "PNP0A03";
constexpr const char* kHostBridgeHidPcie = "PNP0A08";

constexpr char kPrtName[4] = { '_', 'P', 'R', 'T' };
constexpr char kBbnName[4] = { '_', 'B', 'B', 'N' };
constexpr char kSegName[4] = { '_', 'S', 'E', 'G' };
constexpr char kStaName[4] = { '_', 'S', 'T', 'A' };

bool log_console() { return false; }

void log_str(const char* s) {
    Cereal::get().write(s);
}

void log_hex(uint64_t value) {
    Cereal::get().write("0x");
    char buffer[17];
    int pos = 0;
    if (value == 0) {
        buffer[pos++] = '0';
    } else {
        char temp[16];
        int t = 0;
        while (value && t < 16) {
            const uint8_t nibble = value & 0xF;
            temp[t++] = nibble < 10 ? static_cast<char>('0' + nibble)
                                    : static_cast<char>('A' + (nibble - 10));
            value >>= 4;
        }
        while (t > 0) {
            buffer[pos++] = temp[--t];
        }
    }
    buffer[pos] = 0;
    Cereal::get().write(buffer);
}

void log_dec(uint64_t value) {
    char buffer[21];
    int pos = 0;
    if (value == 0) {
        buffer[pos++] = '0';
    } else {
        char temp[20];
        int t = 0;
        while (value && t < 20) {
            temp[t++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (t > 0) {
            buffer[pos++] = temp[--t];
        }
    }
    buffer[pos] = 0;
    Cereal::get().write(buffer);
}

// Translate the polarity/trigger of an ACPI interrupt resource into the IOAPIC
// redirection-entry flag word expected by APICManager::mapGSI. The MADT/IOAPIC
// flag encoding uses bits[1:0]=polarity (1=high,3=low) and bits[3:2]=trigger
// (1=edge,3=level).
uint16_t iaApicFlags(bool activeLow, bool levelTriggered) {
    uint16_t flags = 0;
    flags |= activeLow ? 0x3 : 0x1;
    flags |= levelTriggered ? (0x3 << 2) : (0x1 << 2);
    return flags;
}

}

PciBus& PciBus::get() {
    static PciBus instance;
    return instance;
}

size_t PciBus::scan() {
    count = 0;
    routeCount = 0;

    // Resolve INTx routing from ACPI before walking config space so newly
    // discovered functions can be matched against the _PRT tables.
    buildRoutingTables();

    ACPI& acpi = ACPI::get();
    if (acpi.hasEcam()) {
        const AcpiEcamRegion* regions = acpi.ecamRegions();
        const size_t regionCount = acpi.ecamRegionCount();
        for (size_t i = 0; i < regionCount; ++i) {
            scanSegment(regions[i].segment, regions[i].startBus);
        }
    } else {
        // No MCFG: legacy single-segment machine. Probe bus 0 and recurse.
        scanSegment(0, 0);
    }

    log_str("[pci] scan complete devices=");
    log_dec(count);
    log_str(" routes=");
    log_dec(routeCount);
    log_str("\n");
    return count;
}

void PciBus::scanSegment(uint16_t segment, uint8_t startBus) {
    // Start at the segment's first bus. Bridges encountered during the walk add
    // their subordinate buses recursively, so a single entry point suffices for
    // typical topologies. We also defensively scan the configured start bus.
    scanBus(segment, startBus);
}

void PciBus::scanBus(uint16_t segment, uint8_t bus) {
    for (uint8_t device = 0; device < 32; ++device) {
        const uint16_t vendor = PCI::get().readConfig16(segment, bus, device, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) {
            continue;
        }

        const uint8_t headerType = PCI::get().readConfig8(segment, bus, device, 0, PCI_HEADER_TYPE);
        const uint8_t functions = (headerType & PCI_HEADER_MULTIFUNCTION) ? 8 : 1;
        for (uint8_t function = 0; function < functions; ++function) {
            scanFunction(segment, bus, device, function);
        }
    }
}

void PciBus::scanFunction(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) {
    const uint16_t vendor = PCI::get().readConfig16(segment, bus, device, function, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) {
        return;
    }

    PciDeviceInfo info;
    info.segment = segment;
    info.bus = bus;
    info.device = device;
    info.function = function;
    info.vendorId = vendor;
    info.deviceId = PCI::get().readConfig16(segment, bus, device, function, PCI_DEVICE_ID);
    info.revision = PCI::get().readConfig8(segment, bus, device, function, PCI_REVISION);
    info.progIf = PCI::get().readConfig8(segment, bus, device, function, PCI_PROG_IF);
    info.subclass = PCI::get().readConfig8(segment, bus, device, function, PCI_SUBCLASS);
    info.classCode = PCI::get().readConfig8(segment, bus, device, function, PCI_CLASS);
    info.headerType = PCI::get().readConfig8(segment, bus, device, function, PCI_HEADER_TYPE);
    info.interruptPin = PCI::get().readConfig8(segment, bus, device, function, PCI_INTERRUPT_PIN);
    info.interruptLine = PCI::get().readConfig8(segment, bus, device, function, PCI_INTERRUPT_LINE);
    info.hasEcam = ACPI::get().ecamAddress(segment, bus, device, function, 0) != 0;

    if (count < kMaxDevices) {
        entries[count++] = info;
    }

    log_str("[pci] ");
    log_dec(segment);
    log_str(":");
    log_dec(bus);
    log_str(":");
    log_dec(device);
    log_str(".");
    log_dec(function);
    log_str(" vendor=");
    log_hex(info.vendorId);
    log_str(" device=");
    log_hex(info.deviceId);
    log_str(" class=");
    log_hex(info.classCode);
    log_str("/");
    log_hex(info.subclass);
    log_str("/");
    log_hex(info.progIf);
    log_str("\n");

    // Recurse behind PCI-to-PCI bridges so devices on downstream buses are
    // discovered without assuming a flat bus 0 topology.
    if ((info.headerType & PCI_HEADER_TYPE_MASK) == PCI_HEADER_PCI_BRIDGE &&
        info.classCode == PCI_CLASS_BRIDGE && info.subclass == PCI_SUBCLASS_PCI_BRIDGE) {
        const uint8_t secondaryBus = PCI::get().readConfig8(segment, bus, device, function,
                                                            PCI_SECONDARY_BUS);
        if (secondaryBus != 0 && secondaryBus != bus) {
            scanBus(segment, secondaryBus);
        }
    }
}

void PciBus::buildRoutingTables() {
    ACPI& acpi = ACPI::get();
    AML::Interpreter& aml = acpi.aml();
    if (!aml.isInitialized()) {
        // Force the namespace to load (DSDT + SSDTs).
        acpi.enumerate();
        if (!aml.isInitialized()) {
            return;
        }
    }

    // Find a host bridge node (PNP0A08 preferred, then PNP0A03) and read its
    // _PRT. Only the root bridge's table is parsed here; this covers the common
    // single-root-complex case used by QEMU and most laptops.
    AML::NamespaceNode* bridge = acpi.findDeviceByHid(kHostBridgeHidPcie);
    if (!bridge) {
        bridge = acpi.findDeviceByHid(kHostBridgeHidPci);
    }
    if (!bridge) {
        return;
    }

    // Determine the bridge's segment and base bus number.
    uint16_t segment = 0;
    uint8_t baseBus = 0;
    {
        AML::Object segObj;
        uint64_t value = 0;
        if (aml.evaluateDeviceObject(bridge, kSegName, &segObj)) {
            if (segObj.type == AML::ObjectType::Integer) {
                segment = static_cast<uint16_t>(segObj.integer);
            }
        }
        AML::Object bbnObj;
        if (aml.evaluateDeviceObject(bridge, kBbnName, &bbnObj)) {
            if (bbnObj.type == AML::ObjectType::Integer) {
                baseBus = static_cast<uint8_t>(bbnObj.integer);
            }
        }
        (void)value;
    }

    AML::Object prt;
    if (!aml.evaluateDeviceObject(bridge, kPrtName, &prt)) {
        return;
    }
    if (prt.type != AML::ObjectType::Package || !prt.elements) {
        return;
    }

    for (size_t i = 0; i < prt.elementCount && routeCount < 256; ++i) {
        const AML::Object& entry = prt.elements[i];
        if (entry.type != AML::ObjectType::Package || !entry.elements || entry.elementCount < 4) {
            continue;
        }

        const AML::Object& addrObj = entry.elements[0];
        const AML::Object& pinObj = entry.elements[1];
        const AML::Object& sourceObj = entry.elements[2];
        const AML::Object& indexObj = entry.elements[3];

        if (addrObj.type != AML::ObjectType::Integer || pinObj.type != AML::ObjectType::Integer) {
            continue;
        }

        PrtEntry route{};
        route.segment = segment;
        route.bus = baseBus;
        route.device = static_cast<uint8_t>((addrObj.integer >> 16) & 0xFFFF);
        route.pin = static_cast<uint8_t>(pinObj.integer & 0x3);
        route.flags = iaApicFlags(/*activeLow=*/true, /*levelTriggered=*/true); // PCI INTx default
        route.valid = false;

        if (sourceObj.type == AML::ObjectType::Integer && sourceObj.integer == 0) {
            // Source == 0: SourceIndex is the GSI directly.
            if (indexObj.type == AML::ObjectType::Integer) {
                route.gsi = static_cast<uint32_t>(indexObj.integer);
                route.valid = true;
            }
        } else if (sourceObj.type == AML::ObjectType::String && sourceObj.string) {
            // Source names an interrupt link device (PNP0C0F). Resolve its _CRS
            // to obtain the GSI it is currently programmed to.
            AML::NamespaceNode* link = aml.resolvePath(sourceObj.string, bridge);
            if (link) {
                AML::AcpiResource resources[4];
                const size_t n = aml.readNodeResources(link, resources, 4);
                for (size_t r = 0; r < n; ++r) {
                    if ((resources[r].kind == AML::ResourceKind::Irq) &&
                        resources[r].interruptCount > 0) {
                        route.gsi = resources[r].interrupts[0];
                        route.flags = iaApicFlags(resources[r].activeLow,
                                                  resources[r].levelTriggered);
                        route.valid = true;
                        break;
                    }
                }
            }
        }

        if (route.valid) {
            routes[routeCount++] = route;
        }
    }
}

void PciBus::forEachDevice(DeviceCallback callback, void* context) const {
    if (!callback) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!callback(entries[i], context)) {
            return;
        }
    }
}

const PciDeviceInfo* PciBus::findByClass(uint8_t classCode, uint8_t subclass, uint8_t progIf) const {
    for (size_t i = 0; i < count; ++i) {
        const PciDeviceInfo& info = entries[i];
        if ((classCode == 0xFF || info.classCode == classCode) &&
            (subclass == 0xFF || info.subclass == subclass) &&
            (progIf == 0xFF || info.progIf == progIf)) {
            return &info;
        }
    }
    return nullptr;
}

bool PciBus::resolveInterruptGsi(const PciDeviceInfo& info, uint32_t* outGsi,
                                 uint16_t* outFlags) const {
    if (!outGsi || info.interruptPin == 0) {
        return false;
    }

    // _PRT routing is expressed per PCI device (function ignored) and per INTx
    // pin. PCIe functions behind the same device share the device's INTx pin
    // mapping after the standard swizzle.
    const uint8_t pin = static_cast<uint8_t>((info.interruptPin - 1) & 0x3);
    for (size_t i = 0; i < routeCount; ++i) {
        const PrtEntry& route = routes[i];
        if (route.segment == info.segment && route.bus == info.bus &&
            route.device == info.device && route.pin == pin) {
            *outGsi = route.gsi;
            if (outFlags) {
                *outFlags = route.flags;
            }
            return true;
        }
    }
    return false;
}
