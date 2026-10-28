#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/net/net_device.hpp>

// Realtek RTL8169 / RTL8111 / RTL8168 / RTL8101 ("r8169") Ethernet driver.
//
// This is the NIC on most PC boards (10ec:8168, the RTL8111/8168 gigabit
// family). The chips share the C+ descriptor model: rings of 16-byte
// descriptors whose OWN bit hands them between driver and NIC, EOR marking the
// ring's end. The family is told apart by the XID in TxConfig, as in Linux's
// r8169 (the RTL_GIGA_MAC_VER_* numbers are Linux's); 8168G and later (mac
// version 40+) reach the PHY through the GPHY_OCP window and gate their
// receiver (MISC.RXDV_GATED_EN), which the driver must open. MSI, else INTx.
// The 2.5G RTL8125 (10ec:8125) has a different register layout and is not
// handled.

constexpr size_t RTL8169_RX_DESCRIPTORS = 256;
constexpr size_t RTL8169_TX_DESCRIPTORS = 256;
constexpr size_t RTL8169_RX_BUFFER_SIZE = 2048;   // frame + 4 CRC bytes fit

struct RTL8169Desc {
    uint32_t opts1;   // OWN, EOR, FS, LS, length / buffer size
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed));

class RTL8169Driver : public NetDevice {
public:
    static RTL8169Driver& get();

    bool initialize() override;
    bool isInitialized() const override { return initialized; }
    bool isAvailable() const override { return deviceFound; }

    void getMacAddress(uint8_t* mac) override;
    bool sendPacket(const void* data, size_t len) override;
    int receivePacket(void* buffer, size_t maxLen) override;
    bool isLinkUp() const override;
    bool interruptDriven() const override { return irqMode != IRQMode::None; }
    const char* name() const override { return chipName; }
    int probePriority() const override { return 60; }

    // Interrupt handler hooks.
    bool claimInterrupt();
    bool usesMsi() const { return irqMode == IRQMode::Msi; }

private:
    enum class IRQMode : uint8_t { None, Msi, Legacy };

    RTL8169Driver() = default;

    bool detectDevice();
    bool initDevice();
    void identifyChip();
    void hwInit8168g();
    void enableRxdvGate();
    void reset();
    void readMacAddress();
    void writeMacAddress();
    void startPhy();
    bool setupRings();
    void start();
    void setupInterrupts();

    // Register access (MMIO BAR, identity-mapped uncached).
    uint8_t read8(uint16_t reg) const;
    uint16_t read16(uint16_t reg) const;
    uint32_t read32(uint16_t reg) const;
    void write8(uint16_t reg, uint8_t value) const;
    void write16(uint16_t reg, uint16_t value) const;
    void write32(uint16_t reg, uint32_t value) const;

    // Side channels: PHY (PHYAR, or GPHY_OCP on 8168G+), MAC OCP, ERI.
    int phyRead(uint8_t reg);
    void phyWrite(uint8_t reg, uint16_t value);
    uint16_t macOcpRead(uint16_t reg);
    void macOcpWrite(uint16_t reg, uint16_t value);
    void macOcpModify(uint16_t reg, uint16_t clear, uint16_t set);
    uint32_t eriRead(uint16_t addr);
    bool waitFor(uint16_t reg, uint32_t mask, bool high, uint32_t polls, uint32_t intervalUs) const;

    bool initialized = false;
    bool deviceFound = false;
    uint8_t bus = 0;
    uint8_t device = 0;
    uint8_t function = 0;
    uint16_t pciDeviceId = 0;
    volatile uint8_t* mmio = nullptr;
    IRQMode irqMode = IRQMode::None;

    uint32_t xid = 0;
    int macVersion = 0;          // Linux RTL_GIGA_MAC_VER_*, 0 = unknown
    const char* chipName = "rtl8169";

    RTL8169Desc* rxRing = nullptr;
    uint8_t* rxBuffers[RTL8169_RX_DESCRIPTORS] {};
    uint16_t rxCurrent = 0;

    RTL8169Desc* txRing = nullptr;
    uint8_t* txBuffers[RTL8169_TX_DESCRIPTORS] {};
    uint16_t txCurrent = 0;

    uint8_t macAddr[6] {};
};
