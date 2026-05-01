#include <drivers/usb/xhci.hpp>

#include <cpu/acpi/pci.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/process/scheduler.hpp>
#include <common/string.hpp>
#include <drivers/usb/usb.hpp>
#include <graphics/console.hpp>
#include <interrupts/keyboard.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>

namespace {
constexpr uint16_t PCI_VENDOR_ID = 0x00;
constexpr uint16_t PCI_DEVICE_ID = 0x02;
constexpr uint16_t PCI_COMMAND = 0x04;
constexpr uint16_t PCI_PROG_IF = 0x09;
constexpr uint16_t PCI_SUBCLASS = 0x0A;
constexpr uint16_t PCI_CLASS = 0x0B;
constexpr uint16_t PCI_HEADER_TYPE = 0x0E;
constexpr uint16_t PCI_BAR0 = 0x10;
constexpr uint16_t PCI_BAR1 = 0x14;

constexpr uint8_t PCI_CLASS_SERIAL_BUS = 0x0C;
constexpr uint8_t PCI_SUBCLASS_USB = 0x03;
constexpr uint8_t PCI_PROGIF_XHCI = 0x30;

constexpr uint16_t PCI_COMMAND_IO = 1 << 0;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

constexpr uint32_t PCI_BAR_IO = 1 << 0;
constexpr uint32_t PCI_BAR_MEM_TYPE_MASK = 0x6;
constexpr uint32_t PCI_BAR_MEM_TYPE_64 = 0x4;
constexpr uint64_t PCI_BAR_MEM_ADDR_MASK = 0xFFFFFFFFFFFFFFF0ULL;

constexpr uint32_t XHCI_CAP_CAPLENGTH = 0x00;
constexpr uint32_t XHCI_CAP_HCIVERSION = 0x02;
constexpr uint32_t XHCI_CAP_HCSPARAMS1 = 0x04;
constexpr uint32_t XHCI_CAP_HCSPARAMS2 = 0x08;
constexpr uint32_t XHCI_CAP_HCSPARAMS3 = 0x0C;
constexpr uint32_t XHCI_CAP_HCCPARAMS1 = 0x10;
constexpr uint32_t XHCI_CAP_DBOFF = 0x14;
constexpr uint32_t XHCI_CAP_RTSOFF = 0x18;
constexpr uint32_t XHCI_HCCPARAMS1_XECP_SHIFT = 16;
constexpr uint32_t XHCI_HCCPARAMS1_XECP_MASK = 0xFFFFU << XHCI_HCCPARAMS1_XECP_SHIFT;
constexpr uint32_t XHCI_HCCPARAMS1_CSZ = 1 << 2;

constexpr uint32_t XHCI_OP_USBCMD = 0x00;
constexpr uint32_t XHCI_OP_USBSTS = 0x04;
constexpr uint32_t XHCI_OP_PAGESIZE = 0x08;
constexpr uint32_t XHCI_OP_CRCR = 0x18;
constexpr uint32_t XHCI_OP_DCBAAP = 0x30;
constexpr uint32_t XHCI_OP_CONFIG = 0x38;
constexpr uint32_t XHCI_OP_PORT_BASE = 0x400;
constexpr uint32_t XHCI_PORT_STRIDE = 0x10;
constexpr uint32_t XHCI_PORTSC = 0x00;

constexpr uint32_t XHCI_RT_IR0 = 0x20;
constexpr uint32_t XHCI_IR_IMAN = 0x00;
constexpr uint32_t XHCI_IR_IMOD = 0x04;
constexpr uint32_t XHCI_IR_ERSTSZ = 0x08;
constexpr uint32_t XHCI_IR_ERSTBA = 0x10;
constexpr uint32_t XHCI_IR_ERDP = 0x18;
constexpr uint32_t XHCI_IMAN_IP = 1 << 0;
constexpr uint32_t XHCI_IMAN_IE = 1 << 1;

constexpr uint32_t XHCI_CMD_RUN = 1 << 0;
constexpr uint32_t XHCI_CMD_HCRST = 1 << 1;
constexpr uint32_t XHCI_CMD_INTE = 1 << 2;
constexpr uint32_t XHCI_STS_HCH = 1 << 0;
constexpr uint32_t XHCI_STS_EINT = 1 << 3;
constexpr uint32_t XHCI_STS_PCD = 1 << 4;
constexpr uint32_t XHCI_STS_CNR = 1 << 11;

constexpr uint32_t XHCI_PORTSC_CCS = 1 << 0;
constexpr uint32_t XHCI_PORTSC_PED = 1 << 1;
constexpr uint32_t XHCI_PORTSC_PR = 1 << 4;
constexpr uint32_t XHCI_PORTSC_PP = 1 << 9;
constexpr uint32_t XHCI_PORTSC_SPEED_SHIFT = 10;
constexpr uint32_t XHCI_PORTSC_SPEED_MASK = 0xF << XHCI_PORTSC_SPEED_SHIFT;
constexpr uint32_t XHCI_PORTSC_PIC_MASK = 0x3 << 14;
constexpr uint32_t XHCI_PORTSC_LWS = 1 << 16;
constexpr uint32_t XHCI_PORTSC_CSC = 1 << 17;
constexpr uint32_t XHCI_PORTSC_PEC = 1 << 18;
constexpr uint32_t XHCI_PORTSC_WRC = 1 << 19;
constexpr uint32_t XHCI_PORTSC_OCC = 1 << 20;
constexpr uint32_t XHCI_PORTSC_PRC = 1 << 21;
constexpr uint32_t XHCI_PORTSC_PLC = 1 << 22;
constexpr uint32_t XHCI_PORTSC_CEC = 1 << 23;
constexpr uint32_t XHCI_PORTSC_WCE = 1 << 25;
constexpr uint32_t XHCI_PORTSC_WDE = 1 << 26;
constexpr uint32_t XHCI_PORTSC_WOE = 1 << 27;
constexpr uint32_t XHCI_PORTSC_WPR = 1U << 31;
constexpr uint32_t XHCI_PORTSC_CHANGE_MASK =
    XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC |
    XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC;
constexpr uint32_t XHCI_PORTSC_PRESERVE_MASK =
    XHCI_PORTSC_PP | XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_LWS |
    XHCI_PORTSC_WCE | XHCI_PORTSC_WDE | XHCI_PORTSC_WOE;

constexpr uint32_t XHCI_HCCPARAMS1_PPC = 1 << 3;

constexpr uint32_t XHCI_CRCR_RCS = 1 << 0;
constexpr uint32_t XHCI_ERDP_EHB = 1 << 3;

constexpr uint32_t XHCI_TRB_TYPE_LINK = 6;
constexpr uint32_t XHCI_TRB_TYPE_SETUP_STAGE = 2;
constexpr uint32_t XHCI_TRB_TYPE_DATA_STAGE = 3;
constexpr uint32_t XHCI_TRB_TYPE_STATUS_STAGE = 4;
constexpr uint32_t XHCI_TRB_TYPE_NORMAL = 1;
constexpr uint32_t XHCI_TRB_TYPE_ENABLE_SLOT_COMMAND = 9;
constexpr uint32_t XHCI_TRB_TYPE_ADDRESS_DEVICE_COMMAND = 11;
constexpr uint32_t XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_COMMAND = 12;
constexpr uint32_t XHCI_TRB_TYPE_EVALUATE_CONTEXT_COMMAND = 13;
constexpr uint32_t XHCI_TRB_TYPE_RESET_ENDPOINT_COMMAND = 14;
constexpr uint32_t XHCI_TRB_TYPE_STOP_ENDPOINT_COMMAND = 15;
constexpr uint32_t XHCI_TRB_CYCLE = 1 << 0;
constexpr uint32_t XHCI_TRB_ISP = 1 << 2;
constexpr uint32_t XHCI_TRB_IOC = 1 << 5;
constexpr uint32_t XHCI_TRB_IDT = 1 << 6;
constexpr uint32_t XHCI_LINK_TOGGLE_CYCLE = 1 << 1;
constexpr uint32_t XHCI_TRB_TYPE_SHIFT = 10;
constexpr uint32_t XHCI_TRB_TYPE_MASK = 0x3F << XHCI_TRB_TYPE_SHIFT;
constexpr uint32_t XHCI_TRB_COMPLETION_CODE_SHIFT = 24;
constexpr uint32_t XHCI_TRB_SLOT_ID_SHIFT = 24;
constexpr uint32_t XHCI_TRB_ENDPOINT_ID_SHIFT = 16;
constexpr uint32_t XHCI_TRB_SETUP_TRT_SHIFT = 16;
constexpr uint32_t XHCI_TRB_SETUP_TRT_NO_DATA = 0;
constexpr uint32_t XHCI_TRB_SETUP_TRT_OUT = 2;
constexpr uint32_t XHCI_TRB_SETUP_TRT_IN = 3;
constexpr uint32_t XHCI_TRB_DATA_DIR_IN = 1 << 16;
constexpr uint32_t XHCI_TRB_STATUS_DIR_IN = 1 << 16;

constexpr uint32_t XHCI_TRB_TYPE_TRANSFER_EVENT = 32;
constexpr uint32_t XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT = 33;
constexpr uint32_t XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT = 34;

constexpr uint32_t XHCI_COMPLETION_SUCCESS = 1;

constexpr uint32_t XHCI_EXT_CAP_ID_MASK = 0xFF;
constexpr uint32_t XHCI_EXT_CAP_NEXT_SHIFT = 8;
constexpr uint32_t XHCI_EXT_CAP_NEXT_MASK = 0xFF << XHCI_EXT_CAP_NEXT_SHIFT;
constexpr uint32_t XHCI_EXT_CAP_LEGACY_SUPPORT = 1;
constexpr uint32_t XHCI_EXT_CAP_SUPPORTED_PROTOCOL = 2;
constexpr uint32_t XHCI_USBLEGSUP_BIOS_OWNED = 1 << 16;
constexpr uint32_t XHCI_USBLEGSUP_OS_OWNED = 1 << 24;

constexpr uint32_t XHCI_COMMAND_RING_TRBS = 256;
constexpr uint32_t XHCI_EVENT_RING_TRBS = 256;
constexpr uint32_t XHCI_TRANSFER_RING_TRBS = 256;
constexpr uint32_t XHCI_MAX_TRACKED_CONTROLLERS = 4;
constexpr uint32_t XHCI_MAX_SLOTS = 256;
constexpr uint32_t XHCI_COMMAND_TIMEOUT = 2000000;
constexpr uint32_t XHCI_TRANSFER_TIMEOUT = 4000000;
constexpr uint32_t XHCI_PORT_RESET_TIMEOUT = 2000000;
constexpr uint32_t XHCI_PORT_SETTLE_DELAY = 200000;
constexpr uint32_t XHCI_PORT_POST_RESET_DELAY = 100000;
constexpr uint32_t XHCI_POST_ADDRESS_DELAY = 100000;
constexpr uint8_t XHCI_ENUMERATION_RETRIES = 3;
constexpr uint8_t USB_HID_MAX_KEYBOARD_REPORT = 64;
constexpr uint16_t XHCI_MAX_CONTROL_TRANSFER_BYTES = 4096;
constexpr uint16_t XHCI_MAX_CONFIG_DESCRIPTOR_BYTES = 1024;
constexpr uint16_t XHCI_MAX_HID_REPORT_DESCRIPTOR_BYTES = 1024;
constexpr uint8_t XHCI_MAX_HUB_PORTS = 15;
constexpr uint8_t XHCI_MAX_HUB_DEPTH = 5;

constexpr uint8_t USB_REQUEST_GET_STATUS = 0x00;
constexpr uint8_t USB_REQUEST_CLEAR_FEATURE = 0x01;
constexpr uint8_t USB_REQUEST_SET_FEATURE = 0x03;
constexpr uint8_t USB_DESCRIPTOR_HUB = 0x29;
constexpr uint8_t USB_DESCRIPTOR_SUPERSPEED_HUB = 0x2A;
constexpr uint8_t USB_CLASS_HUB = 0x09;

constexpr uint8_t USB_HUB_FEATURE_PORT_RESET = 4;
constexpr uint8_t USB_HUB_FEATURE_PORT_POWER = 8;
constexpr uint8_t USB_HUB_FEATURE_C_PORT_CONNECTION = 16;
constexpr uint8_t USB_HUB_FEATURE_C_PORT_ENABLE = 17;
constexpr uint8_t USB_HUB_FEATURE_C_PORT_OVER_CURRENT = 19;
constexpr uint8_t USB_HUB_FEATURE_C_PORT_RESET = 20;

constexpr uint32_t USB_HUB_PORT_CONNECTION = 1 << 0;
constexpr uint32_t USB_HUB_PORT_ENABLE = 1 << 1;
constexpr uint32_t USB_HUB_PORT_RESET = 1 << 4;
constexpr uint32_t USB_HUB_PORT_POWER = 1 << 8;
constexpr uint32_t USB_HUB_PORT_LOW_SPEED = 1 << 9;
constexpr uint32_t USB_HUB_PORT_HIGH_SPEED = 1 << 10;
constexpr uint32_t USB_HUB_PORT_CONNECTION_CHANGE = 1 << 16;
constexpr uint32_t USB_HUB_PORT_ENABLE_CHANGE = 1 << 17;
constexpr uint32_t USB_HUB_PORT_OVER_CURRENT_CHANGE = 1 << 19;
constexpr uint32_t USB_HUB_PORT_RESET_CHANGE = 1 << 20;

constexpr uint8_t XHCI_ENDPOINT_TYPE_CONTROL = 4;
constexpr uint8_t XHCI_ENDPOINT_TYPE_INTERRUPT_IN = 7;

struct XHCICapabilityRegisters {
    uint8_t capLength;
    uint8_t reserved;
    uint16_t hciVersion;
    uint32_t hcsParams1;
    uint32_t hcsParams2;
    uint32_t hcsParams3;
    uint32_t hccParams1;
    uint32_t dbOff;
    uint32_t rtsOff;
} __attribute__((packed));

struct XHCIOperationalRegisters {
    uint32_t usbCommand;
    uint32_t usbStatus;
    uint32_t pageSize;
    uint8_t reserved0[0x18 - 0x0C];
    uint64_t commandRingControl;
    uint8_t reserved1[0x30 - 0x20];
    uint64_t dcbaaPointer;
    uint32_t config;
} __attribute__((packed));

struct XHCIRuntimeInterrupterRegisters {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstSize;
    uint32_t reserved;
    uint64_t erstBase;
    uint64_t eventRingDequeuePointer;
} __attribute__((packed));

struct XHCIPortRegisters {
    uint32_t portStatusControl;
    uint32_t portPowerManagementStatusControl;
    uint32_t portLinkInfo;
    uint32_t portHardwareLpmControl;
} __attribute__((packed));

struct XHCIExtendedCapability {
    uint32_t header;
} __attribute__((packed));

struct XHCITransferRequestBlock {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16)));

struct HIDKeyboardReportLayout {
    bool valid;
    bool bootProtocol;
    bool keyBitmap;
    uint8_t reportId;
    uint16_t keyUsageMinimum;
    uint16_t modifierBitOffset;
    uint16_t keyArrayBitOffset;
    uint8_t keyArrayCount;
    uint8_t keyArrayReportSize;
};

struct HIDKeyboardCandidate {
    uint8_t interfaceNumber;
    uint8_t endpointAddress;
    uint16_t endpointMaxPacket;
    uint8_t endpointInterval;
    uint16_t reportDescriptorLength;
};

enum class HIDKeyboardParseStatus : uint8_t {
    Keyboard,
    NotKeyboard,
    KeyboardWithoutKeys,
};

struct XHCIEventRingSegmentTableEntry {
    uint64_t ringSegmentBase;
    uint32_t ringSegmentSize;
    uint32_t reserved;
} __attribute__((packed, aligned(16)));

