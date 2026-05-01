#include <drivers/hid/i2c_hid.hpp>

#include <common/string.hpp>
#include <cpu/acpi/acpi.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/isr.hpp>
#include <cpu/process/scheduler.hpp>
#include <interrupts/keyboard.hpp>
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

constexpr uint16_t PCI_VENDOR_INTEL = 0x8086;
constexpr uint8_t PCI_CLASS_SERIAL_BUS = 0x0C;
constexpr uint8_t PCI_SUBCLASS_I2C = 0x80;
constexpr uint8_t PCI_HEADER_MULTIFUNCTION = 0x80;
constexpr uint16_t PCI_COMMAND_IO = 1 << 0;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;
constexpr uint32_t PCI_BAR_IO = 1 << 0;
constexpr uint32_t PCI_BAR_MEM_TYPE_MASK = 0x6;
constexpr uint32_t PCI_BAR_MEM_TYPE_64 = 0x4;
constexpr uint64_t PCI_BAR_MEM_ADDR_MASK = 0xFFFFFFFFFFFFFFF0ULL;

constexpr uint32_t DW_IC_CON = 0x00;
constexpr uint32_t DW_IC_TAR = 0x04;
constexpr uint32_t DW_IC_DATA_CMD = 0x10;
constexpr uint32_t DW_IC_INTR_MASK = 0x30;
constexpr uint32_t DW_IC_RAW_INTR_STAT = 0x34;
constexpr uint32_t DW_IC_RX_TL = 0x38;
constexpr uint32_t DW_IC_TX_TL = 0x3C;
constexpr uint32_t DW_IC_CLR_INTR = 0x40;
constexpr uint32_t DW_IC_CLR_TX_ABRT = 0x54;
constexpr uint32_t DW_IC_ENABLE = 0x6C;
constexpr uint32_t DW_IC_STATUS = 0x70;
constexpr uint32_t DW_IC_TXFLR = 0x74;
constexpr uint32_t DW_IC_RXFLR = 0x78;
constexpr uint32_t DW_IC_ENABLE_STATUS = 0x9C;
constexpr uint32_t DW_IC_COMP_PARAM_1 = 0xF4;
constexpr uint32_t DW_IC_COMP_TYPE = 0xFC;

constexpr uint32_t DW_IC_CON_MASTER = 1 << 0;
constexpr uint32_t DW_IC_CON_SPEED_STANDARD = 1 << 1;
constexpr uint32_t DW_IC_CON_SPEED_FAST = 2 << 1;
constexpr uint32_t DW_IC_CON_RESTART_EN = 1 << 5;
constexpr uint32_t DW_IC_CON_SLAVE_DISABLE = 1 << 6;
constexpr uint32_t DW_IC_DATA_CMD_READ = 1 << 8;
constexpr uint32_t DW_IC_DATA_CMD_STOP = 1 << 9;
constexpr uint32_t DW_IC_DATA_CMD_RESTART = 1 << 10;
constexpr uint32_t DW_IC_RAW_INTR_TX_ABRT = 1 << 6;
constexpr uint32_t DW_IC_STATUS_ACTIVITY = 1 << 0;
constexpr uint32_t DW_IC_STATUS_TFNF = 1 << 1;
constexpr uint32_t DW_IC_STATUS_TFE = 1 << 2;
constexpr uint32_t DW_IC_STATUS_RFNE = 1 << 3;
constexpr uint32_t DW_IC_STATUS_MST_ACTIVITY = 1 << 5;

constexpr uint32_t DW_IC_COMP_TYPE_VALUE = 0x44570140;
constexpr uint32_t I2C_WAIT_ITERATIONS = 200000;
constexpr uint16_t I2C_HID_DESCRIPTOR_LENGTH = 30;
constexpr uint8_t I2C_HID_OPCODE_RESET = 0x01;
constexpr uint8_t I2C_HID_OPCODE_SET_POWER = 0x08;
constexpr uint8_t I2C_HID_POWER_ON = 0x00;
constexpr uint8_t GPIO_CONNECTION_TYPE_INTERRUPT = 0;
constexpr uint16_t GPIO_INTERRUPT_FLAG_EDGE_TRIGGERED = 1 << 0;
constexpr uint16_t GPIO_INTERRUPT_FLAG_ACTIVE_LOW = 1 << 1;
constexpr uint16_t GPIO_INTERRUPT_FLAG_ACTIVE_BOTH = 1 << 2;
constexpr uint16_t GPIO_GENERAL_FLAG_SHARED = 1 << 1;
constexpr uint16_t GPIO_GENERAL_FLAG_WAKE = 1 << 2;
constexpr uint32_t INTEL_GPIO_GPI_IS = 0x100;
constexpr uint32_t INTEL_GPIO_GPI_IE = 0x110;
constexpr uint32_t INTEL_GPIO_GPI_GPE_STS = 0x140;
constexpr uint32_t INTEL_GPIO_GPI_GPE_EN = 0x160;
constexpr uint32_t INTEL_GPIO_MIN_WINDOW = 0x1000;

struct AcpiHeaderView {
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

struct AcpiScanContext {
    I2CHIDController* driver;
    uint8_t hints;
};

struct GpioInterruptRegistration {
    bool active = false;
    uint32_t gsi = 0;
    uint8_t vector = 0;
};

class I2CHIDGpioInterrupt final : public Interrupt {
public:
    void initialize() override {
    }

    void configure(uint32_t gsiValue) {
        gsi = gsiValue;
    }

    void Run(InterruptFrame*) override {
        I2CHIDController::get().handleGpioInterrupt(gsi);
        LAPIC::get().sendEOI();
    }

private:
    uint32_t gsi = 0;
};

GpioInterruptRegistration g_gpioRegistrations[I2CHIDController::MaxDevices] = {};
I2CHIDGpioInterrupt g_gpioHandlers[I2CHIDController::MaxDevices] = {};

void map_mmio_window(uint64_t base, uint64_t bytes);
uint32_t mmio_read32(uint64_t base, uint32_t offset);
void mmio_write32(uint64_t base, uint32_t offset, uint32_t value);

uint16_t acpi_irq_info_to_ioapic_flags(uint8_t info, bool hasInfo) {
    if (!hasInfo) {
        return static_cast<uint16_t>(1 | (1 << 2));
    }

    const bool edgeTriggered = (info & 0x01) != 0;
    const bool activeLow = (info & 0x08) != 0;
    return static_cast<uint16_t>((activeLow ? 3 : 1) | ((edgeTriggered ? 1 : 3) << 2));
}

uint16_t acpi_ext_irq_flags_to_ioapic_flags(uint8_t flags) {
    const bool edgeTriggered = (flags & (1 << 1)) != 0;
    const bool activeLow = (flags & (1 << 2)) != 0;
    return static_cast<uint16_t>((activeLow ? 3 : 1) | ((edgeTriggered ? 1 : 3) << 2));
}

bool gpio_source_matches_controller(const char* source, const char* controllerName) {
    if (!source || !source[0] || !controllerName || !controllerName[0]) {
        return false;
    }

    const size_t sourceLength = strlen(source);
    const size_t nameLength = strlen(controllerName);
    if (sourceLength < nameLength) {
        return false;
    }

    const char* suffix = source + sourceLength - nameLength;
    return memcmp(suffix, controllerName, nameLength) == 0 &&
           (suffix == source || suffix[-1] == '.' || suffix[-1] == '\\' || suffix[-1] == '^');
}

uint32_t gpio_register_group(uint32_t pin) {
    return (pin / 32) * sizeof(uint32_t);
}

uint32_t gpio_register_bit(uint32_t pin) {
    return 1U << (pin % 32);
}

uint32_t gpio_mmio_window_length(const I2CHIDController::GpioController& controller) {
    return controller.mmioLength > INTEL_GPIO_MIN_WINDOW ? controller.mmioLength : INTEL_GPIO_MIN_WINDOW;
}

void intel_gpio_enable_interrupt_pin(const I2CHIDController::GpioController& controller, uint32_t pin) {
    if (!controller.hasMmio || pin >= 256) {
        return;
    }

    map_mmio_window(controller.mmioBase, gpio_mmio_window_length(controller));
    const uint32_t group = gpio_register_group(pin);
    const uint32_t bit = gpio_register_bit(pin);

    mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_IS + group, bit);
    mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_GPE_STS + group, bit);

    uint32_t enabled = mmio_read32(controller.mmioBase, INTEL_GPIO_GPI_IE + group);
    mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_IE + group, enabled | bit);

    enabled = mmio_read32(controller.mmioBase, INTEL_GPIO_GPI_GPE_EN + group);
    mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_GPE_EN + group, enabled | bit);
}

bool intel_gpio_ack_interrupt_pin(const I2CHIDController::GpioController& controller, uint32_t pin) {
    if (!controller.hasMmio || pin >= 256) {
        return false;
    }

    map_mmio_window(controller.mmioBase, gpio_mmio_window_length(controller));
    const uint32_t group = gpio_register_group(pin);
    const uint32_t bit = gpio_register_bit(pin);
    const bool pending = ((mmio_read32(controller.mmioBase, INTEL_GPIO_GPI_IS + group) |
                           mmio_read32(controller.mmioBase, INTEL_GPIO_GPI_GPE_STS + group)) & bit) != 0;
    if (pending) {
        mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_IS + group, bit);
        mmio_write32(controller.mmioBase, INTEL_GPIO_GPI_GPE_STS + group, bit);
    }
    return pending;
}

