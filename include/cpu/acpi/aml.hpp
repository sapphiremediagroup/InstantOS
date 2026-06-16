#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AML {

struct NamespaceNode;

enum class ObjectType : uint8_t {
    None,
    Integer,
    String,
    Buffer,
    Package,
    Method,
    Scope,
    Device,
    Processor,
    PowerResource,
    ThermalZone,
    OperationRegion,
    FieldUnit,
    Mutex,
    Event,
    Alias,
    Reference,
};

struct Object {
    ObjectType type = ObjectType::None;
    uint64_t integer = 0;
    const char* string = nullptr;
    const uint8_t* buffer = nullptr;
    size_t length = 0;
    Object* elements = nullptr;
    size_t elementCount = 0;
    NamespaceNode* node = nullptr;
    const uint8_t* methodBody = nullptr;
    size_t methodLength = 0;
    uint8_t methodArgCount = 0;
    uint8_t methodFlags = 0;
};

// A decoded ACPI resource from a device's _CRS. The kind selects which of the
// fields are meaningful. This is the data drivers need to claim hardware:
// interrupt lines, I/O port windows, MMIO windows, and the serial-bus
// connection descriptors used by modern I2C/SPI/UART peripherals.
enum class ResourceKind : uint8_t {
    None,
    Irq,           // legacy IRQ mask or extended interrupt
    Io,            // I/O port range
    Memory,        // memory-mapped range
    Dma,           // legacy DMA channel
    GpioInt,       // GPIO interrupt connection
    GpioIo,        // GPIO I/O connection
    I2cSerialBus,  // I2C serial bus connection
    SpiSerialBus,  // SPI serial bus connection
    UartSerialBus, // UART serial bus connection
};

struct AcpiResource {
    ResourceKind kind = ResourceKind::None;

    // Irq / GpioInt: interrupt numbers (GSI for extended/GPIO, ISA IRQ for the
    // legacy small descriptor). Memory/Io: address window. SerialBus: parent
    // controller + per-bus parameters.
    uint64_t base = 0;        // Io/Memory: window start; SerialBus: device address
    uint64_t length = 0;      // Io/Memory: window length in bytes
    uint32_t interrupts[16] = {}; // Irq/Gpio: GSI / pin numbers
    uint8_t interruptCount = 0;
    bool levelTriggered = false;
    bool activeLow = false;
    bool shared = false;

    // Serial-bus connection details. resourceSource names the parent controller
    // device path inside the buffer that carried this descriptor.
    uint32_t busSpeedHz = 0;  // I2C/SPI/UART bit rate or baud
    uint16_t serialAddress = 0; // I2C slave address / SPI device selection
    const char* resourceSource = nullptr;
    size_t resourceSourceLength = 0;
};

// Summary of an ACPI device node gathered while walking the namespace.
struct DeviceInfo {
    NamespaceNode* node = nullptr;
    char path[256] = {};       // full namespace path, e.g. "\\_SB.PCI0.I2C1.TPD0"
    char hid[16] = {};         // decoded _HID (EISA ID or string), empty if absent
    char cid[16] = {};         // decoded first _CID, empty if absent
    uint64_t adr = 0;          // _ADR value (bus address), 0 if absent
    uint64_t uid = 0;          // _UID integer, 0 if absent
    bool hasAdr = false;
    bool hasUid = false;
    uint32_t sta = 0x0F;       // _STA result; defaults to present+enabled
    bool present = true;       // (_STA bit0) device is present
    bool enabled = true;       // (_STA bit1) device is enabled/decoding
};


class Interpreter {
public:
    struct Cursor {
        const uint8_t* ptr = nullptr;
        const uint8_t* end = nullptr;
    };

    Interpreter();

    bool initialize();
    bool loadTable(void* table);

    bool evaluate(const char* path, Object* result);
    bool evaluateInteger(const char* path, uint64_t* result);
    bool getS5SleepTypes(uint16_t* slpTypA, uint16_t* slpTypB);

