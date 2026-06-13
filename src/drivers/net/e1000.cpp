#include <drivers/net/e1000.hpp>
#include <cpu/acpi/pci.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>

namespace {
// PCI config-space register offsets.
constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;
constexpr uint16_t PCI_BAR0_REG = 0x10;

constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

// e1000 register offsets (byte offsets into MMIO space).
constexpr uint16_t REG_CTRL = 0x0000;     // Device Control
constexpr uint16_t REG_STATUS = 0x0008;   // Device Status
constexpr uint16_t REG_EERD = 0x0014;     // EEPROM Read
constexpr uint16_t REG_ICR = 0x00C0;      // Interrupt Cause Read
constexpr uint16_t REG_IMS = 0x00D0;      // Interrupt Mask Set
constexpr uint16_t REG_IMC = 0x00D8;      // Interrupt Mask Clear
constexpr uint16_t REG_RCTL = 0x0100;     // Receive Control
constexpr uint16_t REG_TCTL = 0x0400;     // Transmit Control
constexpr uint16_t REG_TIPG = 0x0410;     // Transmit Inter-Packet Gap
constexpr uint16_t REG_RDBAL = 0x2800;    // RX Descriptor Base Low
constexpr uint16_t REG_RDBAH = 0x2804;    // RX Descriptor Base High
constexpr uint16_t REG_RDLEN = 0x2808;    // RX Descriptor Length
constexpr uint16_t REG_RDH = 0x2810;      // RX Descriptor Head
constexpr uint16_t REG_RDT = 0x2818;      // RX Descriptor Tail
constexpr uint16_t REG_TDBAL = 0x3800;    // TX Descriptor Base Low
constexpr uint16_t REG_TDBAH = 0x3804;    // TX Descriptor Base High
constexpr uint16_t REG_TDLEN = 0x3808;    // TX Descriptor Length
constexpr uint16_t REG_TDH = 0x3810;      // TX Descriptor Head
constexpr uint16_t REG_TDT = 0x3818;      // TX Descriptor Tail
constexpr uint16_t REG_RAL0 = 0x5400;     // Receive Address Low (0)
constexpr uint16_t REG_RAH0 = 0x5404;     // Receive Address High (0)

// Device Status bits.
constexpr uint32_t STATUS_LU = 1 << 1;    // Link Up

// Device Control bits.
constexpr uint32_t CTRL_RST = 1 << 26;    // Reset
constexpr uint32_t CTRL_SLU = 1 << 6;     // Set Link Up
constexpr uint32_t CTRL_ASDE = 1 << 5;    // Auto-Speed Detection Enable
constexpr uint32_t CTRL_PHY_RST = 1u << 31;  // PHY Reset (must be cleared to run)

// Receive Control bits.
constexpr uint32_t RCTL_EN = 1 << 1;      // Receiver Enable
constexpr uint32_t RCTL_SBP = 1 << 2;     // Store Bad Packets
constexpr uint32_t RCTL_UPE = 1 << 3;     // Unicast Promiscuous Enable
constexpr uint32_t RCTL_MPE = 1 << 4;     // Multicast Promiscuous Enable
constexpr uint32_t RCTL_BAM = 1 << 15;    // Broadcast Accept Mode
constexpr uint32_t RCTL_SECRC = 1 << 26;  // Strip Ethernet CRC
// RCTL_BSIZE = 00 with BSEX=0 -> 2048 byte buffers.

// Transmit Control bits.
constexpr uint32_t TCTL_EN = 1 << 1;      // Transmit Enable
constexpr uint32_t TCTL_PSP = 1 << 3;     // Pad Short Packets
constexpr uint32_t TCTL_CT_SHIFT = 4;     // Collision Threshold
constexpr uint32_t TCTL_COLD_SHIFT = 12;  // Collision Distance

// Receive descriptor status bits.
constexpr uint8_t RX_STATUS_DD = 1 << 0;  // Descriptor Done
constexpr uint8_t RX_STATUS_EOP = 1 << 1; // End Of Packet

// Transmit descriptor command/status bits.
constexpr uint8_t TX_CMD_EOP = 1 << 0;    // End Of Packet
constexpr uint8_t TX_CMD_IFCS = 1 << 1;   // Insert FCS
constexpr uint8_t TX_CMD_RS = 1 << 3;     // Report Status
constexpr uint8_t TX_STATUS_DD = 1 << 0;  // Descriptor Done

constexpr uint32_t TX_DD_SPIN_LIMIT = 1000000;

void mem_barrier() {
    asm volatile("" ::: "memory");
}
}

