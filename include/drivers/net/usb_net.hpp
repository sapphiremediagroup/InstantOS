#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/net/net_device.hpp>

// A USB Ethernet adapter speaking CDC-ECM, on an xHCI controller
// (src/drivers/usb/xhci.cpp does the USB side). Polled by the stack on every
// tick: the xHCI interrupt handler must not wake processes.
class UsbNetDriver : public NetDevice {
public:
    static UsbNetDriver& get();

    bool initialize() override;
    bool isInitialized() const override { return initialized; }
    bool isAvailable() const override;

    void getMacAddress(uint8_t* mac) override;
    bool sendPacket(const void* data, size_t len) override;
    int receivePacket(void* buffer, size_t maxLen) override;
    bool isLinkUp() const override;
    const char* name() const override { return "usb-cdc-ecm"; }
    // After the PCI NICs: a machine with a working one keeps it.
    int probePriority() const override { return 90; }

private:
    UsbNetDriver() = default;

    bool initialized = false;
};
