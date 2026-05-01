#include <cpu/apic/apic.hpp>
#include <memory/vmm.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpu/cereal/cereal.hpp>

namespace {
void traceStr(const char* text) {
  Cereal::get().write(text);
}

void traceDec(uint64_t value) {
  char buffer[21];
  int pos = 0;
  if (value == 0) {
    Cereal::get().write('0');
    return;
  }
  while (value > 0 && pos < static_cast<int>(sizeof(buffer))) {
    buffer[pos++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  while (pos > 0) {
    Cereal::get().write(buffer[--pos]);
  }
}

void traceHex(uint64_t value) {
  static constexpr char digits[] = "0123456789abcdef";
  Cereal::get().write("0x");
  for (int shift = 60; shift >= 0; shift -= 4) {
    Cereal::get().write(digits[(value >> shift) & 0xFULL]);
  }
}
}

LAPIC &LAPIC::get() {
  static LAPIC instance;
  return instance;
}

bool LAPIC::initialize() {
  uint32_t eax, edx;
  asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0x1B));
  uint64_t apic_base_msr = (static_cast<uint64_t>(edx) << 32) | eax;

  apic_base_msr |= (1 << 11);

  eax = apic_base_msr & 0xFFFFFFFF;
  edx = apic_base_msr >> 32;
  asm volatile("wrmsr" ::"a"(eax), "d"(edx), "c"(0x1B));

  void* table = ACPI::get().findTable("APIC");
  if (!table) {
    return false;
  }

  struct madt_header {
    uint32_t lapic_address;
    uint32_t flags;
  } __attribute__((packed));

  auto *madt = reinterpret_cast<madt_header *>(
      reinterpret_cast<uint8_t *>(table) + 36);

  uint64_t lapic_phys = madt->lapic_address;
  uint64_t lapic_virt = lapic_phys; // identity map

  VMM::MapPage(lapic_virt, lapic_phys,
          PageFlags::Present | PageFlags::ReadWrite | PageFlags::CacheDisab | PageFlags::WriteThru | PageFlags::NoExecute);

  base = reinterpret_cast<volatile uint32_t *>(lapic_virt);
  initialized = true;

  return true;
}

void LAPIC::enable() {
  if (!initialized)
    return;

  write(0x80, 0);

  uint32_t spurious = read(LAPIC_SPURIOUS);
  spurious |= 0x100;
  spurious |= 0xFF;
  write(LAPIC_SPURIOUS, spurious);
}

void LAPIC::sendEOI() { write(LAPIC_EOI, 0); }

void LAPIC::sendIPI(uint32_t lapicId, uint32_t vector, uint32_t deliveryMode,
                    uint32_t level, uint32_t trigger) {
  write(LAPIC_ICR_HIGH, lapicId << 24);

  uint32_t icrLow =
      vector | (deliveryMode << 8) | (level << 14) | (trigger << 15);
  write(LAPIC_ICR_LOW, icrLow);

  // Wait for delivery status bit (bit 12) to clear
  while (read(LAPIC_ICR_LOW) & (1 << 12))
    ;
}

void LAPIC::sendInitIPI(uint32_t lapicId) {
  sendIPI(lapicId, 0, 5, 1, 1); // Delivery Mode 5 = INIT
}

void LAPIC::sendStartupIPI(uint32_t lapicId, uint8_t vector) {
  sendIPI(lapicId, vector, 6, 1, 0); // Delivery Mode 6 = Startup
}

uint32_t LAPIC::getId() {
  if (!initialized)
    return 0;
  return read(LAPIC_ID) >> 24;
}

void LAPIC::setTimerDivide(uint8_t divide) {
  if (!initialized)
    return;
  write(LAPIC_TIMER_DIV, divide);
}