struct XHCISlotState {
    bool allocated;
    bool transferPending;
    bool transferCompleted;
    bool configured;
    bool usable;
    bool keyboardReady;
    bool keyboardPending;
    bool mouseReady;
    bool mousePending;
    uint8_t slotId;
    uint8_t portId;
    uint8_t rootPortId;
    uint8_t speedId;
    uint8_t routeDepth;
    uint8_t hubPortCount;
    uint8_t ttHubSlotId;
    uint8_t ttPortId;
    uint8_t ttThinkTime;
    uint16_t ep0MaxPacket;
    uint32_t routeString;
    uint8_t keyboardDci;
    uint8_t keyboardInterface;
    uint8_t keyboardMaxPacket;
    HIDKeyboardReportLayout keyboardLayout;
    uint8_t mouseDci;
    uint8_t mouseInterface;
    uint8_t mouseMaxPacket;
    uint64_t inputContextPhys;
    uint64_t outputContextPhys;
    uint64_t ep0RingPhys;
    uint64_t keyboardRingPhys;
    uint64_t keyboardReportPhys;
    uint64_t mouseRingPhys;
    uint64_t mouseReportPhys;
    uint64_t pendingTransferPhys;
    uint64_t pendingKeyboardTransferPhys;
    uint64_t pendingMouseTransferPhys;
    uint32_t inputContextPages;
    uint32_t outputContextPages;
    uint32_t ep0RingPages;
    uint32_t keyboardRingPages;
    uint32_t keyboardReportPages;
    uint32_t mouseRingPages;
    uint32_t mouseReportPages;
    uint32_t ep0Enqueue;
    uint32_t keyboardEnqueue;
    uint32_t mouseEnqueue;
    uint32_t lastTransferCompletionCode;
    uint32_t lastKeyboardCompletionCode;
    uint32_t lastMouseCompletionCode;
    bool ep0Cycle;
    bool keyboardCycle;
    bool mouseCycle;
    uint8_t* inputContext;
    uint8_t* outputContext;
    uint8_t* keyboardReport;
    uint8_t* mouseReport;
    XHCITransferRequestBlock* ep0Ring;
    XHCITransferRequestBlock* keyboardRing;
    XHCITransferRequestBlock* mouseRing;
    uint8_t lastKeyboardReport[USB_HID_MAX_KEYBOARD_REPORT];
};

struct XHCIControllerState {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t maxPorts;
    uint8_t contextSize;
    uint32_t hccParams1;
    uint64_t mmioBase;
    uint64_t operationalBase;
    uint64_t runtimeBase;
    uint64_t doorbellBase;
    uint64_t dcbaaPhys;
    uint64_t scratchpadArrayPhys;
    uint64_t commandRingPhys;
    uint64_t eventRingPhys;
    uint64_t erstPhys;
    uint64_t* dcbaa;
    uint64_t* scratchpadArray;
    XHCITransferRequestBlock* commandRing;
    XHCITransferRequestBlock* eventRing;
    XHCIEventRingSegmentTableEntry* erst;
    XHCISlotState slots[XHCI_MAX_SLOTS];
    bool portBusy[256];
    uint32_t commandEnqueue;
    uint32_t eventDequeue;
    uint8_t irqLine;
    uint8_t irqVector;
    bool irqRegistered;
    bool msiEnabled;
    bool commandCycle;
    bool eventCycle;
    bool commandPending;
    bool commandCompleted;
    uint64_t pendingCommandPhys;
    uint32_t lastCommandCompletionCode;
    uint8_t lastCommandSlotId;
    uint16_t scratchpadCount;
};

struct XHCIEnumerationTarget {
    uint8_t portId;
    uint8_t rootPortId;
    uint8_t speedId;
    uint8_t routeDepth;
    uint8_t ttHubSlotId;
    uint8_t ttPortId;
    uint8_t ttThinkTime;
    uint32_t routeString;
};

XHCIControllerState g_controllers[XHCI_MAX_TRACKED_CONTROLLERS] = {};
volatile bool g_inputActivity = false;

void handle_controller_interrupt(XHCIControllerState& state);
void io_wait();

class XHCIInterruptHandler : public Interrupt {
public:
    void initialize() override {
    }

    bool shouldDispatch() override {
        return XHCIController::get().claimPendingInterrupt();
    }

    void Run(InterruptFrame* frame) override {
        XHCIController::get().handleInterrupt();
        if (g_inputActivity) {
            g_inputActivity = false;
            Process* current = Scheduler::get().getCurrentProcess();
            const bool interruptedUser = frame && frame->cs == 0x23;
            const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;
            if (frame && (interruptedUser || runningIdle)) {
                Scheduler::get().schedule(frame);
            }
        }
    }
};

XHCIInterruptHandler& get_xhci_interrupt_handler() {
    static XHCIInterruptHandler handler;
    return handler;
}

void log_dec(uint64_t value);

bool log_console_available() {
    return false;
}

void log_str(const char* text) {
    if (!text) {
        return;
    }

    if (log_console_available()) {
        Console::get().drawText(text);
    } else {
        Cereal::get().write(text);
    }
}

void log_boot_str(const char* text) {
    log_str(text);
}

void log_boot_dec(uint64_t value) {
    log_dec(value);
}

void log_hex(uint64_t value) {
    if (log_console_available()) {
        Console::get().drawText("0x");
        Console::get().drawHex(value);
        return;
    }

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
    if (log_console_available()) {
        Console::get().drawNumber(static_cast<int64_t>(value));
        return;
    }

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

void spin_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        io_wait();
    }
}

uint8_t mmio_read8(uint64_t base, uint32_t offset) {
    return *reinterpret_cast<volatile uint8_t*>(base + offset);
}

uint16_t mmio_read16(uint64_t base, uint32_t offset) {
    return *reinterpret_cast<volatile uint16_t*>(base + offset);
}

uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

uint64_t mmio_read64(uint64_t base, uint32_t offset) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(base + offset);
    const uint32_t lo = ptr[0];
    const uint32_t hi = ptr[1];
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

void mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

void mmio_write64(uint64_t base, uint32_t offset, uint64_t value) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(base + offset);
    ptr[0] = static_cast<uint32_t>(value);
    ptr[1] = static_cast<uint32_t>(value >> 32);
}

uint32_t trb_type(const XHCITransferRequestBlock& trb) {
    return (trb.control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
}

uint32_t trb_completion_code(const XHCITransferRequestBlock& trb) {
    return trb.status >> XHCI_TRB_COMPLETION_CODE_SHIFT;
}

uint64_t event_trb_pointer(const XHCITransferRequestBlock& trb) {
    return trb.parameter & ~0xFULL;
}

const char* event_type_name(uint32_t type) {
    switch (type) {
        case XHCI_TRB_TYPE_TRANSFER_EVENT:
            return "transfer";
        case XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT:
            return "command";
        case XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT:
            return "port";
        default:
            return "unknown";
    }
}

const char* completion_code_name(uint32_t code) {
    switch (code) {
        case 1: return "Success";
        case 2: return "Data Buffer Error";
        case 3: return "Babble Detected Error";
        case 4: return "USB Transaction Error";
        case 5: return "TRB Error";
        case 6: return "Stall Error";
        case 7: return "Resource Error";
        case 8: return "Bandwidth Error";
        case 9: return "No Slots Available";
        case 10: return "Invalid Stream Type";
        case 11: return "Slot Not Enabled";
        case 12: return "Endpoint Not Enabled";
        case 13: return "Short Packet";
        case 14: return "Ring Underrun";
        case 15: return "Ring Overrun";
        case 16: return "VF Event Ring Full";
        case 17: return "Parameter Error";
        case 18: return "Bandwidth Overrun";
        case 19: return "Context State Error";
        case 20: return "No Ping Response";
        case 21: return "Event Ring Full";
        case 22: return "Incompatible Device";
        case 23: return "Missed Service";
        case 24: return "Command Ring Stopped";
        case 25: return "Command Aborted";
        case 26: return "Stopped";
        case 27: return "Stopped Length Invalid";
        case 28: return "Stopped Short Packet";
        case 29: return "Max Exit Latency Too Large";
        case 31: return "Isoch Buffer Overrun";
        case 32: return "Event Lost";
        case 33: return "Undefined Error";
        case 34: return "Invalid Stream ID";
        case 35: return "Secondary Bandwidth";
        case 36: return "Split Transaction";
        default: return "Unknown";
    }
}

void log_event(const XHCITransferRequestBlock& trb) {
    const uint32_t type = trb_type(trb);
    log_str("[usb:xhci] event type=");
    log_dec(type);
    log_str("(");
    log_str(event_type_name(type));
    log_str(") cc=");
    log_dec(trb_completion_code(trb));
    log_str("(");
    log_str(completion_code_name(trb_completion_code(trb)));
    log_str(")");
    log_str(" param=");
    log_hex(trb.parameter);
    log_str(" status=");
    log_hex(trb.status);
    log_str(" control=");
    log_hex(trb.control);
    log_str("\n");
}

void log_command_completion(uint64_t commandPhys, uint32_t code, uint8_t slotId) {
    log_str("[usb:xhci] command completion trb=");
    log_hex(commandPhys);
    log_str(" cc=");
    log_dec(code);
    log_str("(");
    log_str(completion_code_name(code));
    log_str(") slot=");
    log_dec(slotId);
    log_str("\n");
}

const char* port_speed_name(uint8_t speedId) {
    switch (speedId) {
        case 1:
            return "full";
        case 2:
            return "low";
        case 3:
            return "high";
        case 4:
            return "super";
        case 5:
            return "superplus";
        default:
            return "unknown";
    }
}

uint16_t port_speed_mbps(uint8_t speedId) {
    switch (speedId) {
        case 1:
            return 12;
        case 2:
            return 2;
        case 3:
            return 480;
        case 4:
            return 5000;
        case 5:
            return 10000;
        default:
            return 0;
    }
}

uint64_t page_floor(uint64_t value) {
    return value & ~(PAGE_SIZE - 1);
}

uint64_t page_ceil(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

void map_mmio_window(uint64_t base, uint64_t bytes) {
    if (!VMM::IsInitialized()) {
        return;
    }

    const uint64_t start = page_floor(base);
    const uint64_t end = page_ceil(base + bytes);
    const uint64_t pages = (end - start) / PAGE_SIZE;
    VMM::MapRange(start, start, pages, Present | ReadWrite | CacheDisab | WriteThru | NoExecute);
}

bool claim_bios_ownership(uint64_t mmioBase, const XHCICapabilityRegisters& caps) {
    const uint32_t xecp = (caps.hccParams1 & XHCI_HCCPARAMS1_XECP_MASK) >> XHCI_HCCPARAMS1_XECP_SHIFT;
    if (xecp == 0) {
        return true;
    }

    uint32_t extOffset = xecp << 2;
    uint32_t visited = 0;
    while (extOffset != 0 && visited++ < 64) {
        const uint32_t header = mmio_read32(mmioBase, extOffset);
        const uint32_t capId = header & XHCI_EXT_CAP_ID_MASK;
        const uint32_t next = (header & XHCI_EXT_CAP_NEXT_MASK) >> XHCI_EXT_CAP_NEXT_SHIFT;
        if (capId == XHCI_EXT_CAP_LEGACY_SUPPORT) {
            uint32_t legsup = mmio_read32(mmioBase, extOffset);
            if ((legsup & XHCI_USBLEGSUP_BIOS_OWNED) == 0) {
                return true;
            }

            mmio_write32(mmioBase, extOffset, legsup | XHCI_USBLEGSUP_OS_OWNED);
            for (uint32_t i = 0; i < 2000000; ++i) {
                io_wait();
                legsup = mmio_read32(mmioBase, extOffset);
                if ((legsup & XHCI_USBLEGSUP_BIOS_OWNED) == 0) {
                    mmio_write32(mmioBase, extOffset + 4, 0);
                    log_str("[usb:xhci] BIOS ownership released\n");
                    return true;
                }
            }

            log_str("[usb:xhci] BIOS ownership handoff timeout\n");
            return false;
        }
        if (next == 0) {
            break;
        }
        extOffset += next << 2;
    }

    return true;
}

uint64_t max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

uint16_t max_scratchpad_buffers(uint32_t hcsParams2) {
    const uint16_t lo = static_cast<uint16_t>((hcsParams2 >> 27) & 0x1F);
    const uint16_t hi = static_cast<uint16_t>((hcsParams2 >> 21) & 0x1F);
    return static_cast<uint16_t>((hi << 5) | lo);
}

void* alloc_dma_pages(uint64_t pages, uint64_t& outPhys) {
    outPhys = PMM::AllocFrames(pages);
    if (!outPhys) {
        return nullptr;
    }
    memset(reinterpret_cast<void*>(outPhys), 0, pages * PMM::PAGE_SIZE);
    return reinterpret_cast<void*>(outPhys);
}

uint32_t pages_for_bytes(uint64_t bytes) {
    return static_cast<uint32_t>((bytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE);
}

void io_wait() {
    asm volatile("pause");
}

bool wait_register_set(uint64_t base, uint32_t offset, uint32_t mask, bool set, uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        const bool isSet = (mmio_read32(base, offset) & mask) == mask;
        if (isSet == set) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool controller_has_pending_interrupt(const XHCIControllerState& state) {
    if (!state.mmioBase) {
        return false;
    }

    const uint32_t status = mmio_read32(state.operationalBase, XHCI_OP_USBSTS);
    if (status & (XHCI_STS_EINT | XHCI_STS_PCD)) {
        return true;
    }

    const uint64_t interrupterBase = state.runtimeBase + XHCI_RT_IR0;
    return (mmio_read32(interrupterBase, XHCI_IR_IMAN) & XHCI_IMAN_IP) != 0;
}

uint64_t port_base(const XHCIControllerState& state, uint8_t portIndex) {
    return state.operationalBase + XHCI_OP_PORT_BASE + static_cast<uint64_t>(portIndex) * XHCI_PORT_STRIDE;
}

uint32_t* input_control_context(XHCIControllerState& state, XHCISlotState& slot) {
    (void)state;
    return reinterpret_cast<uint32_t*>(slot.inputContext);
}

uint32_t* input_slot_context(XHCIControllerState& state, XHCISlotState& slot) {
    return reinterpret_cast<uint32_t*>(slot.inputContext + state.contextSize);
}

uint32_t* input_endpoint_context(XHCIControllerState& state, XHCISlotState& slot, uint8_t endpointIndex) {
    return reinterpret_cast<uint32_t*>(slot.inputContext + (static_cast<uint64_t>(endpointIndex) + 1) * state.contextSize);
}

uint32_t* output_slot_context(XHCIControllerState& state, XHCISlotState& slot) {
    return reinterpret_cast<uint32_t*>(slot.outputContext);
}

uint32_t* output_endpoint_context(XHCIControllerState& state, XHCISlotState& slot, uint8_t endpointIndex) {
    return reinterpret_cast<uint32_t*>(slot.outputContext + static_cast<uint64_t>(endpointIndex) * state.contextSize);
}

void populate_slot_context(XHCIControllerState& state, XHCISlotState& slot, uint8_t contextEntries) {
    auto* slotContext = input_slot_context(state, slot);
    memset(slotContext, 0, state.contextSize);
    if (contextEntries == 0) {
        contextEntries = 1;
    }

    slotContext[0] = (slot.routeString & 0xFFFFF) |
        (static_cast<uint32_t>(slot.speedId) << 20) |
        (static_cast<uint32_t>(contextEntries & 0x1F) << 27);
    slotContext[1] = (static_cast<uint32_t>(slot.rootPortId) << 16) |
        (static_cast<uint32_t>(slot.hubPortCount) << 24);
    slotContext[2] = static_cast<uint32_t>(slot.ttHubSlotId) |
        (static_cast<uint32_t>(slot.ttPortId) << 8) |
        (static_cast<uint32_t>(slot.ttThinkTime & 0x3) << 16);
}

uint16_t default_ep0_max_packet(uint8_t speedId) {
    switch (speedId) {
        case 3:
            return 64;
        case 4:
        case 5:
            return 512;
        default:
            return 8;
    }
}

uint16_t descriptor_ep0_max_packet(uint8_t speedId, uint8_t encoded, uint16_t fallback) {
    if (encoded == 0) {
        return fallback;
    }
    if (speedId >= 4 && encoded <= 9) {
        return static_cast<uint16_t>(1U << encoded);
    }
    return encoded;
}

uint16_t ep0_average_trb_length(uint16_t maxPacket) {
    return maxPacket ? maxPacket : 8;
}

uint8_t floor_log2_u16(uint16_t value) {
    uint8_t log = 0;
    while (value > 1) {
        value >>= 1;
        ++log;
    }
    return log;
}

uint8_t xhci_interrupt_interval(uint8_t speedId, uint8_t descriptorInterval) {
    if (descriptorInterval == 0) {
        descriptorInterval = 10;
    }
    if (speedId == 1 || speedId == 2) {
        uint16_t microframes = static_cast<uint16_t>(descriptorInterval) * 8;
        uint8_t interval = floor_log2_u16(microframes);
        if ((1U << interval) < microframes && interval < 10) {
            ++interval;
        }
        return interval;
    }

    uint8_t interval = static_cast<uint8_t>(descriptorInterval - 1);
    if (interval > 15) {
        interval = 15;
    }
    return interval;
}

uint8_t endpoint_dci(uint8_t endpointAddress) {
    const uint8_t endpointNumber = endpointAddress & 0x0F;
    const bool in = (endpointAddress & USB_ENDPOINT_IN) != 0;
    return static_cast<uint8_t>(endpointNumber * 2 + (in ? 1 : 0));
}

uint8_t max_endpoint_context_dci(const XHCISlotState& slot, uint8_t dci) {
    uint8_t maxDci = dci;
    if (slot.keyboardReady && slot.keyboardDci > maxDci) {
        maxDci = slot.keyboardDci;
    }
    if (slot.mouseReady && slot.mouseDci > maxDci) {
        maxDci = slot.mouseDci;
    }
    return maxDci;
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

uint16_t hid_modifier_byte_to_key_modifiers(uint8_t modifiers) {
    uint16_t out = KeyModifierNone;
    if (modifiers & 0x22) {
        out |= KeyModifierShift;
    }
    if (modifiers & 0x11) {
        out |= KeyModifierControl;
    }
    if (modifiers & 0x44) {
        out |= KeyModifierAlt;
    }
    if (modifiers & 0x88) {
        out |= KeyModifierSuper;
    }
    return out;
}

HIDKeyboardReportLayout boot_keyboard_layout() {
    HIDKeyboardReportLayout layout = {};
    layout.valid = true;
    layout.bootProtocol = true;
    layout.keyBitmap = false;
    layout.reportId = 0;
    layout.keyUsageMinimum = 0;
    layout.modifierBitOffset = 0;
    layout.keyArrayBitOffset = 16;
    layout.keyArrayCount = 6;
    layout.keyArrayReportSize = 8;
    return layout;
}

uint32_t hid_read_bits(const uint8_t* report, uint16_t reportLength, uint16_t bitOffset, uint8_t bitSize) {
    if (bitSize == 0 || bitSize > 16) {
        return 0;
    }

    uint32_t value = 0;
    for (uint8_t bit = 0; bit < bitSize; ++bit) {
        const uint16_t sourceBit = static_cast<uint16_t>(bitOffset + bit);
        const uint16_t byteIndex = sourceBit / 8;
        if (byteIndex >= reportLength) {
            break;
        }
        if ((report[byteIndex] & (1U << (sourceBit & 7))) != 0) {
            value |= 1U << bit;
        }
    }
    return value;
}

bool keyboard_report_matches_id(const HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    return layout.reportId == 0 ||
        (reportLength > 0 && report[0] == layout.reportId);
}

uint16_t keyboard_payload_bit_base(const HIDKeyboardReportLayout& layout) {
    return layout.reportId == 0 ? 0 : 8;
}

uint8_t keyboard_modifier_byte(const HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    uint8_t modifiers = 0;
    const uint16_t base = keyboard_payload_bit_base(layout);
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.modifierBitOffset + bit), 1) != 0) {
            modifiers |= static_cast<uint8_t>(1U << bit);
        }
    }
    return modifiers;
}

uint8_t keyboard_key_usage_at(const HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength, uint8_t index) {
    if (index >= layout.keyArrayCount) {
        return 0;
    }

    const uint16_t base = keyboard_payload_bit_base(layout);
    const uint16_t bitOffset = static_cast<uint16_t>(base + layout.keyArrayBitOffset +
        static_cast<uint16_t>(index) * layout.keyArrayReportSize);
    if (layout.keyBitmap) {
        return hid_read_bits(report, reportLength, bitOffset, layout.keyArrayReportSize) != 0 ?
            static_cast<uint8_t>((layout.keyUsageMinimum + index) & 0xFF) : 0;
    }
    return static_cast<uint8_t>(hid_read_bits(report, reportLength, bitOffset, layout.keyArrayReportSize) & 0xFF);
}

bool report_had_key(const HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength, uint8_t usage) {
    for (uint8_t i = 0; i < layout.keyArrayCount; ++i) {
        if (keyboard_key_usage_at(layout, report, reportLength, i) == usage) {
            return true;
        }
    }
    return false;
}

void log_hid_report_prefix(uint8_t interfaceNumber, const uint8_t* reportDescriptor, uint16_t length) {
    log_str("[usb:hid] if=");
    log_dec(interfaceNumber);
    log_str(" report-prefix=");
    const uint16_t prefixLength = length < 32 ? length : 32;
    for (uint16_t i = 0; i < prefixLength; ++i) {
        log_hex(reportDescriptor[i]);
        if (i + 1 < prefixLength) {
            log_str(" ");
        }
    }
    if (length > prefixLength) {
        log_str(" ...");
    }
    log_str("\n");
}

HIDKeyboardParseStatus parse_hid_keyboard_report_descriptor(const uint8_t* reportDescriptor, uint16_t length, HIDKeyboardReportLayout& outLayout) {
    HIDKeyboardReportLayout layout = {};
    layout.reportId = 0;
    layout.modifierBitOffset = 0xFFFF;
    layout.keyArrayBitOffset = 0xFFFF;
    layout.keyArrayReportSize = 8;
    layout.keyUsageMinimum = 0;
    bool sawKeyboardCollection = false;

    uint16_t offset = 0;
    uint16_t reportBitOffset = 0;
    uint16_t usagePage = 0;
    uint16_t usageMinimum = 0;
    uint16_t usageMaximum = 0;
    uint8_t reportSize = 0;
    uint8_t reportCount = 0;
    uint8_t reportId = 0;
    uint8_t collectionDepth = 0;
    uint8_t keyboardCollectionDepth = 0;
    bool pendingKeyboardApplication = false;

    while (offset < length) {
        const uint8_t prefix = reportDescriptor[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) {
                break;
            }
            const uint8_t dataSize = reportDescriptor[offset];
            offset = static_cast<uint16_t>(offset + 2 + dataSize);
            continue;
        }

        const uint8_t sizeCode = prefix & 0x03;
        const uint8_t dataSize = sizeCode == 3 ? 4 : sizeCode;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = (prefix >> 4) & 0x0F;
        if (offset + dataSize > length) {
            break;
        }

        uint32_t value = 0;
        for (uint8_t i = 0; i < dataSize; ++i) {
            value |= static_cast<uint32_t>(reportDescriptor[offset + i]) << (i * 8);
        }
        offset = static_cast<uint16_t>(offset + dataSize);

        if (type == 1) {
            if (tag == 0x0) {
                usagePage = static_cast<uint16_t>(value);
            } else if (tag == 0x7) {
                reportSize = static_cast<uint8_t>(value);
            } else if (tag == 0x8) {
                reportId = static_cast<uint8_t>(value);
                reportBitOffset = 0;
            } else if (tag == 0x9) {
                reportCount = static_cast<uint8_t>(value);
            }
        } else if (type == 2) {
            if (tag == 0x0) {
                const uint16_t usage = dataSize <= 2 ? static_cast<uint16_t>(value) : static_cast<uint16_t>(value & 0xFFFF);
                if (usagePage == 0x01 && usage == 0x06) {
                    pendingKeyboardApplication = true;
                }
            } else if (tag == 0x1) {
                usageMinimum = static_cast<uint16_t>(value);
            } else if (tag == 0x2) {
                usageMaximum = static_cast<uint16_t>(value);
            }
        } else if (type == 0) {
            if (tag == 0xA) {
                ++collectionDepth;
                if (pendingKeyboardApplication && value == 0x01) {
                    keyboardCollectionDepth = collectionDepth;
                    layout.reportId = reportId;
                    sawKeyboardCollection = true;
                }
                pendingKeyboardApplication = false;
                usageMinimum = 0;
                usageMaximum = 0;
            } else if (tag == 0xC) {
                if (keyboardCollectionDepth == collectionDepth) {
                    keyboardCollectionDepth = 0;
                }
                if (collectionDepth > 0) {
                    --collectionDepth;
                }
            } else if (tag == 0x8) {
                const bool inKeyboardCollection = keyboardCollectionDepth != 0;
                const bool variable = (value & 0x02) != 0;
                if (inKeyboardCollection && usagePage == 0x07) {
                    if (variable && usageMinimum <= 0xE0 && usageMaximum >= 0xE7 && reportSize == 1 && reportCount >= 8) {
                        layout.modifierBitOffset = reportBitOffset;
                    } else if (!variable && reportSize <= 8 && reportCount > 0) {
                        layout.keyArrayBitOffset = reportBitOffset;
                        layout.keyArrayCount = reportCount;
                        layout.keyArrayReportSize = reportSize ? reportSize : 8;
                        layout.keyBitmap = false;
                        layout.keyUsageMinimum = 0;
                    } else if (variable && usageMaximum < 0xE0 && reportSize <= 8 && reportCount > 0) {
                        layout.keyArrayBitOffset = reportBitOffset;
                        layout.keyArrayCount = reportCount;
                        layout.keyArrayReportSize = reportSize ? reportSize : 1;
                        layout.keyBitmap = true;
                        layout.keyUsageMinimum = usageMinimum;
                    }
                }
                reportBitOffset = static_cast<uint16_t>(reportBitOffset + static_cast<uint16_t>(reportSize) * reportCount);
                usageMinimum = 0;
                usageMaximum = 0;
            } else if (tag == 0x9 || tag == 0xB) {
                usageMinimum = 0;
                usageMaximum = 0;
            }
        }
    }

    if (!sawKeyboardCollection) {
        return HIDKeyboardParseStatus::NotKeyboard;
    }
    if (layout.keyArrayBitOffset == 0xFFFF || layout.keyArrayCount == 0) {
        return HIDKeyboardParseStatus::KeyboardWithoutKeys;
    }
    if (layout.modifierBitOffset == 0xFFFF) {
        layout.modifierBitOffset = 0;
    }
    if (!layout.keyBitmap && layout.keyArrayCount > 16) {
        layout.keyArrayCount = 16;
    } else if (layout.keyBitmap && layout.keyArrayCount > 128) {
        layout.keyArrayCount = 128;
    }
    layout.valid = true;
    layout.bootProtocol = false;
    outLayout = layout;
    return HIDKeyboardParseStatus::Keyboard;
}

