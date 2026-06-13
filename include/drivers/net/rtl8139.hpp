#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/net/net_device.hpp>

// Realtek RTL8139 (RTL8139C/8139D) Fast Ethernet NIC driver.
//
// The RTL8139 is one of the simplest real NICs and is emulated by QEMU
// (-device rtl8139), VirtualBox and others.  This driver programs the chip
// through its PCI I/O BAR (port I/O) and uses the classic single linear
// receive ring plus four transmit descriptors.

constexpr uint16_t RTL8139_VENDOR_ID = 0x10EC;
constexpr uint16_t RTL8139_DEVICE_ID = 0x8139;

// Receive buffer: 8 KiB ring + 16 byte slack for the WRAP overflow + extra
// page so the chip can write a full max-size frame past the wrap point.
constexpr size_t RTL8139_RX_BUFFER_SIZE = 8192 + 16 + 1536;

// Number of hardware transmit descriptors (fixed by the chip).
constexpr size_t RTL8139_TX_DESCRIPTORS = 4;

class RTL8139Driver : public NetDevice {
public:
    static RTL8139Driver& get();

    bool initialize() override;
    bool isInitialized() const override { return initialized; }
    bool isAvailable() const override { return deviceFound; }

    void getMacAddress(uint8_t* mac) override;
    bool sendPacket(const void* data, size_t len) override;
    int receivePacket(void* buffer, size_t maxLen) override;
    bool isLinkUp() const override;
    const char* name() const override { return "rtl8139"; }
    int probePriority() const override { return 60; }

private:
    RTL8139Driver();

    bool detectDevice();
    bool initDevice();

    // I/O port register access relative to the I/O BAR base.
    uint8_t  read8(uint16_t reg) const;
    uint16_t read16(uint16_t reg) const;
    uint32_t read32(uint16_t reg) const;
    void write8(uint16_t reg, uint8_t value) const;
    void write16(uint16_t reg, uint16_t value) const;
    void write32(uint16_t reg, uint32_t value) const;

    bool initialized;
    bool deviceFound;

    // PCI location.
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    // I/O BAR base port.
    uint16_t ioBase;

    // DMA buffers (physical == virtual; identity-mapped like virtio-net).
    uint64_t rxBufferPhys;
    uint8_t* rxBuffer;
    uint64_t txBufferPhys[RTL8139_TX_DESCRIPTORS];
    uint8_t* txBuffer[RTL8139_TX_DESCRIPTORS];

    // Software read offset into the RX ring (mirrors CAPR).
    uint16_t rxOffset;

    // Next TX descriptor to use (round-robin 0..3).
    uint8_t txCurrent;

    uint8_t macAddr[6];
};
