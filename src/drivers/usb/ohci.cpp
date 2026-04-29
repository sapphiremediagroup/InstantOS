#include <drivers/usb/ohci.hpp>

#include <common/string.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/cereal/cereal.hpp>
#include <interrupts/keyboard.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>

namespace {
constexpr uint16_t PCI_COMMAND = 0x04;
constexpr uint16_t PCI_PROG_IF = 0x09;
constexpr uint16_t PCI_SUBCLASS = 0x0A;
constexpr uint16_t PCI_CLASS = 0x0B;
constexpr uint16_t PCI_BAR0 = 0x10;

constexpr uint8_t PCI_CLASS_SERIAL_BUS = 0x0C;
constexpr uint8_t PCI_SUBCLASS_USB = 0x03;
constexpr uint8_t PCI_PROGIF_UHCI = 0x00;
constexpr uint8_t PCI_PROGIF_OHCI = 0x10;
constexpr uint8_t PCI_PROGIF_EHCI = 0x20;
constexpr uint8_t PCI_PROGIF_XHCI = 0x30;

constexpr uint16_t PCI_COMMAND_IO = 1 << 0;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

constexpr uint32_t OHCI_REVISION = 0x00;
constexpr uint32_t OHCI_CONTROL = 0x04;
constexpr uint32_t OHCI_COMMAND_STATUS = 0x08;
constexpr uint32_t OHCI_INTERRUPT_STATUS = 0x0C;
constexpr uint32_t OHCI_INTERRUPT_ENABLE = 0x10;
constexpr uint32_t OHCI_INTERRUPT_DISABLE = 0x14;
constexpr uint32_t OHCI_HCCA = 0x18;
constexpr uint32_t OHCI_CONTROL_HEAD_ED = 0x20;
constexpr uint32_t OHCI_BULK_HEAD_ED = 0x28;
constexpr uint32_t OHCI_DONE_HEAD = 0x30;
constexpr uint32_t OHCI_FM_INTERVAL = 0x34;
constexpr uint32_t OHCI_PERIODIC_START = 0x40;
constexpr uint32_t OHCI_RH_DESCRIPTOR_A = 0x48;
constexpr uint32_t OHCI_RH_STATUS = 0x50;
constexpr uint32_t OHCI_RH_PORT_STATUS_BASE = 0x54;

constexpr uint32_t OHCI_CONTROL_PLE = 1 << 2;
constexpr uint32_t OHCI_CONTROL_CLE = 1 << 4;
constexpr uint32_t OHCI_CONTROL_HCFS_OPERATIONAL = 2 << 6;
constexpr uint32_t OHCI_CONTROL_IR = 1 << 8;

constexpr uint32_t OHCI_CMD_HCR = 1 << 0;
constexpr uint32_t OHCI_CMD_CLF = 1 << 1;
constexpr uint32_t OHCI_CMD_OCR = 1 << 3;

constexpr uint32_t OHCI_INT_WDH = 1 << 1;
constexpr uint32_t OHCI_INT_RHSC = 1 << 6;
constexpr uint32_t OHCI_INT_MIE = 1U << 31;

constexpr uint32_t OHCI_PORT_CCS = 1 << 0;
constexpr uint32_t OHCI_PORT_PES = 1 << 1;
constexpr uint32_t OHCI_PORT_PRS = 1 << 4;
constexpr uint32_t OHCI_PORT_PPS = 1 << 8;
constexpr uint32_t OHCI_PORT_LSDA = 1 << 9;
constexpr uint32_t OHCI_PORT_CSC = 1 << 16;
constexpr uint32_t OHCI_PORT_PESC = 1 << 17;
constexpr uint32_t OHCI_PORT_PSSC = 1 << 18;
constexpr uint32_t OHCI_PORT_OCIC = 1 << 19;
constexpr uint32_t OHCI_PORT_PRSC = 1 << 20;
constexpr uint32_t OHCI_PORT_CHANGE_MASK =
    OHCI_PORT_CSC | OHCI_PORT_PESC | OHCI_PORT_PSSC | OHCI_PORT_OCIC | OHCI_PORT_PRSC;

constexpr uint32_t OHCI_ED_LOW_SPEED = 1 << 13;
constexpr uint32_t OHCI_TD_ROUNDING = 1 << 18;
constexpr uint32_t OHCI_TD_PID_SETUP = 0 << 19;
constexpr uint32_t OHCI_TD_PID_OUT = 1 << 19;
constexpr uint32_t OHCI_TD_PID_IN = 2 << 19;
constexpr uint32_t OHCI_TD_NO_INTERRUPT = 7 << 21;
constexpr uint32_t OHCI_TD_DATA0 = 2 << 24;
constexpr uint32_t OHCI_TD_DATA1 = 3 << 24;
constexpr uint32_t OHCI_TD_TOGGLE_FROM_ED = 0 << 24;
constexpr uint32_t OHCI_TD_CC_NOT_ACCESSED = 0xFU << 28;

constexpr uint8_t USB_REQUEST_GET_DESCRIPTOR = 0x06;
constexpr uint8_t USB_REQUEST_SET_ADDRESS = 0x05;
constexpr uint8_t USB_REQUEST_SET_CONFIGURATION = 0x09;
constexpr uint8_t USB_REQUEST_SET_IDLE = 0x0A;
constexpr uint8_t USB_REQUEST_SET_PROTOCOL = 0x0B;
constexpr uint8_t USB_DESCRIPTOR_DEVICE = 0x01;
constexpr uint8_t USB_DESCRIPTOR_CONFIGURATION = 0x02;
constexpr uint8_t USB_DESCRIPTOR_INTERFACE = 0x04;
constexpr uint8_t USB_DESCRIPTOR_ENDPOINT = 0x05;

constexpr uint8_t USB_CLASS_HID = 0x03;
constexpr uint8_t USB_HID_SUBCLASS_BOOT = 0x01;
constexpr uint8_t USB_HID_PROTOCOL_KEYBOARD = 0x01;
constexpr uint8_t USB_ENDPOINT_IN = 0x80;
constexpr uint8_t USB_ENDPOINT_TYPE_INTERRUPT = 0x03;

struct UsbSetupPacket {
    uint8_t requestType;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

struct OHCIEndpointDescriptor {
    volatile uint32_t control;
    volatile uint32_t tailP;
    volatile uint32_t headP;
    volatile uint32_t nextED;
} __attribute__((packed, aligned(16)));

struct OHCITransferDescriptor {
    volatile uint32_t control;
    volatile uint32_t cbp;
    volatile uint32_t nextTD;
    volatile uint32_t be;
} __attribute__((packed, aligned(16)));

struct OHCIHCCA {
    volatile uint32_t interruptTable[32];
    volatile uint16_t frameNumber;
    volatile uint16_t pad1;
    volatile uint32_t doneHead;
    uint8_t reserved[116];
} __attribute__((packed, aligned(256)));

void io_wait() {
    asm volatile("pause");
}

void spin_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        io_wait();
    }
}

