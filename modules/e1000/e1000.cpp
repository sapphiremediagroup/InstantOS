// Intel gigabit Ethernet as a loadable module (e1000.idrv): the 8254x (e1000),
// 8257x/82583 (e1000e), the PCH LAN-on-motherboard parts (82566 through the
// I219, e1000e), the igb family (82575/82576, 82580, I350/I354, I210/I211) and
// the 2.5G I225/I226 (igc). It was the kernel's src/drivers/net/e1000.cpp; the
// hardware side is unchanged. Register names and bits follow Linux's e1000,
// e1000e, igb and igc drivers.
//
// They share the register core (CTRL, STATUS, ICR/IMS, RCTL/TCTL, RAL/RAH,
// MDIC). The igb and igc parts keep their queue registers elsewhere, must have
// each queue enabled, and use advanced descriptors; the others legacy ones.
// The PCH parts reach their PHY over an interconnect that the firmware may
// have left in SMBus or ultra-low-power mode, which the driver undoes (a
// subset of Linux's e1000_init_phy_workarounds_pchlan), and a register read
// straight after their reset hangs the bus. MMIO BAR0, MSI, else INTx.

#include "../sdk/idrv_sdk.h"

namespace {

constexpr uint16_t INTEL_VENDOR = 0x8086;

constexpr size_t RX_DESCRIPTORS = 256;
constexpr size_t TX_DESCRIPTORS = 256;
constexpr size_t BUFFER_SIZE = 2048;
constexpr size_t MTU = 1514;

// Register offsets (byte offsets into MMIO space).
constexpr uint32_t REG_CTRL = 0x0000;     // Device Control
constexpr uint32_t REG_STATUS = 0x0008;   // Device Status
constexpr uint32_t REG_EECD = 0x0010;     // EEPROM/Flash Control
constexpr uint32_t REG_CTRL_EXT = 0x0018; // Extended Device Control
constexpr uint32_t REG_MDIC = 0x0020;     // MDI Control (PHY access)
constexpr uint32_t REG_FEXTNVM3 = 0x003C;
constexpr uint32_t REG_ICR = 0x00C0;      // Interrupt Cause Read
constexpr uint32_t REG_IMS = 0x00D0;      // Interrupt Mask Set
constexpr uint32_t REG_IMC = 0x00D8;      // Interrupt Mask Clear
constexpr uint32_t REG_RCTL = 0x0100;     // Receive Control
constexpr uint32_t REG_TCTL = 0x0400;     // Transmit Control
constexpr uint32_t REG_TIPG = 0x0410;     // Transmit Inter-Packet Gap
constexpr uint32_t REG_EXTCNF_CTRL = 0x0F00;
constexpr uint32_t REG_RDTR = 0x2820;     // RX Delay Timer
constexpr uint32_t REG_RAL0 = 0x5400;     // Receive Address Low (0)
constexpr uint32_t REG_RAH0 = 0x5404;     // Receive Address High (0)
constexpr uint32_t REG_H2ME = 0x5B50;     // host to ME (PCH)
constexpr uint32_t REG_FWSM = 0x5B54;     // firmware semaphore (PCH)

// Queue 0 register blocks: 0x2800/0x3800 on the e1000(e), 0xC000/0xE000 on
// the igb. Offsets within a block:
constexpr uint32_t Q_BAL = 0x00;
constexpr uint32_t Q_BAH = 0x04;
constexpr uint32_t Q_LEN = 0x08;
constexpr uint32_t Q_SRRCTL = 0x0C;       // igb receive only
constexpr uint32_t Q_HEAD = 0x10;
constexpr uint32_t Q_TAIL = 0x18;
constexpr uint32_t Q_DCTL = 0x28;         // RXDCTL / TXDCTL
constexpr uint32_t QUEUE_ENABLE = 1 << 25;
constexpr uint32_t SRRCTL_BSIZE_2K = 2;   // 1 KiB units
constexpr uint32_t SRRCTL_DESCTYPE_ADV_ONEBUF = 1 << 25;
constexpr uint32_t SRRCTL_DROP_EN = 1u << 31;

constexpr uint32_t STATUS_LU = 1 << 1;    // Link Up
constexpr uint32_t STATUS_LAN_INIT_DONE = 1 << 9;
constexpr uint32_t STATUS_PHYRA = 1 << 10;
constexpr uint32_t STATUS_GIO_MASTER_ENABLE = 1 << 19;

constexpr uint32_t CTRL_GIO_MASTER_DISABLE = 1 << 2;
constexpr uint32_t CTRL_ASDE = 1 << 5;    // Auto-Speed Detection Enable
constexpr uint32_t CTRL_SLU = 1 << 6;     // Set Link Up
constexpr uint32_t CTRL_LANPHYPC_OVERRIDE = 1 << 16;
constexpr uint32_t CTRL_LANPHYPC_VALUE = 1 << 17;
constexpr uint32_t CTRL_RST = 1 << 26;    // Reset
constexpr uint32_t CTRL_PHY_RST = 1u << 31;  // PHY Reset (must be cleared to run)

constexpr uint32_t CTRL_EXT_LPCD = 1 << 2;          // LANPHYPC cycle done
constexpr uint32_t CTRL_EXT_FORCE_SMBUS = 1 << 11;
constexpr uint32_t CTRL_EXT_DRV_LOAD = 1 << 28;     // tells the firmware a driver runs

constexpr uint32_t EECD_AUTO_RD = 1 << 9;
constexpr uint32_t EXTCNF_CTRL_SWFLAG = 1 << 5;
constexpr uint32_t FWSM_RSPCIPHY = 1 << 6;          // clear: ME blocks PHY resets
constexpr uint32_t FWSM_ULP_CFG_DONE = 1 << 10;
constexpr uint32_t FWSM_FW_VALID = 1 << 15;
constexpr uint32_t H2ME_ULP = 1 << 11;
constexpr uint32_t H2ME_ENFORCE_SETTINGS = 1 << 12;
constexpr uint32_t FEXTNVM3_PHY_CFG_COUNTER_MASK = 0x0C000000;
constexpr uint32_t FEXTNVM3_PHY_CFG_COUNTER_50MS = 0x08000000;

constexpr uint32_t MDIC_REG_SHIFT = 16;
constexpr uint32_t MDIC_PHY_SHIFT = 21;
constexpr uint32_t MDIC_OP_WRITE = 1 << 26;
constexpr uint32_t MDIC_OP_READ = 2 << 26;
constexpr uint32_t MDIC_READY = 1 << 28;
constexpr uint32_t MDIC_ERROR = 1 << 30;

constexpr uint8_t MII_BMCR = 0;
constexpr uint8_t MII_PHYSID1 = 2;
constexpr uint16_t BMCR_ANRESTART = 0x0200;
constexpr uint16_t BMCR_ISOLATE = 0x0400;
constexpr uint16_t BMCR_PDOWN = 0x0800;
constexpr uint16_t BMCR_ANENABLE = 0x1000;

constexpr uint32_t RCTL_EN = 1 << 1;      // Receiver Enable
constexpr uint32_t RCTL_BAM = 1 << 15;    // Broadcast Accept Mode
constexpr uint32_t RCTL_SECRC = 1 << 26;  // Strip Ethernet CRC (BSIZE 00: 2048-byte buffers)

constexpr uint32_t TCTL_EN = 1 << 1;      // Transmit Enable
constexpr uint32_t TCTL_PSP = 1 << 3;     // Pad Short Packets
constexpr uint32_t TCTL_CT_SHIFT = 4;     // Collision Threshold
constexpr uint32_t TCTL_COLD_SHIFT = 12;  // Collision Distance

constexpr uint8_t RX_STATUS_DD = 1 << 0;  // Descriptor Done
constexpr uint8_t RX_STATUS_EOP = 1 << 1; // End Of Packet
constexpr uint8_t TX_CMD_EOP = 1 << 0;    // End Of Packet
constexpr uint8_t TX_CMD_IFCS = 1 << 1;   // Insert FCS
constexpr uint8_t TX_CMD_RS = 1 << 3;     // Report Status
constexpr uint8_t TX_STATUS_DD = 1 << 0;  // Descriptor Done

// Advanced transmit data descriptor (igb).
constexpr uint32_t ADV_TX_DTYP_DATA = 0x3 << 20;
constexpr uint32_t ADV_TX_CMD_EOP = 1 << 24;
constexpr uint32_t ADV_TX_CMD_IFCS = 1 << 25;
constexpr uint32_t ADV_TX_CMD_RS = 1 << 27;
constexpr uint32_t ADV_TX_CMD_DEXT = 1 << 29;
constexpr uint32_t ADV_TX_PAYLEN_SHIFT = 14;
constexpr uint32_t ADV_TX_STATUS_DD = 1 << 0;
// Advanced receive descriptor write-back (igb).
constexpr uint32_t ADV_RX_STATUS_DD = 1 << 0;
constexpr uint32_t ADV_RX_STATUS_EOP = 1 << 1;

constexpr uint32_t TX_DD_SPIN_LIMIT = 1000000;

// Interrupt causes: link status change, RX descriptor minimum threshold,
// receiver overrun, receive timer.
constexpr uint32_t INT_LSC = 1 << 2;
constexpr uint32_t INT_RXDMT0 = 1 << 4;
constexpr uint32_t INT_RXO = 1 << 6;
constexpr uint32_t INT_RXT0 = 1 << 7;
constexpr uint32_t INT_RECEIVE = INT_LSC | INT_RXDMT0 | INT_RXO | INT_RXT0;

struct RxDesc {   // legacy
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct TxDesc {   // legacy
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
union AdvRxDesc {
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
struct AdvTxDesc {
    uint64_t addr;
    uint32_t cmdTypeLen;
    uint32_t olinfoStatus;
} __attribute__((packed));

static_assert(sizeof(RxDesc) == 16 && sizeof(TxDesc) == 16 && sizeof(AdvRxDesc) == 16 && sizeof(AdvTxDesc) == 16);

enum class Family : uint8_t {
    Classic,   // 8254x
    Pcie,      // 82571-82574, 82583
    Pch,       // ICH/PCH LOM: 82567, 82577-82579, I217, I218, I219
    Igb,       // 82575, 82576, 82580, I350, I354, I210, I211
    Igc,       // I225, I226 (2.5G; Linux's igc): igb's queues and descriptors
};

struct DeviceEntry {
    uint16_t id;
    Family family;
    const char* name;
};

constexpr DeviceEntry kDevices[] = {
    // 8254x. 0x100E is QEMU's -device e1000 (and VirtualBox's default).
    {0x1004, Family::Classic, "e1000"}, {0x1008, Family::Classic, "e1000"},
    {0x100C, Family::Classic, "e1000"}, {0x100D, Family::Classic, "e1000"},
    {0x100E, Family::Classic, "e1000"}, {0x100F, Family::Classic, "e1000"},
    {0x1010, Family::Classic, "e1000"}, {0x1013, Family::Classic, "e1000"},
    {0x1014, Family::Classic, "e1000"}, {0x1015, Family::Classic, "e1000"},
    {0x1016, Family::Classic, "e1000"}, {0x1017, Family::Classic, "e1000"},
    {0x1018, Family::Classic, "e1000"}, {0x1019, Family::Classic, "e1000"},
    {0x101A, Family::Classic, "e1000"}, {0x101D, Family::Classic, "e1000"},
    {0x101E, Family::Classic, "e1000"}, {0x1026, Family::Classic, "e1000"},
    {0x1075, Family::Classic, "e1000"}, {0x1076, Family::Classic, "e1000"},
    {0x1077, Family::Classic, "e1000"}, {0x1078, Family::Classic, "e1000"},
    {0x1079, Family::Classic, "e1000"}, {0x107C, Family::Classic, "e1000"},
    {0x1099, Family::Classic, "e1000"}, {0x10B5, Family::Classic, "e1000"},
    // 82571/82572/82573/82574/82583. 0x10D3 is QEMU's -device e1000e.
    {0x105E, Family::Pcie, "82571"}, {0x10A4, Family::Pcie, "82571"},
    {0x10BC, Family::Pcie, "82571"}, {0x10D5, Family::Pcie, "82571"},
    {0x107D, Family::Pcie, "82572"}, {0x10B9, Family::Pcie, "82572"},
    {0x108B, Family::Pcie, "82573"}, {0x108C, Family::Pcie, "82573"},
    {0x109A, Family::Pcie, "82573"},
    {0x10D3, Family::Pcie, "82574l"}, {0x10F6, Family::Pcie, "82574l"},
    {0x150C, Family::Pcie, "82583v"},
    // ICH8/9/10 (82566, 82567).
    {0x1049, Family::Pch, "82566"}, {0x104A, Family::Pch, "82566"},
    {0x104B, Family::Pch, "82566"}, {0x104D, Family::Pch, "82566"},
    {0x10BD, Family::Pch, "82566"}, {0x294C, Family::Pch, "82566"},
    {0x10CB, Family::Pch, "82567"}, {0x10F5, Family::Pch, "82567"},
    {0x10BF, Family::Pch, "82567"}, {0x10E5, Family::Pch, "82567"},
    {0x10CC, Family::Pch, "82567"}, {0x10CD, Family::Pch, "82567"},
    {0x10CE, Family::Pch, "82567"}, {0x10DE, Family::Pch, "82567"},
    {0x10DF, Family::Pch, "82567"}, {0x1501, Family::Pch, "82567"},
    // PCH (82577/82578), PCH2 (82579).
    {0x10EA, Family::Pch, "82577"}, {0x10EB, Family::Pch, "82577"},
    {0x10EF, Family::Pch, "82578"}, {0x10F0, Family::Pch, "82578"},
    {0x1502, Family::Pch, "82579lm"}, {0x1503, Family::Pch, "82579v"},
    // I217 and later: ULP mode, LANPHYPC (pchLpt).
    {0x153A, Family::Pch, "i217-lm"}, {0x153B, Family::Pch, "i217-v"},
    {0x155A, Family::Pch, "i218-lm"}, {0x1559, Family::Pch, "i218-v"},
    {0x15A0, Family::Pch, "i218-lm"}, {0x15A1, Family::Pch, "i218-v"},
    {0x15A2, Family::Pch, "i218-lm"}, {0x15A3, Family::Pch, "i218-v"},
    {0x156F, Family::Pch, "i219-lm"}, {0x1570, Family::Pch, "i219-v"},
    {0x15B7, Family::Pch, "i219-lm"}, {0x15B8, Family::Pch, "i219-v"},
    {0x15B9, Family::Pch, "i219-lm"}, {0x15D7, Family::Pch, "i219-lm"},
    {0x15D8, Family::Pch, "i219-v"}, {0x15E3, Family::Pch, "i219-lm"},
    {0x15D6, Family::Pch, "i219-v"}, {0x15BD, Family::Pch, "i219-lm"},
    {0x15BE, Family::Pch, "i219-v"}, {0x15BB, Family::Pch, "i219-lm"},
    {0x15BC, Family::Pch, "i219-v"}, {0x15DF, Family::Pch, "i219-lm"},
    {0x15E0, Family::Pch, "i219-v"}, {0x15E1, Family::Pch, "i219-lm"},
    {0x15E2, Family::Pch, "i219-v"}, {0x0D4E, Family::Pch, "i219-lm"},
    {0x0D4F, Family::Pch, "i219-v"}, {0x0D4C, Family::Pch, "i219-lm"},
    {0x0D4D, Family::Pch, "i219-v"}, {0x0D53, Family::Pch, "i219-lm"},
    {0x0D55, Family::Pch, "i219-v"}, {0x15FB, Family::Pch, "i219-lm"},
    {0x15FC, Family::Pch, "i219-v"}, {0x15F9, Family::Pch, "i219-lm"},
    {0x15FA, Family::Pch, "i219-v"}, {0x15F4, Family::Pch, "i219-lm"},
    {0x15F5, Family::Pch, "i219-v"}, {0x1A1E, Family::Pch, "i219-lm"},
    {0x1A1F, Family::Pch, "i219-v"}, {0x1A1C, Family::Pch, "i219-lm"},
    {0x1A1D, Family::Pch, "i219-v"}, {0x0DC5, Family::Pch, "i219-lm"},
    {0x0DC6, Family::Pch, "i219-v"}, {0x0DC7, Family::Pch, "i219-lm"},
    {0x0DC8, Family::Pch, "i219-v"}, {0x550A, Family::Pch, "i219-lm"},
    {0x550B, Family::Pch, "i219-v"}, {0x550C, Family::Pch, "i219-lm"},
    {0x550D, Family::Pch, "i219-v"}, {0x550E, Family::Pch, "i219-lm"},
    {0x550F, Family::Pch, "i219-v"}, {0x5510, Family::Pch, "i219-lm"},
    {0x5511, Family::Pch, "i219-v"}, {0x57A0, Family::Pch, "i219-lm"},
    {0x57A1, Family::Pch, "i219-v"},
    // igb. 0x10C9 is QEMU's -device igb.
    {0x10A7, Family::Igb, "82575"}, {0x10A9, Family::Igb, "82575"},
    {0x10D6, Family::Igb, "82575"},
    {0x10C9, Family::Igb, "82576"}, {0x10E6, Family::Igb, "82576"},
    {0x10E7, Family::Igb, "82576"}, {0x10E8, Family::Igb, "82576"},
    {0x1526, Family::Igb, "82576"}, {0x150A, Family::Igb, "82576"},
    {0x1518, Family::Igb, "82576"}, {0x150D, Family::Igb, "82576"},
    {0x150E, Family::Igb, "82580"}, {0x150F, Family::Igb, "82580"},
    {0x1510, Family::Igb, "82580"}, {0x1511, Family::Igb, "82580"},
    {0x1516, Family::Igb, "82580"}, {0x1527, Family::Igb, "82580"},
    {0x1521, Family::Igb, "i350"}, {0x1522, Family::Igb, "i350"},
    {0x1523, Family::Igb, "i350"}, {0x1524, Family::Igb, "i350"},
    {0x1F40, Family::Igb, "i354"}, {0x1F41, Family::Igb, "i354"},
    {0x1F45, Family::Igb, "i354"},
    {0x1533, Family::Igb, "i210"}, {0x1534, Family::Igb, "i210"},
    {0x1535, Family::Igb, "i210"}, {0x1536, Family::Igb, "i210"},
    {0x1537, Family::Igb, "i210"}, {0x1538, Family::Igb, "i210"},
    {0x157B, Family::Igb, "i210"}, {0x157C, Family::Igb, "i210"},
    {0x1539, Family::Igb, "i211"},
    // I225/I226 (igc_hw.h): igb's register layout for everything used here.
    {0x15F2, Family::Igc, "i225-lm"}, {0x15F3, Family::Igc, "i225-v"},
    {0x15F8, Family::Igc, "i225-i"}, {0x15F7, Family::Igc, "i220-v"},
    {0x3100, Family::Igc, "i225-k"}, {0x3101, Family::Igc, "i225-k2"},
    {0x3102, Family::Igc, "i226-k"}, {0x5502, Family::Igc, "i225-lmvp"},
    {0x5503, Family::Igc, "i226-lmvp"}, {0x0D9F, Family::Igc, "i225-it"},
    {0x125B, Family::Igc, "i226-lm"}, {0x125C, Family::Igc, "i226-v"},
    {0x125D, Family::Igc, "i226-it"}, {0x125E, Family::Igc, "i221-v"},
    {0x125F, Family::Igc, "i226"}, {0x15FD, Family::Igc, "i225"},
};
constexpr size_t kDeviceCount = sizeof(kDevices) / sizeof(kDevices[0]);

const IdrvKernelApi* gApi;

void log(const char* text) {
    gApi->log(text);
}

void mem_barrier() {
    idrv_barrier();
}

void delayUs(uint64_t us) {
    gApi->delayUs(us);
}

class E1000 {
public:
    bool bind(const IdrvPciDevice* dev, const DeviceEntry& entry);

    bool advanced() const { return family == Family::Igb || family == Family::Igc; }
    bool claimInterrupt();
    void getMacAddress(uint8_t* mac) const { memcpy(mac, macAddr, 6); }
    bool sendPacket(const void* data, size_t len);
    int receivePacket(void* buffer, size_t maxLen);
    bool isLinkUp() const { return (mmioRead(REG_STATUS) & STATUS_LU) != 0; }

    bool bound;
    uint32_t irqMode;   // IDRV_IRQ_*
    const char* chipName;

private:
    void setupInterrupts();
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

    uint32_t mmioRead(uint32_t reg) const { return idrv_mmio_read32(mmio, reg); }
    void mmioWrite(uint32_t reg, uint32_t value) const { idrv_mmio_write32(mmio, reg, value); }

    IdrvPciDevice pci;
    Family family;
    bool pchLpt;      // I217 and later: ULP and LANPHYPC handling
    uint8_t phyAddress;
    uint32_t rxBase;  // queue 0 register blocks
    uint32_t txBase;
    uint64_t mmio;

    RxDesc* rxDescs;          // the CPU's view (direct map)
    uint64_t rxDescsPhys;
    uint8_t* rxBuffers;       // RX_DESCRIPTORS slots of BUFFER_SIZE
    uint64_t rxBuffersPhys;
    uint16_t rxCurrent;

    TxDesc* txDescs;
    uint64_t txDescsPhys;
    uint8_t* txBuffers;
    uint64_t txBuffersPhys;
    uint16_t txCurrent;

    uint8_t macAddr[6];
};

E1000 gNic;

bool E1000::bind(const IdrvPciDevice* dev, const DeviceEntry& entry) {
    pci = *dev;
    family = entry.family;
    chipName = entry.name;
    // The I217, I218 and I219 are the PCH parts named "i...".
    pchLpt = family == Family::Pch && chipName[0] == 'i';
    phyAddress = 1;

    // Enable MMIO and bus mastering.
    const uint32_t command = gApi->pciRead(&pci, IDRV_PCI_COMMAND, 2);
    gApi->pciWrite(&pci, IDRV_PCI_COMMAND, 2, command | IDRV_PCI_COMMAND_MEMORY | IDRV_PCI_COMMAND_BUS_MASTER);

    // BAR0 is the 128 KiB register space.
    mmio = gApi->pciMapBar(&pci, 0, 0x20000);
    if (mmio == 0) {
        log("[net] e1000: BAR0 not mapped\n");
        return false;
    }
    rxBase = advanced() ? 0xC000 : 0x2800;
    txBase = advanced() ? 0xE000 : 0x3800;

    // Mask all interrupts until the rings are set up (setupInterrupts).
    mmioWrite(REG_IMC, 0xFFFFFFFFu);
    (void)mmioRead(REG_ICR);   // clear pending causes

    if (family == Family::Pch) {
        pchPhyWorkarounds();
    }
    resetMac();
    if (family != Family::Classic) {
        mmioWrite(REG_CTRL_EXT, mmioRead(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    }

    // Bring the link up and let the PHY auto-negotiate speed.
    mmioWrite(REG_CTRL, (mmioRead(REG_CTRL) & ~CTRL_PHY_RST) | CTRL_SLU | CTRL_ASDE);
    startPhy();
    readMacAddress();
    if (!setupRx() || !setupTx()) {
        return false;
    }
    rxCurrent = 0;
    txCurrent = 0;
    setupInterrupts();
    return true;
}

void E1000::resetMac() {
    if (family == Family::Pch) {
        // Reading any register right after the reset hangs the PCH LOMs
        // ("cannot issue a flush here because it hangs the hardware").
        const bool flag = acquireSwFlag();
        mmioWrite(REG_CTRL, mmioRead(REG_CTRL) | CTRL_RST);
        delayUs(20000);
        (void)flag;   // the reset releases it
        for (uint32_t i = 0; i < 1500; ++i) {
            if ((mmioRead(REG_STATUS) & STATUS_LAN_INIT_DONE) != 0) {
                break;
            }
            delayUs(150);
        }
        mmioWrite(REG_STATUS, mmioRead(REG_STATUS) & ~(STATUS_LAN_INIT_DONE | STATUS_PHYRA));
    } else {
        if (family == Family::Igc) {
            // igc_disable_pcie_master: no DMA in flight across the reset.
            mmioWrite(REG_CTRL, mmioRead(REG_CTRL) | CTRL_GIO_MASTER_DISABLE);
            for (uint32_t i = 0; i < 800 && (mmioRead(REG_STATUS) & STATUS_GIO_MASTER_ENABLE) != 0; ++i) {
                delayUs(100);
            }
            mmioWrite(REG_IMC, 0xFFFFFFFFu);
            mmioWrite(REG_RCTL, 0);
            mmioWrite(REG_TCTL, TCTL_PSP);
            (void)mmioRead(REG_STATUS);
            delayUs(10000);
        }
        mmioWrite(REG_CTRL, mmioRead(REG_CTRL) | CTRL_RST);
        delayUs(family == Family::Classic ? 1000 : 10000);
        for (uint32_t i = 0; i < 1000; ++i) {
            if ((mmioRead(REG_CTRL) & CTRL_RST) == 0) {
                break;
            }
            delayUs(100);
        }
        if (family != Family::Classic) {
            // The NVM is read back into the registers (the MAC among them).
            for (uint32_t i = 0; i < 100; ++i) {
                if ((mmioRead(REG_EECD) & EECD_AUTO_RD) != 0) {
                    break;
                }
                delayUs(1000);
            }
        }
    }
    mmioWrite(REG_IMC, 0xFFFFFFFFu);
    (void)mmioRead(REG_ICR);
}

// The PCH LOMs talk to their PHY over an interconnect that the firmware may
// have left in SMBus mode, and the I217+ PHY may sit in ultra-low-power mode
// (entered at shutdown without a cable). After this the PHY must answer on
// MDIC. Linux's e1000_init_phy_workarounds_pchlan, minus the PHY reset and
// the PHY-side ULP registers.
void E1000::pchPhyWorkarounds() {
    const bool flag = acquireSwFlag();
    if (pchLpt && (mmioRead(REG_FWSM) & FWSM_FW_VALID) != 0) {
        // The ME owns ULP: ask it to undo it, up to 2.5 s.
        mmioWrite(REG_H2ME, (mmioRead(REG_H2ME) & ~H2ME_ULP) | H2ME_ENFORCE_SETTINGS);
        for (uint32_t i = 0; i < 250 && (mmioRead(REG_FWSM) & FWSM_ULP_CFG_DONE) != 0; ++i) {
            delayUs(10000);
        }
        mmioWrite(REG_H2ME, mmioRead(REG_H2ME) & ~H2ME_ENFORCE_SETTINGS);
    }
    bool accessible = phyAccessible();
    if (!accessible && pchLpt) {
        mmioWrite(REG_CTRL_EXT, mmioRead(REG_CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
        delayUs(50000);
        accessible = phyAccessible();
    }
    if (!accessible) {
        if ((mmioRead(REG_FWSM) & FWSM_RSPCIPHY) == 0) {
            log("[net] e1000: PHY unreachable and its reset is blocked by the ME\n");
        } else {
            togglePhyPowerPin();
        }
    }
    if (pchLpt) {
        mmioWrite(REG_CTRL_EXT, mmioRead(REG_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    }
    if (!accessible) {
        accessible = phyAccessible();
    }
    if (flag) {
        releaseSwFlag();
    }
    log(accessible ? "[net] e1000: PHY answers\n" : "[net] e1000: PHY does not answer\n");
}

// Cycle the LANPHYPC pin: the PHY powers back up and the interconnect comes up
// in PCIe mode.
void E1000::togglePhyPowerPin() {
    mmioWrite(REG_FEXTNVM3, (mmioRead(REG_FEXTNVM3) & ~FEXTNVM3_PHY_CFG_COUNTER_MASK) |
                                FEXTNVM3_PHY_CFG_COUNTER_50MS);
    uint32_t ctrl = mmioRead(REG_CTRL);
    ctrl = (ctrl | CTRL_LANPHYPC_OVERRIDE) & ~CTRL_LANPHYPC_VALUE;
    mmioWrite(REG_CTRL, ctrl);
    (void)mmioRead(REG_STATUS);
    delayUs(20);
    mmioWrite(REG_CTRL, ctrl & ~CTRL_LANPHYPC_OVERRIDE);
    (void)mmioRead(REG_STATUS);
    if (!pchLpt) {
        delayUs(50000);
        return;
    }
    for (uint32_t i = 0; i < 20 && (mmioRead(REG_CTRL_EXT) & CTRL_EXT_LPCD) == 0; ++i) {
        delayUs(5000);
    }
    delayUs(30000);
}

// The PHY ID answers on MDIC (at address 1, or 2 on some ICH10 parts).
bool E1000::phyAccessible() {
    for (uint8_t address = 1; address <= 2; ++address) {
        phyAddress = address;
        uint16_t id = 0;
        if (mdicRead(MII_PHYSID1, id) && id != 0 && id != 0xFFFF) {
            return true;
        }
    }
    phyAddress = 1;
    return false;
}

bool E1000::acquireSwFlag() {
    for (uint32_t i = 0; i < 100 && (mmioRead(REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) != 0; ++i) {
        delayUs(1000);
    }
    mmioWrite(REG_EXTCNF_CTRL, mmioRead(REG_EXTCNF_CTRL) | EXTCNF_CTRL_SWFLAG);
    for (uint32_t i = 0; i < 1000; ++i) {
        if ((mmioRead(REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) != 0) {
            return true;
        }
        delayUs(1000);
    }
    log("[net] e1000: firmware holds the PHY semaphore\n");
    return false;
}

void E1000::releaseSwFlag() {
    mmioWrite(REG_EXTCNF_CTRL, mmioRead(REG_EXTCNF_CTRL) & ~EXTCNF_CTRL_SWFLAG);
}

// A PHY the firmware powered down or isolated, or without a link after the
// reset (the igb's, QEMU's too, waits for this), gets autonegotiation
// (re)started; one with a link is left alone, since a restart costs seconds.
void E1000::startPhy() {
    if (family == Family::Classic) {
        return;   // the EEPROM sets it up; QEMU's has a link at once
    }
    const bool flag = family == Family::Pch && acquireSwFlag();
    const bool noLink = (mmioRead(REG_STATUS) & STATUS_LU) == 0;
    uint16_t bmcr = 0;
    if (mdicRead(MII_BMCR, bmcr) &&
        ((bmcr & (BMCR_PDOWN | BMCR_ISOLATE)) != 0 || (bmcr & BMCR_ANENABLE) == 0 || noLink)) {
        mdicWrite(MII_BMCR, static_cast<uint16_t>((bmcr & ~(BMCR_PDOWN | BMCR_ISOLATE)) |
                                                  BMCR_ANENABLE | BMCR_ANRESTART));
        log("[net] e1000: PHY autonegotiating\n");
    }
    if (flag) {
        releaseSwFlag();
    }
}

bool E1000::mdicRead(uint8_t reg, uint16_t& value) {
    mmioWrite(REG_MDIC, (static_cast<uint32_t>(reg) << MDIC_REG_SHIFT) |
                            (static_cast<uint32_t>(phyAddress) << MDIC_PHY_SHIFT) | MDIC_OP_READ);
    for (uint32_t i = 0; i < 1920; ++i) {
        delayUs(50);
        const uint32_t mdic = mmioRead(REG_MDIC);
        if ((mdic & MDIC_READY) != 0) {
            value = static_cast<uint16_t>(mdic);
            return (mdic & MDIC_ERROR) == 0;
        }
    }
    return false;
}

bool E1000::mdicWrite(uint8_t reg, uint16_t value) {
    mmioWrite(REG_MDIC, value | (static_cast<uint32_t>(reg) << MDIC_REG_SHIFT) |
                            (static_cast<uint32_t>(phyAddress) << MDIC_PHY_SHIFT) | MDIC_OP_WRITE);
    for (uint32_t i = 0; i < 1920; ++i) {
        delayUs(50);
        const uint32_t mdic = mmioRead(REG_MDIC);
        if ((mdic & MDIC_READY) != 0) {
            return (mdic & MDIC_ERROR) == 0;
        }
    }
    return false;
}

int irqClaim(void* context);
uint32_t irqHandle(void*) { return IDRV_IRQ_NET_RECEIVE; }
const IdrvIrqOps kIrqOps = {irqClaim, irqHandle};

void E1000::setupInterrupts() {
    irqMode = gApi->irqRegister(&pci, IDRV_IRQ_ALLOW_MSI | IDRV_IRQ_ALLOW_LEGACY, &kIrqOps, this);
    if (irqMode != IDRV_IRQ_NONE) {
        if (!advanced()) {
            mmioWrite(REG_RDTR, 0);   // interrupt as soon as a frame lands
        }
        (void)mmioRead(REG_ICR);
        mmioWrite(REG_IMS, INT_RECEIVE);
    }
    log("[net] e1000: ");
    log(chipName);
    log(irqMode == IDRV_IRQ_MSI ? ", MSI receive interrupt\n"
        : irqMode == IDRV_IRQ_LEGACY ? ", INTx receive interrupt\n"
        : ", no interrupt, polling\n");
}

bool E1000::claimInterrupt() {
    // ICR is read-to-clear: it both says whether we raised a (possibly
    // shared) INTx and deasserts it; under MSI it acknowledges the causes.
    return mmio && mmioRead(REG_ICR) != 0;
}

void E1000::readMacAddress() {
    // The reset reloads RAL0/RAH0 with the MAC from the NVM.
    const uint32_t low = mmioRead(REG_RAL0);
    const uint32_t high = mmioRead(REG_RAH0);
    macAddr[0] = static_cast<uint8_t>(low & 0xFF);
    macAddr[1] = static_cast<uint8_t>((low >> 8) & 0xFF);
    macAddr[2] = static_cast<uint8_t>((low >> 16) & 0xFF);
    macAddr[3] = static_cast<uint8_t>((low >> 24) & 0xFF);
    macAddr[4] = static_cast<uint8_t>(high & 0xFF);
    macAddr[5] = static_cast<uint8_t>((high >> 8) & 0xFF);
}

// igb: a queue only runs once enabled in its RXDCTL/TXDCTL.
bool E1000::enableQueue(uint32_t reg) {
    mmioWrite(reg, mmioRead(reg) | QUEUE_ENABLE);
    for (uint32_t i = 0; i < 100; ++i) {
        if ((mmioRead(reg) & QUEUE_ENABLE) != 0) {
            return true;
        }
        delayUs(100);
    }
    log("[net] e1000: queue did not enable\n");
    return false;
}

bool E1000::setupRx() {
    void* virt = nullptr;
    const uint64_t ringBytes = RX_DESCRIPTORS * sizeof(RxDesc);
    rxDescsPhys = gApi->dmaAlloc((ringBytes + 4095) / 4096, 0, &virt);
    rxDescs = static_cast<RxDesc*>(virt);
    rxBuffersPhys = gApi->dmaAlloc(RX_DESCRIPTORS * BUFFER_SIZE / 4096, 0, &virt);
    rxBuffers = static_cast<uint8_t*>(virt);
    if (rxDescsPhys == 0 || rxBuffersPhys == 0) {
        log("[net] e1000: no memory for the receive ring\n");
        return false;
    }
    for (size_t i = 0; i < RX_DESCRIPTORS; ++i) {
        rxDescs[i].addr = rxBuffersPhys + i * BUFFER_SIZE;
        rxDescs[i].status = 0;   // igb: the header address, 0
    }

    if (advanced()) {
        mmioWrite(rxBase + Q_DCTL, 0);
    }
    mmioWrite(rxBase + Q_BAL, static_cast<uint32_t>(rxDescsPhys & 0xFFFFFFFFu));
    mmioWrite(rxBase + Q_BAH, static_cast<uint32_t>(rxDescsPhys >> 32));
    mmioWrite(rxBase + Q_LEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(rxBase + Q_HEAD, 0);
    if (advanced()) {
        mmioWrite(rxBase + Q_TAIL, 0);
        mmioWrite(rxBase + Q_SRRCTL, SRRCTL_BSIZE_2K | SRRCTL_DESCTYPE_ADV_ONEBUF | SRRCTL_DROP_EN);
        if (!enableQueue(rxBase + Q_DCTL)) {
            return false;
        }
    }
    mmioWrite(rxBase + Q_TAIL, RX_DESCRIPTORS - 1);

    // Enable receiver: broadcast accept, strip CRC, 2 KiB buffers.
    mmioWrite(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    return true;
}

bool E1000::setupTx() {
    void* virt = nullptr;
    const uint64_t ringBytes = TX_DESCRIPTORS * sizeof(TxDesc);
    txDescsPhys = gApi->dmaAlloc((ringBytes + 4095) / 4096, 0, &virt);
    txDescs = static_cast<TxDesc*>(virt);
    txBuffersPhys = gApi->dmaAlloc(TX_DESCRIPTORS * BUFFER_SIZE / 4096, 0, &virt);
    txBuffers = static_cast<uint8_t*>(virt);
    if (txDescsPhys == 0 || txBuffersPhys == 0) {
        log("[net] e1000: no memory for the transmit ring\n");
        return false;
    }
    for (size_t i = 0; i < TX_DESCRIPTORS; ++i) {
        txDescs[i].addr = txBuffersPhys + i * BUFFER_SIZE;
        // Mark all free initially (DD; bit 0 of the advanced write-back too).
        if (advanced()) {
            reinterpret_cast<AdvTxDesc*>(&txDescs[i])->olinfoStatus = ADV_TX_STATUS_DD;
        } else {
            txDescs[i].status = TX_STATUS_DD;
        }
    }

    if (advanced()) {
        mmioWrite(txBase + Q_DCTL, 0);
    }
    mmioWrite(txBase + Q_BAL, static_cast<uint32_t>(txDescsPhys & 0xFFFFFFFFu));
    mmioWrite(txBase + Q_BAH, static_cast<uint32_t>(txDescsPhys >> 32));
    mmioWrite(txBase + Q_LEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(txBase + Q_HEAD, 0);
    mmioWrite(txBase + Q_TAIL, 0);
    if (advanced() && !enableQueue(txBase + Q_DCTL)) {
        return false;
    }

    // Standard IPG for copper (10 / 8 / 6).
    mmioWrite(REG_TIPG, 10u | (8u << 10) | (6u << 20));
    // Enable transmitter: pad short packets, collision threshold 15,
    // collision distance 64 (full-duplex).
    mmioWrite(REG_TCTL, TCTL_EN | TCTL_PSP | (15u << TCTL_CT_SHIFT) | (64u << TCTL_COLD_SHIFT));
    return true;
}

bool E1000::sendPacket(const void* data, size_t len) {
    if (!data || len == 0 || len > MTU || len > BUFFER_SIZE) {
        return false;
    }
    const uint16_t desc = txCurrent;
    auto* adv = reinterpret_cast<volatile AdvTxDesc*>(&txDescs[desc]);
    auto* legacy = reinterpret_cast<volatile TxDesc*>(&txDescs[desc]);
    const bool igb = advanced();

    // Wait for the descriptor to be free (DD set) before reusing it.
    for (uint32_t spin = 0; spin < TX_DD_SPIN_LIMIT; ++spin) {
        const bool done = igb ? (adv->olinfoStatus & ADV_TX_STATUS_DD) != 0 : (legacy->status & TX_STATUS_DD) != 0;
        if (done) {
            break;
        }
        if (spin + 1 == TX_DD_SPIN_LIMIT) {
            return false;
        }
        mem_barrier();
    }

    memcpy(txBuffers + desc * BUFFER_SIZE, data, len);
    const uint64_t phys = txBuffersPhys + desc * BUFFER_SIZE;
    if (igb) {
        adv->addr = phys;
        adv->cmdTypeLen = static_cast<uint32_t>(len) | ADV_TX_DTYP_DATA | ADV_TX_CMD_DEXT | ADV_TX_CMD_EOP |
                          ADV_TX_CMD_IFCS | ADV_TX_CMD_RS;
        adv->olinfoStatus = static_cast<uint32_t>(len) << ADV_TX_PAYLEN_SHIFT;
    } else {
        legacy->addr = phys;
        legacy->length = static_cast<uint16_t>(len);
        legacy->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
        legacy->status = 0;
    }
    mem_barrier();

    // Advance the tail to hand the descriptor to the NIC.
    txCurrent = static_cast<uint16_t>((desc + 1) % TX_DESCRIPTORS);
    mmioWrite(txBase + Q_TAIL, txCurrent);
    return true;
}

int E1000::receivePacket(void* buffer, size_t maxLen) {
    if (advanced()) {
        return receiveAdvanced(buffer, maxLen);
    }
    auto* desc = reinterpret_cast<volatile RxDesc*>(&rxDescs[rxCurrent]);
    if ((desc->status & RX_STATUS_DD) == 0) {
        return -1;   // no packet ready
    }
    mem_barrier();
    int result = -1;
    // Only deliver complete (EOP) packets; the stack expects whole frames.
    if ((desc->status & RX_STATUS_EOP) != 0) {
        const size_t length = desc->length;
        const size_t copied = length < maxLen ? length : maxLen;
        if (copied != 0) {
            memcpy(buffer, rxBuffers + rxCurrent * BUFFER_SIZE, copied);
        }
        result = static_cast<int>(copied);
    }
    // Hand the descriptor back to the NIC and advance the tail.
    desc->status = 0;
    mem_barrier();
    mmioWrite(rxBase + Q_TAIL, rxCurrent);
    rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % RX_DESCRIPTORS);
    return result;
}

// igb: advanced descriptors (QEMU's igb writes back nothing else). The
// write-back overwrites the buffer address, so each one is handed back whole.
int E1000::receiveAdvanced(void* buffer, size_t maxLen) {
    auto* desc = reinterpret_cast<volatile AdvRxDesc*>(&rxDescs[rxCurrent]);
    const uint32_t status = desc->statusError;
    if ((status & ADV_RX_STATUS_DD) == 0) {
        return -1;   // no packet ready
    }
    mem_barrier();
    int result = -1;
    if ((status & ADV_RX_STATUS_EOP) != 0) {
        const size_t length = desc->length;
        const size_t copied = length < maxLen ? length : maxLen;
        if (copied != 0) {
            memcpy(buffer, rxBuffers + rxCurrent * BUFFER_SIZE, copied);
        }
        result = static_cast<int>(copied);
    }
    desc->packetAddr = rxBuffersPhys + rxCurrent * BUFFER_SIZE;
    desc->headerAddr = 0;
    mem_barrier();
    mmioWrite(rxBase + Q_TAIL, rxCurrent);
    rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % RX_DESCRIPTORS);
    return result;
}

// ---- The kernel's callbacks ------------------------------------------------

int irqClaim(void* context) {
    return static_cast<E1000*>(context)->claimInterrupt();
}

void macAddress(void* context, uint8_t mac[6]) {
    static_cast<E1000*>(context)->getMacAddress(mac);
}

int send(void* context, const void* frame, size_t length) {
    return static_cast<E1000*>(context)->sendPacket(frame, length);
}

int receive(void* context, void* buffer, size_t maxLength) {
    return static_cast<E1000*>(context)->receivePacket(buffer, maxLength);
}

int linkUp(void* context) {
    return static_cast<E1000*>(context)->isLinkUp();
}

int interruptDriven(void* context) {
    return static_cast<E1000*>(context)->irqMode != IDRV_IRQ_NONE;
}

// netcfg shows the chip ("82574l", "i219-v"): the name is filled in at probe.
IdrvNetOps gNetOps = {
    "e1000", 50, 0, macAddress, send, receive, linkUp, interruptDriven,
};

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    gApi = api;
    if (gNic.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    const DeviceEntry* entry = nullptr;
    for (size_t i = 0; i < kDeviceCount; ++i) {
        if (kDevices[i].id == dev->deviceId) {
            entry = &kDevices[i];
            break;
        }
    }
    if (!entry || !gNic.bind(dev, *entry)) {
        return IDRV_ENODEV;
    }
    gNic.bound = true;
    gNetOps.name = gNic.chipName;
    return api->netRegister(&gNetOps, &gNic);
}

// Every device of the table (the probe picks the family by id).
struct MatchTable {
    IdrvPciMatch entries[kDeviceCount];
};

constexpr MatchTable makeMatches() {
    MatchTable t = {};
    for (size_t i = 0; i < kDeviceCount; ++i) {
        t.entries[i] = {INTEL_VENDOR, kDevices[i].id, 0, 0};
    }
    return t;
}

constexpr MatchTable kMatches = makeMatches();

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "e1000",
    "1.0",
    "Intel gigabit/2.5G Ethernet: 8254x, 8257x, PCH LOMs (I217-I219), igb, I225/I226",
    "InstantOS",
    kMatches.entries,
    kDeviceCount,
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
