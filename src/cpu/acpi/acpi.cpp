#include <cpu/acpi/acpi.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <memory/vmm.hpp>

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
    uint16_t bootArchitectureFlags;
    uint8_t reserved2;
    uint32_t flags;
    GenericAddressStructure resetReg;
    uint8_t resetValue;
    uint8_t reserved3[3];
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
} __attribute__((packed));

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

    if (base->revision >= 2) {
        Rsdp20* ext = reinterpret_cast<Rsdp20*>(rsdpAddr);
        this->rsdt = reinterpret_cast<void*>(ext->xsdtAddress);
    } else {
        this->rsdt = reinterpret_cast<void*>((uint64_t)base->rsdtAddress);
    }

    initialized = true;
    return true;
}

void* ACPI::findTable(const char* signature) {
    if (!initialized || !rsdt) return nullptr;

    Rsdp* base = reinterpret_cast<Rsdp*>(rsdp);
    bool useXsdt = (base->revision >= 2);

    AcpiHeader* header = reinterpret_cast<AcpiHeader*>(rsdt);
    size_t entries = (header->length - sizeof(AcpiHeader)) / (useXsdt ? 8 : 4);

    if (useXsdt) {
        Xsdt* xsdtPtr = reinterpret_cast<Xsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>(xsdtPtr->pointers[i]);
            if (strncmp(h->signature, signature, 4) == 0) {
                return h;
            }
        }
    } else {
        Rsdt* rsdtPtr = reinterpret_cast<Rsdt*>(rsdt);
        for (size_t i = 0; i < entries; i++) {
            AcpiHeader* h = reinterpret_cast<AcpiHeader*>((uint64_t)rsdtPtr->pointers[i]);
            if (strncmp(h->signature, signature, 4) == 0) {
                return h;
            }
        }
    }

    return nullptr;
}

void ACPI::shutdown() {
    initialized = false;
}

void ACPI::reboot() {
    if (!initialized) return;

    Fadt* fadt = static_cast<Fadt*>(findTable("FACP"));
    if (fadt && (fadt->header.revision >= 2)) {
        if (fadt->resetReg.addressSpace == 1) {
            outb(fadt->resetReg.address, fadt->resetValue);
        } else if (fadt->resetReg.addressSpace == 0) {
            uint64_t addr = fadt->resetReg.address;
            *reinterpret_cast<volatile uint8_t*>(addr) = fadt->resetValue;
        }
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

    uint64_t dsdtAddr = fadt->dsdt;
    if (fadt->header.revision >= 2 && fadt->xDsdt) {
        dsdtAddr = fadt->xDsdt;
    }

    if (dsdtAddr) {
        AcpiHeader* dsdt = reinterpret_cast<AcpiHeader*>(dsdtAddr);
        uint8_t* S5Addr = reinterpret_cast<uint8_t*>(dsdt) + sizeof(AcpiHeader);
        int dsdtLength = dsdt->length - sizeof(AcpiHeader);

        while (0 < dsdtLength--) {
            if (memcmp(S5Addr, "_S5_", 4) == 0) {
                break;
            }
            S5Addr++;
        }

        if (dsdtLength > 0) {
            if ((*(S5Addr - 1) == 0x08 || *(S5Addr - 2) == 0x08) && *(S5Addr + 4) == 0x12) {
                S5Addr += 5;
                S5Addr += ((*S5Addr & 0xC0) >> 6) + 2;

                if (*S5Addr == 0x0A) S5Addr++;
                uint16_t SLP_TYPa = *(S5Addr) << 10;
                S5Addr++;

                if (*S5Addr == 0x0A) S5Addr++;
                uint16_t SLP_TYPb = *(S5Addr) << 10;

                uint32_t SMI_CMD = fadt->smiCommandPort;
                uint8_t ACPI_ENABLE = fadt->acpiEnable;
                uint32_t PM1a_CNT = fadt->pm1aControlBlock;
                uint32_t PM1b_CNT = fadt->pm1bControlBlock;

                outb(SMI_CMD, ACPI_ENABLE);
                for (int i = 0; i < 3000; i++) asm volatile("pause");

                outw(PM1a_CNT, SLP_TYPa | (1 << 13));
                if (PM1b_CNT != 0) {
                    outw(PM1b_CNT, SLP_TYPb | (1 << 13));
                }
            }
        }
    }

    outw(0xB004, 0x2000);
    outw(0x604, 0x2000);
    asm volatile("cli; hlt");
}