void log_str(const char* text) {
    Cereal::get().write(text);
}

void log_hex(uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    Cereal::get().write("0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble || started || shift == 0) {
            started = true;
            Cereal::get().write(digits[nibble]);
        }
    }
}

void log_dec(uint64_t value) {
    char buf[21];
    int pos = 0;
    if (value == 0) {
        Cereal::get().write('0');
        return;
    }
    while (value && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (pos > 0) {
        Cereal::get().write(buf[--pos]);
    }
}

uint32_t phys32(void* ptr) {
    uint64_t phys = VMM::VirtualToPhysical(reinterpret_cast<uint64_t>(ptr));
    if (phys == 0) {
        phys = reinterpret_cast<uint64_t>(ptr);
    }
    if (phys > 0xFFFFFFFFULL) {
        return 0;
    }
    return static_cast<uint32_t>(phys);
}

void* alloc_dma_page() {
    uint64_t phys = PMM::AllocFrame();
    if (!phys) {
        return nullptr;
    }
    void* ptr = reinterpret_cast<void*>(phys);
    memset(ptr, 0, PMM::PAGE_SIZE);
    return ptr;
}

uint32_t td_cc(const OHCITransferDescriptor* td) {
    return (td->control >> 28) & 0xF;
}

bool td_ok(const OHCITransferDescriptor* td) {
    uint32_t cc = td_cc(td);
    return cc == 0 || cc == 0xD;
}

char hid_usage_to_char(uint8_t usage, bool shift) {
    static constexpr char normal[] = {
        0, 0, 0, 0,
        'a','b','c','d','e','f','g','h','i','j','k','l','m',
        'n','o','p','q','r','s','t','u','v','w','x','y','z',
        '1','2','3','4','5','6','7','8','9','0',
        '\n', 0, '\b', '\t', ' ', '-', '=', '[', ']', '\\',
        0, ';', '\'', '`', ',', '.', '/'
    };
    static constexpr char shifted[] = {
        0, 0, 0, 0,
        'A','B','C','D','E','F','G','H','I','J','K','L','M',
        'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        '!','@','#','$','%','^','&','*','(',')',
        '\n', 0, '\b', '\t', ' ', '_', '+', '{', '}', '|',
        0, ':', '"', '~', '<', '>', '?'
    };

    if (usage >= sizeof(normal)) {
        return 0;
    }
    return shift ? shifted[usage] : normal[usage];
}

bool report_had_key(const uint8_t* report, uint8_t usage) {
    for (int i = 2; i < 8; ++i) {
        if (report[i] == usage) {
            return true;
        }
    }
    return false;
}
}

