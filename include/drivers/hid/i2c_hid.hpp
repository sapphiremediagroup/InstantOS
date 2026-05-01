#pragma once

#include <stdint.h>

class I2CHIDController {
public:
    struct HIDDevice {
        char amlName[5];
        char tableName[5];
        uint32_t amlOffset = 0;
        uint16_t i2cAddress = 0;
        uint16_t descriptorRegister = 0;
        uint32_t speedHz = 0;
        uint32_t irq = 0;
        uint32_t gpioPin = 0;
        uint16_t gpioFlags = 0;
        uint16_t gpioInterruptFlags = 0;
        uint16_t gpioIoApicFlags = 0;
        char resourceSource[64];
        char gpioResourceSource[64];
        bool hasI2c = false;
        bool hasGpio = false;
        bool hasDescriptorRegister = false;
    };

    struct GpioController {
        char amlName[5];
        char tableName[5];
        uint32_t amlOffset = 0;
        uint64_t mmioBase = 0;
        uint32_t mmioLength = 0;
        uint32_t irqGsi = 0;
        uint16_t irqFlags = 0;
        uint8_t irqVector = 0;
        bool hasMmio = false;
        bool hasIrq = false;
        bool irqRegistered = false;
    };

    static I2CHIDController& get();

    bool initialize();
    void poll();

    uint8_t getControllerCount() const { return controllerCount; }
    uint8_t getHidHintCount() const { return hidHintCount; }
    uint8_t getDeviceCount() const { return deviceCount; }
    bool hasKeyboard() const;
    bool hasMouse() const;
    void handleGpioInterrupt(uint32_t gsi);
    void recordAmlDevice(const HIDDevice& device);
    void recordAmlGpioController(const GpioController& controller);

public:
    static constexpr uint8_t MaxControllers = 16;
    static constexpr uint8_t MaxDevices = 8;
    static constexpr uint16_t MaxReportDescriptorBytes = 512;
    static constexpr uint16_t MaxInputReportBytes = 128;

    struct Controller {
        bool present = false;
        bool ready = false;
        uint8_t bus = 0;
        uint8_t device = 0;
        uint8_t function = 0;
        uint16_t vendorId = 0;
        uint16_t deviceId = 0;
        uint8_t progIf = 0;
        uint64_t mmioBase = 0;
        uint8_t txDepth = 16;
        uint8_t rxDepth = 16;
    };

    struct HIDKeyboardReportLayout {
        bool valid = false;
        bool keyBitmap = false;
        uint8_t reportId = 0;
        uint16_t keyUsageMinimum = 0;
        uint16_t modifierBitOffset = 0;
        uint16_t keyArrayBitOffset = 0;
        uint8_t keyArrayCount = 0;
        uint8_t keyArrayReportSize = 8;
    };

    struct HIDMouseReportLayout {
        bool valid = false;
        bool relative = false;
        uint8_t reportId = 0;
        uint16_t buttonsBitOffset = 0;
        uint8_t buttonCount = 0;
        uint16_t xBitOffset = 0;
        uint16_t yBitOffset = 0;
        uint16_t wheelBitOffset = 0;
        uint8_t axisReportSize = 8;
        bool hasWheel = false;
    };

    struct DeviceRuntime {
        bool probed = false;
        bool active = false;
        bool keyboard = false;
        bool mouse = false;
        uint8_t controllerIndex = 0xFF;
        uint16_t reportDescriptorLength = 0;
        uint16_t reportDescriptorRegister = 0;
        uint16_t inputRegister = 0;
        uint16_t maxInputLength = 0;
        uint16_t outputRegister = 0;
        uint16_t maxOutputLength = 0;
        uint16_t commandRegister = 0;
        uint16_t dataRegister = 0;
        uint16_t vendorId = 0;
        uint16_t productId = 0;
        uint16_t versionId = 0;
        HIDKeyboardReportLayout keyboardLayout;
        HIDMouseReportLayout mouseLayout;
        uint8_t lastKeyboardReport[MaxInputReportBytes] = {};
        bool gpioInterruptRegistered = false;
        uint8_t gpioVector = 0;
        uint32_t gpioGsi = 0;
        uint8_t gpioControllerIndex = 0xFF;
        uint32_t gpioPin = 0;
    };

private:
    I2CHIDController() = default;

    void scanPciControllers();
    void scanAcpiTables();
    void probeDevices();
    void registerGpioInterrupts();

    bool initialized = false;
    bool devicesProbed = false;
    bool gpioInterruptsRegistered = false;
    volatile bool pollingLock = false;
    uint8_t controllerCount = 0;
    uint8_t hidHintCount = 0;
    uint8_t deviceCount = 0;
    uint8_t gpioControllerCount = 0;
    Controller controllers[MaxControllers];
    DeviceRuntime runtimes[MaxDevices];
    HIDDevice devices[8];
    GpioController gpioControllers[16];
};