E1000Driver& E1000Driver::get() {
    static E1000Driver instance;
    return instance;
}

E1000Driver::E1000Driver()
    : initialized(false),
      deviceFound(false),
      bus(0),
      device(0),
      function(0),
      pciDeviceId(0),
      isI219(false),
      mmio(nullptr),
      rxDescs(nullptr),
      rxDescsPhys(0),
      rxBuffers{},
      rxBuffersPhys{},
      rxCurrent(0),
      txDescs(nullptr),
      txDescsPhys(0),
      txBuffers{},
      txBuffersPhys{},
      txCurrent(0),
      macAddr{} {}

bool E1000Driver::initialize() {
    if (initialized) {
        return true;
    }
    if (!detectDevice()) {
        return false;
    }
    if (!initDevice()) {
        return false;
    }
    initialized = true;
    return true;
}

bool E1000Driver::isI219DeviceId(uint16_t devId) {
    switch (devId) {
        case E1000_DEVICE_ID_I219LM:
        case E1000_DEVICE_ID_I219LM_2:
        case E1000_DEVICE_ID_I219LM_3:
        case E1000_DEVICE_ID_I219LM_4:
        case E1000_DEVICE_ID_I219LM_5:
        case E1000_DEVICE_ID_I219LM_6:
        case E1000_DEVICE_ID_I219LM_7:
        case E1000_DEVICE_ID_I219LM_8:
            return true;
        default:
            return false;
    }
}

bool E1000Driver::matchesDeviceId(uint16_t devId) {
    if (devId == E1000_DEVICE_ID_82540EM ||
        devId == E1000_DEVICE_ID_82545EM ||
        devId == E1000_DEVICE_ID_82574L) {
        return true;
    }
    return isI219DeviceId(devId);
}

bool E1000Driver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_VENDOR_ID_REG);
                if (vendor != E1000_VENDOR_ID) {
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_DEVICE_ID_REG);
                if (matchesDeviceId(devId)) {
                    bus = static_cast<uint8_t>(b);
                    device = d;
                    function = f;
                    pciDeviceId = devId;
                    isI219 = isI219DeviceId(devId);
                    deviceFound = true;
                    return true;
                }
            }
        }
    }
    return false;
}