USBInput& USBInput::get() {
    static USBInput instance;
    return instance;
}

void USBInput::initialize() {
    if (initialized) {
        return;
    }

    initialized = true;
    log_str("[usb] init\n");
    controllerReady = detectController();
    if (!controllerReady) {
        log_str("[usb] no OHCI keyboard controller ready\n");
    }
}

void USBInput::poll() {
    if (!keyboardReady) {
        return;
    }

    if (!interruptPending) {
        submitInterruptTransfer();
        return;
    }

    completeInterruptTransfer();
}

bool USBInput::detectController() {
    bool sawUsb = false;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                uint16_t vendor = PCI::get().readConfig16(0, bus, slot, func, 0x00);
                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                uint8_t classCode = PCI::get().readConfig8(0, bus, slot, func, PCI_CLASS);
                uint8_t subclass = PCI::get().readConfig8(0, bus, slot, func, PCI_SUBCLASS);
                uint8_t progIf = PCI::get().readConfig8(0, bus, slot, func, PCI_PROG_IF);

                if (classCode != PCI_CLASS_SERIAL_BUS || subclass != PCI_SUBCLASS_USB) {
                    continue;
                }

                sawUsb = true;
                log_str("[usb] controller ");
                log_dec(bus);
                log_str(":");
                log_dec(slot);
                log_str(".");
                log_dec(func);
                log_str(" prog-if=");
                log_hex(progIf);
                log_str("\n");

                if (progIf != PCI_PROGIF_OHCI) {
                    if (progIf == PCI_PROGIF_UHCI) log_str("[usb] UHCI unsupported\n");
                    else if (progIf == PCI_PROGIF_EHCI) log_str("[usb] EHCI unsupported\n");
                    else if (progIf == PCI_PROGIF_XHCI) log_str("[usb] xHCI unsupported\n");
                    else log_str("[usb] unsupported USB controller\n");
                    continue;
                }

                uint32_t bar0 = PCI::get().readConfig32(0, bus, slot, func, PCI_BAR0);
                if (initializeController(static_cast<uint8_t>(bus), slot, func, bar0)) {
                    return true;
                }
            }
        }
    }

    if (!sawUsb) {
        log_str("[usb] no USB controllers found\n");
    }
    return false;
}

