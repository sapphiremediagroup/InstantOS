#pragma once

#include <stdint.h>

class USBInput {
public:
    static USBInput& get();

    void initialize();
    void poll();
    bool isInitialized() const { return initialized; }
    bool hasController() const { return controllerReady; }
    bool isXhciActive() const { return xhciActive; }
    bool hasKeyboard() const;
    bool hasMouse() const;

private:
    USBInput() = default;

    bool initialized = false;
    bool controllerReady = false;
    bool xhciActive = false;
    bool keyboardReady = false;
    bool interruptPending = false;

    volatile uint32_t* regs = nullptr;
    bool keyboardLowSpeed = false;
    uint8_t keyboardAddress = 0;
    uint8_t keyboardEndpoint = 0;
    uint8_t keyboardMaxPacket = 8;
    uint8_t keyboardInterface = 0;
    uint8_t lastReport[8] = {};

    void* hcca = nullptr;
    void* interruptEd = nullptr;
    void* interruptTd = nullptr;
    void* interruptTail = nullptr;
    void* interruptBuffer = nullptr;

    uint32_t interruptEdPhys = 0;
    uint32_t interruptTdPhys = 0;
    uint32_t interruptTailPhys = 0;
    uint32_t interruptBufferPhys = 0;

    bool detectController();
    bool initializeController(uint8_t bus, uint8_t slot, uint8_t func, uint32_t bar0);
    bool initializeRootPorts();
    bool enumerateDevice(uint8_t port, bool lowSpeed);
    void submitInterruptTransfer();
    void completeInterruptTransfer();

    volatile bool polling_lock = false;
};
