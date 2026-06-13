#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/net/net_device.hpp>

// Intel 8254x / e1000(e) Gigabit Ethernet NIC driver.
//
// Targets the 82540EM (PCI device 0x100E), which is QEMU's classic emulated
// gigabit NIC (-device e1000) and is also found on a great deal of real
// hardware.  The driver uses memory-mapped registers (MMIO BAR0) and the
// legacy descriptor ring model, polling for receive/transmit completion.
//
// The same legacy descriptor model and core register layout also drive the
// e1000e family, including the Intel I219-LM (PCH LAN-on-motherboard) NICs
// found on most Intel-based business laptops/desktops from ~2015 onward, so
// this driver also matches those device IDs.

constexpr uint16_t E1000_VENDOR_ID = 0x8086;
constexpr uint16_t E1000_DEVICE_ID_82540EM = 0x100E;  // QEMU default -device e1000
constexpr uint16_t E1000_DEVICE_ID_82545EM = 0x100F;
constexpr uint16_t E1000_DEVICE_ID_82574L = 0x10D3;   // e1000e

// Intel I219-LM device IDs.  Each Intel CPU/PCH generation shipped a new ID
// for what is functionally the same I219-LM LOM controller, so several must be
// matched.  (LM = vPro/managed variant; the consumer "V" parts use adjacent
// IDs and behave the same for this driver.)
constexpr uint16_t E1000_DEVICE_ID_I219LM = 0x156F;       // Skylake (gen 1)
constexpr uint16_t E1000_DEVICE_ID_I219LM_2 = 0x15B7;     // Kaby Lake (gen 2)
constexpr uint16_t E1000_DEVICE_ID_I219LM_3 = 0x15BB;     // gen 3
constexpr uint16_t E1000_DEVICE_ID_I219LM_4 = 0x15D7;     // gen 4 (Coffee Lake)
constexpr uint16_t E1000_DEVICE_ID_I219LM_5 = 0x15E3;     // gen 5
constexpr uint16_t E1000_DEVICE_ID_I219LM_6 = 0x15BE;     // Comet Lake
constexpr uint16_t E1000_DEVICE_ID_I219LM_7 = 0x0D4E;     // Ice/Tiger Lake
constexpr uint16_t E1000_DEVICE_ID_I219LM_8 = 0x1A1E;     // Alder Lake

constexpr size_t E1000_RX_DESCRIPTORS = 32;
constexpr size_t E1000_TX_DESCRIPTORS = 32;
constexpr size_t E1000_RX_BUFFER_SIZE = 2048;  // 2 KiB per descriptor
constexpr size_t E1000_TX_BUFFER_SIZE = 2048;

// Legacy receive descriptor.
struct E1000RxDesc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

// Legacy transmit descriptor.
struct E1000TxDesc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

class E1000Driver : public NetDevice {
public:
    static E1000Driver& get();

    bool initialize() override;
    bool isInitialized() const override { return initialized; }
    bool isAvailable() const override { return deviceFound; }

    void getMacAddress(uint8_t* mac) override;
    bool sendPacket(const void* data, size_t len) override;
    int receivePacket(void* buffer, size_t maxLen) override;
    bool isLinkUp() const override;
    const char* name() const override { return isI219 ? "i219-lm" : "e1000"; }
    int probePriority() const override { return 50; }

private:
    E1000Driver();

    bool detectDevice();
    static bool matchesDeviceId(uint16_t devId);
    static bool isI219DeviceId(uint16_t devId);
    bool initDevice();
    bool setupRx();
    bool setupTx();
    void readMacAddress();

    uint32_t mmioRead(uint16_t reg) const;
    void mmioWrite(uint16_t reg, uint32_t value) const;

    bool initialized;
    bool deviceFound;

    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t pciDeviceId;
    bool isI219;

    volatile uint8_t* mmio;  // MMIO register base (identity-mapped physical)

    E1000RxDesc* rxDescs;
    uint64_t rxDescsPhys;
    uint8_t* rxBuffers[E1000_RX_DESCRIPTORS];
    uint64_t rxBuffersPhys[E1000_RX_DESCRIPTORS];
    uint16_t rxCurrent;

    E1000TxDesc* txDescs;
    uint64_t txDescsPhys;
    uint8_t* txBuffers[E1000_TX_DESCRIPTORS];
    uint64_t txBuffersPhys[E1000_TX_DESCRIPTORS];
    uint16_t txCurrent;

    uint8_t macAddr[6];
};