bool USBInput::initializeController(uint8_t bus, uint8_t slot, uint8_t func, uint32_t bar0) {
    if (bar0 & 0x1) {
        log_str("[usb:ohci] BAR0 is I/O, expected MMIO\n");
        return false;
    }

    uint32_t base = bar0 & 0xFFFFFFF0U;
    if (!base) {
        log_str("[usb:ohci] BAR0 missing\n");
        return false;
    }

    VMM::MapPage(base, base, Present | ReadWrite | CacheDisab | NoExecute);
    regs = reinterpret_cast<volatile uint32_t*>(static_cast<uint64_t>(base));

    uint16_t command = PCI::get().readConfig16(0, bus, slot, func, PCI_COMMAND);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    command &= static_cast<uint16_t>(~PCI_COMMAND_IO);
    PCI::get().writeConfig16(0, bus, slot, func, PCI_COMMAND, command);

    log_str("[usb:ohci] mmio=");
    log_hex(base);
    log_str(" rev=");
    log_hex(regs[OHCI_REVISION / 4] & 0xFF);
    log_str("\n");

    uint32_t savedFmInterval = regs[OHCI_FM_INTERVAL / 4];
    if (regs[OHCI_CONTROL / 4] & OHCI_CONTROL_IR) {
        regs[OHCI_COMMAND_STATUS / 4] = OHCI_CMD_OCR;
        for (int i = 0; i < 1000000 && (regs[OHCI_CONTROL / 4] & OHCI_CONTROL_IR); ++i) {
            io_wait();
        }
    }

    regs[OHCI_CONTROL / 4] = 0;
    regs[OHCI_COMMAND_STATUS / 4] = OHCI_CMD_HCR;
    for (int i = 0; i < 1000000 && (regs[OHCI_COMMAND_STATUS / 4] & OHCI_CMD_HCR); ++i) {
        io_wait();
    }

    hcca = alloc_dma_page();
    if (!hcca) {
        return false;
    }
    uint32_t hccaPhys = phys32(hcca);
    if (!hccaPhys) {
        log_str("[usb:ohci] HCCA is above 4G\n");
        return false;
    }

    regs[OHCI_HCCA / 4] = hccaPhys;
    regs[OHCI_CONTROL_HEAD_ED / 4] = 0;
    regs[OHCI_BULK_HEAD_ED / 4] = 0;
    regs[OHCI_DONE_HEAD / 4] = 0;
    regs[OHCI_INTERRUPT_DISABLE / 4] = 0xFFFFFFFFU;
    regs[OHCI_INTERRUPT_STATUS / 4] = 0xFFFFFFFFU;
    if (savedFmInterval != 0) {
        regs[OHCI_FM_INTERVAL / 4] = savedFmInterval;
        regs[OHCI_PERIODIC_START / 4] = ((savedFmInterval & 0x3FFFU) * 9U) / 10U;
    }
    regs[OHCI_CONTROL / 4] = 3 | OHCI_CONTROL_CLE | OHCI_CONTROL_PLE | OHCI_CONTROL_HCFS_OPERATIONAL;
    regs[OHCI_INTERRUPT_ENABLE / 4] = OHCI_INT_WDH | OHCI_INT_RHSC | OHCI_INT_MIE;
    spin_delay(100000);

    return initializeRootPorts();
}