void submit_keyboard_transfer(XHCIControllerState& state, XHCISlotState& slot);
void complete_keyboard_transfer(XHCIControllerState& state, XHCISlotState& slot);
void submit_mouse_transfer(XHCIControllerState& state, XHCISlotState& slot);
void complete_mouse_transfer(XHCIControllerState& state, XHCISlotState& slot);
bool xhci_control_transfer(XHCIControllerState& state, XHCISlotState& slot, const UsbSetupPacket& setup, void* data, uint16_t length);
bool bring_up_connected_port(XHCIControllerState& state, uint8_t portIndex);
bool enumerate_device(XHCIControllerState& state, const XHCIEnumerationTarget& target);
bool enumerate_hub(XHCIControllerState& state, XHCISlotState& hubSlot);

void portsc_write(XHCIControllerState& state, uint8_t portIndex, uint32_t status, uint32_t setBits, uint32_t clearChangeBits) {
    const uint32_t value = (status & XHCI_PORTSC_PRESERVE_MASK) |
        setBits |
        (clearChangeBits & XHCI_PORTSC_CHANGE_MASK);
    mmio_write32(port_base(state, portIndex), XHCI_PORTSC, value);
}

uint8_t port_speed_id(uint32_t status) {
    return static_cast<uint8_t>((status & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);
}

void log_port_status(uint8_t port, uint32_t status) {
    const uint8_t speedId = port_speed_id(status);
    log_str("[usb:xhci] port ");
    log_dec(port);
    log_str(" status=");
    log_hex(status);
    log_str(" connected=");
    log_dec((status & XHCI_PORTSC_CCS) ? 1 : 0);
    log_str(" enabled=");
    log_dec((status & XHCI_PORTSC_PED) ? 1 : 0);
    log_str(" speed=");
    log_dec(speedId);
    log_str("(");
    log_str(port_speed_name(speedId));
    log_str(" ");
    log_dec(port_speed_mbps(speedId));
    log_str("Mbps)");
    log_str("\n");
}

void log_enumeration_stage(
    XHCIControllerState& state,
    uint8_t portId,
    uint8_t slotId,
    const char* stage,
    const char* detail = nullptr
) {
    log_str("[usb:xhci] bus=");
    log_dec(state.bus);
    log_str(" port=");
    log_dec(portId);
    log_str(" addr=");
    log_dec(slotId);
    log_str(" stage=");
    log_str(stage);
    if (detail && detail[0] != '\0') {
        log_str(" ");
        log_str(detail);
    }
    log_str("\n");
}

void log_enumeration_failure(
    XHCIControllerState& state,
    uint8_t portId,
    uint8_t slotId,
    const char* stage,
    uint32_t status = 0,
    bool includeStatus = false
) {
    log_str("[usb:xhci] bus=");
    log_dec(state.bus);
    log_str(" port=");
    log_dec(portId);
    log_str(" addr=");
    log_dec(slotId);
    log_str(" stage=");
    log_str(stage);
    log_str(" failed");
    if (includeStatus) {
        log_str(" status=");
        log_hex(status);
    }
    log_str("\n");
}

bool wait_for_port_reset_complete(XHCIControllerState& state, uint8_t portIndex, uint32_t& status) {
    for (uint32_t i = 0; i < XHCI_PORT_RESET_TIMEOUT; ++i) {
        handle_controller_interrupt(state);
        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        if ((status & XHCI_PORTSC_CCS) == 0) {
            return false;
        }
        if ((status & XHCI_PORTSC_PR) == 0 &&
            ((status & XHCI_PORTSC_PRC) || (status & XHCI_PORTSC_PED))) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool wait_for_port_enabled(XHCIControllerState& state, uint8_t portIndex, uint32_t& status) {
    for (uint32_t i = 0; i < XHCI_PORT_RESET_TIMEOUT; ++i) {
        handle_controller_interrupt(state);
        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        if ((status & XHCI_PORTSC_PR) == 0 && (status & XHCI_PORTSC_PED)) {
            return true;
        }
        if ((status & XHCI_PORTSC_CCS) == 0) {
            return false;
        }
        io_wait();
    }
    return false;
}

void drain_events(XHCIControllerState& state) {
    if (!state.eventRing || !state.eventRingPhys) {
        return;
    }

    uint32_t drained = 0;
    while (drained < XHCI_EVENT_RING_TRBS) {
        XHCITransferRequestBlock& event = state.eventRing[state.eventDequeue];
        const bool cycle = (event.control & XHCI_TRB_CYCLE) != 0;
        if (cycle != state.eventCycle) {
            break;
        }

        log_event(event);
        if (trb_type(event) == XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT) {
            const uint32_t code = trb_completion_code(event);
            const uint8_t slotId = static_cast<uint8_t>(event.control >> XHCI_TRB_SLOT_ID_SHIFT);
            log_command_completion(event.parameter, code, slotId);
            if (state.commandPending && event_trb_pointer(event) == state.pendingCommandPhys) {
                state.commandPending = false;
                state.commandCompleted = true;
                state.lastCommandCompletionCode = code;
                state.lastCommandSlotId = slotId;
            }
        } else if (trb_type(event) == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            const uint8_t slotId = static_cast<uint8_t>(event.control >> XHCI_TRB_SLOT_ID_SHIFT);
            if (slotId != 0) {
                XHCISlotState& slot = state.slots[slotId];
                if (slot.transferPending && event_trb_pointer(event) == slot.pendingTransferPhys) {
                    slot.transferPending = false;
                    slot.transferCompleted = true;
                    slot.lastTransferCompletionCode = trb_completion_code(event);
                }
                if (slot.keyboardPending && event_trb_pointer(event) == slot.pendingKeyboardTransferPhys) {
                    slot.keyboardPending = false;
                    slot.lastKeyboardCompletionCode = trb_completion_code(event);
                    complete_keyboard_transfer(state, slot);
                }
                if (slot.mousePending && event_trb_pointer(event) == slot.pendingMouseTransferPhys) {
                    slot.mousePending = false;
                    slot.lastMouseCompletionCode = trb_completion_code(event);
                    complete_mouse_transfer(state, slot);
                }
            }
        } else if (trb_type(event) == XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT) {
            const uint8_t portId = static_cast<uint8_t>((event.parameter >> 24) & 0xFF);
            if (portId != 0 && portId <= state.maxPorts) {
                const uint8_t portIndex = static_cast<uint8_t>(portId - 1);
                uint32_t status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
                log_port_status(portId, status);
                if (!state.portBusy[portIndex]) {
                    bring_up_connected_port(state, portIndex);
                    status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
                    portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
                }
            }
        }

        ++state.eventDequeue;
        if (state.eventDequeue >= XHCI_EVENT_RING_TRBS) {
            state.eventDequeue = 0;
            state.eventCycle = !state.eventCycle;
        }
        ++drained;
    }

    const uint64_t erdp = state.eventRingPhys +
        static_cast<uint64_t>(state.eventDequeue) * sizeof(XHCITransferRequestBlock);
    const uint64_t interrupterBase = state.runtimeBase + XHCI_RT_IR0;
    mmio_write64(interrupterBase, XHCI_IR_ERDP, erdp | XHCI_ERDP_EHB);
}

uint64_t command_trb_phys(const XHCIControllerState& state, uint32_t index) {
    return state.commandRingPhys + static_cast<uint64_t>(index) * sizeof(XHCITransferRequestBlock);
}

void advance_command_ring(XHCIControllerState& state) {
    if (state.commandEnqueue + 1 >= XHCI_COMMAND_RING_TRBS - 1) {
        XHCITransferRequestBlock& link = state.commandRing[XHCI_COMMAND_RING_TRBS - 1];
        link.parameter = state.commandRingPhys;
        link.status = 0;
        link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_LINK_TOGGLE_CYCLE |
            (state.commandCycle ? XHCI_TRB_CYCLE : 0);
        state.commandEnqueue = 0;
        state.commandCycle = !state.commandCycle;
        return;
    }

    ++state.commandEnqueue;
}

bool submit_command(XHCIControllerState& state, XHCITransferRequestBlock command, XHCITransferRequestBlock* completion) {
    if (!state.commandRing || state.commandPending) {
        return false;
    }

    const uint32_t enqueue = state.commandEnqueue;
    command.control = (command.control & ~XHCI_TRB_CYCLE) | (state.commandCycle ? XHCI_TRB_CYCLE : 0);
    state.commandRing[enqueue] = command;

    state.pendingCommandPhys = command_trb_phys(state, enqueue);
    state.commandPending = true;
    state.commandCompleted = false;
    state.lastCommandCompletionCode = 0;
    state.lastCommandSlotId = 0;

    advance_command_ring(state);
    asm volatile("" ::: "memory");
    mmio_write32(state.doorbellBase, 0, 0);

    for (uint32_t i = 0; i < XHCI_COMMAND_TIMEOUT; ++i) {
        handle_controller_interrupt(state);
        if (state.commandCompleted) {
            if (completion) {
                completion->parameter = state.pendingCommandPhys;
                completion->status = state.lastCommandCompletionCode << XHCI_TRB_COMPLETION_CODE_SHIFT;
                completion->control = static_cast<uint32_t>(state.lastCommandSlotId) << XHCI_TRB_SLOT_ID_SHIFT;
            }
            return state.lastCommandCompletionCode == XHCI_COMPLETION_SUCCESS;
        }
        io_wait();
    }

    log_str("[usb:xhci] command timeout trb=");
    log_hex(state.pendingCommandPhys);
    log_str(" type=");
    log_dec(trb_type(command));
    log_str("\n");
    state.commandPending = false;
    return false;
}

bool command_enable_slot(XHCIControllerState& state, uint8_t* outSlotId) {
    XHCITransferRequestBlock command = {};
    command.control = XHCI_TRB_TYPE_ENABLE_SLOT_COMMAND << XHCI_TRB_TYPE_SHIFT;

    XHCITransferRequestBlock completion = {};
    if (!submit_command(state, command, &completion)) {
        return false;
    }

    if (outSlotId) {
        *outSlotId = static_cast<uint8_t>(completion.control >> XHCI_TRB_SLOT_ID_SHIFT);
    }
    return true;
}

bool command_address_device(XHCIControllerState& state, uint8_t slotId, uint64_t inputContextPhys, bool blockSetAddressRequest) {
    XHCITransferRequestBlock command = {};
    command.parameter = inputContextPhys;
    command.control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_COMMAND << XHCI_TRB_TYPE_SHIFT) |
        (blockSetAddressRequest ? (1U << 9) : 0) |
        (static_cast<uint32_t>(slotId) << XHCI_TRB_SLOT_ID_SHIFT);
    return submit_command(state, command, nullptr);
}

bool command_configure_endpoint(XHCIControllerState& state, uint8_t slotId, uint64_t inputContextPhys, bool deconfigure) {
    XHCITransferRequestBlock command = {};
    command.parameter = inputContextPhys;
    command.control = (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_COMMAND << XHCI_TRB_TYPE_SHIFT) |
        (deconfigure ? (1U << 9) : 0) |
        (static_cast<uint32_t>(slotId) << XHCI_TRB_SLOT_ID_SHIFT);
    return submit_command(state, command, nullptr);
}

bool command_evaluate_context(XHCIControllerState& state, uint8_t slotId, uint64_t inputContextPhys) {
    XHCITransferRequestBlock command = {};
    command.parameter = inputContextPhys;
    command.control = (XHCI_TRB_TYPE_EVALUATE_CONTEXT_COMMAND << XHCI_TRB_TYPE_SHIFT) |
        (static_cast<uint32_t>(slotId) << XHCI_TRB_SLOT_ID_SHIFT);
    return submit_command(state, command, nullptr);
}

bool command_stop_endpoint(XHCIControllerState& state, uint8_t slotId, uint8_t endpointId, bool suspend) {
    XHCITransferRequestBlock command = {};
    command.control = (XHCI_TRB_TYPE_STOP_ENDPOINT_COMMAND << XHCI_TRB_TYPE_SHIFT) |
        (static_cast<uint32_t>(endpointId) << XHCI_TRB_ENDPOINT_ID_SHIFT) |
        (suspend ? (1U << 23) : 0) |
        (static_cast<uint32_t>(slotId) << XHCI_TRB_SLOT_ID_SHIFT);
    return submit_command(state, command, nullptr);
}

bool command_reset_endpoint(XHCIControllerState& state, uint8_t slotId, uint8_t endpointId, bool transferStatePreserve) {
    XHCITransferRequestBlock command = {};
    command.control = (XHCI_TRB_TYPE_RESET_ENDPOINT_COMMAND << XHCI_TRB_TYPE_SHIFT) |
        (transferStatePreserve ? (1U << 9) : 0) |
        (static_cast<uint32_t>(endpointId) << XHCI_TRB_ENDPOINT_ID_SHIFT) |
        (static_cast<uint32_t>(slotId) << XHCI_TRB_SLOT_ID_SHIFT);
    return submit_command(state, command, nullptr);
}

void setup_keyboard_ring(XHCISlotState& slot) {
    memset(slot.keyboardRing, 0, slot.keyboardRingPages * PMM::PAGE_SIZE);
    slot.keyboardEnqueue = 0;
    slot.keyboardCycle = true;

    auto& link = slot.keyboardRing[XHCI_TRANSFER_RING_TRBS - 1];
    link.parameter = slot.keyboardRingPhys;
    link.status = 0;
    link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;
}

uint64_t keyboard_trb_phys(const XHCISlotState& slot, uint32_t index) {
    return slot.keyboardRingPhys + static_cast<uint64_t>(index) * sizeof(XHCITransferRequestBlock);
}

void advance_keyboard_ring(XHCISlotState& slot) {
    if (slot.keyboardEnqueue + 1 >= XHCI_TRANSFER_RING_TRBS - 1) {
        auto& link = slot.keyboardRing[XHCI_TRANSFER_RING_TRBS - 1];
        link.parameter = slot.keyboardRingPhys;
        link.status = 0;
        link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_LINK_TOGGLE_CYCLE |
            (slot.keyboardCycle ? XHCI_TRB_CYCLE : 0);
        slot.keyboardEnqueue = 0;
        slot.keyboardCycle = !slot.keyboardCycle;
        return;
    }

    ++slot.keyboardEnqueue;
}

uint64_t enqueue_keyboard_trb(XHCISlotState& slot, XHCITransferRequestBlock trb) {
    const uint32_t index = slot.keyboardEnqueue;
    trb.control = (trb.control & ~XHCI_TRB_CYCLE) | (slot.keyboardCycle ? XHCI_TRB_CYCLE : 0);
    slot.keyboardRing[index] = trb;
    advance_keyboard_ring(slot);
    return keyboard_trb_phys(slot, index);
}

void submit_keyboard_transfer(XHCIControllerState& state, XHCISlotState& slot) {
    if (!slot.keyboardReady || slot.keyboardPending || !slot.keyboardReport || !slot.keyboardRing) {
        return;
    }

    memset(slot.keyboardReport, 0, slot.keyboardMaxPacket);
    XHCITransferRequestBlock trb = {};
    trb.parameter = slot.keyboardReportPhys;
    trb.status = slot.keyboardMaxPacket;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_ISP |
        XHCI_TRB_IOC;
    slot.pendingKeyboardTransferPhys = enqueue_keyboard_trb(slot, trb);
    slot.keyboardPending = true;
    slot.lastKeyboardCompletionCode = 0;
    asm volatile("" ::: "memory");
    mmio_write32(state.doorbellBase, slot.slotId * 4, slot.keyboardDci);
}

void complete_keyboard_transfer(XHCIControllerState& state, XHCISlotState& slot) {
    if (slot.lastKeyboardCompletionCode != XHCI_COMPLETION_SUCCESS &&
        slot.lastKeyboardCompletionCode != 13) {
        log_str("[usb:kbd] interrupt cc=");
        log_dec(slot.lastKeyboardCompletionCode);
        log_str("(");
        log_str(completion_code_name(slot.lastKeyboardCompletionCode));
        log_str(")\n");
        submit_keyboard_transfer(state, slot);
        return;
    }

    const uint8_t* report = slot.keyboardReport;
    const HIDKeyboardReportLayout& layout = slot.keyboardLayout.valid ? slot.keyboardLayout : boot_keyboard_layout();
    if (!keyboard_report_matches_id(layout, report, slot.keyboardMaxPacket)) {
        submit_keyboard_transfer(state, slot);
        return;
    }

    const uint8_t hidModifiers = keyboard_modifier_byte(layout, report, slot.keyboardMaxPacket);
    const uint16_t keyModifiers = hid_modifier_byte_to_key_modifiers(hidModifiers);
    const bool shift = (keyModifiers & KeyModifierShift) != 0;
    for (uint8_t i = 0; i < layout.keyArrayCount; ++i) {
        const uint8_t usage = keyboard_key_usage_at(layout, report, slot.keyboardMaxPacket, i);
        if (usage == 0 || report_had_key(layout, slot.lastKeyboardReport, slot.keyboardMaxPacket, usage)) {
            continue;
        }

        const char c = hid_usage_to_char(usage, shift);
        if (c != 0) {
            Keyboard::get().injectKey(c, keyModifiers, "[usb:kbd]");
            g_inputActivity = true;
        }
    }

    memcpy(slot.lastKeyboardReport, report, sizeof(slot.lastKeyboardReport));
    submit_keyboard_transfer(state, slot);
}

bool send_hid_boot_requests(XHCIControllerState& state, XHCISlotState& slot) {
    UsbSetupPacket setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_PROTOCOL;
    setup.value = 0;
    setup.index = slot.keyboardInterface;
    setup.length = 0;
    if (!xhci_control_transfer(state, slot, setup, nullptr, 0)) {
        log_str("[usb:kbd] set boot protocol failed\n");
    }

    setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_IDLE;
    setup.value = 0;
    setup.index = slot.keyboardInterface;
    setup.length = 0;
    if (!xhci_control_transfer(state, slot, setup, nullptr, 0)) {
        log_str("[usb:kbd] set idle failed\n");
    }

    return true;
}

bool configure_keyboard_endpoint(
    XHCIControllerState& state,
    XHCISlotState& slot,
    uint8_t interfaceNumber,
    uint8_t endpointAddress,
    uint16_t maxPacket,
    uint8_t interval,
    const HIDKeyboardReportLayout& layout
) {
    const uint8_t dci = endpoint_dci(endpointAddress);
    if (dci == 0 || dci >= 32) {
        return false;
    }

    slot.keyboardDci = dci;
    slot.keyboardInterface = interfaceNumber;
    slot.keyboardMaxPacket = static_cast<uint8_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 8);
    if (slot.keyboardMaxPacket > USB_HID_MAX_KEYBOARD_REPORT) {
        slot.keyboardMaxPacket = USB_HID_MAX_KEYBOARD_REPORT;
    }
    slot.keyboardLayout = layout.valid ? layout : boot_keyboard_layout();
    if (interval == 0) {
        interval = 10;
    }

    slot.keyboardRingPages = pages_for_bytes(XHCI_TRANSFER_RING_TRBS * sizeof(XHCITransferRequestBlock));
    slot.keyboardReportPages = pages_for_bytes(slot.keyboardMaxPacket);
    slot.keyboardRing = reinterpret_cast<XHCITransferRequestBlock*>(alloc_dma_pages(slot.keyboardRingPages, slot.keyboardRingPhys));
    slot.keyboardReport = reinterpret_cast<uint8_t*>(alloc_dma_pages(slot.keyboardReportPages, slot.keyboardReportPhys));
    if (!slot.keyboardRing || !slot.keyboardReport) {
        log_str("[usb:kbd] interrupt ring allocation failed\n");
        return false;
    }
    setup_keyboard_ring(slot);
    memset(slot.lastKeyboardReport, 0, sizeof(slot.lastKeyboardReport));

    memset(slot.inputContext, 0, slot.inputContextPages * PMM::PAGE_SIZE);
    auto* control = input_control_context(state, slot);
    control[1] = (1U << 0) | (1U << dci);

    populate_slot_context(state, slot, max_endpoint_context_dci(slot, dci));

    auto* endpoint = input_endpoint_context(state, slot, dci);
    memset(endpoint, 0, state.contextSize);
    endpoint[0] = static_cast<uint32_t>(xhci_interrupt_interval(slot.speedId, interval)) << 16;
    endpoint[1] = (3U << 1) |
        (static_cast<uint32_t>(XHCI_ENDPOINT_TYPE_INTERRUPT_IN) << 3) |
        (static_cast<uint32_t>(slot.keyboardMaxPacket) << 16);
    endpoint[2] = static_cast<uint32_t>(slot.keyboardRingPhys | 1U);
    endpoint[3] = static_cast<uint32_t>(slot.keyboardRingPhys >> 32);
    endpoint[4] = slot.keyboardMaxPacket;

    if (!command_configure_endpoint(state, slot.slotId, slot.inputContextPhys, false)) {
        log_str("[usb:kbd] configure endpoint failed\n");
        return false;
    }

    if (slot.keyboardLayout.bootProtocol) {
        send_hid_boot_requests(state, slot);
    } else {
        UsbSetupPacket setup = {};
        setup.requestType = 0x21;
        setup.request = USB_REQUEST_SET_IDLE;
        setup.value = 0;
        setup.index = slot.keyboardInterface;
        setup.length = 0;
        if (!xhci_control_transfer(state, slot, setup, nullptr, 0)) {
            log_str("[usb:kbd] set idle failed\n");
        }
    }

    slot.keyboardReady = true;
    log_str("[usb:kbd] ready slot=");
    log_dec(slot.slotId);
    log_str(" if=");
    log_dec(slot.keyboardInterface);
    log_str(" dci=");
    log_dec(slot.keyboardDci);
    log_str(" max=");
    log_dec(slot.keyboardMaxPacket);
    log_str("\n");
    submit_keyboard_transfer(state, slot);
    return true;
}

void setup_mouse_ring(XHCISlotState& slot) {
    memset(slot.mouseRing, 0, slot.mouseRingPages * PMM::PAGE_SIZE);
    slot.mouseEnqueue = 0;
    slot.mouseCycle = true;

    auto& link = slot.mouseRing[XHCI_TRANSFER_RING_TRBS - 1];
    link.parameter = slot.mouseRingPhys;
    link.status = 0;
    link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;
}

uint64_t mouse_trb_phys(const XHCISlotState& slot, uint32_t index) {
    return slot.mouseRingPhys + static_cast<uint64_t>(index) * sizeof(XHCITransferRequestBlock);
}

void advance_mouse_ring(XHCISlotState& slot) {
    if (slot.mouseEnqueue + 1 >= XHCI_TRANSFER_RING_TRBS - 1) {
        auto& link = slot.mouseRing[XHCI_TRANSFER_RING_TRBS - 1];
        link.parameter = slot.mouseRingPhys;
        link.status = 0;
        link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_LINK_TOGGLE_CYCLE |
            (slot.mouseCycle ? XHCI_TRB_CYCLE : 0);
        slot.mouseEnqueue = 0;
        slot.mouseCycle = !slot.mouseCycle;
        return;
    }

    ++slot.mouseEnqueue;
}

uint64_t enqueue_mouse_trb(XHCISlotState& slot, XHCITransferRequestBlock trb) {
    const uint32_t index = slot.mouseEnqueue;
    trb.control = (trb.control & ~XHCI_TRB_CYCLE) | (slot.mouseCycle ? XHCI_TRB_CYCLE : 0);
    slot.mouseRing[index] = trb;
    advance_mouse_ring(slot);
    return mouse_trb_phys(slot, index);
}

void submit_mouse_transfer(XHCIControllerState& state, XHCISlotState& slot) {
    if (!slot.mouseReady || slot.mousePending || !slot.mouseReport || !slot.mouseRing) {
        return;
    }

    memset(slot.mouseReport, 0, slot.mouseMaxPacket);
    XHCITransferRequestBlock trb = {};
    trb.parameter = slot.mouseReportPhys;
    trb.status = slot.mouseMaxPacket;
    trb.control = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_ISP |
        XHCI_TRB_IOC;
    slot.pendingMouseTransferPhys = enqueue_mouse_trb(slot, trb);
    slot.mousePending = true;
    slot.lastMouseCompletionCode = 0;
    asm volatile("" ::: "memory");
    mmio_write32(state.doorbellBase, slot.slotId * 4, slot.mouseDci);
}

void complete_mouse_transfer(XHCIControllerState& state, XHCISlotState& slot) {
    if (slot.lastMouseCompletionCode != XHCI_COMPLETION_SUCCESS &&
        slot.lastMouseCompletionCode != 13) {
        log_str("[usb:mouse] interrupt cc=");
        log_dec(slot.lastMouseCompletionCode);
        log_str("(");
        log_str(completion_code_name(slot.lastMouseCompletionCode));
        log_str(")\n");
        submit_mouse_transfer(state, slot);
        return;
    }

    if (slot.mouseMaxPacket >= 3) {
        const uint8_t* report = slot.mouseReport;
        const int8_t dx = static_cast<int8_t>(report[1]);
        const int8_t dy = static_cast<int8_t>(report[2]);
        const int8_t wheel = slot.mouseMaxPacket >= 4 ? static_cast<int8_t>(report[3]) : 0;
        if ((report[0] & 0x07) != 0 || dx != 0 || dy != 0 || wheel != 0) {
            Keyboard::get().injectPointerDelta(report[0], dx, dy, wheel, "[usb:mouse]");
            g_inputActivity = true;
        }
    }

    submit_mouse_transfer(state, slot);
}

bool send_hid_mouse_boot_requests(XHCIControllerState& state, XHCISlotState& slot) {
    UsbSetupPacket setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_PROTOCOL;
    setup.value = 0;
    setup.index = slot.mouseInterface;
    setup.length = 0;
    if (!xhci_control_transfer(state, slot, setup, nullptr, 0)) {
        log_str("[usb:mouse] set boot protocol failed\n");
    }

    setup = {};
    setup.requestType = 0x21;
    setup.request = USB_REQUEST_SET_IDLE;
    setup.value = 0;
    setup.index = slot.mouseInterface;
    setup.length = 0;
    if (!xhci_control_transfer(state, slot, setup, nullptr, 0)) {
        log_str("[usb:mouse] set idle failed\n");
    }

    return true;
}

bool configure_mouse_endpoint(
    XHCIControllerState& state,
    XHCISlotState& slot,
    uint8_t interfaceNumber,
    uint8_t endpointAddress,
    uint16_t maxPacket,
    uint8_t interval
) {
    const uint8_t dci = endpoint_dci(endpointAddress);
    if (dci == 0 || dci >= 32) {
        return false;
    }

    slot.mouseDci = dci;
    slot.mouseInterface = interfaceNumber;
    slot.mouseMaxPacket = static_cast<uint8_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 3);
    if (slot.mouseMaxPacket < 3) {
        slot.mouseMaxPacket = 3;
    }
    if (slot.mouseMaxPacket > 8) {
        slot.mouseMaxPacket = 8;
    }
    if (interval == 0) {
        interval = 10;
    }

    slot.mouseRingPages = pages_for_bytes(XHCI_TRANSFER_RING_TRBS * sizeof(XHCITransferRequestBlock));
    slot.mouseReportPages = pages_for_bytes(slot.mouseMaxPacket);
    slot.mouseRing = reinterpret_cast<XHCITransferRequestBlock*>(alloc_dma_pages(slot.mouseRingPages, slot.mouseRingPhys));
    slot.mouseReport = reinterpret_cast<uint8_t*>(alloc_dma_pages(slot.mouseReportPages, slot.mouseReportPhys));
    if (!slot.mouseRing || !slot.mouseReport) {
        log_str("[usb:mouse] interrupt ring allocation failed\n");
        return false;
    }
    setup_mouse_ring(slot);

    memset(slot.inputContext, 0, slot.inputContextPages * PMM::PAGE_SIZE);
    auto* control = input_control_context(state, slot);
    control[1] = (1U << 0) | (1U << dci);

    populate_slot_context(state, slot, max_endpoint_context_dci(slot, dci));

    auto* endpoint = input_endpoint_context(state, slot, dci);
    memset(endpoint, 0, state.contextSize);
    endpoint[0] = static_cast<uint32_t>(xhci_interrupt_interval(slot.speedId, interval)) << 16;
    endpoint[1] = (3U << 1) |
        (static_cast<uint32_t>(XHCI_ENDPOINT_TYPE_INTERRUPT_IN) << 3) |
        (static_cast<uint32_t>(slot.mouseMaxPacket) << 16);
    endpoint[2] = static_cast<uint32_t>(slot.mouseRingPhys | 1U);
    endpoint[3] = static_cast<uint32_t>(slot.mouseRingPhys >> 32);
    endpoint[4] = slot.mouseMaxPacket;

    if (!command_configure_endpoint(state, slot.slotId, slot.inputContextPhys, false)) {
        log_str("[usb:mouse] configure endpoint failed\n");
        return false;
    }

    send_hid_mouse_boot_requests(state, slot);

    slot.mouseReady = true;
    log_str("[usb:mouse] ready slot=");
    log_dec(slot.slotId);
    log_str(" if=");
    log_dec(slot.mouseInterface);
    log_str(" dci=");
    log_dec(slot.mouseDci);
    log_str(" max=");
    log_dec(slot.mouseMaxPacket);
    log_str("\n");
    submit_mouse_transfer(state, slot);
    return true;
}