void LAPIC::startTimer(uint32_t initialCount, uint8_t vector, bool periodic) {
  if (!initialized) {
    return;
  }

  uint32_t mode = periodic ? 0x20000 : 0;
  uint32_t lvt = vector | mode;
  write(LAPIC_TIMER, lvt);
  write(LAPIC_TIMER_INITCNT, initialCount);
}

uint32_t LAPIC::read(uint32_t reg) { return base[reg / 4]; }

void LAPIC::write(uint32_t reg, uint32_t value) {
  base[reg / 4] = value;
  asm volatile("mfence" ::: "memory");
}

void IOAPIC::initialize(uint64_t physAddr, uint32_t gsiBaseIn) {
  this->gsiBase = gsiBaseIn;
  uint64_t virt = physAddr;

  VMM::MapPage(virt, physAddr,
          PageFlags::Present | PageFlags::ReadWrite | PageFlags::CacheDisab | PageFlags::WriteThru | PageFlags::NoExecute);

  base = reinterpret_cast<volatile uint32_t *>(virt);
}

void IOAPIC::setRedirect(uint8_t irq, uint8_t vector, uint32_t lapicId,
                         bool masked, uint16_t flags) {
  uint64_t entry = vector;

  entry |= (0ULL << 8);  // Fixed
  entry |= (0ULL << 11); // Physical

  // Polarity: Bit 13 (0=High, 1=Low)
  // MADT Flags: Bits 0-1 (1=High, 3=Low)
  if ((flags & 3) == 3)
    entry |= (1ULL << 13);

  // Trigger Mode: Bit 15 (0=Edge, 1=Level)
  // MADT Flags: Bits 2-3 (1=Edge, 3=Level)
  if (((flags >> 2) & 3) == 3)
    entry |= (1ULL << 15);

  if (masked) {
    entry |= (1ULL << 16);
  }

  entry |= (static_cast<uint64_t>(lapicId) << 56);

  uint32_t reg = IOAPIC_REG_REDTBL + (irq * 2);
  write(reg, static_cast<uint32_t>(entry));
  write(reg + 1, static_cast<uint32_t>(entry >> 32));
}

void IOAPIC::maskIRQ(uint8_t irq) {
  uint32_t reg = IOAPIC_REG_REDTBL + (irq * 2);
  uint32_t low = read(reg);
  write(reg, low | (1 << 16));
}

void IOAPIC::unmaskIRQ(uint8_t irq) {
  uint32_t reg = IOAPIC_REG_REDTBL + (irq * 2);
  uint32_t low = read(reg);
  write(reg, low & ~(1 << 16));
}

uint32_t IOAPIC::getMaxRedirect() {
  uint32_t ver = read(IOAPIC_REG_VER);
  return (ver >> 16) & 0xFF;
}

void IOAPIC::write(uint8_t reg, uint32_t value) {
  base[IOAPIC_REGSEL / 4] = reg;
  base[IOAPIC_IOWIN / 4] = value;
}

uint32_t IOAPIC::read(uint8_t reg) {
  base[IOAPIC_REGSEL / 4] = reg;
  return base[IOAPIC_IOWIN / 4];
}

APICManager &APICManager::get() {
  static APICManager instance;
  return instance;
}