bool USBInput::initializeRootPorts() {
    uint32_t descriptorA = regs[OHCI_RH_DESCRIPTOR_A / 4];
    uint8_t portCount = static_cast<uint8_t>(descriptorA & 0xFF);
    if (portCount == 0 || portCount > 15) {
        log_str("[usb:ohci] invalid root port count\n");
        return false;
    }

    log_str("[usb:ohci] root ports=");
    log_dec(portCount);
    log_str("\n");

    regs[OHCI_RH_STATUS / 4] = 1 << 16;
    for (uint8_t port = 0; port < portCount; ++port) {
        regs[(OHCI_RH_PORT_STATUS_BASE / 4) + port] = OHCI_PORT_PPS;
    }
    spin_delay(200000);

    bool foundKeyboard = false;
    for (uint8_t port = 0; port < portCount; ++port) {
        volatile uint32_t* portReg = &regs[(OHCI_RH_PORT_STATUS_BASE / 4) + port];
        uint32_t status = *portReg;
        *portReg = OHCI_PORT_CHANGE_MASK;
        if ((status & OHCI_PORT_CCS) == 0) {
            continue;
        }

        log_str("[usb:ohci] port ");
        log_dec(port + 1);
        log_str(" connected status=");
        log_hex(status);
        log_str("\n");

        *portReg = OHCI_PORT_PRS;
        for (int i = 0; i < 2000000; ++i) {
            status = *portReg;
            if ((status & OHCI_PORT_PRS) == 0 && (status & OHCI_PORT_PRSC)) {
                break;
            }
            io_wait();
        }
        *portReg = OHCI_PORT_CHANGE_MASK;
        spin_delay(100000);
        status = *portReg;
        if ((status & OHCI_PORT_PES) == 0) {
            *portReg = OHCI_PORT_PES;
            spin_delay(100000);
            status = *portReg;
        }

        if ((status & OHCI_PORT_CCS) == 0 || (status & OHCI_PORT_PES) == 0) {
            log_str("[usb:ohci] port not enabled status=");
            log_hex(status);
            log_str("\n");
            continue;
        }

        bool lowSpeed = (status & OHCI_PORT_LSDA) != 0;
        if (enumerateDevice(port, lowSpeed)) {
            foundKeyboard = true;
            break;
        }
    }

    if (!foundKeyboard) {
        log_str("[usb:ohci] no boot keyboard found\n");
    }
    return true;
}