void log_str(const char* s) {
    Cereal::get().write(s);
}

void log_hex(uint64_t value) {
    Cereal::get().write("0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble || started || shift == 0) {
            started = true;
            Cereal::get().write(static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10)));
        }
    }
}

void log_dec(uint64_t value) {
    char buffer[21];
    int index = 0;
    if (value == 0) {
        Cereal::get().write('0');
        return;
    }

    while (value && index < static_cast<int>(sizeof(buffer))) {
        buffer[index++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (index > 0) {
        Cereal::get().write(buffer[--index]);
    }
}

bool signature_equals(const char* signature, const char* expected) {
    return signature[0] == expected[0] && signature[1] == expected[1] &&
           signature[2] == expected[2] && signature[3] == expected[3];
}

bool bytes_match(const uint8_t* data, const char* needle, size_t needleLen) {
    for (size_t i = 0; i < needleLen; i++) {
        if (data[i] != static_cast<uint8_t>(needle[i])) {
            return false;
        }
    }
    return true;
}

bool scan_bytes_for(const uint8_t* data, size_t length, const char* needle, size_t needleLen, size_t* offset) {
    if (!data || !needle || needleLen == 0 || length < needleLen) {
        return false;
    }

    for (size_t i = 0; i <= length - needleLen; i++) {
        if (bytes_match(data + i, needle, needleLen)) {
            if (offset) {
                *offset = i;
            }
            return true;
        }
    }

    return false;
}

uint16_t read_le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t read_le32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t page_floor(uint64_t value) {
    return value & ~(PAGE_SIZE - 1);
}

uint64_t page_ceil(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

void map_mmio_window(uint64_t base, uint64_t bytes) {
    if (!VMM::IsInitialized() || base == 0 || bytes == 0) {
        return;
    }

    const uint64_t start = page_floor(base);
    const uint64_t end = page_ceil(base + bytes);
    const uint64_t pages = (end - start) / PAGE_SIZE;
    VMM::MapRange(start, start, pages, Present | ReadWrite | CacheDisab | WriteThru | NoExecute);
}

uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

void mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

void io_wait() {
    asm volatile("pause");
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
    if (modifiers & 0x22) out |= KeyModifierShift;
    if (modifiers & 0x11) out |= KeyModifierControl;
    if (modifiers & 0x44) out |= KeyModifierAlt;
    if (modifiers & 0x88) out |= KeyModifierSuper;
    return out;
}

uint32_t hid_read_bits(const uint8_t* report, uint16_t reportLength, uint16_t bitOffset, uint8_t bitSize) {
    if (!report || bitSize == 0 || bitSize > 32) {
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

int32_t sign_extend(uint32_t value, uint8_t bitSize) {
    if (bitSize == 0 || bitSize >= 32) {
        return static_cast<int32_t>(value);
    }

    const uint32_t signBit = 1U << (bitSize - 1);
    if ((value & signBit) == 0) {
        return static_cast<int32_t>(value);
    }
    return static_cast<int32_t>(value | (~0U << bitSize));
}

uint16_t keyboard_payload_bit_base(const I2CHIDController::HIDKeyboardReportLayout& layout) {
    return layout.reportId == 0 ? 0 : 8;
}

bool keyboard_report_matches_id(const I2CHIDController::HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    return layout.reportId == 0 || (reportLength > 0 && report[0] == layout.reportId);
}

uint8_t keyboard_modifier_byte(const I2CHIDController::HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    uint8_t modifiers = 0;
    const uint16_t base = keyboard_payload_bit_base(layout);
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if (hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.modifierBitOffset + bit), 1) != 0) {
            modifiers |= static_cast<uint8_t>(1U << bit);
        }
    }
    return modifiers;
}

uint8_t keyboard_key_usage_at(const I2CHIDController::HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength, uint8_t index) {
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

bool report_had_key(const I2CHIDController::HIDKeyboardReportLayout& layout, const uint8_t* report, uint16_t reportLength, uint8_t usage) {
    for (uint8_t i = 0; i < layout.keyArrayCount; ++i) {
        if (keyboard_key_usage_at(layout, report, reportLength, i) == usage) {
            return true;
        }
    }
    return false;
}

bool parse_hid_keyboard_report_descriptor(const uint8_t* reportDescriptor, uint16_t length, I2CHIDController::HIDKeyboardReportLayout& outLayout) {
    I2CHIDController::HIDKeyboardReportLayout layout = {};
    layout.reportId = 0;
    layout.modifierBitOffset = 0xFFFF;
    layout.keyArrayBitOffset = 0xFFFF;
    layout.keyArrayReportSize = 8;

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
    bool sawKeyboardCollection = false;

    while (offset < length) {
        const uint8_t prefix = reportDescriptor[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) break;
            const uint8_t dataSize = reportDescriptor[offset];
            offset = static_cast<uint16_t>(offset + 2 + dataSize);
            continue;
        }

        const uint8_t sizeCode = prefix & 0x03;
        const uint8_t dataSize = sizeCode == 3 ? 4 : sizeCode;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = (prefix >> 4) & 0x0F;
        if (offset + dataSize > length) break;

        uint32_t value = 0;
        for (uint8_t i = 0; i < dataSize; ++i) {
            value |= static_cast<uint32_t>(reportDescriptor[offset + i]) << (i * 8);
        }
        offset = static_cast<uint16_t>(offset + dataSize);

        if (type == 1) {
            if (tag == 0x0) usagePage = static_cast<uint16_t>(value);
            else if (tag == 0x7) reportSize = static_cast<uint8_t>(value);
            else if (tag == 0x8) {
                reportId = static_cast<uint8_t>(value);
                reportBitOffset = 0;
            } else if (tag == 0x9) reportCount = static_cast<uint8_t>(value);
        } else if (type == 2) {
            if (tag == 0x0) {
                const uint16_t usage = static_cast<uint16_t>(value & 0xFFFF);
                if (usagePage == 0x01 && usage == 0x06) {
                    pendingKeyboardApplication = true;
                }
            } else if (tag == 0x1) usageMinimum = static_cast<uint16_t>(value);
            else if (tag == 0x2) usageMaximum = static_cast<uint16_t>(value);
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
                if (keyboardCollectionDepth == collectionDepth) keyboardCollectionDepth = 0;
                if (collectionDepth > 0) --collectionDepth;
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

    if (!sawKeyboardCollection || layout.keyArrayBitOffset == 0xFFFF || layout.keyArrayCount == 0) {
        return false;
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
    outLayout = layout;
    return true;
}

uint16_t mouse_payload_bit_base(const I2CHIDController::HIDMouseReportLayout& layout) {
    return layout.reportId == 0 ? 0 : 8;
}

bool mouse_report_matches_id(const I2CHIDController::HIDMouseReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    return layout.reportId == 0 || (reportLength > 0 && report[0] == layout.reportId);
}

bool parse_hid_mouse_report_descriptor(const uint8_t* reportDescriptor, uint16_t length, I2CHIDController::HIDMouseReportLayout& outLayout) {
    I2CHIDController::HIDMouseReportLayout layout = {};
    layout.buttonsBitOffset = 0xFFFF;
    layout.xBitOffset = 0xFFFF;
    layout.yBitOffset = 0xFFFF;
    layout.wheelBitOffset = 0xFFFF;
    layout.axisReportSize = 8;

    uint16_t offset = 0;
    uint16_t reportBitOffset = 0;
    uint16_t usagePage = 0;
    uint16_t usageMinimum = 0;
    uint16_t usageMaximum = 0;
    uint16_t localUsages[16] = {};
    uint8_t localUsageCount = 0;
    uint8_t reportSize = 0;
    uint8_t reportCount = 0;
    uint8_t reportId = 0;
    uint8_t collectionDepth = 0;
    uint8_t mouseCollectionDepth = 0;
    bool pendingMouseApplication = false;

    while (offset < length) {
        const uint8_t prefix = reportDescriptor[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) break;
            const uint8_t dataSize = reportDescriptor[offset];
            offset = static_cast<uint16_t>(offset + 2 + dataSize);
            continue;
        }

        const uint8_t sizeCode = prefix & 0x03;
        const uint8_t dataSize = sizeCode == 3 ? 4 : sizeCode;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = (prefix >> 4) & 0x0F;
        if (offset + dataSize > length) break;

        uint32_t value = 0;
        for (uint8_t i = 0; i < dataSize; ++i) {
            value |= static_cast<uint32_t>(reportDescriptor[offset + i]) << (i * 8);
        }
        offset = static_cast<uint16_t>(offset + dataSize);

        if (type == 1) {
            if (tag == 0x0) usagePage = static_cast<uint16_t>(value);
            else if (tag == 0x7) reportSize = static_cast<uint8_t>(value);
            else if (tag == 0x8) {
                reportId = static_cast<uint8_t>(value);
                reportBitOffset = 0;
            } else if (tag == 0x9) reportCount = static_cast<uint8_t>(value);
        } else if (type == 2) {
            if (tag == 0x0) {
                const uint16_t usage = static_cast<uint16_t>(value & 0xFFFF);
                if (usagePage == 0x01 && usage == 0x02) {
                    pendingMouseApplication = true;
                }
                if (localUsageCount < sizeof(localUsages) / sizeof(localUsages[0])) {
                    localUsages[localUsageCount++] = usage;
                }
            } else if (tag == 0x1) usageMinimum = static_cast<uint16_t>(value);
            else if (tag == 0x2) usageMaximum = static_cast<uint16_t>(value);
        } else if (type == 0) {
            if (tag == 0xA) {
                ++collectionDepth;
                if (pendingMouseApplication && value == 0x01) {
                    mouseCollectionDepth = collectionDepth;
                    layout.reportId = reportId;
                }
                pendingMouseApplication = false;
                localUsageCount = 0;
                usageMinimum = 0;
                usageMaximum = 0;
            } else if (tag == 0xC) {
                if (mouseCollectionDepth == collectionDepth) mouseCollectionDepth = 0;
                if (collectionDepth > 0) --collectionDepth;
                localUsageCount = 0;
                usageMinimum = 0;
                usageMaximum = 0;
            } else if (tag == 0x8) {
                const bool inMouseCollection = mouseCollectionDepth != 0;
                const bool variable = (value & 0x02) != 0;
                const bool relative = (value & 0x04) != 0;
                if (inMouseCollection && variable) {
                    if (usagePage == 0x09 && usageMinimum >= 1 && usageMaximum >= usageMinimum && layout.buttonsBitOffset == 0xFFFF) {
                        layout.buttonsBitOffset = reportBitOffset;
                        layout.buttonCount = reportCount > 8 ? 8 : reportCount;
                    } else if (usagePage == 0x01) {
                        for (uint8_t i = 0; i < reportCount; ++i) {
                            uint16_t usage = 0;
                            if (i < localUsageCount) {
                                usage = localUsages[i];
                            } else if (usageMinimum != 0) {
                                usage = static_cast<uint16_t>(usageMinimum + i);
                            }

                            const uint16_t bitOffset = static_cast<uint16_t>(reportBitOffset + static_cast<uint16_t>(i) * reportSize);
                            if (usage == 0x30) {
                                layout.xBitOffset = bitOffset;
                                layout.axisReportSize = reportSize ? reportSize : 8;
                                layout.relative = relative;
                            } else if (usage == 0x31) {
                                layout.yBitOffset = bitOffset;
                                layout.axisReportSize = reportSize ? reportSize : 8;
                                layout.relative = layout.relative || relative;
                            } else if (usage == 0x38) {
                                layout.wheelBitOffset = bitOffset;
                                layout.hasWheel = true;
                            }
                        }
                    }
                }
                reportBitOffset = static_cast<uint16_t>(reportBitOffset + static_cast<uint16_t>(reportSize) * reportCount);
                localUsageCount = 0;
                usageMinimum = 0;
                usageMaximum = 0;
            } else if (tag == 0x9 || tag == 0xB) {
                localUsageCount = 0;
                usageMinimum = 0;
                usageMaximum = 0;
            }
        }
    }

    if (layout.xBitOffset == 0xFFFF || layout.yBitOffset == 0xFFFF || !layout.relative) {
        return false;
    }
    if (layout.buttonsBitOffset == 0xFFFF) {
        layout.buttonsBitOffset = 0;
        layout.buttonCount = 0;
    }
    if (layout.axisReportSize == 0 || layout.axisReportSize > 16) {
        layout.axisReportSize = 8;
    }
    layout.valid = true;
    outLayout = layout;
    return true;
}

bool dw_wait_enable_status(const I2CHIDController::Controller& controller, bool enabled) {
    for (uint32_t i = 0; i < I2C_WAIT_ITERATIONS; ++i) {
        const bool isEnabled = (mmio_read32(controller.mmioBase, DW_IC_ENABLE_STATUS) & 1) != 0;
        if (isEnabled == enabled) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool dw_set_enabled(const I2CHIDController::Controller& controller, bool enabled) {
    mmio_write32(controller.mmioBase, DW_IC_ENABLE, enabled ? 1 : 0);
    return dw_wait_enable_status(controller, enabled);
}

bool dw_wait_status(const I2CHIDController::Controller& controller, uint32_t mask, bool set) {
    for (uint32_t i = 0; i < I2C_WAIT_ITERATIONS; ++i) {
        const bool isSet = (mmio_read32(controller.mmioBase, DW_IC_STATUS) & mask) == mask;
        if (isSet == set) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool dw_wait_bus_idle(const I2CHIDController::Controller& controller) {
    for (uint32_t i = 0; i < I2C_WAIT_ITERATIONS; ++i) {
        const uint32_t status = mmio_read32(controller.mmioBase, DW_IC_STATUS);
        if ((status & (DW_IC_STATUS_ACTIVITY | DW_IC_STATUS_MST_ACTIVITY)) == 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

bool dw_check_abort(const I2CHIDController::Controller& controller) {
    if ((mmio_read32(controller.mmioBase, DW_IC_RAW_INTR_STAT) & DW_IC_RAW_INTR_TX_ABRT) == 0) {
        return false;
    }

    (void)mmio_read32(controller.mmioBase, DW_IC_CLR_TX_ABRT);
    (void)mmio_read32(controller.mmioBase, DW_IC_CLR_INTR);
    return true;
}

bool dw_prepare_target(const I2CHIDController::Controller& controller, uint16_t address) {
    if (!dw_set_enabled(controller, false)) {
        return false;
    }
    mmio_write32(controller.mmioBase, DW_IC_TAR, address & 0x3FF);
    (void)mmio_read32(controller.mmioBase, DW_IC_CLR_INTR);
    return dw_set_enabled(controller, true);
}

bool dw_write_data_cmd(const I2CHIDController::Controller& controller, uint32_t value) {
    for (uint32_t i = 0; i < I2C_WAIT_ITERATIONS; ++i) {
        if (dw_check_abort(controller)) {
            return false;
        }
        if ((mmio_read32(controller.mmioBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) != 0) {
            mmio_write32(controller.mmioBase, DW_IC_DATA_CMD, value);
            return true;
        }
        io_wait();
    }
    return false;
}

bool i2c_write_register(const I2CHIDController::Controller& controller, uint16_t address, uint16_t reg, const uint8_t* data, uint16_t length) {
    if (!dw_prepare_target(controller, address)) {
        return false;
    }

    if (!dw_write_data_cmd(controller, reg & 0xFF)) {
        return false;
    }

    const bool stopOnRegisterHigh = length == 0;
    if (!dw_write_data_cmd(controller, static_cast<uint32_t>((reg >> 8) & 0xFF) | (stopOnRegisterHigh ? DW_IC_DATA_CMD_STOP : 0))) {
        return false;
    }

    for (uint16_t i = 0; i < length; ++i) {
        const bool last = i + 1 == length;
        const uint32_t command = data[i] | (last ? DW_IC_DATA_CMD_STOP : 0);
        if (!dw_write_data_cmd(controller, command)) {
            return false;
        }
    }

    if (!dw_wait_status(controller, DW_IC_STATUS_TFE, true)) {
        return false;
    }
    return dw_wait_bus_idle(controller) && !dw_check_abort(controller);
}

bool i2c_read_register(const I2CHIDController::Controller& controller, uint16_t address, uint16_t reg, uint8_t* out, uint16_t length) {
    if (!out || length == 0) {
        return false;
    }
    if (!dw_prepare_target(controller, address)) {
        return false;
    }

    if (!dw_write_data_cmd(controller, reg & 0xFF)) {
        return false;
    }
    if (!dw_write_data_cmd(controller, (reg >> 8) & 0xFF)) {
        return false;
    }

    uint16_t queued = 0;
    uint16_t received = 0;
    uint32_t idleIterations = 0;
    while (received < length) {
        bool progressed = false;
        while (queued < length) {
            if (queued - received >= controller.rxDepth - 1) {
                break;
            }
            if ((mmio_read32(controller.mmioBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) == 0) {
                break;
            }

            uint32_t command = DW_IC_DATA_CMD_READ;
            if (queued == 0) {
                command |= DW_IC_DATA_CMD_RESTART;
            }
            if (queued + 1 == length) {
                command |= DW_IC_DATA_CMD_STOP;
            }
            mmio_write32(controller.mmioBase, DW_IC_DATA_CMD, command);
            ++queued;
            progressed = true;
        }

        if (dw_check_abort(controller)) {
            return false;
        }

        if ((mmio_read32(controller.mmioBase, DW_IC_STATUS) & DW_IC_STATUS_RFNE) != 0) {
            out[received++] = static_cast<uint8_t>(mmio_read32(controller.mmioBase, DW_IC_DATA_CMD) & 0xFF);
            idleIterations = 0;
            continue;
        }

        if (progressed) {
            idleIterations = 0;
        } else if (++idleIterations > I2C_WAIT_ITERATIONS) {
            return false;
        }
        io_wait();
    }

    return dw_wait_bus_idle(controller) && !dw_check_abort(controller);
}

bool send_i2c_hid_simple_command(const I2CHIDController::Controller& controller, uint16_t address, uint16_t commandRegister, uint8_t opcode, uint8_t argument) {
    uint8_t payload[2] = { opcode, argument };
    return i2c_write_register(controller, address, commandRegister, payload, sizeof(payload));
}

bool parse_i2c_hid_descriptor(const uint8_t* descriptor, I2CHIDController::DeviceRuntime& runtime) {
    if (!descriptor || read_le16(descriptor) < I2C_HID_DESCRIPTOR_LENGTH) {
        return false;
    }

    runtime.reportDescriptorLength = read_le16(descriptor + 4);
    runtime.reportDescriptorRegister = read_le16(descriptor + 6);
    runtime.inputRegister = read_le16(descriptor + 8);
    runtime.maxInputLength = read_le16(descriptor + 10);
    runtime.outputRegister = read_le16(descriptor + 12);
    runtime.maxOutputLength = read_le16(descriptor + 14);
    runtime.commandRegister = read_le16(descriptor + 16);
    runtime.dataRegister = read_le16(descriptor + 18);
    runtime.vendorId = read_le16(descriptor + 20);
    runtime.productId = read_le16(descriptor + 22);
    runtime.versionId = read_le16(descriptor + 24);

    return runtime.reportDescriptorLength != 0 &&
        runtime.reportDescriptorRegister != 0 &&
        runtime.inputRegister != 0 &&
        runtime.maxInputLength >= 2;
}

void log_hid_report_prefix(uint8_t deviceIndex, const uint8_t* reportDescriptor, uint16_t length) {
    log_str("[i2c:hid] dev=");
    log_dec(deviceIndex);
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

uint16_t clamp_input_length(uint16_t length) {
    if (length < 2) {
        return 2;
    }
    if (length > I2CHIDController::MaxInputReportBytes) {
        return I2CHIDController::MaxInputReportBytes;
    }
    return length;
}

uint8_t mouse_buttons_from_report(const I2CHIDController::HIDMouseReportLayout& layout, const uint8_t* report, uint16_t reportLength) {
    uint8_t buttons = 0;
    const uint16_t base = mouse_payload_bit_base(layout);
    for (uint8_t i = 0; i < layout.buttonCount && i < 8; ++i) {
        if (hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.buttonsBitOffset + i), 1) != 0) {
            buttons |= static_cast<uint8_t>(1U << i);
        }
    }
    return buttons;
}

bool read_pkg_length(const uint8_t* data, size_t length, size_t offset, uint32_t* value, size_t* consumed) {
    if (!data || offset >= length || !value || !consumed) {
        return false;
    }

    const uint8_t lead = data[offset];
    const uint8_t followBytes = lead >> 6;
    if (offset + 1 + followBytes > length) {
        return false;
    }

    uint32_t result = lead & 0x3F;
    if (followBytes > 0) {
        result = lead & 0x0F;
        for (uint8_t i = 0; i < followBytes; i++) {
            result |= static_cast<uint32_t>(data[offset + 1 + i]) << (4 + i * 8);
        }
    }

    *value = result;
    *consumed = 1 + followBytes;
    return true;
}

bool read_aml_integer(const uint8_t* data, size_t length, size_t offset, uint32_t* value, size_t* consumed) {
    if (!data || offset >= length || !value || !consumed) {
        return false;
    }

    const uint8_t op = data[offset];
    if (op == 0x00) {
        *value = 0;
        *consumed = 1;
        return true;
    }
    if (op == 0x01) {
        *value = 1;
        *consumed = 1;
        return true;
    }
    if (op == 0x0A && offset + 2 <= length) {
        *value = data[offset + 1];
        *consumed = 2;
        return true;
    }
    if (op == 0x0B && offset + 3 <= length) {
        *value = read_le16(data + offset + 1);
        *consumed = 3;
        return true;
    }
    if (op == 0x0C && offset + 5 <= length) {
        *value = read_le32(data + offset + 1);
        *consumed = 5;
        return true;
    }
    if (op <= 0x3F) {
        *value = op;
        *consumed = 1;
        return true;
    }

    return false;
}

bool parse_name_string(const uint8_t* data, size_t length, size_t offset, char* output, size_t outputLength, size_t* consumed) {
    if (!data || !output || outputLength == 0 || !consumed || offset >= length) {
        return false;
    }

    size_t cursor = offset;
    size_t out = 0;
    if (data[cursor] == '\\' || data[cursor] == '^') {
        if (out + 1 < outputLength) {
            output[out++] = static_cast<char>(data[cursor]);
        }
        cursor++;
    }

    uint8_t segmentCount = 1;
    if (cursor < length && data[cursor] == 0x2E) {
        segmentCount = 2;
        cursor++;
    } else if (cursor < length && data[cursor] == 0x2F) {
        if (cursor + 1 >= length) {
            return false;
        }
        segmentCount = data[cursor + 1];
        cursor += 2;
    }

    if (cursor + static_cast<size_t>(segmentCount) * 4 > length) {
        return false;
    }

    for (uint8_t segment = 0; segment < segmentCount; segment++) {
        if (segment && out + 1 < outputLength) {
            output[out++] = '.';
        }
        for (uint8_t i = 0; i < 4; i++) {
            const char c = static_cast<char>(data[cursor++]);
            if (c != '_' && out + 1 < outputLength) {
                output[out++] = c;
            }
        }
    }

    output[out] = 0;
    *consumed = cursor - offset;
    return true;
}

void parse_i2c_serial_bus(const uint8_t* descriptor, size_t length, I2CHIDController::HIDDevice& device) {
    if (length < 14 || descriptor[2] != 1) {
        return;
    }

    device.hasI2c = true;
    device.speedHz = read_le32(descriptor + 6);
    device.i2cAddress = read_le16(descriptor + 10);

    const uint16_t sourceOffset = read_le16(descriptor + 12);
    if (sourceOffset < length) {
        size_t consumed = 0;
        if (!parse_name_string(descriptor, length, sourceOffset, device.resourceSource,
                               sizeof(device.resourceSource), &consumed)) {
            size_t out = 0;
            for (size_t i = sourceOffset; i < length && descriptor[i] && out + 1 < sizeof(device.resourceSource); i++) {
                device.resourceSource[out++] = static_cast<char>(descriptor[i]);
            }
            device.resourceSource[out] = 0;
        }
    }
}

void parse_gpio_connection(const uint8_t* descriptor, size_t length, I2CHIDController::HIDDevice& device) {
    if (length < 19 || descriptor[1] != GPIO_CONNECTION_TYPE_INTERRUPT) {
        return;
    }

    const uint16_t generalFlags = read_le16(descriptor + 2);
    const uint16_t interruptFlags = read_le16(descriptor + 4);
    const uint16_t pinOffset = read_le16(descriptor + 11);
    const uint16_t sourceOffset = read_le16(descriptor + 14);
    const uint16_t vendorOffset = read_le16(descriptor + 16);
    if (pinOffset + 2 > length) {
        return;
    }

    uint16_t pinEnd = static_cast<uint16_t>(length);
    if (sourceOffset > pinOffset && sourceOffset < pinEnd) {
        pinEnd = sourceOffset;
    }
    if (vendorOffset > pinOffset && vendorOffset < pinEnd) {
        pinEnd = vendorOffset;
    }
    if (pinEnd < pinOffset + 2) {
        return;
    }

    const bool edgeTriggered = (interruptFlags & GPIO_INTERRUPT_FLAG_EDGE_TRIGGERED) != 0;
    const bool activeLow = (interruptFlags & GPIO_INTERRUPT_FLAG_ACTIVE_LOW) != 0 ||
                           (interruptFlags & GPIO_INTERRUPT_FLAG_ACTIVE_BOTH) != 0;
    uint16_t ioApicFlags = static_cast<uint16_t>((edgeTriggered ? 1 : 3) << 2);
    ioApicFlags |= activeLow ? 3 : 1;

    device.hasGpio = true;
    device.gpioPin = read_le16(descriptor + pinOffset);
    device.irq = device.gpioPin;
    device.gpioFlags = generalFlags;
    device.gpioInterruptFlags = interruptFlags;
    device.gpioIoApicFlags = ioApicFlags;

    if (sourceOffset > 0 && sourceOffset < length) {
        size_t consumed = 0;
        if (!parse_name_string(descriptor, length, sourceOffset, device.gpioResourceSource,
                               sizeof(device.gpioResourceSource), &consumed)) {
            size_t out = 0;
            for (size_t i = sourceOffset; i < length && descriptor[i] && out + 1 < sizeof(device.gpioResourceSource); i++) {
                device.gpioResourceSource[out++] = static_cast<char>(descriptor[i]);
            }
            device.gpioResourceSource[out] = 0;
        }
    }
}

void parse_resource_template(const uint8_t* data, size_t length, I2CHIDController::HIDDevice& device) {
    size_t cursor = 0;
    while (cursor < length) {
        const uint8_t item = data[cursor++];
        if ((item & 0x80) == 0) {
            const uint8_t type = (item >> 3) & 0x0F;
            const uint8_t itemLength = item & 0x07;
            if (cursor + itemLength > length) {
                return;
            }
            if (type == 0x0F) {
                return;
            }
            cursor += itemLength;
            continue;
        }

        if (cursor + 2 > length) {
            return;
        }
        const uint8_t type = item & 0x7F;
        const uint16_t itemLength = read_le16(data + cursor);
        cursor += 2;
        if (cursor + itemLength > length) {
            return;
        }

        const uint8_t* descriptor = data + cursor;
        if (type == 0x0E) {
            parse_i2c_serial_bus(descriptor, itemLength, device);
        } else if (type == 0x0C) {
            parse_gpio_connection(descriptor, itemLength, device);
        }
        cursor += itemLength;
    }
}

bool parse_resource_template_gpio_controller(const uint8_t* data, size_t length,
                                             I2CHIDController::GpioController& controller) {
    bool found = false;
    size_t cursor = 0;
    while (cursor < length) {
        const uint8_t item = data[cursor++];
        if ((item & 0x80) == 0) {
            const uint8_t type = (item >> 3) & 0x0F;
            const uint8_t itemLength = item & 0x07;
            if (cursor + itemLength > length) {
                return found;
            }
            const uint8_t* descriptor = data + cursor;
            if (type == 0x04 && itemLength >= 2) {
                const uint16_t mask = read_le16(descriptor);
                for (uint8_t irq = 0; irq < 16; ++irq) {
                    if (mask & (1U << irq)) {
                        controller.irqGsi = irq;
                        controller.irqFlags = acpi_irq_info_to_ioapic_flags(itemLength >= 3 ? descriptor[2] : 0,
                                                                             itemLength >= 3);
                        controller.hasIrq = true;
                        found = true;
                        break;
                    }
                }
            }
            if (type == 0x0F) {
                return found;
            }
            cursor += itemLength;
            continue;
        }

        if (cursor + 2 > length) {
            return found;
        }
        const uint8_t type = item & 0x7F;
        const uint16_t itemLength = read_le16(data + cursor);
        cursor += 2;
        if (cursor + itemLength > length) {
            return found;
        }

        const uint8_t* descriptor = data + cursor;
        if (type == 0x06 && itemLength >= 9) {
            controller.mmioBase = read_le32(descriptor + 1);
            controller.mmioLength = read_le32(descriptor + 5);
            controller.hasMmio = controller.mmioBase != 0 && controller.mmioLength != 0;
            found |= controller.hasMmio;
        } else if (type == 0x07 && itemLength >= 23 && descriptor[0] == 0) {
            controller.mmioBase = read_le32(descriptor + 7);
            controller.mmioLength = read_le32(descriptor + 19);
            controller.hasMmio = controller.mmioBase != 0 && controller.mmioLength != 0;
            found |= controller.hasMmio;
        } else if (type == 0x09 && itemLength >= 6 && descriptor[1] > 0) {
            controller.irqGsi = read_le32(descriptor + 2);
            controller.irqFlags = acpi_ext_irq_flags_to_ioapic_flags(descriptor[0]);
            controller.hasIrq = true;
            found = true;
        }

        cursor += itemLength;
    }

    return found;
}

bool find_crs_buffer(const uint8_t* data, size_t length, const uint8_t** resourceData, size_t* resourceLength) {
    if (!data || !resourceData || !resourceLength) {
        return false;
    }

    for (size_t i = 0; i + 7 < length; i++) {
        if (data[i] != 0x08 || memcmp(data + i + 1, "_CRS", 4) != 0 || data[i + 5] != 0x11) {
            continue;
        }

        uint32_t pkgLength = 0;
        size_t pkgConsumed = 0;
        const size_t pkgOffset = i + 6;
        if (!read_pkg_length(data, length, pkgOffset, &pkgLength, &pkgConsumed)) {
            continue;
        }

        const size_t bufferSizeOffset = pkgOffset + pkgConsumed;
        uint32_t bufferSize = 0;
        size_t sizeConsumed = 0;
        if (!read_aml_integer(data, length, bufferSizeOffset, &bufferSize, &sizeConsumed)) {
            continue;
        }

        const size_t payloadOffset = bufferSizeOffset + sizeConsumed;
        const size_t pkgEnd = pkgOffset + pkgLength;
        if (pkgEnd > length || payloadOffset > pkgEnd) {
            continue;
        }

        const size_t available = pkgEnd - payloadOffset;
        *resourceData = data + payloadOffset;
        *resourceLength = bufferSize < available ? bufferSize : available;
        return true;
    }

    return false;
}

bool find_hid_descriptor_register(const uint8_t* data, size_t length, uint16_t* descriptorRegister) {
    static constexpr uint8_t kHidOverI2cDsmUuid[] = {
        0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
        0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE,
    };

    size_t uuidOffset = 0;
    if (!scan_bytes_for(data, length, reinterpret_cast<const char*>(kHidOverI2cDsmUuid),
                        sizeof(kHidOverI2cDsmUuid), &uuidOffset)) {
        return false;
    }

    const size_t searchStart = uuidOffset + sizeof(kHidOverI2cDsmUuid);
    const size_t searchEnd = (searchStart + 192 < length) ? searchStart + 192 : length;
    for (size_t i = searchStart; i + 1 < searchEnd; i++) {
        if (data[i] != 0xA4) {
            continue;
        }

        uint32_t value = 0;
        size_t consumed = 0;
        if (read_aml_integer(data, searchEnd, i + 1, &value, &consumed) && value <= 0xFFFF) {
            *descriptorRegister = static_cast<uint16_t>(value);
            return true;
        }
    }

    return false;
}

void scan_aml_devices(const char* signature, const uint8_t* data, size_t length, AcpiScanContext* scan) {
    if (!scan || !scan->driver || !data) {
        return;
    }

    for (size_t i = 0; i + 8 < length; i++) {
        if (data[i] != 0x5B || data[i + 1] != 0x82) {
            continue;
        }

        uint32_t pkgLength = 0;
        size_t pkgConsumed = 0;
        const size_t pkgOffset = i + 2;
        if (!read_pkg_length(data, length, pkgOffset, &pkgLength, &pkgConsumed)) {
            continue;
        }

        const size_t nameOffset = pkgOffset + pkgConsumed;
        const size_t bodyOffset = nameOffset + 4;
        const size_t deviceEnd = pkgOffset + pkgLength;
        if (bodyOffset >= length || deviceEnd > length || deviceEnd <= bodyOffset) {
            continue;
        }

        const uint8_t* body = data + bodyOffset;
        const size_t bodyLength = deviceEnd - bodyOffset;
        size_t hidOffset = 0;
        if (!scan_bytes_for(body, bodyLength, "PNP0C50", 7, &hidOffset) &&
            !scan_bytes_for(body, bodyLength, "ACPI0C50", 8, &hidOffset)) {
            continue;
        }

        I2CHIDController::HIDDevice device = {};
        for (size_t n = 0; n < 4; n++) {
            device.amlName[n] = static_cast<char>(data[nameOffset + n]);
            device.tableName[n] = signature[n];
        }
        device.amlName[4] = 0;
        device.tableName[4] = 0;
        device.amlOffset = static_cast<uint32_t>(i);

        const uint8_t* resourceData = nullptr;
        size_t resourceLength = 0;
        if (find_crs_buffer(body, bodyLength, &resourceData, &resourceLength)) {
            parse_resource_template(resourceData, resourceLength, device);
        }
        device.hasDescriptorRegister = find_hid_descriptor_register(body, bodyLength, &device.descriptorRegister);

        scan->driver->recordAmlDevice(device);
    }
}

bool body_has_gpio_controller_hint(const uint8_t* body, size_t bodyLength) {
    size_t offset = 0;
    return scan_bytes_for(body, bodyLength, "INT34", 5, &offset) ||
           scan_bytes_for(body, bodyLength, "INTC", 4, &offset);
}

void scan_aml_gpio_controllers(const char* signature, const uint8_t* data, size_t length, AcpiScanContext* scan) {
    if (!scan || !scan->driver || !data) {
        return;
    }

    for (size_t i = 0; i + 8 < length; i++) {
        if (data[i] != 0x5B || data[i + 1] != 0x82) {
            continue;
        }

        uint32_t pkgLength = 0;
        size_t pkgConsumed = 0;
        const size_t pkgOffset = i + 2;
        if (!read_pkg_length(data, length, pkgOffset, &pkgLength, &pkgConsumed)) {
            continue;
        }

        const size_t nameOffset = pkgOffset + pkgConsumed;
        const size_t bodyOffset = nameOffset + 4;
        const size_t deviceEnd = pkgOffset + pkgLength;
        if (bodyOffset >= length || deviceEnd > length || deviceEnd <= bodyOffset) {
            continue;
        }

        const uint8_t* body = data + bodyOffset;
        const size_t bodyLength = deviceEnd - bodyOffset;
        if (!body_has_gpio_controller_hint(body, bodyLength)) {
            continue;
        }

        const uint8_t* resourceData = nullptr;
        size_t resourceLength = 0;
        if (!find_crs_buffer(body, bodyLength, &resourceData, &resourceLength)) {
            continue;
        }

        I2CHIDController::GpioController controller = {};
        for (size_t n = 0; n < 4; n++) {
            controller.amlName[n] = static_cast<char>(data[nameOffset + n]);
            controller.tableName[n] = signature[n];
        }
        controller.amlName[4] = 0;
        controller.tableName[4] = 0;
        controller.amlOffset = static_cast<uint32_t>(i);

        if (parse_resource_template_gpio_controller(resourceData, resourceLength, controller) &&
            controller.hasMmio && controller.hasIrq) {
            scan->driver->recordAmlGpioController(controller);
        }
    }
}

void log_table_signature(const char* signature) {
    char name[5] = {
        signature[0],
        signature[1],
        signature[2],
        signature[3],
        0,
    };
    log_str(name);
}

void scan_acpi_table(const char* signature, void* table, void* context) {
    AcpiScanContext* scan = static_cast<AcpiScanContext*>(context);
    AcpiHeaderView* header = static_cast<AcpiHeaderView*>(table);
    if (!scan || !header || header->length <= sizeof(AcpiHeaderView)) {
        return;
    }

    if (!signature_equals(signature, "SSDT") && !signature_equals(signature, "DSDT")) {
        return;
    }

    const uint8_t* body = reinterpret_cast<const uint8_t*>(header) + sizeof(AcpiHeaderView);
    const size_t bodyLength = header->length - sizeof(AcpiHeaderView);
    const char* needles[] = {
        "PNP0C50",
        "ACPI0C50",
        "I2CSerialBus",
        "I2cSerialBus",
        "GpioInt",
    };

    bool tableHasHint = false;
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        size_t offset = 0;
        const char* needle = needles[i];
        if (scan_bytes_for(body, bodyLength, needle, strlen(needle), &offset)) {
            log_str("[i2c:hid] ACPI hint ");
            log_str(needle);
            log_str(" in ");
            log_table_signature(signature);
            log_str(" offset=");
            log_hex(offset);
            log_str("\n");
            tableHasHint = true;
        }
    }

    if (tableHasHint && scan->hints < 0xFF) {
        scan->hints++;
    }

    scan_aml_gpio_controllers(signature, body, bodyLength, scan);
    scan_aml_devices(signature, body, bodyLength, scan);
}
}

I2CHIDController& I2CHIDController::get() {
    static I2CHIDController instance;
    return instance;
}

bool I2CHIDController::initialize() {
    if (initialized) {
        return controllerCount > 0 && hidHintCount > 0;
    }

    initialized = true;
    controllerCount = 0;
    hidHintCount = 0;
    deviceCount = 0;
    gpioControllerCount = 0;
    devicesProbed = false;
    gpioInterruptsRegistered = false;
    memset(controllers, 0, sizeof(controllers));
    memset(runtimes, 0, sizeof(runtimes));
    memset(devices, 0, sizeof(devices));
    memset(gpioControllers, 0, sizeof(gpioControllers));
    memset(g_gpioRegistrations, 0, sizeof(g_gpioRegistrations));

    log_str("[i2c:hid] initialize\n");
    scanPciControllers();
    scanAcpiTables();

    if (controllerCount == 0) {
        log_str("[i2c:hid] no Intel LPSS I2C PCI controllers found\n");
    }
    if (hidHintCount == 0) {
        log_str("[i2c:hid] no ACPI HID-over-I2C device hints found\n");
    }
    if (controllerCount > 0 || hidHintCount > 0) {
        log_str("[i2c:hid] detected controllers=");
        log_dec(controllerCount);
        log_str(" acpi-hints=");
        log_dec(hidHintCount);
        log_str(" devices=");
        log_dec(deviceCount);
        log_str(" gpio-controllers=");
        log_dec(gpioControllerCount);
        log_str("\n");
        log_str("[i2c:hid] pending LPSS I2C transactions and HID descriptor reads\n");
    }

    probeDevices();
    registerGpioInterrupts();
    return controllerCount > 0 && hidHintCount > 0;
}

void I2CHIDController::poll() {
    if (!initialized || deviceCount == 0 || controllerCount == 0) {
        return;
    }

    if (__atomic_test_and_set(&pollingLock, __ATOMIC_ACQUIRE)) {
        return;
    }

    if (!devicesProbed) {
        probeDevices();
    }

    bool inputDelivered = false;
    for (uint8_t i = 0; i < deviceCount && i < MaxDevices; ++i) {
        DeviceRuntime& runtime = runtimes[i];
        if (!runtime.active || runtime.controllerIndex >= controllerCount) {
            continue;
        }

        const HIDDevice& device = devices[i];
        const Controller& controller = controllers[runtime.controllerIndex];
        uint8_t buffer[MaxInputReportBytes];
        memset(buffer, 0, sizeof(buffer));

        const uint16_t readLength = clamp_input_length(runtime.maxInputLength);
        if (!i2c_read_register(controller, device.i2cAddress, runtime.inputRegister, buffer, readLength)) {
            continue;
        }

        const uint16_t reportLengthWithPrefix = read_le16(buffer);
        if (reportLengthWithPrefix <= 2 || reportLengthWithPrefix == 0xFFFF) {
            continue;
        }

        uint16_t reportLength = static_cast<uint16_t>(reportLengthWithPrefix - 2);
        if (reportLength > readLength - 2) {
            reportLength = static_cast<uint16_t>(readLength - 2);
        }
        const uint8_t* report = buffer + 2;

        if (runtime.keyboard && keyboard_report_matches_id(runtime.keyboardLayout, report, reportLength)) {
            const HIDKeyboardReportLayout& layout = runtime.keyboardLayout;
            const uint8_t hidModifiers = keyboard_modifier_byte(layout, report, reportLength);
            const uint16_t keyModifiers = hid_modifier_byte_to_key_modifiers(hidModifiers);
            const bool shift = (keyModifiers & KeyModifierShift) != 0;
            for (uint8_t keyIndex = 0; keyIndex < layout.keyArrayCount; ++keyIndex) {
                const uint8_t usage = keyboard_key_usage_at(layout, report, reportLength, keyIndex);
                if (usage == 0 || report_had_key(layout, runtime.lastKeyboardReport, MaxInputReportBytes, usage)) {
                    continue;
                }

                const char c = hid_usage_to_char(usage, shift);
                if (c != 0) {
                    Keyboard::get().injectKey(c, keyModifiers, "[i2c:kbd]");
                    inputDelivered = true;
                }
            }
            memset(runtime.lastKeyboardReport, 0, sizeof(runtime.lastKeyboardReport));
            memcpy(runtime.lastKeyboardReport, report, reportLength < sizeof(runtime.lastKeyboardReport) ? reportLength : sizeof(runtime.lastKeyboardReport));
        }

        if (runtime.mouse && mouse_report_matches_id(runtime.mouseLayout, report, reportLength)) {
            const HIDMouseReportLayout& layout = runtime.mouseLayout;
            const uint16_t base = mouse_payload_bit_base(layout);
            const uint8_t buttons = mouse_buttons_from_report(layout, report, reportLength);
            const int32_t dx = sign_extend(hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.xBitOffset), layout.axisReportSize), layout.axisReportSize);
            const int32_t dy = sign_extend(hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.yBitOffset), layout.axisReportSize), layout.axisReportSize);
            int32_t wheel = 0;
            if (layout.hasWheel && layout.wheelBitOffset != 0xFFFF) {
                wheel = sign_extend(hid_read_bits(report, reportLength, static_cast<uint16_t>(base + layout.wheelBitOffset), layout.axisReportSize), layout.axisReportSize);
            }

            if (buttons != 0 || dx != 0 || dy != 0 || wheel != 0) {
                if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127 && wheel >= -128 && wheel <= 127) {
                    Keyboard::get().injectPointerDelta(
                        buttons,
                        static_cast<int8_t>(dx),
                        static_cast<int8_t>(dy),
                        static_cast<int8_t>(wheel),
                        "[i2c:mouse]"
                    );
                    inputDelivered = true;
                }
            }
        }
    }

    __atomic_clear(&pollingLock, __ATOMIC_RELEASE);
    if (inputDelivered) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (current && current->getPriority() == ProcessPriority::Idle) {
            Scheduler::get().yield();
        }
    }
}

void I2CHIDController::handleGpioInterrupt(uint32_t gsi) {
    if (!initialized) {
        return;
    }

    for (uint8_t controllerIndex = 0; controllerIndex < gpioControllerCount; ++controllerIndex) {
        GpioController& controller = gpioControllers[controllerIndex];
        if (!controller.irqRegistered || controller.irqGsi != gsi) {
            continue;
        }

        bool matchedPin = false;
        bool pendingPin = false;
        for (uint8_t i = 0; i < deviceCount && i < MaxDevices; ++i) {
            DeviceRuntime& runtime = runtimes[i];
            if (runtime.active && runtime.gpioInterruptRegistered &&
                runtime.gpioControllerIndex == controllerIndex) {
                matchedPin = true;
                pendingPin |= intel_gpio_ack_interrupt_pin(controller, runtime.gpioPin);
            }
        }

        if (matchedPin) {
            poll();
            return;
        }
        (void)pendingPin;
    }

    for (uint8_t i = 0; i < deviceCount && i < MaxDevices; ++i) {
        DeviceRuntime& runtime = runtimes[i];
        if (runtime.active && runtime.gpioInterruptRegistered && runtime.gpioGsi == gsi) {
            poll();
            return;
        }
    }
}

bool I2CHIDController::hasKeyboard() const {
    for (uint8_t i = 0; i < deviceCount && i < MaxDevices; ++i) {
        if (runtimes[i].active && runtimes[i].keyboard) {
            return true;
        }
    }
    return false;
}

bool I2CHIDController::hasMouse() const {
    for (uint8_t i = 0; i < deviceCount && i < MaxDevices; ++i) {
        if (runtimes[i].active && runtimes[i].mouse) {
            return true;
        }
    }
    return false;
}

void I2CHIDController::scanPciControllers() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            const uint8_t headerType = PCI::get().readConfig8(0, bus, device, 0, PCI_HEADER_TYPE);
            const uint8_t functions = (headerType & PCI_HEADER_MULTIFUNCTION) ? 8 : 1;

            for (uint8_t function = 0; function < functions; function++) {
                const uint16_t vendor = PCI::get().readConfig16(0, bus, device, function, PCI_VENDOR_ID);
                if (vendor == 0xFFFF) {
                    continue;
                }

                const uint8_t classCode = PCI::get().readConfig8(0, bus, device, function, PCI_CLASS);
                const uint8_t subclass = PCI::get().readConfig8(0, bus, device, function, PCI_SUBCLASS);
                if (classCode != PCI_CLASS_SERIAL_BUS || subclass != PCI_SUBCLASS_I2C) {
                    continue;
                }

                const uint16_t deviceId = PCI::get().readConfig16(0, bus, device, function, PCI_DEVICE_ID);
                const uint8_t progIf = PCI::get().readConfig8(0, bus, device, function, PCI_PROG_IF);
                const uint32_t bar0 = PCI::get().readConfig32(0, bus, device, function, PCI_BAR0);
                const bool mmio = (bar0 & PCI_BAR_IO) == 0;
                const bool bar64 = (bar0 & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64;
                uint64_t mmioBase = static_cast<uint64_t>(bar0 & 0xFFFFFFF0U);
                if (bar64) {
                    const uint32_t bar1 = PCI::get().readConfig32(0, bus, device, function, PCI_BAR1);
                    mmioBase |= static_cast<uint64_t>(bar1) << 32;
                }
                mmioBase &= PCI_BAR_MEM_ADDR_MASK;

                log_str("[i2c:hid] pci i2c ");
                log_dec(bus);
                log_str(":");
                log_dec(device);
                log_str(".");
                log_dec(function);
                log_str(" vendor=");
                log_hex(vendor);
                log_str(" device=");
                log_hex(deviceId);
                log_str(" prog=");
                log_hex(progIf);
                log_str(" bar0=");
                log_hex(bar0);
                if (vendor == PCI_VENDOR_INTEL) {
                    log_str(" intel-lpss");
                }
                if (mmio && mmioBase) {
                    log_str(" mmio=");
                    log_hex(mmioBase);
                }
                log_str("\n");

                if (controllerCount < MaxControllers) {
                    Controller& controller = controllers[controllerCount];
                    controller.present = true;
                    controller.bus = static_cast<uint8_t>(bus);
                    controller.device = device;
                    controller.function = function;
                    controller.vendorId = vendor;
                    controller.deviceId = deviceId;
                    controller.progIf = progIf;
                    controller.mmioBase = mmio && mmioBase ? mmioBase : 0;
                    controller.txDepth = 16;
                    controller.rxDepth = 16;
                    controllerCount++;
                } else if (controllerCount < 0xFF) {
                    controllerCount++;
                }
            }
        }
    }
}

void I2CHIDController::probeDevices() {
    if (devicesProbed) {
        return;
    }
    devicesProbed = true;

    const uint8_t storedControllerCount = controllerCount < MaxControllers ? controllerCount : MaxControllers;
    for (uint8_t controllerIndex = 0; controllerIndex < storedControllerCount; ++controllerIndex) {
        Controller& controller = controllers[controllerIndex];
        if (!controller.present || controller.mmioBase == 0) {
            continue;
        }

        uint16_t command = PCI::get().readConfig16(0, controller.bus, controller.device, controller.function, PCI_COMMAND);
        command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
        command &= static_cast<uint16_t>(~PCI_COMMAND_IO);
        PCI::get().writeConfig16(0, controller.bus, controller.device, controller.function, PCI_COMMAND, command);

        map_mmio_window(controller.mmioBase, PAGE_SIZE);
        const uint32_t compType = mmio_read32(controller.mmioBase, DW_IC_COMP_TYPE);
        if (compType != DW_IC_COMP_TYPE_VALUE) {
            log_str("[i2c:hid] controller ");
            log_dec(controller.bus);
            log_str(":");
            log_dec(controller.device);
            log_str(".");
            log_dec(controller.function);
            log_str(" unexpected comp=");
            log_hex(compType);
            log_str("\n");
        }

        if (!dw_set_enabled(controller, false)) {
            log_str("[i2c:hid] controller disable timeout\n");
            continue;
        }

        const uint32_t compParam = mmio_read32(controller.mmioBase, DW_IC_COMP_PARAM_1);
        const uint8_t rxDepth = static_cast<uint8_t>(((compParam >> 8) & 0xFF) + 1);
        const uint8_t txDepth = static_cast<uint8_t>(((compParam >> 16) & 0xFF) + 1);
        if (rxDepth > 1) {
            controller.rxDepth = rxDepth > MaxInputReportBytes ? MaxInputReportBytes : rxDepth;
        }
        if (txDepth != 0) {
            controller.txDepth = txDepth;
        }

        mmio_write32(controller.mmioBase, DW_IC_INTR_MASK, 0);
        mmio_write32(controller.mmioBase, DW_IC_RX_TL, 0);
        mmio_write32(controller.mmioBase, DW_IC_TX_TL, 0);
        mmio_write32(controller.mmioBase, DW_IC_CON,
            DW_IC_CON_MASTER |
            DW_IC_CON_SPEED_FAST |
            DW_IC_CON_RESTART_EN |
            DW_IC_CON_SLAVE_DISABLE);
        controller.ready = dw_set_enabled(controller, true);
        if (controller.ready) {
            log_str("[i2c:hid] controller ready ");
            log_dec(controller.bus);
            log_str(":");
            log_dec(controller.device);
            log_str(".");
            log_dec(controller.function);
            log_str(" rx=");
            log_dec(controller.rxDepth);
            log_str(" tx=");
            log_dec(controller.txDepth);
            log_str("\n");
        } else {
            log_str("[i2c:hid] controller enable timeout\n");
        }
    }

    for (uint8_t deviceIndex = 0; deviceIndex < deviceCount && deviceIndex < MaxDevices; ++deviceIndex) {
        const HIDDevice& device = devices[deviceIndex];
        DeviceRuntime& runtime = runtimes[deviceIndex];
        runtime.probed = true;
        if (!device.hasI2c || !device.hasDescriptorRegister || device.i2cAddress == 0) {
            continue;
        }

        for (uint8_t controllerIndex = 0; controllerIndex < storedControllerCount; ++controllerIndex) {
            Controller& controller = controllers[controllerIndex];
            if (!controller.ready) {
                continue;
            }

            uint8_t hidDescriptor[I2C_HID_DESCRIPTOR_LENGTH];
            memset(hidDescriptor, 0, sizeof(hidDescriptor));
            if (!i2c_read_register(controller, device.i2cAddress, device.descriptorRegister, hidDescriptor, sizeof(hidDescriptor))) {
                continue;
            }

            DeviceRuntime candidate = {};
            candidate.probed = true;
            candidate.controllerIndex = controllerIndex;
            if (!parse_i2c_hid_descriptor(hidDescriptor, candidate)) {
                continue;
            }

            uint16_t reportLength = candidate.reportDescriptorLength;
            if (reportLength > MaxReportDescriptorBytes) {
                reportLength = MaxReportDescriptorBytes;
            }
            uint8_t reportDescriptor[MaxReportDescriptorBytes];
            memset(reportDescriptor, 0, sizeof(reportDescriptor));
            if (!i2c_read_register(controller, device.i2cAddress, candidate.reportDescriptorRegister, reportDescriptor, reportLength)) {
                log_str("[i2c:hid] report descriptor read failed dev=");
                log_dec(deviceIndex);
                log_str("\n");
                continue;
            }

            log_hid_report_prefix(deviceIndex, reportDescriptor, reportLength);
            candidate.keyboard = parse_hid_keyboard_report_descriptor(reportDescriptor, reportLength, candidate.keyboardLayout);
            candidate.mouse = parse_hid_mouse_report_descriptor(reportDescriptor, reportLength, candidate.mouseLayout);
            candidate.active = candidate.keyboard || candidate.mouse;
            if (!candidate.active) {
                log_str("[i2c:hid] dev=");
                log_dec(deviceIndex);
                log_str(" has no supported keyboard/mouse report\n");
                continue;
            }

            send_i2c_hid_simple_command(controller, device.i2cAddress, candidate.commandRegister, I2C_HID_OPCODE_SET_POWER, I2C_HID_POWER_ON);
            send_i2c_hid_simple_command(controller, device.i2cAddress, candidate.commandRegister, I2C_HID_OPCODE_RESET, 0);

            runtime = candidate;
            log_str("[i2c:hid] dev=");
            log_dec(deviceIndex);
            log_str(" ready addr=");
            log_hex(device.i2cAddress);
            log_str(" ctrl=");
            log_dec(controllerIndex);
            log_str(" vid=");
            log_hex(runtime.vendorId);
            log_str(" pid=");
            log_hex(runtime.productId);
            log_str(" input=");
            log_hex(runtime.inputRegister);
            log_str(" max=");
            log_dec(runtime.maxInputLength);
            log_str(" keyboard=");
            log_dec(runtime.keyboard ? 1 : 0);
            log_str(" mouse=");
            log_dec(runtime.mouse ? 1 : 0);
            log_str("\n");
            break;
        }

        if (!runtime.active) {
            log_str("[i2c:hid] dev=");
            log_dec(deviceIndex);
            log_str(" not reachable on available controllers\n");
        }
    }
}

void I2CHIDController::registerGpioInterrupts() {
    if (gpioInterruptsRegistered) {
        return;
    }
    gpioInterruptsRegistered = true;

    uint8_t nextVector = VECTOR_GPIO_BASE;
    for (uint8_t deviceIndex = 0; deviceIndex < deviceCount && deviceIndex < MaxDevices; ++deviceIndex) {
        const HIDDevice& device = devices[deviceIndex];
        DeviceRuntime& runtime = runtimes[deviceIndex];
        if (!runtime.active || !device.hasGpio) {
            continue;
        }

        uint8_t controllerIndex = 0xFF;
        if (device.gpioResourceSource[0]) {
            for (uint8_t i = 0; i < gpioControllerCount; ++i) {
                if (gpio_source_matches_controller(device.gpioResourceSource, gpioControllers[i].amlName)) {
                    controllerIndex = i;
                    break;
                }
            }
        }

        if (controllerIndex != 0xFF) {
            GpioController& controller = gpioControllers[controllerIndex];
            if (!controller.irqRegistered) {
                bool reused = false;
                for (uint8_t i = 0; i < MaxDevices; ++i) {
                    if (g_gpioRegistrations[i].active && g_gpioRegistrations[i].gsi == controller.irqGsi) {
                        controller.irqVector = g_gpioRegistrations[i].vector;
                        controller.irqRegistered = true;
                        reused = true;
                        break;
                    }
                }

                if (!reused) {
                    if (nextVector > VECTOR_GPIO_LIMIT) {
                        log_str("[i2c:hid] gpio irq vector range exhausted\n");
                        break;
                    }

                    const uint8_t vector = nextVector++;
                    g_gpioHandlers[deviceIndex].configure(controller.irqGsi);
                    ISR::registerIRQ(vector, &g_gpioHandlers[deviceIndex]);

                    const uint8_t targetCore = static_cast<uint8_t>(LAPIC::get().getId());
                    if (!APICManager::get().mapGSI(controller.irqGsi, vector, targetCore, controller.irqFlags)) {
                        log_str("[i2c:hid] gpio controller irq map failed dev=");
                        log_dec(deviceIndex);
                        log_str(" controller=");
                        log_str(controller.amlName);
                        log_str(" irq=");
                        log_dec(controller.irqGsi);
                        log_str("\n");
                    } else {
                        g_gpioRegistrations[deviceIndex].active = true;
                        g_gpioRegistrations[deviceIndex].gsi = controller.irqGsi;
                        g_gpioRegistrations[deviceIndex].vector = vector;
                        controller.irqVector = vector;
                        controller.irqRegistered = true;
                    }
                }
            }

            if (controller.irqRegistered) {
                intel_gpio_enable_interrupt_pin(controller, device.gpioPin);
                runtime.gpioInterruptRegistered = true;
                runtime.gpioVector = controller.irqVector;
                runtime.gpioGsi = controller.irqGsi;
                runtime.gpioControllerIndex = controllerIndex;
                runtime.gpioPin = device.gpioPin;

                log_str("[i2c:hid] gpio controller irq dev=");
                log_dec(deviceIndex);
                log_str(" controller=");
                log_str(controller.amlName);
                log_str(" pin=");
                log_dec(device.gpioPin);
                log_str(" irq=");
                log_dec(controller.irqGsi);
                log_str(" vector=");
                log_hex(controller.irqVector);
                log_str("\n");
                continue;
            }
        }

        bool registered = false;
        for (uint8_t i = 0; i < MaxDevices; ++i) {
            if (g_gpioRegistrations[i].active && g_gpioRegistrations[i].gsi == device.gpioPin) {
                runtime.gpioInterruptRegistered = true;
                runtime.gpioVector = g_gpioRegistrations[i].vector;
                runtime.gpioGsi = device.gpioPin;
                runtime.gpioPin = device.gpioPin;
                registered = true;
                break;
            }
        }
        if (registered) {
            continue;
        }

        if (nextVector > VECTOR_GPIO_LIMIT) {
            log_str("[i2c:hid] gpio irq vector range exhausted\n");
            break;
        }

        const uint8_t vector = nextVector++;
        g_gpioHandlers[deviceIndex].configure(device.gpioPin);
        ISR::registerIRQ(vector, &g_gpioHandlers[deviceIndex]);

        const uint8_t targetCore = static_cast<uint8_t>(LAPIC::get().getId());
        if (!APICManager::get().mapGSI(device.gpioPin, vector, targetCore, device.gpioIoApicFlags)) {
            log_str("[i2c:hid] gpio irq map failed dev=");
            log_dec(deviceIndex);
            log_str(" pin=");
            log_dec(device.gpioPin);
            log_str("\n");
            continue;
        }

        g_gpioRegistrations[deviceIndex].active = true;
        g_gpioRegistrations[deviceIndex].gsi = device.gpioPin;
        g_gpioRegistrations[deviceIndex].vector = vector;
        runtime.gpioInterruptRegistered = true;
        runtime.gpioVector = vector;
        runtime.gpioGsi = device.gpioPin;
        runtime.gpioPin = device.gpioPin;

        log_str("[i2c:hid] gpio irq dev=");
        log_dec(deviceIndex);
        log_str(" gsi=");
        log_dec(device.gpioPin);
        log_str(" vector=");
        log_hex(vector);
        log_str(" flags=");
        log_hex(device.gpioIoApicFlags);
        if (device.gpioResourceSource[0]) {
            log_str(" controller=");
            log_str(device.gpioResourceSource);
        }
        log_str("\n");
    }
}

void I2CHIDController::recordAmlDevice(const HIDDevice& device) {
    if (deviceCount >= sizeof(devices) / sizeof(devices[0])) {
        log_str("[i2c:hid] AML device table full\n");
        return;
    }

    devices[deviceCount++] = device;

    log_str("[i2c:hid] aml device ");
    log_str(device.tableName);
    log_str(":");
    log_str(device.amlName);
    log_str(" off=");
    log_hex(device.amlOffset);
    if (device.hasI2c) {
        log_str(" addr=");
        log_hex(device.i2cAddress);
        if (device.hasDescriptorRegister) {
            log_str(" hid-desc=");
            log_hex(device.descriptorRegister);
        } else {
            log_str(" no-hid-desc-dsm");
        }
        log_str(" speed=");
        log_dec(device.speedHz);
        if (device.resourceSource[0]) {
            log_str(" controller=");
            log_str(device.resourceSource);
        }
    } else {
        log_str(" no-i2c-crs");
    }
    if (device.hasGpio) {
        log_str(" gpio=");
        log_dec(device.gpioPin);
        log_str(" gpio-flags=");
        log_hex(device.gpioFlags);
        log_str(" gpio-int=");
        log_hex(device.gpioInterruptFlags);
        if (device.gpioFlags & GPIO_GENERAL_FLAG_SHARED) {
            log_str(" shared");
        }
        if (device.gpioFlags & GPIO_GENERAL_FLAG_WAKE) {
            log_str(" wake");
        }
        if (device.gpioResourceSource[0]) {
            log_str(" gpio-controller=");
            log_str(device.gpioResourceSource);
        }
    } else {
        log_str(" no-gpio-crs");
    }
    log_str("\n");
}

void I2CHIDController::recordAmlGpioController(const GpioController& controller) {
    if (gpioControllerCount >= sizeof(gpioControllers) / sizeof(gpioControllers[0])) {
        log_str("[i2c:hid] GPIO controller table full\n");
        return;
    }

    for (uint8_t i = 0; i < gpioControllerCount; ++i) {
        if (memcmp(gpioControllers[i].amlName, controller.amlName, 4) == 0 &&
            gpioControllers[i].mmioBase == controller.mmioBase) {
            return;
        }
    }

    gpioControllers[gpioControllerCount++] = controller;

    log_str("[i2c:hid] gpio controller ");
    log_str(controller.tableName);
    log_str(":");
    log_str(controller.amlName);
    log_str(" off=");
    log_hex(controller.amlOffset);
    log_str(" mmio=");
    log_hex(controller.mmioBase);
    log_str(" len=");
    log_hex(controller.mmioLength);
    log_str(" irq=");
    log_dec(controller.irqGsi);
    log_str(" flags=");
    log_hex(controller.irqFlags);
    log_str("\n");
}

void I2CHIDController::scanAcpiTables() {
    AcpiScanContext context = {
        this,
        0,
    };

    void* dsdt = ACPI::get().findDsdt();
    if (dsdt) {
        scan_acpi_table("DSDT", dsdt, &context);
    }

    ACPI::get().forEachTable(scan_acpi_table, &context);
    hidHintCount = context.hints;
}