void setup_ep0_context(XHCIControllerState& state, XHCISlotState& slot) {
    auto* ep0 = input_endpoint_context(state, slot, 1);
    memset(ep0, 0, state.contextSize);
    ep0[1] = (3U << 1) |
        (static_cast<uint32_t>(XHCI_ENDPOINT_TYPE_CONTROL) << 3) |
        (static_cast<uint32_t>(slot.ep0MaxPacket) << 16);
    ep0[2] = static_cast<uint32_t>(slot.ep0RingPhys | 1U);
    ep0[3] = static_cast<uint32_t>(slot.ep0RingPhys >> 32);
    ep0[4] = ep0_average_trb_length(slot.ep0MaxPacket);
}

void setup_address_input_context(XHCIControllerState& state, XHCISlotState& slot) {
    memset(slot.inputContext, 0, slot.inputContextPages * PMM::PAGE_SIZE);

    auto* control = input_control_context(state, slot);
    control[1] = (1U << 0) | (1U << 1);

    populate_slot_context(state, slot, 1);

    setup_ep0_context(state, slot);
}

void setup_evaluate_ep0_context(XHCIControllerState& state, XHCISlotState& slot) {
    memset(slot.inputContext, 0, slot.inputContextPages * PMM::PAGE_SIZE);
    auto* control = input_control_context(state, slot);
    control[1] = 1U << 1;
    setup_ep0_context(state, slot);
}