static bool ohci_control_transfer(
    volatile uint32_t* regs,
    uint8_t address,
    bool lowSpeed,
    uint8_t maxPacket,
    const UsbSetupPacket& setup,
    void* data,
    uint16_t length
) {
    if (length > 512 || maxPacket == 0) {
        return false;
    }

    void* page = alloc_dma_page();
    if (!page) {
        return false;
    }

    auto* bytes = reinterpret_cast<uint8_t*>(page);
    auto* ed = reinterpret_cast<OHCIEndpointDescriptor*>(bytes);
    auto* setupTd = reinterpret_cast<OHCITransferDescriptor*>(bytes + 0x20);
    auto* dataTd = reinterpret_cast<OHCITransferDescriptor*>(bytes + 0x40);
    auto* statusTd = reinterpret_cast<OHCITransferDescriptor*>(bytes + 0x60);
    auto* dummyTd = reinterpret_cast<OHCITransferDescriptor*>(bytes + 0x80);
    auto* setupBuf = reinterpret_cast<UsbSetupPacket*>(bytes + 0x100);
    auto* transferBuf = bytes + 0x200;

    *setupBuf = setup;
    const bool in = (setup.requestType & 0x80) != 0;
    if (!in && length > 0 && data) {
        memcpy(transferBuf, data, length);
    }

    uint32_t edPhys = phys32(ed);
    uint32_t setupTdPhys = phys32(setupTd);
    uint32_t dataTdPhys = phys32(dataTd);
    uint32_t statusTdPhys = phys32(statusTd);
    uint32_t dummyTdPhys = phys32(dummyTd);
    uint32_t setupBufPhys = phys32(setupBuf);
    uint32_t transferBufPhys = phys32(transferBuf);
    if (!edPhys || !setupTdPhys || !dataTdPhys || !statusTdPhys || !dummyTdPhys ||
        !setupBufPhys || (!transferBufPhys && length > 0)) {
        PMM::FreeFrame(reinterpret_cast<uint64_t>(page));
        return false;
    }

    ed->control = address |
        (lowSpeed ? OHCI_ED_LOW_SPEED : 0) |
        (static_cast<uint32_t>(maxPacket) << 16);
    ed->headP = setupTdPhys;
    ed->tailP = dummyTdPhys;
    ed->nextED = 0;

    setupTd->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_NO_INTERRUPT |
        OHCI_TD_DATA0 | OHCI_TD_PID_SETUP;
    setupTd->cbp = setupBufPhys;
    setupTd->be = setupBufPhys + sizeof(UsbSetupPacket) - 1;

    OHCITransferDescriptor* lastDataTd = setupTd;
    if (length > 0) {
        setupTd->nextTD = dataTdPhys;
        dataTd->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_NO_INTERRUPT |
            OHCI_TD_ROUNDING | OHCI_TD_DATA1 | (in ? OHCI_TD_PID_IN : OHCI_TD_PID_OUT);
        dataTd->cbp = transferBufPhys;
        dataTd->be = transferBufPhys + length - 1;
        lastDataTd = dataTd;
    } else {
        setupTd->nextTD = statusTdPhys;
    }

    lastDataTd->nextTD = statusTdPhys;
    statusTd->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_NO_INTERRUPT |
        OHCI_TD_DATA1 | (in ? OHCI_TD_PID_OUT : OHCI_TD_PID_IN);
    statusTd->cbp = 0;
    statusTd->be = 0;
    statusTd->nextTD = dummyTdPhys;

    regs[OHCI_CONTROL_HEAD_ED / 4] = edPhys;
    regs[OHCI_COMMAND_STATUS / 4] = OHCI_CMD_CLF;

    bool completed = false;
    for (int i = 0; i < 20000000; ++i) {
        if ((ed->headP & ~0xFU) == dummyTdPhys) {
            completed = true;
            break;
        }
        io_wait();
    }

    regs[OHCI_INTERRUPT_STATUS / 4] = OHCI_INT_WDH;
    regs[OHCI_CONTROL_HEAD_ED / 4] = 0;

    bool ok = completed && td_ok(setupTd) && td_ok(statusTd) && (length == 0 || td_ok(dataTd));
    if (ok && in && length > 0 && data) {
        memcpy(data, transferBuf, length);
    }

    if (!ok) {
        log_str("[usb:ohci] control failed cc=");
        log_hex(td_cc(setupTd));
        log_str("/");
        log_hex(length ? td_cc(dataTd) : 0);
        log_str("/");
        log_hex(td_cc(statusTd));
        log_str("\n");
    }

    PMM::FreeFrame(reinterpret_cast<uint64_t>(page));
    return ok;
}