    // Device enumeration. Walks every Device(...) node in the namespace and
    // invokes `callback` with a populated DeviceInfo (decoded _HID/_CID, _ADR,
    // _UID and evaluated _STA). Returning false from the callback stops the walk.
    using DeviceCallback = bool (*)(const DeviceInfo& info, void* context);
    void forEachDevice(DeviceCallback callback, void* context);

    // Convenience lookups for drivers binding to a specific hardware ID. The id
    // is compared (case-insensitively) against the device's decoded _HID and
    // _CID. Returns the matching node, or nullptr.
    NamespaceNode* findDeviceByHid(const char* hid);

    // Fill `out` for the given device node. `outResources` (optional) receives
    // up to `maxResources` parsed _CRS entries; `outCount` returns how many were
    // produced. Returns false if `node` is not a Device.
    bool describeDevice(NamespaceNode* node, DeviceInfo* out);
    size_t readDeviceResources(NamespaceNode* node, AcpiResource* outResources,
                               size_t maxResources);

    // Build the full namespace path string ("\\_SB.PCI0...") for a node into
    // `out` (capacity `cap`). Returns the number of characters written.
    size_t nodePath(NamespaceNode* node, char* out, size_t cap);

    size_t namespaceNodeCount() const { return nodeCount; }
    bool isInitialized() const { return initialized; }

private:
    struct ExecutionContext {
        Interpreter* interpreter = nullptr;
        NamespaceNode* scope = nullptr;
        Object args[7];
        Object locals[8];
        bool returned = false;
        bool broke = false;
        Object returnValue;
        uint8_t depth = 0;
    };

    NamespaceNode* root = nullptr;
    bool initialized = false;
    size_t nodeCount = 0;

    NamespaceNode* allocateNode(const char name[4], NamespaceNode* parent);
    NamespaceNode* findChild(NamespaceNode* parent, const char name[4]);
    NamespaceNode* findOrCreateChild(NamespaceNode* parent, const char name[4]);
    NamespaceNode* resolveAsciiPath(const char* path, NamespaceNode* scope, bool create);
    NamespaceNode* resolveNameString(Cursor& cursor, NamespaceNode* scope, bool create);
    NamespaceNode* resolveNameStringAt(const uint8_t* data, const uint8_t* end, NamespaceNode* scope,
                                       bool create, size_t* consumed);

    bool parseTermList(Cursor& cursor, NamespaceNode* scope);
    bool parseNamedObject(Cursor& cursor, NamespaceNode* scope, uint8_t op);
    bool skipTerm(Cursor& cursor);
    bool skipTermArg(Cursor& cursor);
    bool skipNameString(Cursor& cursor);
    bool skipSuperName(Cursor& cursor);
    bool skipFieldList(Cursor& cursor, const uint8_t* fieldEnd, NamespaceNode* scope);

    bool evaluateObject(ExecutionContext& context, Cursor& cursor, Object* result);
    bool executeTermList(ExecutionContext& context, Cursor& cursor, const uint8_t* listEnd);
    bool executeMethod(NamespaceNode* methodNode, const Object* args, size_t argCount, Object* result,
                       uint8_t depth);
    bool evaluateNamedObject(ExecutionContext& context, NamespaceNode* node, Object* result);
    bool evaluateTarget(ExecutionContext& context, Cursor& cursor, Object** targetObject,
                        NamespaceNode** targetNode);
    bool storeToTarget(const Object& value, Object* targetObject, NamespaceNode* targetNode);

    bool makeInteger(uint64_t value, Object* result);
    bool objectToInteger(const Object& object, uint64_t* value);
    bool copyObject(const Object& source, Object* dest);
    Object* allocateObjects(size_t count);

    // Device-enumeration internals.
    void walkDevices(NamespaceNode* node, DeviceCallback callback, void* context, bool* stop);
    bool evaluateChildInteger(NamespaceNode* device, const char name[4], uint64_t* value);
    bool evaluateChildObject(NamespaceNode* device, const char name[4], Object* out);
    NamespaceNode* findDeviceByHidRecursive(NamespaceNode* node, const char* hid);
    size_t parseCrsBuffer(const uint8_t* data, size_t length, AcpiResource* out, size_t maxResources);
};

}