void setup_ep0_ring(XHCISlotState& slot) {
    memset(slot.ep0Ring, 0, slot.ep0RingPages * PMM::PAGE_SIZE);
    slot.ep0Enqueue = 0;
    slot.ep0Cycle = true;

    auto& link = slot.ep0Ring[XHCI_TRANSFER_RING_TRBS - 1];
    link.parameter = slot.ep0RingPhys;
    link.status = 0;
    link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;
}

bool allocate_slot_resources(
    XHCIControllerState& state,
    XHCISlotState& slot,
    uint8_t slotId,
    const XHCIEnumerationTarget& target
) {
    slot = {};
    slot.allocated = true;
    slot.slotId = slotId;
    slot.portId = target.portId;
    slot.rootPortId = target.rootPortId ? target.rootPortId : target.portId;
    slot.speedId = target.speedId;
    slot.routeDepth = target.routeDepth;
    slot.ttHubSlotId = target.ttHubSlotId;
    slot.ttPortId = target.ttPortId;
    slot.ttThinkTime = target.ttThinkTime;
    slot.routeString = target.routeString;
    slot.ep0MaxPacket = default_ep0_max_packet(target.speedId);
    slot.inputContextPages = pages_for_bytes(static_cast<uint64_t>(state.contextSize) * 33);
    slot.outputContextPages = pages_for_bytes(static_cast<uint64_t>(state.contextSize) * 32);
    slot.ep0RingPages = pages_for_bytes(XHCI_TRANSFER_RING_TRBS * sizeof(XHCITransferRequestBlock));

    slot.inputContext = reinterpret_cast<uint8_t*>(alloc_dma_pages(slot.inputContextPages, slot.inputContextPhys));
    slot.outputContext = reinterpret_cast<uint8_t*>(alloc_dma_pages(slot.outputContextPages, slot.outputContextPhys));
    slot.ep0Ring = reinterpret_cast<XHCITransferRequestBlock*>(alloc_dma_pages(slot.ep0RingPages, slot.ep0RingPhys));
    if (!slot.inputContext || !slot.outputContext || !slot.ep0Ring) {
        log_str("[usb:xhci] slot resource allocation failed\n");
        return false;
    }

    setup_ep0_ring(slot);
    state.dcbaa[slotId] = slot.outputContextPhys;
    return true;
}

uint64_t ep0_trb_phys(const XHCISlotState& slot, uint32_t index) {
    return slot.ep0RingPhys + static_cast<uint64_t>(index) * sizeof(XHCITransferRequestBlock);
}

void advance_ep0_ring(XHCISlotState& slot) {
    if (slot.ep0Enqueue + 1 >= XHCI_TRANSFER_RING_TRBS - 1) {
        auto& link = slot.ep0Ring[XHCI_TRANSFER_RING_TRBS - 1];
        link.parameter = slot.ep0RingPhys;
        link.status = 0;
        link.control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_LINK_TOGGLE_CYCLE |
            (slot.ep0Cycle ? XHCI_TRB_CYCLE : 0);
        slot.ep0Enqueue = 0;
        slot.ep0Cycle = !slot.ep0Cycle;
        return;
    }

    ++slot.ep0Enqueue;
}

uint64_t enqueue_ep0_trb(XHCISlotState& slot, XHCITransferRequestBlock trb) {
    const uint32_t index = slot.ep0Enqueue;
    trb.control = (trb.control & ~XHCI_TRB_CYCLE) | (slot.ep0Cycle ? XHCI_TRB_CYCLE : 0);
    slot.ep0Ring[index] = trb;
    advance_ep0_ring(slot);
    return ep0_trb_phys(slot, index);
}

bool wait_transfer(XHCIControllerState& state, XHCISlotState& slot) {
    for (uint32_t i = 0; i < XHCI_TRANSFER_TIMEOUT; ++i) {
        handle_controller_interrupt(state);
        if (slot.transferCompleted) {
            return slot.lastTransferCompletionCode == XHCI_COMPLETION_SUCCESS ||
                slot.lastTransferCompletionCode == 13;
        }
        io_wait();
    }

    log_str("[usb:xhci] transfer timeout slot=");
    log_dec(slot.slotId);
    log_str(" trb=");
    log_hex(slot.pendingTransferPhys);
    log_str("\n");
    slot.transferPending = false;
    return false;
}