bool USBInput::enumerateDevice(uint8_t port, bool lowSpeed) {
    (void)port;

    uint8_t descriptor[256];
    memset(descriptor, 0, sizeof(descriptor));

    UsbSetupPacket setup {};
    setup.requestType = 0x80;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = static_cast<uint16_t>(USB_DESCRIPTOR_DEVICE << 8);
    setup.index = 0;
    setup.length = 8;
    if (!ohci_control_transfer(regs, 0, lowSpeed, 8, setup, descriptor, 8)) {
        return false;
    }

    uint8_t ep0MaxPacket = descriptor[7] ? descriptor[7] : 8;
    if (lowSpeed && ep0MaxPacket > 8) {
        ep0MaxPacket = 8;
    }

    const uint8_t address = 1;
    setup = {};
    setup.requestType = 0x00;
    setup.request = USB_REQUEST_SET_ADDRESS;
    setup.value = address;
    setup.length = 0;
    if (!ohci_control_transfer(regs, 0, lowSpeed, ep0MaxPacket, setup, nullptr, 0)) {
        return false;
    }
    spin_delay(200000);

    memset(descriptor, 0, sizeof(descriptor));
    setup = {};
    setup.requestType = 0x80;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = static_cast<uint16_t>(USB_DESCRIPTOR_DEVICE << 8);
    setup.length = 18;
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, descriptor, 18)) {
        return false;
    }

    uint8_t configHeader[9];
    memset(configHeader, 0, sizeof(configHeader));
    setup = {};
    setup.requestType = 0x80;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = static_cast<uint16_t>(USB_DESCRIPTOR_CONFIGURATION << 8);
    setup.length = sizeof(configHeader);
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, configHeader, sizeof(configHeader))) {
        return false;
    }

    uint16_t totalLength = static_cast<uint16_t>(configHeader[2] | (configHeader[3] << 8));
    if (totalLength == 0 || totalLength > sizeof(descriptor)) {
        totalLength = sizeof(descriptor);
    }

    memset(descriptor, 0, sizeof(descriptor));
    setup.length = totalLength;
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, descriptor, totalLength)) {
        return false;
    }

    uint8_t configValue = descriptor[5];
    bool inBootKeyboardInterface = false;
    bool foundEndpoint = false;
    uint8_t interfaceNumber = 0;
    uint8_t endpointAddress = 0;
    uint8_t endpointMaxPacket = 8;

    for (uint16_t offset = 0; offset + 2 <= totalLength;) {
        uint8_t len = descriptor[offset];
        uint8_t type = descriptor[offset + 1];
        if (len < 2 || offset + len > totalLength) {
            break;
        }

        if (type == USB_DESCRIPTOR_INTERFACE && len >= 9) {
            interfaceNumber = descriptor[offset + 2];
            inBootKeyboardInterface =
                descriptor[offset + 5] == USB_CLASS_HID &&
                descriptor[offset + 6] == USB_HID_SUBCLASS_BOOT &&
                descriptor[offset + 7] == USB_HID_PROTOCOL_KEYBOARD;
            if (inBootKeyboardInterface) {
                log_str("[usb:kbd] boot keyboard interface=");
                log_dec(interfaceNumber);
                log_str("\n");
            }
        } else if (type == USB_DESCRIPTOR_ENDPOINT && len >= 7 && inBootKeyboardInterface) {
            uint8_t addr = descriptor[offset + 2];
            uint8_t attributes = descriptor[offset + 3];
            uint16_t maxPacket = static_cast<uint16_t>(descriptor[offset + 4] | (descriptor[offset + 5] << 8));
            if ((addr & USB_ENDPOINT_IN) && ((attributes & 0x3) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                endpointAddress = addr;
                endpointMaxPacket = static_cast<uint8_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 8);
                if (endpointMaxPacket > 8) {
                    endpointMaxPacket = 8;
                }
                foundEndpoint = true;
                break;
            }
        }

        offset += len;
    }

    if (!foundEndpoint) {
        log_str("[usb:kbd] no boot keyboard endpoint\n");
        return false;
    }

    setup = {};
    setup.requestType = 0x00;
    setup.request = USB_REQUEST_SET_CONFIGURATION;
    setup.value = configValue;
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, nullptr, 0)) {
        return false;
    }

    setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_PROTOCOL;
    setup.value = 0;
    setup.index = interfaceNumber;
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, nullptr, 0)) {
        log_str("[usb:kbd] set boot protocol failed\n");
    }

    setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_IDLE;
    setup.value = 0;
    setup.index = interfaceNumber;
    if (!ohci_control_transfer(regs, address, lowSpeed, ep0MaxPacket, setup, nullptr, 0)) {
        log_str("[usb:kbd] set idle failed\n");
    }

    void* page = alloc_dma_page();
    if (!page) {
        return false;
    }
    auto* bytes = reinterpret_cast<uint8_t*>(page);
    interruptEd = bytes;
    interruptTd = bytes + 0x20;
    interruptTail = bytes + 0x40;
    interruptBuffer = bytes + 0x100;

    interruptEdPhys = phys32(interruptEd);
    interruptTdPhys = phys32(interruptTd);
    interruptTailPhys = phys32(interruptTail);
    interruptBufferPhys = phys32(interruptBuffer);
    if (!interruptEdPhys || !interruptTdPhys || !interruptTailPhys || !interruptBufferPhys) {
        log_str("[usb:kbd] interrupt DMA above 4G\n");
        return false;
    }

    keyboardAddress = address;
    keyboardLowSpeed = lowSpeed;
    keyboardEndpoint = static_cast<uint8_t>(endpointAddress & 0x0F);
    keyboardMaxPacket = endpointMaxPacket;
    keyboardInterface = interfaceNumber;
    memset(lastReport, 0, sizeof(lastReport));

    auto* ed = reinterpret_cast<OHCIEndpointDescriptor*>(interruptEd);
    ed->control = keyboardAddress |
        (static_cast<uint32_t>(keyboardEndpoint) << 7) |
        (2U << 11) |
        (keyboardLowSpeed ? OHCI_ED_LOW_SPEED : 0) |
        (static_cast<uint32_t>(keyboardMaxPacket) << 16);
    ed->tailP = interruptTailPhys;
    ed->headP = interruptTailPhys;
    ed->nextED = 0;

    auto* hccaPtr = reinterpret_cast<OHCIHCCA*>(hcca);
    for (int i = 0; i < 32; ++i) {
        hccaPtr->interruptTable[i] = interruptEdPhys;
    }

    keyboardReady = true;
    log_str("[usb:kbd] ready addr=");
    log_dec(keyboardAddress);
    log_str(" ep=");
    log_dec(keyboardEndpoint);
    log_str(" low=");
    log_dec(keyboardLowSpeed ? 1 : 0);
    log_str("\n");
    submitInterruptTransfer();
    return true;
}