bool E1000Driver::initDevice() {
    PCI& pci = PCI::get();

    // Enable MMIO and bus mastering.
    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND_REG);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    pci.writeConfig16(0, bus, device, function, PCI_COMMAND_REG, command);

    // BAR0 is a memory BAR pointing at the register space.
    const uint32_t bar0 = pci.readConfig32(0, bus, device, function, PCI_BAR0_REG);
    if ((bar0 & 0x1) != 0) {
        return false;  // expected a memory BAR
    }
    uint64_t base = bar0 & ~0xFULL;
    const uint32_t barType = (bar0 >> 1) & 0x3;
    if (barType == 0x2) {
        const uint32_t barHigh = pci.readConfig32(0, bus, device, function, PCI_BAR0_REG + 4);
        base |= static_cast<uint64_t>(barHigh) << 32;
    }
    if (base == 0) {
        return false;
    }
    mmio = reinterpret_cast<volatile uint8_t*>(base);

    // Mask (disable) all interrupts; this driver polls.
    mmioWrite(REG_IMC, 0xFFFFFFFFu);
    (void)mmioRead(REG_ICR);  // clear pending causes

    // Full device reset, then re-mask interrupts.
    mmioWrite(REG_CTRL, mmioRead(REG_CTRL) | CTRL_RST);
    for (uint32_t i = 0; i < 1000000; ++i) {
        if ((mmioRead(REG_CTRL) & CTRL_RST) == 0) {
            break;
        }
        mem_barrier();
    }
    mmioWrite(REG_IMC, 0xFFFFFFFFu);
    (void)mmioRead(REG_ICR);

    if (isI219) {
        // On the I219 the MAC and PHY sit on opposite sides of an internal
        // interconnect.  After a device reset the PHY needs time to come back
        // up; make sure CTRL.PHY_RST is de-asserted (a left-asserted PHY reset
        // would keep the link held down) and give the interconnect a moment to
        // settle before bringing the link up.  We deliberately avoid the full
        // EXTCNF_CTRL/SWFLAG PHY-register handshake here because this polled
        // driver never touches MDIO PHY registers directly.
        mmioWrite(REG_CTRL, mmioRead(REG_CTRL) & ~CTRL_PHY_RST);
        for (uint32_t i = 0; i < 2000000; ++i) {
            mem_barrier();
        }
    }

    // Bring the link up and let the PHY auto-negotiate speed.
    mmioWrite(REG_CTRL, mmioRead(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    readMacAddress();

    if (!setupRx()) {
        return false;
    }
    if (!setupTx()) {
        return false;
    }

    rxCurrent = 0;
    txCurrent = 0;
    return true;
}

void E1000Driver::readMacAddress() {
    // On QEMU and most platforms the firmware pre-loads RAL0/RAH0 with the MAC.
    const uint32_t low = mmioRead(REG_RAL0);
    const uint32_t high = mmioRead(REG_RAH0);
    macAddr[0] = static_cast<uint8_t>(low & 0xFF);
    macAddr[1] = static_cast<uint8_t>((low >> 8) & 0xFF);
    macAddr[2] = static_cast<uint8_t>((low >> 16) & 0xFF);
    macAddr[3] = static_cast<uint8_t>((low >> 24) & 0xFF);
    macAddr[4] = static_cast<uint8_t>(high & 0xFF);
    macAddr[5] = static_cast<uint8_t>((high >> 8) & 0xFF);
}

bool E1000Driver::setupRx() {
    // Allocate the descriptor ring (must be 16-byte aligned; a page is fine).
    const uint64_t ringBytes = E1000_RX_DESCRIPTORS * sizeof(E1000RxDesc);
    const uint64_t ringFrames = (ringBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    rxDescsPhys = PMM::AllocFrames(ringFrames);
    if (rxDescsPhys == 0) {
        return false;
    }
    rxDescs = reinterpret_cast<E1000RxDesc*>(rxDescsPhys);
    memset(rxDescs, 0, ringFrames * PMM::PAGE_SIZE);

    for (size_t i = 0; i < E1000_RX_DESCRIPTORS; ++i) {
        const uint64_t phys = PMM::AllocFrames(1);
        if (phys == 0) {
            return false;
        }
        rxBuffersPhys[i] = phys;
        rxBuffers[i] = reinterpret_cast<uint8_t*>(phys);
        rxDescs[i].addr = phys;
        rxDescs[i].status = 0;
    }

    mmioWrite(REG_RDBAL, static_cast<uint32_t>(rxDescsPhys & 0xFFFFFFFFu));
    mmioWrite(REG_RDBAH, static_cast<uint32_t>(rxDescsPhys >> 32));
    mmioWrite(REG_RDLEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(REG_RDH, 0);
    mmioWrite(REG_RDT, E1000_RX_DESCRIPTORS - 1);

    // Enable receiver: broadcast accept, strip CRC, 2 KiB buffers.
    mmioWrite(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    return true;
}

bool E1000Driver::setupTx() {
    const uint64_t ringBytes = E1000_TX_DESCRIPTORS * sizeof(E1000TxDesc);
    const uint64_t ringFrames = (ringBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    txDescsPhys = PMM::AllocFrames(ringFrames);
    if (txDescsPhys == 0) {
        return false;
    }
    txDescs = reinterpret_cast<E1000TxDesc*>(txDescsPhys);
    memset(txDescs, 0, ringFrames * PMM::PAGE_SIZE);

    for (size_t i = 0; i < E1000_TX_DESCRIPTORS; ++i) {
        const uint64_t phys = PMM::AllocFrames(1);
        if (phys == 0) {
            return false;
        }
        txBuffersPhys[i] = phys;
        txBuffers[i] = reinterpret_cast<uint8_t*>(phys);
        txDescs[i].addr = phys;
        txDescs[i].status = TX_STATUS_DD;  // mark all free initially
    }

    mmioWrite(REG_TDBAL, static_cast<uint32_t>(txDescsPhys & 0xFFFFFFFFu));
    mmioWrite(REG_TDBAH, static_cast<uint32_t>(txDescsPhys >> 32));
    mmioWrite(REG_TDLEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(REG_TDH, 0);
    mmioWrite(REG_TDT, 0);

    // Standard IPG for copper (10 / 8 / 6).
    mmioWrite(REG_TIPG, 10u | (8u << 10) | (6u << 20));

    // Enable transmitter: pad short packets, collision threshold 15,
    // collision distance 64 (full-duplex).
    mmioWrite(REG_TCTL, TCTL_EN | TCTL_PSP | (15u << TCTL_CT_SHIFT) | (64u << TCTL_COLD_SHIFT));
    return true;
}

void E1000Driver::getMacAddress(uint8_t* mac) {
    if (!mac) {
        return;
    }
    memcpy(mac, macAddr, sizeof(macAddr));
}

bool E1000Driver::sendPacket(const void* data, size_t len) {
    if (!initialized || !data || len == 0 || len > NET_DEVICE_MTU || len > E1000_TX_BUFFER_SIZE) {
        return false;
    }

    const uint16_t desc = txCurrent;

    // Wait for the descriptor to be free (DD set) before reusing it.
    for (uint32_t spin = 0; spin < TX_DD_SPIN_LIMIT; ++spin) {
        if ((txDescs[desc].status & TX_STATUS_DD) != 0) {
            break;
        }
        if (spin + 1 == TX_DD_SPIN_LIMIT) {
            return false;
        }
        mem_barrier();
    }

    memcpy(txBuffers[desc], data, len);
    txDescs[desc].addr = txBuffersPhys[desc];
    txDescs[desc].length = static_cast<uint16_t>(len);
    txDescs[desc].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    txDescs[desc].status = 0;
    mem_barrier();

    // Advance the tail to hand the descriptor to the NIC.
    txCurrent = static_cast<uint16_t>((desc + 1) % E1000_TX_DESCRIPTORS);
    mmioWrite(REG_TDT, txCurrent);
    return true;
}

int E1000Driver::receivePacket(void* buffer, size_t maxLen) {
    if (!initialized || !buffer || maxLen == 0) {
        return -1;
    }

    E1000RxDesc& desc = rxDescs[rxCurrent];
    if ((desc.status & RX_STATUS_DD) == 0) {
        return -1;  // no packet ready
    }

    int result = -1;
    // Only deliver complete (EOP) packets; the stack expects whole frames.
    if ((desc.status & RX_STATUS_EOP) != 0) {
        const size_t length = desc.length;
        const size_t copied = length < maxLen ? length : maxLen;
        if (copied != 0) {
            memcpy(buffer, rxBuffers[rxCurrent], copied);
        }
        result = static_cast<int>(copied);
    }

    // Hand the descriptor back to the NIC and advance the tail.
    desc.status = 0;
    mem_barrier();
    mmioWrite(REG_RDT, rxCurrent);
    rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % E1000_RX_DESCRIPTORS);
    return result;
}

bool E1000Driver::isLinkUp() const {
    if (!initialized) {
        return false;
    }
    return (mmioRead(REG_STATUS) & STATUS_LU) != 0;
}

uint32_t E1000Driver::mmioRead(uint16_t reg) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio + reg);
}

void E1000Driver::mmioWrite(uint16_t reg, uint32_t value) const {
    *reinterpret_cast<volatile uint32_t*>(mmio + reg) = value;
}
