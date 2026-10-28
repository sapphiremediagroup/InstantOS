#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/net/net_device.hpp>

// Intel gigabit Ethernet: the 8254x (e1000), 8257x/82583 (e1000e), the PCH
// LAN-on-motherboard parts (82577 through the I219, e1000e) and the igb family
// (82575/82576, 82580, I350/I354, I210/I211).
//
// They share the register core (CTRL, STATUS, ICR/IMS, RCTL/TCTL, RAL/RAH,
// MDIC). The igb parts keep their queue registers elsewhere, must have each
// queue enabled, and use advanced descriptors; the others legacy ones. The PCH parts reach their PHY over an interconnect
// that the firmware may have left in SMBus or ultra-low-power mode, which the
// driver undoes (a subset of Linux's e1000_init_phy_workarounds_pchlan), and
// a register read straight after their reset hangs the bus. MMIO BAR0, MSI,
// else INTx.

constexpr size_t E1000_RX_DESCRIPTORS = 256;
constexpr size_t E1000_TX_DESCRIPTORS = 256;
constexpr size_t E1000_RX_BUFFER_SIZE = 2048;
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

// Advanced (igb) receive descriptor: the driver writes the two addresses, the
// NIC writes back over them (statusError and length in the upper half).
union E1000AdvRxDesc {
    struct {
        uint64_t packetAddr;
        uint64_t headerAddr;
    };
    struct {
        uint64_t info;
        uint32_t statusError;
        uint16_t length;
        uint16_t vlan;
    };
} __attribute__((packed));

// Advanced (igb) transmit data descriptor; written back with DD in bit 0 of
// olinfoStatus.
struct E1000AdvTxDesc {
    uint64_t addr;
    uint32_t cmdTypeLen;
    uint32_t olinfoStatus;
} __attribute__((packed));

class E1000Driver : public NetDevice {
public:
    enum class Family : uint8_t {
        Classic,   // 8254x
        Pcie,      // 82571-82574, 82583
        Pch,       // ICH/PCH LOM: 82567, 82577-82579, I217, I218, I219
        Igb,       // 82575, 82576, 82580, I350, I354, I210, I211
    };

    static E1000Driver& get();

    bool initialize() override;
    bool isInitialized() const override { return initialized; }
    bool isAvailable() const override { return deviceFound; }

    void getMacAddress(uint8_t* mac) override;
    bool sendPacket(const void* data, size_t len) override;
    int receivePacket(void* buffer, size_t maxLen) override;
    bool isLinkUp() const override;
    bool interruptDriven() const override { return irqMode != IRQMode::None; }
    const char* name() const override { return chipName; }

    // Interrupt handler hooks.
    bool claimInterrupt();
    bool usesMsi() const { return irqMode == IRQMode::Msi; }
    int probePriority() const override { return 50; }

private:
    enum class IRQMode : uint8_t { None, Msi, Legacy };

    E1000Driver() = default;
    void setupInterrupts();

    bool detectDevice();
    bool initDevice();
    void resetMac();
    void pchPhyWorkarounds();
    void togglePhyPowerPin();
    bool phyAccessible();
    bool acquireSwFlag();
    void releaseSwFlag();
    void startPhy();
    bool setupRx();
    bool setupTx();
    void readMacAddress();
    bool enableQueue(uint32_t reg);
    int receiveAdvanced(void* buffer, size_t maxLen);

    bool mdicRead(uint8_t reg, uint16_t& value);
    bool mdicWrite(uint8_t reg, uint16_t value);

    uint32_t mmioRead(uint32_t reg) const;
    void mmioWrite(uint32_t reg, uint32_t value) const;

    bool initialized = false;
    bool deviceFound = false;

    uint8_t bus = 0;
    uint8_t device = 0;
    uint8_t function = 0;
    uint16_t pciDeviceId = 0;
    Family family = Family::Classic;
    const char* chipName = "e1000";
    bool pchLpt = false;      // I217 and later: ULP and LANPHYPC handling
    uint8_t phyAddress = 1;
    uint32_t rxBase = 0;      // queue 0 register blocks
    uint32_t txBase = 0;

    volatile uint8_t* mmio = nullptr;  // MMIO register base (identity-mapped physical)
    IRQMode irqMode = IRQMode::None;

    E1000RxDesc* rxDescs = nullptr;
    uint8_t* rxBuffers[E1000_RX_DESCRIPTORS] {};
    uint16_t rxCurrent = 0;

    E1000TxDesc* txDescs = nullptr;
    uint8_t* txBuffers[E1000_TX_DESCRIPTORS] {};
    uint64_t txBuffersPhys[E1000_TX_DESCRIPTORS] {};
    uint16_t txCurrent = 0;

    uint8_t macAddr[6] {};
};