void USBInput::submitInterruptTransfer() {
    if (!keyboardReady || interruptPending) {
        return;
    }

    memset(interruptBuffer, 0, 8);
    auto* td = reinterpret_cast<OHCITransferDescriptor*>(interruptTd);
    td->control = OHCI_TD_CC_NOT_ACCESSED | OHCI_TD_NO_INTERRUPT |
        OHCI_TD_ROUNDING | OHCI_TD_TOGGLE_FROM_ED | OHCI_TD_PID_IN;
    td->cbp = interruptBufferPhys;
    td->be = interruptBufferPhys + keyboardMaxPacket - 1;
    td->nextTD = interruptTailPhys;

    auto* tail = reinterpret_cast<OHCITransferDescriptor*>(interruptTail);
    memset(tail, 0, sizeof(OHCITransferDescriptor));

    auto* ed = reinterpret_cast<OHCIEndpointDescriptor*>(interruptEd);
    ed->tailP = interruptTailPhys;
    ed->headP = (ed->headP & 0x2U) | interruptTdPhys;
    interruptPending = true;
}

void USBInput::completeInterruptTransfer() {
    auto* ed = reinterpret_cast<OHCIEndpointDescriptor*>(interruptEd);
    if ((ed->headP & 0x1U) != 0) {
        log_str("[usb:kbd] interrupt halted\n");
        ed->headP = interruptTailPhys;
        interruptPending = false;
        return;
    }

    if ((ed->headP & ~0xFU) != interruptTailPhys) {
        return;
    }

    auto* td = reinterpret_cast<OHCITransferDescriptor*>(interruptTd);
    if (!td_ok(td)) {
        log_str("[usb:kbd] interrupt cc=");
        log_hex(td_cc(td));
        log_str("\n");
        interruptPending = false;
        return;
    }

    auto* report = reinterpret_cast<uint8_t*>(interruptBuffer);
    const bool shift = (report[0] & 0x22) != 0;
    for (int i = 2; i < 8; ++i) {
        uint8_t usage = report[i];
        if (usage == 0 || report_had_key(lastReport, usage)) {
            continue;
        }

        char c = hid_usage_to_char(usage, shift);
        if (c != 0) {
            Keyboard::get().injectChar(c, "[usb:kbd]");
        }
    }

    memcpy(lastReport, report, sizeof(lastReport));
    interruptPending = false;
    submitInterruptTransfer();
}