bool APICManager::initialize() {
  if (initialized)
    return true;

  if (!LAPIC::get().initialize()) {
    return false;
  }

  LAPIC::get().enable();

  void* table = ACPI::get().findTable("APIC");

  if (!table) {
    return false;
  }

  uint32_t table_length = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(table) + 4);

      uint8_t *madt_entries = reinterpret_cast<uint8_t *>(table) + 44;
  uint8_t *madt_end =
      reinterpret_cast<uint8_t *>(table) + table_length;

  while (madt_entries < madt_end) {
    uint8_t type = madt_entries[0];
    uint8_t length = madt_entries[1];

    if (type == 0 && cpuCount < 16) {
      struct lapic_entry {
        uint8_t type;
        uint8_t length;
        uint8_t processor_id;
        uint8_t apic_id;
        uint32_t flags;
      } __attribute__((packed));

      auto *entry = reinterpret_cast<lapic_entry *>(madt_entries);
      if (entry->flags & 1) { // Enabled
        apicIds[cpuCount++] = entry->apic_id;
      }
    } else if (type == 1 && ioapicCount < 16) {
      struct ioapic_entry {
        uint8_t type;
        uint8_t length;
        uint8_t id;
        uint8_t reserved;
        uint32_t address;
        uint32_t gsi_base;
      } __attribute__((packed));

      auto *entry = reinterpret_cast<ioapic_entry *>(madt_entries);
      ioapics[ioapicCount].initialize(entry->address, entry->gsi_base);
      traceStr("[apic] ioapic id=");
      traceDec(entry->id);
      traceStr(" addr=");
      traceHex(entry->address);
      traceStr(" gsi_base=");
      traceDec(entry->gsi_base);
      traceStr("\n");
      ioapicCount++;

    } else if (type == 2 && overrideCount < 16) {
      struct iso_entry {
        uint8_t type;
        uint8_t length;
        uint8_t bus;
        uint8_t source;
        uint32_t gsi;
        uint16_t flags;
      } __attribute__((packed));

      auto *entry = reinterpret_cast<iso_entry *>(madt_entries);
      overrides[overrideCount].source = entry->source;
      overrides[overrideCount].gsi = entry->gsi;
      overrides[overrideCount].flags = entry->flags;
      traceStr("[apic] iso source=");
      traceDec(entry->source);
      traceStr(" gsi=");
      traceDec(entry->gsi);
      traceStr(" flags=");
      traceHex(entry->flags);
      traceStr("\n");
      overrideCount++;
    }

    madt_entries += length;
  }

  initialized = true;
  return true;
}

uint32_t APICManager::resolveIRQ(uint8_t irq) {
  for (uint8_t i = 0; i < overrideCount; i++) {
    if (overrides[i].source == irq) {
      return overrides[i].gsi;
    }
  }
  return irq;
}

void APICManager::mapIRQ(uint8_t irq, uint8_t vector, uint32_t dest) {
  uint32_t gsi = resolveIRQ(irq);
  uint16_t flags = getFlags(irq);
  mapGSI(gsi, vector, dest, flags);
}

bool APICManager::mapGSI(uint32_t gsi, uint8_t vector, uint32_t dest, uint16_t flags) {
  IOAPIC *ioapic = getIOAPICForGSI(gsi);
  if (!ioapic) {
    traceStr("[apic] mapGSI failed gsi=");
    traceDec(gsi);
    traceStr("\n");
    return false;
  }

  uint32_t lapicId = dest;
  uint8_t redirectIndex = gsi - ioapic->getGSIBase();
  traceStr("[apic] mapGSI gsi=");
  traceDec(gsi);
  traceStr(" index=");
  traceDec(redirectIndex);
  traceStr(" vector=");
  traceHex(vector);
  traceStr(" dest=");
  traceDec(lapicId);
  traceStr(" flags=");
  traceHex(flags);
  traceStr("\n");

  ioapic->setRedirect(redirectIndex, vector, lapicId, false, flags);
  return true;
}

uint16_t APICManager::getFlags(uint8_t irq) {
  for (uint8_t i = 0; i < overrideCount; i++) {
    if (overrides[i].source == irq) {
      return overrides[i].flags;
    }
  }
  return 0; // Default: Active High, Edge Triggered
}

IOAPIC *APICManager::getIOAPICForGSI(uint32_t gsi) {
  for (uint8_t i = 0; i < ioapicCount; i++) {
    uint32_t base = ioapics[i].getGSIBase();
    uint32_t max = base + ioapics[i].getMaxRedirect();

    if (gsi >= base && gsi <= max) {
      return &ioapics[i];
    }
  }

  return nullptr;
}
