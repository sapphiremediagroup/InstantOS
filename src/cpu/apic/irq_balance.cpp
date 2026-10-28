#include <cpu/apic/irq_balance.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/smp.hpp>

namespace {

struct Route {
    bool msi;
    bool msix;
    uint8_t vector;
    uint32_t gsi;            // IOAPIC routes
    uint16_t segment;        // MSI routes
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t cap;
    uint64_t msixEntry;      // MSI-X routes: the table entry's address
    uint32_t cpu;            // SMP index it is routed to
};

constexpr uint32_t kMaxRoutes = 32;
Route s_routes[kMaxRoutes] = {};
uint32_t s_routeCount = 0;
bool s_distributed = false;
uint32_t s_nextCpu = 1;

void writeHex(uint64_t value) {
    char buf[19];
    int pos = 18;
    buf[pos] = '\0';
    do {
        const uint8_t digit = value & 0xF;
        buf[--pos] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value >>= 4;
    } while (value != 0 && pos > 2);
    buf[--pos] = 'x';
    buf[--pos] = '0';
    Cereal::get().write(&buf[pos]);
}

// Round-robin over the APs; the boot CPU already takes the PIT tick and the
// idle-time input polling.
uint32_t nextCpu() {
    const uint32_t count = SMP::onlineCount();
    if (count <= 1) return 0;
    if (s_nextCpu >= count) s_nextCpu = 1;
    return s_nextCpu++;
}

uint32_t lapicOf(uint32_t cpu) {
    PerCPUData* data = SMP::cpu(cpu);
    return data ? data->lapicId : 0;
}

void program(Route& route) {
    const uint32_t lapic = lapicOf(route.cpu);
    if (route.msix) {
        PCI::get().setMSIXDestination(route.msixEntry, lapic);
    } else if (route.msi) {
        PCI::get().setMSIDestination(route.segment, route.bus, route.device, route.function,
                                     route.cap, lapic);
    } else {
        APICManager::get().setGSIDestination(route.gsi, lapic);
    }
    Cereal::get().write("[irq] vector=");
    writeHex(route.vector);
    Cereal::get().write(route.msix ? " msi-x -> cpu " : route.msi ? " msi -> cpu " : " ioapic -> cpu ");
    char digit[3] = {static_cast<char>('0' + route.cpu / 10), static_cast<char>('0' + route.cpu % 10), 0};
    Cereal::get().write(route.cpu < 10 ? &digit[1] : digit);
    Cereal::get().write("\n");
}

// Returns the LAPIC ID the caller programs now.
uint32_t add(const Route& route, uint32_t lapicId) {
    // A re-routed interrupt replaces its old record.
    Route* slot = nullptr;
    for (uint32_t i = 0; i < s_routeCount; ++i) {
        Route& existing = s_routes[i];
        if (existing.msi == route.msi && existing.msix == route.msix &&
            (route.msix ? existing.msixEntry == route.msixEntry
             : route.msi ? existing.segment == route.segment && existing.bus == route.bus &&
                               existing.device == route.device && existing.function == route.function
                         : existing.gsi == route.gsi)) {
            slot = &existing;
            break;
        }
    }
    if (route.vector == VECTOR_TIMER) {
        return lapicId;   // the boot CPU's tick stays put
    }
    if (!slot) {
        if (s_routeCount == kMaxRoutes) return lapicId;
        slot = &s_routes[s_routeCount++];
        *slot = route;
        slot->cpu = s_distributed ? nextCpu() : 0;
    } else {
        const uint32_t cpu = slot->cpu;
        *slot = route;
        slot->cpu = cpu;
    }
    return s_distributed ? lapicOf(slot->cpu) : lapicId;
}

} // namespace

namespace IRQBalance {

uint32_t addGsiRoute(uint32_t gsi, uint8_t vector, uint32_t lapicId) {
    Route route {};
    route.vector = vector;
    route.gsi = gsi;
    return add(route, lapicId);
}

uint32_t addMsiRoute(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function,
                     uint16_t cap, uint8_t vector, uint32_t lapicId) {
    Route route {};
    route.msi = true;
    route.vector = vector;
    route.segment = segment;
    route.bus = bus;
    route.device = device;
    route.function = function;
    route.cap = cap;
    return add(route, lapicId);
}

uint32_t addMsixRoute(uint64_t entryAddress, uint8_t vector, uint32_t lapicId) {
    Route route {};
    route.msix = true;
    route.vector = vector;
    route.msixEntry = entryAddress;
    return add(route, lapicId);
}

void distribute() {
    if (s_distributed || SMP::onlineCount() <= 1) return;
    s_distributed = true;
    for (uint32_t i = 0; i < s_routeCount; ++i) {
        s_routes[i].cpu = nextCpu();
        program(s_routes[i]);
    }
}

} // namespace IRQBalance
