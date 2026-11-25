#include <drivers/net/usb_net.hpp>

#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <drivers/usb/xhci.hpp>

UsbNetDriver& UsbNetDriver::get() {
    static UsbNetDriver instance;
    return instance;
}

bool UsbNetDriver::isAvailable() const {
    return usbnet::present();
}

bool UsbNetDriver::initialize() {
    if (initialized) {
        return true;
    }
    if (!usbnet::present()) {
        return false;
    }
    initialized = true;
    Cereal::get().write("[net] usb-cdc-ecm: USB Ethernet adapter, polled\n");
    return true;
}

void UsbNetDriver::getMacAddress(uint8_t* mac) {
    if (mac) {
        usbnet::macAddress(mac);
    }
}

bool UsbNetDriver::sendPacket(const void* data, size_t len) {
    return initialized && len <= NET_DEVICE_MTU && usbnet::send(data, len);
}

int UsbNetDriver::receivePacket(void* buffer, size_t maxLen) {
    return initialized ? usbnet::receive(buffer, maxLen) : -1;
}

bool UsbNetDriver::isLinkUp() const {
    return initialized && usbnet::linkUp();
}
