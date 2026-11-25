#pragma once

#include <stdint.h>
#include <stddef.h>

class XHCIController {
public:
    static XHCIController& get();

    bool initialize();
    void poll();
    bool claimPendingInterrupt();
    void handleInterrupt();

    uint8_t controllerCount() const { return controllersFound; }
    uint8_t initializedControllerCount() const { return initializedControllers; }
    bool hasInitializedController() const { return initializedControllers > 0; }
    bool hasBootKeyboard() const;
    bool hasBootMouse() const;

private:
    XHCIController() = default;

    uint8_t controllersFound = 0;
    uint8_t initializedControllers = 0;
};

// The USB Ethernet adapter (CDC-ECM) xHCI enumeration found, if any; the
// NetDevice over it is src/drivers/net/usb_net.cpp.
namespace usbnet {
bool present();
bool linkUp();
void macAddress(uint8_t mac[6]);
uint16_t vendorId();
uint16_t productId();
bool send(const void* data, size_t length);
int receive(void* buffer, size_t maxLength);   // -1 when no frame is waiting
}