bool xhci_control_transfer(
    XHCIControllerState& state,
    XHCISlotState& slot,
    const UsbSetupPacket& setup,
    void* data,
    uint16_t length
) {
    if (!slot.ep0Ring || length > XHCI_MAX_CONTROL_TRANSFER_BYTES) {
        return false;
    }

    uint64_t dataPhys = 0;
    void* dma = nullptr;
    uint32_t dmaPages = 0;
    const bool in = (setup.requestType & 0x80) != 0;
    if (length > 0) {
        dmaPages = pages_for_bytes(length);
        dma = alloc_dma_pages(dmaPages, dataPhys);
        if (!dma) {
            return false;
        }
        if (!in && data) {
            memcpy(dma, data, length);
        }
    }

    uint64_t setupParam = 0;
    memcpy(&setupParam, &setup, sizeof(setup));

    XHCITransferRequestBlock setupTrb = {};
    setupTrb.parameter = setupParam;
    setupTrb.status = 8;
    const uint32_t trt = length == 0 ? XHCI_TRB_SETUP_TRT_NO_DATA :
        (in ? XHCI_TRB_SETUP_TRT_IN : XHCI_TRB_SETUP_TRT_OUT);
    setupTrb.control = (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_IDT |
        (trt << XHCI_TRB_SETUP_TRT_SHIFT);
    enqueue_ep0_trb(slot, setupTrb);

    if (length > 0) {
        XHCITransferRequestBlock dataTrb = {};
        dataTrb.parameter = dataPhys;
        dataTrb.status = length;
        dataTrb.control = (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
            (in ? XHCI_TRB_DATA_DIR_IN : 0);
        enqueue_ep0_trb(slot, dataTrb);
    }

    XHCITransferRequestBlock statusTrb = {};
    statusTrb.status = 0;
    statusTrb.control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_IOC |
        (!in ? XHCI_TRB_STATUS_DIR_IN : 0);
    const uint64_t statusTrbPhys = enqueue_ep0_trb(slot, statusTrb);

    slot.pendingTransferPhys = statusTrbPhys;
    slot.transferPending = true;
    slot.transferCompleted = false;
    slot.lastTransferCompletionCode = 0;
    asm volatile("" ::: "memory");
    mmio_write32(state.doorbellBase, slot.slotId * 4, 1);

    const bool ok = wait_transfer(state, slot);
    if (ok && in && data && length > 0) {
        memcpy(data, dma, length);
    }
    if (dma && dmaPages) {
        PMM::FreeFrames(dataPhys, dmaPages);
    }
    if (!ok) {
        log_str("[usb:xhci] control failed slot=");
        log_dec(slot.slotId);
        log_str(" cc=");
        log_dec(slot.lastTransferCompletionCode);
        log_str("(");
        log_str(completion_code_name(slot.lastTransferCompletionCode));
        log_str(")\n");
    }
    return ok;
}

bool get_descriptor(XHCIControllerState& state, XHCISlotState& slot, uint8_t type, uint16_t length, void* out) {
    UsbSetupPacket setup = {};
    setup.requestType = 0x80;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = static_cast<uint16_t>(type << 8);
    setup.length = length;
    return xhci_control_transfer(state, slot, setup, out, length);
}

bool get_hid_report_descriptor(
    XHCIControllerState& state,
    XHCISlotState& slot,
    uint8_t interfaceNumber,
    uint16_t length,
    void* out
) {
    UsbSetupPacket setup = {};
    setup.requestType = 0x81;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = static_cast<uint16_t>(USB_DESCRIPTOR_HID_REPORT << 8);
    setup.index = interfaceNumber;
    setup.length = length;
    return xhci_control_transfer(state, slot, setup, out, length);
}

bool set_configuration(XHCIControllerState& state, XHCISlotState& slot, uint8_t configValue) {
    UsbSetupPacket setup = {};
    setup.requestType = 0x00;
    setup.request = USB_REQUEST_SET_CONFIGURATION;
    setup.value = configValue;
    setup.length = 0;
    return xhci_control_transfer(state, slot, setup, nullptr, 0);
}

bool hub_control_transfer(
    XHCIControllerState& state,
    XHCISlotState& hubSlot,
    uint8_t requestType,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    void* data,
    uint16_t length
) {
    UsbSetupPacket setup = {};
    setup.requestType = requestType;
    setup.request = request;
    setup.value = value;
    setup.index = index;
    setup.length = length;
    return xhci_control_transfer(state, hubSlot, setup, data, length);
}

bool hub_get_descriptor(
    XHCIControllerState& state,
    XHCISlotState& hubSlot,
    uint8_t descriptorType,
    void* out,
    uint16_t length
) {
    return hub_control_transfer(
        state,
        hubSlot,
        0xA0,
        USB_REQUEST_GET_DESCRIPTOR,
        static_cast<uint16_t>(descriptorType << 8),
        0,
        out,
        length
    );
}

bool hub_get_port_status(XHCIControllerState& state, XHCISlotState& hubSlot, uint8_t port, uint32_t& status) {
    uint8_t bytes[4];
    memset(bytes, 0, sizeof(bytes));
    if (!hub_control_transfer(state, hubSlot, 0xA3, USB_REQUEST_GET_STATUS, 0, port, bytes, sizeof(bytes))) {
        return false;
    }
    status = static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

bool hub_set_port_feature(XHCIControllerState& state, XHCISlotState& hubSlot, uint8_t port, uint16_t feature) {
    return hub_control_transfer(state, hubSlot, 0x23, USB_REQUEST_SET_FEATURE, feature, port, nullptr, 0);
}

bool hub_clear_port_feature(XHCIControllerState& state, XHCISlotState& hubSlot, uint8_t port, uint16_t feature) {
    return hub_control_transfer(state, hubSlot, 0x23, USB_REQUEST_CLEAR_FEATURE, feature, port, nullptr, 0);
}

bool read_hub_descriptor(
    XHCIControllerState& state,
    XHCISlotState& hubSlot,
    uint8_t& portCount,
    uint16_t& characteristics,
    uint8_t& powerGoodDelay
) {
    uint8_t descriptor[64];
    memset(descriptor, 0, sizeof(descriptor));

    uint8_t descriptorType = USB_DESCRIPTOR_HUB;
    bool ok = hub_get_descriptor(state, hubSlot, descriptorType, descriptor, 8);
    if (!ok) {
        memset(descriptor, 0, sizeof(descriptor));
        descriptorType = USB_DESCRIPTOR_SUPERSPEED_HUB;
        ok = hub_get_descriptor(state, hubSlot, descriptorType, descriptor, 12);
    }
    if (!ok || descriptor[0] < 7) {
        log_str("[usb:hub] descriptor read failed slot=");
        log_dec(hubSlot.slotId);
        log_str("\n");
        return false;
    }

    uint16_t descriptorLength = descriptor[0];
    if (descriptorLength > sizeof(descriptor)) {
        descriptorLength = sizeof(descriptor);
    }
    if (descriptorLength > 12) {
        memset(descriptor, 0, sizeof(descriptor));
        if (!hub_get_descriptor(state, hubSlot, descriptorType, descriptor, descriptorLength)) {
            log_str("[usb:hub] full descriptor read failed slot=");
            log_dec(hubSlot.slotId);
            log_str("\n");
            return false;
        }
    }

    portCount = descriptor[2];
    characteristics = static_cast<uint16_t>(descriptor[3] | (descriptor[4] << 8));
    powerGoodDelay = descriptor[5];
    return portCount != 0;
}

void clear_hub_port_changes(XHCIControllerState& state, XHCISlotState& hubSlot, uint8_t port, uint32_t status) {
    if (status & USB_HUB_PORT_CONNECTION_CHANGE) {
        hub_clear_port_feature(state, hubSlot, port, USB_HUB_FEATURE_C_PORT_CONNECTION);
    }
    if (status & USB_HUB_PORT_ENABLE_CHANGE) {
        hub_clear_port_feature(state, hubSlot, port, USB_HUB_FEATURE_C_PORT_ENABLE);
    }
    if (status & USB_HUB_PORT_OVER_CURRENT_CHANGE) {
        hub_clear_port_feature(state, hubSlot, port, USB_HUB_FEATURE_C_PORT_OVER_CURRENT);
    }
    if (status & USB_HUB_PORT_RESET_CHANGE) {
        hub_clear_port_feature(state, hubSlot, port, USB_HUB_FEATURE_C_PORT_RESET);
    }
}

bool update_hub_slot_context(
    XHCIControllerState& state,
    XHCISlotState& hubSlot,
    uint8_t portCount,
    uint16_t characteristics
) {
    hubSlot.hubPortCount = portCount;
    hubSlot.ttThinkTime = static_cast<uint8_t>((characteristics >> 5) & 0x3);

    memset(hubSlot.inputContext, 0, hubSlot.inputContextPages * PMM::PAGE_SIZE);
    auto* control = input_control_context(state, hubSlot);
    control[1] = 1U << 0;
    populate_slot_context(state, hubSlot, max_endpoint_context_dci(hubSlot, 1));

    if (!command_evaluate_context(state, hubSlot.slotId, hubSlot.inputContextPhys)) {
        log_str("[usb:hub] evaluate context failed slot=");
        log_dec(hubSlot.slotId);
        log_str("\n");
        return false;
    }
    return true;
}

uint8_t hub_child_speed_id(const XHCISlotState& hubSlot, uint32_t portStatus) {
    if (hubSlot.speedId >= 4) {
        return hubSlot.speedId;
    }
    if (portStatus & USB_HUB_PORT_LOW_SPEED) {
        return 2;
    }
    if (portStatus & USB_HUB_PORT_HIGH_SPEED) {
        return 3;
    }
    return 1;
}

bool reset_hub_port(XHCIControllerState& state, XHCISlotState& hubSlot, uint8_t port, uint32_t& status) {
    if (!hub_set_port_feature(state, hubSlot, port, USB_HUB_FEATURE_PORT_RESET)) {
        log_str("[usb:hub] port reset request failed slot=");
        log_dec(hubSlot.slotId);
        log_str(" port=");
        log_dec(port);
        log_str("\n");
        return false;
    }

    for (uint32_t i = 0; i < XHCI_PORT_RESET_TIMEOUT; ++i) {
        handle_controller_interrupt(state);
        if (!hub_get_port_status(state, hubSlot, port, status)) {
            return false;
        }
        if ((status & USB_HUB_PORT_CONNECTION) == 0) {
            return false;
        }
        if ((status & USB_HUB_PORT_RESET_CHANGE) || ((status & USB_HUB_PORT_RESET) == 0 && (status & USB_HUB_PORT_ENABLE))) {
            clear_hub_port_changes(state, hubSlot, port, status);
            hub_get_port_status(state, hubSlot, port, status);
            return (status & USB_HUB_PORT_CONNECTION) && (status & USB_HUB_PORT_ENABLE);
        }
        io_wait();
    }

    log_str("[usb:hub] port reset timeout slot=");
    log_dec(hubSlot.slotId);
    log_str(" port=");
    log_dec(port);
    log_str(" status=");
    log_hex(status);
    log_str("\n");
    return false;
}

XHCIEnumerationTarget make_child_target(const XHCISlotState& hubSlot, uint8_t hubPort, uint8_t speedId) {
    XHCIEnumerationTarget target = {};
    target.portId = hubSlot.rootPortId;
    target.rootPortId = hubSlot.rootPortId;
    target.speedId = speedId;
    target.routeDepth = static_cast<uint8_t>(hubSlot.routeDepth + 1);
    target.routeString = hubSlot.routeString |
        (static_cast<uint32_t>(hubPort & 0x0F) << (hubSlot.routeDepth * 4));

    if (speedId <= 2) {
        if (hubSlot.speedId == 3) {
            target.ttHubSlotId = hubSlot.slotId;
            target.ttPortId = hubPort;
            target.ttThinkTime = hubSlot.ttThinkTime;
        } else {
            target.ttHubSlotId = hubSlot.ttHubSlotId;
            target.ttPortId = hubSlot.ttPortId;
            target.ttThinkTime = hubSlot.ttThinkTime;
        }
    }
    return target;
}

bool enumerate_hub(XHCIControllerState& state, XHCISlotState& hubSlot) {
    if (hubSlot.routeDepth >= XHCI_MAX_HUB_DEPTH) {
        log_str("[usb:hub] max depth reached slot=");
        log_dec(hubSlot.slotId);
        log_str("\n");
        return false;
    }

    uint8_t portCount = 0;
    uint16_t characteristics = 0;
    uint8_t powerGoodDelay = 0;
    if (!read_hub_descriptor(state, hubSlot, portCount, characteristics, powerGoodDelay)) {
        return false;
    }

    log_str("[usb:hub] slot=");
    log_dec(hubSlot.slotId);
    log_str(" ports=");
    log_dec(portCount);
    log_str(" route=");
    log_hex(hubSlot.routeString);
    log_str(" root=");
    log_dec(hubSlot.rootPortId);
    log_str("\n");

    update_hub_slot_context(state, hubSlot, portCount, characteristics);

    uint8_t portsToScan = portCount;
    if (portsToScan > XHCI_MAX_HUB_PORTS) {
        portsToScan = XHCI_MAX_HUB_PORTS;
    }

    for (uint8_t port = 1; port <= portsToScan; ++port) {
        hub_set_port_feature(state, hubSlot, port, USB_HUB_FEATURE_PORT_POWER);
    }

    spin_delay(static_cast<uint32_t>(powerGoodDelay ? powerGoodDelay : 50) * 10000);

    bool foundChild = false;
    for (uint8_t port = 1; port <= portsToScan; ++port) {
        uint32_t status = 0;
        if (!hub_get_port_status(state, hubSlot, port, status)) {
            continue;
        }
        clear_hub_port_changes(state, hubSlot, port, status);

        log_str("[usb:hub] slot=");
        log_dec(hubSlot.slotId);
        log_str(" port=");
        log_dec(port);
        log_str(" status=");
        log_hex(status);
        log_str("\n");

        if ((status & USB_HUB_PORT_CONNECTION) == 0) {
            continue;
        }
        if (!reset_hub_port(state, hubSlot, port, status)) {
            continue;
        }

        const uint8_t speedId = hub_child_speed_id(hubSlot, status);
        XHCIEnumerationTarget child = make_child_target(hubSlot, port, speedId);
        log_str("[usb:hub] enumerate child hub-slot=");
        log_dec(hubSlot.slotId);
        log_str(" port=");
        log_dec(port);
        log_str(" speed=");
        log_dec(speedId);
        log_str(" route=");
        log_hex(child.routeString);
        log_str("\n");

        if (enumerate_device(state, child)) {
            foundChild = true;
        }
    }

    return foundChild;
}

bool enumerate_device(XHCIControllerState& state, const XHCIEnumerationTarget& target) {
    const uint8_t portId = target.rootPortId ? target.rootPortId : target.portId;
    const uint8_t speedId = target.speedId;
    uint8_t slotId = 0;
    if (!command_enable_slot(state, &slotId) || slotId == 0) {
        log_enumeration_failure(state, portId, slotId, "enable-slot");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "slot-enabled");

    XHCISlotState& slot = state.slots[slotId];
    if (!allocate_slot_resources(state, slot, slotId, target)) {
        log_enumeration_failure(state, portId, slotId, "allocate-slot");
        return false;
    }

    setup_address_input_context(state, slot);
    if (!command_address_device(state, slotId, slot.inputContextPhys, true)) {
        log_enumeration_failure(state, portId, slotId, "default-address");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "default-addressed");

    uint8_t descriptor[XHCI_MAX_CONFIG_DESCRIPTOR_BYTES];
    memset(descriptor, 0, sizeof(descriptor));
    if (!get_descriptor(state, slot, USB_DESCRIPTOR_DEVICE, 8, descriptor)) {
        log_enumeration_failure(state, portId, slotId, "read-device-descriptor-8");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "device-descriptor-8");

    uint16_t ep0MaxPacket = descriptor_ep0_max_packet(speedId, descriptor[7], slot.ep0MaxPacket);
    if (ep0MaxPacket > 1024) {
        ep0MaxPacket = 1024;
    }
    slot.ep0MaxPacket = ep0MaxPacket;
    setup_evaluate_ep0_context(state, slot);
    if (!command_evaluate_context(state, slotId, slot.inputContextPhys)) {
        log_enumeration_failure(state, portId, slotId, "evaluate-ep0");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "ep0-max-packet");

    setup_address_input_context(state, slot);
    if (!command_address_device(state, slotId, slot.inputContextPhys, false)) {
        log_enumeration_failure(state, portId, slotId, "assign-address");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "address-assigned");
    spin_delay(XHCI_POST_ADDRESS_DELAY);

    memset(descriptor, 0, sizeof(descriptor));
    if (!get_descriptor(state, slot, USB_DESCRIPTOR_DEVICE, 18, descriptor)) {
        log_enumeration_failure(state, portId, slotId, "read-device-descriptor");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "device-descriptor");
    const uint8_t deviceClass = descriptor[4];

    log_str("[usb:xhci] device slot=");
    log_dec(slotId);
    log_str(" vid=");
    log_hex(static_cast<uint16_t>(descriptor[8] | (descriptor[9] << 8)));
    log_str(" pid=");
    log_hex(static_cast<uint16_t>(descriptor[10] | (descriptor[11] << 8)));
    log_str(" configs=");
    log_dec(descriptor[17]);
    log_str("\n");

    uint8_t configHeader[9];
    memset(configHeader, 0, sizeof(configHeader));
    if (!get_descriptor(state, slot, USB_DESCRIPTOR_CONFIGURATION, sizeof(configHeader), configHeader)) {
        log_enumeration_failure(state, portId, slotId, "read-config-header");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "config-header");

    uint16_t totalLength = static_cast<uint16_t>(configHeader[2] | (configHeader[3] << 8));
    if (totalLength == 0 || totalLength > sizeof(descriptor)) {
        totalLength = sizeof(descriptor);
    }

    memset(descriptor, 0, sizeof(descriptor));
    if (!get_descriptor(state, slot, USB_DESCRIPTOR_CONFIGURATION, totalLength, descriptor)) {
        log_enumeration_failure(state, portId, slotId, "read-full-descriptors");
        return false;
    }
    log_enumeration_stage(state, portId, slotId, "full-descriptors");

    const uint8_t configValue = descriptor[5];
    bool inBootKeyboardInterface = false;
    bool inReportKeyboardCandidateInterface = false;
    bool foundKeyboardEndpoint = false;
    bool deviceIsHub = deviceClass == USB_CLASS_HUB;
    uint8_t keyboardInterface = 0;
    uint8_t keyboardEndpointAddress = 0;
    uint16_t keyboardEndpointMaxPacket = 8;
    uint8_t keyboardEndpointInterval = 10;
    HIDKeyboardCandidate reportKeyboardCandidates[8] = {};
    uint8_t reportKeyboardCandidateCount = 0;
    uint16_t currentReportDescriptorLength = 0;
    uint8_t currentKeyboardInterface = 0;
    bool inBootMouseInterface = false;
    bool foundMouseEndpoint = false;
    uint8_t mouseInterface = 0;
    uint8_t mouseEndpointAddress = 0;
    uint16_t mouseEndpointMaxPacket = 3;
    uint8_t mouseEndpointInterval = 10;
    uint8_t currentMouseInterface = 0;

    for (uint16_t offset = 0; offset + 2 <= totalLength;) {
        const uint8_t len = descriptor[offset];
        const uint8_t type = descriptor[offset + 1];
        if (len < 2 || offset + len > totalLength) {
            break;
        }

        if (type == USB_DESCRIPTOR_INTERFACE && len >= 9) {
            currentKeyboardInterface = descriptor[offset + 2];
            inBootKeyboardInterface =
                descriptor[offset + 5] == USB_CLASS_HID &&
                descriptor[offset + 6] == USB_HID_SUBCLASS_BOOT &&
                descriptor[offset + 7] == USB_HID_PROTOCOL_KEYBOARD;
            inReportKeyboardCandidateInterface =
                descriptor[offset + 5] == USB_CLASS_HID &&
                !inBootKeyboardInterface &&
                descriptor[offset + 7] != USB_HID_PROTOCOL_MOUSE;
            currentReportDescriptorLength = 0;
            if (inBootKeyboardInterface) {
                keyboardInterface = currentKeyboardInterface;
                log_str("[usb:kbd] boot keyboard interface=");
                log_dec(keyboardInterface);
                log_str("\n");
            }
            currentMouseInterface = descriptor[offset + 2];
            inBootMouseInterface =
                descriptor[offset + 5] == USB_CLASS_HID &&
                descriptor[offset + 6] == USB_HID_SUBCLASS_BOOT &&
                descriptor[offset + 7] == USB_HID_PROTOCOL_MOUSE;
            if (inBootMouseInterface) {
                mouseInterface = currentMouseInterface;
                log_str("[usb:mouse] boot mouse interface=");
                log_dec(mouseInterface);
                log_str("\n");
            }
            if (descriptor[offset + 5] == USB_CLASS_HUB) {
                deviceIsHub = true;
            }
        } else if (type == USB_DESCRIPTOR_HID && len >= 9) {
            currentReportDescriptorLength = 0;
            const uint8_t descriptorCount = descriptor[offset + 5];
            uint16_t hidOffset = static_cast<uint16_t>(offset + 6);
            for (uint8_t i = 0; i < descriptorCount && hidOffset + 3 <= offset + len; ++i) {
                const uint8_t hidDescriptorType = descriptor[hidOffset];
                const uint16_t hidDescriptorLength = static_cast<uint16_t>(descriptor[hidOffset + 1] | (descriptor[hidOffset + 2] << 8));
                if (hidDescriptorType == USB_DESCRIPTOR_HID_REPORT) {
                    currentReportDescriptorLength = hidDescriptorLength;
                    break;
                }
                hidOffset = static_cast<uint16_t>(hidOffset + 3);
            }
        } else if (type == USB_DESCRIPTOR_ENDPOINT && len >= 7) {
            const uint8_t endpointAddress = descriptor[offset + 2];
            const uint8_t attributes = descriptor[offset + 3];
            const uint16_t maxPacket = static_cast<uint16_t>(descriptor[offset + 4] | (descriptor[offset + 5] << 8));
            if (inBootKeyboardInterface &&
                !foundKeyboardEndpoint &&
                (endpointAddress & USB_ENDPOINT_IN) &&
                ((attributes & 0x3) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                keyboardEndpointAddress = endpointAddress;
                keyboardEndpointMaxPacket = static_cast<uint16_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 8);
                keyboardEndpointInterval = descriptor[offset + 6] ? descriptor[offset + 6] : 10;
                keyboardInterface = currentKeyboardInterface;
                foundKeyboardEndpoint = true;
            }
            if (inReportKeyboardCandidateInterface &&
                currentReportDescriptorLength > 0 &&
                (endpointAddress & USB_ENDPOINT_IN) &&
                ((attributes & 0x3) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                bool knownInterface = false;
                for (uint8_t i = 0; i < reportKeyboardCandidateCount; ++i) {
                    if (reportKeyboardCandidates[i].interfaceNumber == currentKeyboardInterface) {
                        knownInterface = true;
                        break;
                    }
                }
                if (!knownInterface && reportKeyboardCandidateCount < sizeof(reportKeyboardCandidates) / sizeof(reportKeyboardCandidates[0])) {
                    HIDKeyboardCandidate& candidate = reportKeyboardCandidates[reportKeyboardCandidateCount++];
                    candidate.interfaceNumber = currentKeyboardInterface;
                    candidate.endpointAddress = endpointAddress;
                    candidate.endpointMaxPacket = static_cast<uint16_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 8);
                    candidate.endpointInterval = descriptor[offset + 6] ? descriptor[offset + 6] : 10;
                    candidate.reportDescriptorLength = currentReportDescriptorLength;
                }
            }
            if (inBootMouseInterface &&
                !foundMouseEndpoint &&
                (endpointAddress & USB_ENDPOINT_IN) &&
                ((attributes & 0x3) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                mouseEndpointAddress = endpointAddress;
                mouseEndpointMaxPacket = static_cast<uint16_t>((maxPacket & 0x7FF) ? (maxPacket & 0x7FF) : 3);
                mouseEndpointInterval = descriptor[offset + 6] ? descriptor[offset + 6] : 10;
                mouseInterface = currentMouseInterface;
                foundMouseEndpoint = true;
            }
        }

        offset += len;
    }

    if (!set_configuration(state, slot, configValue)) {
        log_enumeration_failure(state, portId, slotId, "set-configuration");
        return false;
    }
    slot.configured = true;
    log_enumeration_stage(state, portId, slotId, "configuration-set");

    HIDKeyboardReportLayout keyboardLayout = boot_keyboard_layout();
    if (!foundKeyboardEndpoint) {
        for (uint8_t candidateIndex = 0; candidateIndex < reportKeyboardCandidateCount; ++candidateIndex) {
            const HIDKeyboardCandidate& candidate = reportKeyboardCandidates[candidateIndex];
            uint8_t reportDescriptor[XHCI_MAX_HID_REPORT_DESCRIPTOR_BYTES];
            memset(reportDescriptor, 0, sizeof(reportDescriptor));
            uint16_t reportLength = candidate.reportDescriptorLength;
            if (reportLength > sizeof(reportDescriptor)) {
                reportLength = sizeof(reportDescriptor);
            }

            HIDKeyboardReportLayout parsedLayout = {};
            HIDKeyboardParseStatus parseStatus = HIDKeyboardParseStatus::NotKeyboard;
            const bool gotReport = reportLength > 0 &&
                get_hid_report_descriptor(state, slot, candidate.interfaceNumber, reportLength, reportDescriptor);
            if (gotReport) {
                log_hid_report_prefix(candidate.interfaceNumber, reportDescriptor, reportLength);
                parseStatus = parse_hid_keyboard_report_descriptor(reportDescriptor, reportLength, parsedLayout);
            }
            if (gotReport && parseStatus == HIDKeyboardParseStatus::Keyboard) {
                keyboardInterface = candidate.interfaceNumber;
                keyboardEndpointAddress = candidate.endpointAddress;
                keyboardEndpointMaxPacket = candidate.endpointMaxPacket;
                keyboardEndpointInterval = candidate.endpointInterval;
                keyboardLayout = parsedLayout;
                foundKeyboardEndpoint = true;
                log_str("[usb:kbd] report keyboard interface=");
                log_dec(keyboardInterface);
                log_str(" report-len=");
                log_dec(reportLength);
                log_str(" id=");
                log_dec(keyboardLayout.reportId);
                log_str("\n");
                break;
            }

            log_str("[usb:kbd] interface ");
            log_dec(candidate.interfaceNumber);
            log_str(" report descriptor ");
            if (!gotReport) {
                log_str("read failed\n");
            } else if (parseStatus == HIDKeyboardParseStatus::KeyboardWithoutKeys) {
                log_str("has keyboard collection but no key input field\n");
            } else {
                log_str("did not describe a keyboard\n");
            }
        }
    }

    bool endpointsOk = true;
    bool selectedInterface = false;
    if (foundKeyboardEndpoint) {
        selectedInterface = true;
        if (!configure_keyboard_endpoint(
            state,
            slot,
            keyboardInterface,
            keyboardEndpointAddress,
            keyboardEndpointMaxPacket,
            keyboardEndpointInterval,
            keyboardLayout
        )) {
            log_enumeration_failure(state, portId, slotId, "enable-keyboard-endpoint");
            endpointsOk = false;
        } else {
            log_enumeration_stage(state, portId, slotId, "keyboard-endpoint-enabled");
        }
    } else {
        log_str("[usb:kbd] no keyboard endpoint\n");
    }
    if (foundMouseEndpoint) {
        selectedInterface = true;
        if (!configure_mouse_endpoint(
            state,
            slot,
            mouseInterface,
            mouseEndpointAddress,
            mouseEndpointMaxPacket,
            mouseEndpointInterval
        )) {
            log_enumeration_failure(state, portId, slotId, "enable-mouse-endpoint");
            endpointsOk = false;
        } else {
            log_enumeration_stage(state, portId, slotId, "mouse-endpoint-enabled");
        }
    } else {
        log_str("[usb:mouse] no boot mouse endpoint\n");
    }

    if (deviceIsHub) {
        enumerate_hub(state, slot);
    }

    slot.usable = slot.configured && endpointsOk;
    if (selectedInterface && !endpointsOk) {
        log_enumeration_failure(state, portId, slotId, "final-state");
        return false;
    }

    log_str("[usb:xhci] configured slot=");
    log_dec(slotId);
    log_str(" config=");
    log_dec(configValue);
    log_str(" total=");
    log_dec(totalLength);
    log_str(" usable=");
    log_dec(slot.usable ? 1 : 0);
    log_str("\n");
    log_enumeration_stage(state, portId, slotId, slot.usable ? "usable" : "configured-no-endpoints");
    return true;
}

bool reset_connected_port(XHCIControllerState& state, uint8_t portIndex) {
    const uint8_t portId = static_cast<uint8_t>(portIndex + 1);
    uint32_t status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
    if ((status & XHCI_PORTSC_CCS) == 0) {
        return false;
    }

    log_enumeration_stage(state, portId, 0, "port-reset-started");
    portsc_write(state, portIndex, status, XHCI_PORTSC_PR, XHCI_PORTSC_CHANGE_MASK);
    if (!wait_for_port_reset_complete(state, portIndex, status)) {
        log_enumeration_failure(state, portId, 0, "port-reset-complete", status, true);
        return false;
    }
    log_enumeration_stage(state, portId, 0, "port-reset-completed");

    portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
    spin_delay(XHCI_PORT_POST_RESET_DELAY);

    if (!wait_for_port_enabled(state, portIndex, status)) {
        log_enumeration_failure(state, portId, 0, "port-enabled", status, true);
        return false;
    }
    log_enumeration_stage(state, portId, 0, "port-enabled");

    return true;
}

bool bring_up_connected_port(XHCIControllerState& state, uint8_t portIndex) {
    const uint8_t portId = static_cast<uint8_t>(portIndex + 1);
    if (state.portBusy[portIndex]) {
        return false;
    }
    state.portBusy[portIndex] = true;

    uint32_t status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
    if ((status & XHCI_PORTSC_CCS) == 0) {
        log_enumeration_stage(state, portId, 0, "disconnected");
        portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
        state.portBusy[portIndex] = false;
        return false;
    }

    log_enumeration_stage(state, portId, 0, "port-connected");
    if (state.hccParams1 & XHCI_HCCPARAMS1_PPC) {
        portsc_write(state, portIndex, status, XHCI_PORTSC_PP, XHCI_PORTSC_CHANGE_MASK);
        spin_delay(XHCI_PORT_SETTLE_DELAY);
        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
    }

    portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
    spin_delay(XHCI_PORT_SETTLE_DELAY);

    for (uint8_t attempt = 0; attempt < XHCI_ENUMERATION_RETRIES; ++attempt) {
        log_str("[usb:xhci] bus=");
        log_dec(state.bus);
        log_str(" port=");
        log_dec(portId);
        log_str(" addr=0 stage=enumeration-attempt try=");
        log_dec(static_cast<uint64_t>(attempt) + 1);
        log_str("\n");

        if (!reset_connected_port(state, portIndex)) {
            continue;
        }

        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        log_port_status(portId, status);
        XHCIEnumerationTarget target = {};
        target.portId = portId;
        target.rootPortId = portId;
        target.speedId = port_speed_id(status);
        if (enumerate_device(state, target)) {
            state.portBusy[portIndex] = false;
            return true;
        }

        log_enumeration_failure(state, portId, 0, "enumeration-attempt");
        spin_delay(XHCI_PORT_SETTLE_DELAY);
    }

    log_enumeration_failure(state, portId, 0, "final-state");
    state.portBusy[portIndex] = false;
    return false;
}

void initialize_root_ports(XHCIControllerState& state) {
    log_str("[usb:xhci] root ports=");
    log_dec(state.maxPorts);
    log_str(" ppc=");
    log_dec((state.hccParams1 & XHCI_HCCPARAMS1_PPC) ? 1 : 0);
    log_str("\n");

    for (uint8_t portIndex = 0; portIndex < state.maxPorts; ++portIndex) {
        uint32_t status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        if (state.hccParams1 & XHCI_HCCPARAMS1_PPC) {
            portsc_write(state, portIndex, status, XHCI_PORTSC_PP, XHCI_PORTSC_CHANGE_MASK);
            for (uint32_t i = 0; i < 100000; ++i) {
                io_wait();
            }
            status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        }

        portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);

        if ((status & XHCI_PORTSC_CCS) == 0) {
            log_port_status(static_cast<uint8_t>(portIndex + 1), status);
            continue;
        }

        log_port_status(static_cast<uint8_t>(portIndex + 1), status);
        bring_up_connected_port(state, portIndex);
        status = mmio_read32(port_base(state, portIndex), XHCI_PORTSC);
        portsc_write(state, portIndex, status, 0, XHCI_PORTSC_CHANGE_MASK);
    }
}

void handle_controller_interrupt(XHCIControllerState& state) {
    const uint64_t interrupterBase = state.runtimeBase + XHCI_RT_IR0;
    const uint32_t status = mmio_read32(state.operationalBase, XHCI_OP_USBSTS);
    const uint32_t iman = mmio_read32(interrupterBase, XHCI_IR_IMAN);

    if ((status & (XHCI_STS_EINT | XHCI_STS_PCD)) == 0 && (iman & XHCI_IMAN_IP) == 0) {
        return;
    }

    drain_events(state);

    if (status & (XHCI_STS_EINT | XHCI_STS_PCD)) {
        mmio_write32(state.operationalBase, XHCI_OP_USBSTS, status & (XHCI_STS_EINT | XHCI_STS_PCD));
    }
    if (iman & XHCI_IMAN_IP) {
        mmio_write32(interrupterBase, XHCI_IR_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);
    }
}

bool read_xhci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint64_t& outBase, bool& outIs64) {
    const uint32_t bar0 = PCI::get().readConfig32(0, bus, slot, func, PCI_BAR0);
    outBase = 0;
    outIs64 = false;

    if (bar0 == 0 || bar0 == 0xFFFFFFFFU) {
        log_str("[usb:xhci] BAR0 missing\n");
        return false;
    }
    if (bar0 & PCI_BAR_IO) {
        log_str("[usb:xhci] BAR0 is I/O, expected MMIO\n");
        return false;
    }

    outIs64 = (bar0 & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64;
    outBase = static_cast<uint64_t>(bar0 & 0xFFFFFFF0U);
    if (outIs64) {
        const uint32_t bar1 = PCI::get().readConfig32(0, bus, slot, func, PCI_BAR1);
        if (bar1 == 0xFFFFFFFFU) {
            log_str("[usb:xhci] BAR1 invalid for 64-bit BAR\n");
            return false;
        }
        outBase |= static_cast<uint64_t>(bar1) << 32;
    }
    outBase &= PCI_BAR_MEM_ADDR_MASK;

    if (outBase == 0) {
        log_str("[usb:xhci] MMIO base is zero\n");
        return false;
    }
    return true;
}

void enable_pci_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t command = PCI::get().readConfig16(0, bus, slot, func, PCI_COMMAND);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    command &= static_cast<uint16_t>(~PCI_COMMAND_IO);
    PCI::get().writeConfig16(0, bus, slot, func, PCI_COMMAND, command);
}

void log_capabilities(
    uint8_t bus,
    uint8_t slot,
    uint8_t func,
    uint16_t vendor,
    uint16_t device,
    uint64_t mmioBase,
    bool is64BitBar,
    const XHCICapabilityRegisters& caps
) {
    log_boot_str("[usb:xhci] found controller on ");
    log_boot_dec(bus);
    log_boot_str(":");
    log_boot_dec(slot);
    log_boot_str(".");
    log_boot_dec(func);
    log_boot_str("\n");

    const uint8_t maxSlots = static_cast<uint8_t>(caps.hcsParams1 & 0xFF);
    const uint16_t maxInterrupters = static_cast<uint16_t>((caps.hcsParams1 >> 8) & 0x7FF);
    const uint8_t maxPorts = static_cast<uint8_t>((caps.hcsParams1 >> 24) & 0xFF);

    log_str("[usb:xhci] controller ");
    log_dec(bus);
    log_str(":");
    log_dec(slot);
    log_str(".");
    log_dec(func);
    log_str(" vendor=");
    log_hex(vendor);
    log_str(" device=");
    log_hex(device);
    log_str(" mmio=");
    log_hex(mmioBase);
    log_str(is64BitBar ? " bar=64" : " bar=32");
    log_str(" caplen=");
    log_dec(caps.capLength);
    log_str(" version=");
    log_hex(caps.hciVersion);
    log_str(" slots=");
    log_dec(maxSlots);
    log_str(" interrupters=");
    log_dec(maxInterrupters);
    log_str(" ports=");
    log_dec(maxPorts);
    log_str(" hcs2=");
    log_hex(caps.hcsParams2);
    log_str(" hcs3=");
    log_hex(caps.hcsParams3);
    log_str(" hcc1=");
    log_hex(caps.hccParams1);
    log_str(" dboff=");
    log_hex(caps.dbOff);
    log_str(" rtsoff=");
    log_hex(caps.rtsOff);
    log_str(" xecp=");
    log_hex((caps.hccParams1 & XHCI_HCCPARAMS1_XECP_MASK) >> XHCI_HCCPARAMS1_XECP_SHIFT);
    log_str("\n");
}

bool initialize_register_model(uint8_t bus, uint8_t slot, uint8_t function, uint64_t mmioBase, const XHCICapabilityRegisters& caps, XHCIControllerState& state) {
    const uint8_t maxSlots = static_cast<uint8_t>(caps.hcsParams1 & 0xFF);
    const uint8_t maxPorts = static_cast<uint8_t>((caps.hcsParams1 >> 24) & 0xFF);

    state = {};
    state.bus = bus;
    state.slot = slot;
    state.function = function;
    state.maxPorts = maxPorts;
    state.contextSize = (caps.hccParams1 & XHCI_HCCPARAMS1_CSZ) ? 64 : 32;
    state.hccParams1 = caps.hccParams1;
    state.mmioBase = mmioBase;
    state.operationalBase = mmioBase + caps.capLength;
    state.doorbellBase = mmioBase + caps.dbOff;
    state.runtimeBase = mmioBase + caps.rtsOff;
    state.commandCycle = true;
    state.eventCycle = true;
    state.irqLine = 0xFF;

    if (!claim_bios_ownership(mmioBase, caps)) {
        return false;
    }

    const uint64_t opBytes = caps.capLength + XHCI_OP_PORT_BASE + static_cast<uint64_t>(maxPorts) * XHCI_PORT_STRIDE;
    const uint64_t doorbellBytes = static_cast<uint64_t>(caps.dbOff) + static_cast<uint64_t>(maxSlots + 1) * 4;
    const uint64_t runtimeBytes = static_cast<uint64_t>(caps.rtsOff) + XHCI_RT_IR0 + 0x20;
    map_mmio_window(mmioBase, max_u64(opBytes, max_u64(doorbellBytes, runtimeBytes)));

    if (!wait_register_set(state.operationalBase, XHCI_OP_USBSTS, XHCI_STS_CNR, false, 2000000)) {
        log_str("[usb:xhci] controller not ready timeout\n");
        return false;
    }

    uint32_t command = mmio_read32(state.operationalBase, XHCI_OP_USBCMD);
    command &= ~XHCI_CMD_RUN;
    mmio_write32(state.operationalBase, XHCI_OP_USBCMD, command);
    if (!wait_register_set(state.operationalBase, XHCI_OP_USBSTS, XHCI_STS_HCH, true, 2000000)) {
        log_str("[usb:xhci] halt timeout\n");
        return false;
    }

    mmio_write32(state.operationalBase, XHCI_OP_USBCMD, command | XHCI_CMD_HCRST);
    if (!wait_register_set(state.operationalBase, XHCI_OP_USBCMD, XHCI_CMD_HCRST, false, 2000000) ||
        !wait_register_set(state.operationalBase, XHCI_OP_USBSTS, XHCI_STS_CNR, false, 2000000)) {
        log_str("[usb:xhci] reset timeout\n");
        return false;
    }

    const uint32_t pageSize = mmio_read32(state.operationalBase, XHCI_OP_PAGESIZE);
    if ((pageSize & 0x1) == 0) {
        log_str("[usb:xhci] 4K pages unsupported pagesize=");
        log_hex(pageSize);
        log_str("\n");
        return false;
    }

    const uint64_t dcbaaPages = ((static_cast<uint64_t>(maxSlots) + 1) * sizeof(uint64_t) + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    state.dcbaa = reinterpret_cast<uint64_t*>(alloc_dma_pages(dcbaaPages, state.dcbaaPhys));
    if (!state.dcbaa) {
        log_str("[usb:xhci] DCBAA allocation failed\n");
        return false;
    }

    state.scratchpadCount = max_scratchpad_buffers(caps.hcsParams2);
    if (state.scratchpadCount != 0) {
        const uint32_t scratchpadArrayPages =
            pages_for_bytes(static_cast<uint64_t>(state.scratchpadCount) * sizeof(uint64_t));
        state.scratchpadArray = reinterpret_cast<uint64_t*>(
            alloc_dma_pages(scratchpadArrayPages, state.scratchpadArrayPhys)
        );
        if (!state.scratchpadArray) {
            log_str("[usb:xhci] scratchpad array allocation failed count=");
            log_dec(state.scratchpadCount);
            log_str("\n");
            return false;
        }

        for (uint16_t i = 0; i < state.scratchpadCount; ++i) {
            uint64_t scratchpadPhys = 0;
            if (!alloc_dma_pages(1, scratchpadPhys)) {
                log_str("[usb:xhci] scratchpad buffer allocation failed index=");
                log_dec(i);
                log_str(" count=");
                log_dec(state.scratchpadCount);
                log_str("\n");
                return false;
            }
            state.scratchpadArray[i] = scratchpadPhys;
        }

        state.dcbaa[0] = state.scratchpadArrayPhys;
        log_str("[usb:xhci] scratchpads count=");
        log_dec(state.scratchpadCount);
        log_str(" array=");
        log_hex(state.scratchpadArrayPhys);
        log_str("\n");
    }

    const uint64_t commandRingBytes = XHCI_COMMAND_RING_TRBS * sizeof(XHCITransferRequestBlock);
    const uint64_t commandRingPages = (commandRingBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    state.commandRing = reinterpret_cast<XHCITransferRequestBlock*>(alloc_dma_pages(commandRingPages, state.commandRingPhys));
    if (!state.commandRing) {
        log_str("[usb:xhci] command ring allocation failed\n");
        return false;
    }
    state.commandRing[XHCI_COMMAND_RING_TRBS - 1].parameter = state.commandRingPhys;
    state.commandRing[XHCI_COMMAND_RING_TRBS - 1].status = 0;
    state.commandRing[XHCI_COMMAND_RING_TRBS - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TOGGLE_CYCLE | XHCI_TRB_CYCLE;

    const uint64_t eventRingBytes = XHCI_EVENT_RING_TRBS * sizeof(XHCITransferRequestBlock);
    const uint64_t eventRingPages = (eventRingBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    state.eventRing = reinterpret_cast<XHCITransferRequestBlock*>(alloc_dma_pages(eventRingPages, state.eventRingPhys));
    if (!state.eventRing) {
        log_str("[usb:xhci] event ring allocation failed\n");
        return false;
    }

    state.erst = reinterpret_cast<XHCIEventRingSegmentTableEntry*>(alloc_dma_pages(1, state.erstPhys));
    if (!state.erst) {
        log_str("[usb:xhci] ERST allocation failed\n");
        return false;
    }
    state.erst[0].ringSegmentBase = state.eventRingPhys;
    state.erst[0].ringSegmentSize = XHCI_EVENT_RING_TRBS;
    state.erst[0].reserved = 0;

    mmio_write64(state.operationalBase, XHCI_OP_DCBAAP, state.dcbaaPhys);
    mmio_write64(state.operationalBase, XHCI_OP_CRCR, state.commandRingPhys | XHCI_CRCR_RCS);
    mmio_write32(state.operationalBase, XHCI_OP_CONFIG, maxSlots);

    const uint64_t interrupterBase = state.runtimeBase + XHCI_RT_IR0;
    mmio_write32(interrupterBase, XHCI_IR_IMAN, XHCI_IMAN_IP);
    mmio_write32(interrupterBase, XHCI_IR_IMOD, 0);
    mmio_write32(interrupterBase, XHCI_IR_ERSTSZ, 1);
    mmio_write64(interrupterBase, XHCI_IR_ERSTBA, state.erstPhys);
    mmio_write64(interrupterBase, XHCI_IR_ERDP, state.eventRingPhys | XHCI_ERDP_EHB);

    if (PCI::get().registerMSIInterrupt(0, bus, slot, function, &get_xhci_interrupt_handler(), &state.irqVector)) {
        state.irqRegistered = true;
        state.msiEnabled = true;
        log_str("[usb:xhci] irq msi vector=");
        log_dec(state.irqVector);
        log_str("\n");
    } else if (PCI::get().registerLegacyInterrupt(0, bus, slot, function, &get_xhci_interrupt_handler(),
                                                  &state.irqLine, &state.irqVector)) {
        state.irqRegistered = true;
        state.msiEnabled = false;
        log_str("[usb:xhci] irq legacy line=");
        log_dec(state.irqLine);
        log_str(" vector=");
        log_dec(state.irqVector);
        log_str("\n");
    } else {
        log_str("[usb:xhci] irq registration failed, polling only\n");
    }

    mmio_write32(interrupterBase, XHCI_IR_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);

    mmio_write32(state.operationalBase, XHCI_OP_USBSTS, XHCI_STS_EINT | XHCI_STS_PCD);
    command = mmio_read32(state.operationalBase, XHCI_OP_USBCMD);
    mmio_write32(state.operationalBase, XHCI_OP_USBCMD, command | XHCI_CMD_INTE | XHCI_CMD_RUN);
    if (!wait_register_set(state.operationalBase, XHCI_OP_USBSTS, XHCI_STS_HCH, false, 2000000)) {
        log_str("[usb:xhci] run timeout\n");
        return false;
    }

    log_str("[usb:xhci] initialized op=");
    log_hex(state.operationalBase);
    log_str(" runtime=");
    log_hex(state.runtimeBase);
    log_str(" doorbells=");
    log_hex(state.doorbellBase);
    log_str(" dcbaa=");
    log_hex(state.dcbaaPhys);
    log_str(" cmd=");
    log_hex(state.commandRingPhys);
    log_str(" event=");
    log_hex(state.eventRingPhys);
    log_str(" erst=");
    log_hex(state.erstPhys);
    log_str("\n");

    initialize_root_ports(state);

    return true;
}

bool probe_controller(uint8_t bus, uint8_t slot, uint8_t func, XHCIControllerState* state) {
    const uint16_t vendor = PCI::get().readConfig16(0, bus, slot, func, PCI_VENDOR_ID);
    const uint16_t device = PCI::get().readConfig16(0, bus, slot, func, PCI_DEVICE_ID);

    log_boot_str("[usb:xhci] probe ");
    log_boot_dec(bus);
    log_boot_str(":");
    log_boot_dec(slot);
    log_boot_str(".");
    log_boot_dec(func);
    log_boot_str("\n");

    uint64_t mmioBase = 0;
    bool is64BitBar = false;
    if (!read_xhci_bar(bus, slot, func, mmioBase, is64BitBar)) {
        log_boot_str("[usb:xhci] probe failed before init\n");
        return false;
    }

    enable_pci_device(bus, slot, func);
    map_mmio_window(mmioBase, PAGE_SIZE);

    XHCICapabilityRegisters caps = {};
    const uint32_t capHeader = mmio_read32(mmioBase, XHCI_CAP_CAPLENGTH);
    caps.capLength = static_cast<uint8_t>(capHeader & 0xFF);
    caps.hciVersion = static_cast<uint16_t>((capHeader >> 16) & 0xFFFF);
    caps.hcsParams1 = mmio_read32(mmioBase, XHCI_CAP_HCSPARAMS1);
    caps.hcsParams2 = mmio_read32(mmioBase, XHCI_CAP_HCSPARAMS2);
    caps.hcsParams3 = mmio_read32(mmioBase, XHCI_CAP_HCSPARAMS3);
    caps.hccParams1 = mmio_read32(mmioBase, XHCI_CAP_HCCPARAMS1);
    caps.dbOff = mmio_read32(mmioBase, XHCI_CAP_DBOFF) & ~0x3U;
    caps.rtsOff = mmio_read32(mmioBase, XHCI_CAP_RTSOFF) & ~0x1FU;

    const uint8_t maxSlots = static_cast<uint8_t>(caps.hcsParams1 & 0xFF);
    const uint8_t maxPorts = static_cast<uint8_t>((caps.hcsParams1 >> 24) & 0xFF);

    if (caps.capLength < 0x20 || caps.hciVersion == 0 || maxSlots == 0 || maxPorts == 0 ||
        caps.dbOff == 0 || caps.rtsOff == 0) {
        log_str("[usb:xhci] invalid capability registers at ");
        log_hex(mmioBase);
        log_str(" caplen=");
        log_dec(caps.capLength);
        log_str(" version=");
        log_hex(caps.hciVersion);
        log_str(" slots=");
        log_dec(maxSlots);
        log_str(" ports=");
        log_dec(maxPorts);
        log_str("\n");
        return false;
    }

    log_capabilities(bus, slot, func, vendor, device, mmioBase, is64BitBar, caps);
    const bool ok = state && initialize_register_model(bus, slot, func, mmioBase, caps, *state);
    log_boot_str(ok ? "[usb:xhci] controller initialized\n" : "[usb:xhci] controller init failed\n");
    return ok;
}
}

XHCIController& XHCIController::get() {
    static XHCIController instance;
    return instance;
}

bool XHCIController::initialize() {
    controllersFound = 0;
    initializedControllers = 0;

    for (uint16_t bus16 = 0; bus16 < 256; ++bus16) {
        const uint8_t bus = static_cast<uint8_t>(bus16);
        for (uint8_t slot = 0; slot < 32; ++slot) {
            for (uint8_t func = 0; func < 8; ++func) {
                const uint16_t vendor = PCI::get().readConfig16(0, bus, slot, func, PCI_VENDOR_ID);
                if (vendor == 0xFFFF) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                const uint8_t classCode = PCI::get().readConfig8(0, bus, slot, func, PCI_CLASS);
                const uint8_t subclass = PCI::get().readConfig8(0, bus, slot, func, PCI_SUBCLASS);
                const uint8_t progIf = PCI::get().readConfig8(0, bus, slot, func, PCI_PROG_IF);

                if (classCode == PCI_CLASS_SERIAL_BUS &&
                    subclass == PCI_SUBCLASS_USB &&
                    progIf == PCI_PROGIF_XHCI) {
                    ++controllersFound;
                    if (initializedControllers < XHCI_MAX_TRACKED_CONTROLLERS &&
                        probe_controller(bus, slot, func, &g_controllers[initializedControllers])) {
                        ++initializedControllers;
                    }
                }

                const uint8_t headerType = PCI::get().readConfig8(0, bus, slot, func, PCI_HEADER_TYPE);
                if (func == 0 && (headerType & 0x80) == 0) {
                    break;
                }
            }
        }
    }

    if (controllersFound == 0) {
        log_boot_str("[usb:xhci] no xHCI controller candidates found during PCI scan\n");
        log_str("[usb:xhci] no controllers found\n");
    } else if (initializedControllers == 0) {
        log_boot_str("[usb:xhci] xHCI candidates were found but none initialized\n");
        log_str("[usb:xhci] no controllers initialized\n");
    } else {
        log_boot_str("[usb:xhci] active controllers=");
        log_boot_dec(initializedControllers);
        log_boot_str("/");
        log_boot_dec(controllersFound);
        log_boot_str("\n");
    }
    return initializedControllers > 0;
}

void XHCIController::poll() {
    handleInterrupt();
    if (g_inputActivity) {
        g_inputActivity = false;
        Process* current = Scheduler::get().getCurrentProcess();
        if (current && current->getPriority() == ProcessPriority::Idle) {
            Scheduler::get().yield();
        }
    }
}

bool XHCIController::claimPendingInterrupt() {
    for (uint8_t i = 0; i < initializedControllers; ++i) {
        if (controller_has_pending_interrupt(g_controllers[i])) {
            return true;
        }
    }
    return false;
}

bool XHCIController::hasBootKeyboard() const {
    for (uint8_t i = 0; i < initializedControllers; ++i) {
        for (uint16_t slot = 0; slot < XHCI_MAX_SLOTS; ++slot) {
            if (g_controllers[i].slots[slot].keyboardReady) {
                return true;
            }
        }
    }
    return false;
}

bool XHCIController::hasBootMouse() const {
    for (uint8_t i = 0; i < initializedControllers; ++i) {
        for (uint16_t slot = 0; slot < XHCI_MAX_SLOTS; ++slot) {
            if (g_controllers[i].slots[slot].mouseReady) {
                return true;
            }
        }
    }
    return false;
}

void XHCIController::handleInterrupt() {
    for (uint8_t i = 0; i < initializedControllers; ++i) {
        handle_controller_interrupt(g_controllers[i]);
    }
}
