// Realtek RTL8169 / RTL8111 / RTL8168 / RTL8101 ("r8169") Ethernet and the
// 2.5G RTL8125A/B, as a loadable module (rtl8169.idrv). It was the kernel's
// src/drivers/net/rtl8169.cpp; the hardware side is unchanged (this file's
// functions are its, ported by tools that swapped the kernel calls for the
// driver API). Register names and bits follow Linux's
// drivers/net/ethernet/realtek/r8169.
//
// This is the NIC on most PC boards (10ec:8168, the RTL8111/8168 gigabit
// family). The chips share the C+ descriptor model: rings of 16-byte
// descriptors whose OWN bit hands them between driver and NIC, EOR marking the
// ring's end. The family is told apart by the XID in TxConfig, as in Linux's
// r8169 (the RTL_GIGA_MAC_VER_* numbers are Linux's); 8168G and later (mac
// version 40+) reach the PHY through the GPHY_OCP window and gate their
// receiver (MISC.RXDV_GATED_EN), which the driver must open. MSI, else INTx.
// The 2.5G RTL8125A/B (10ec:8125, 10ec:3000; Linux's mac versions 61 and 63)
// shares the descriptors, PHY OCP and MAC OCP, but has 32-bit interrupt
// registers at 0x38/0x3c, its TX doorbell in TxPoll_8125 (0x90) and its own
// bring-up (rtl_hw_init_8125, rtl_hw_start_8125*); it links at up to 1 Gb/s
// here, the 2.5G advertisement (PHY MMD registers) is not programmed yet.

#include "../sdk/idrv_sdk.h"

namespace {

constexpr size_t RX_DESCRIPTORS = 256;
constexpr size_t TX_DESCRIPTORS = 256;
constexpr size_t BUFFER_SIZE = 2048;   // frame + 4 CRC bytes fit
constexpr size_t MTU = 1514;

struct RTL8169Desc {
    uint32_t opts1;   // OWN, EOR, FS, LS, length / buffer size
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed));

const IdrvKernelApi* gApi;

void log(const char* text) {
    gApi->log(text);
}

constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

constexpr uint16_t REG_MAC0 = 0x00;
constexpr uint16_t REG_MAC4 = 0x04;
constexpr uint16_t REG_MAR0 = 0x08;
constexpr uint16_t REG_TX_DESC_LOW = 0x20;
constexpr uint16_t REG_TX_DESC_HIGH = 0x24;
constexpr uint16_t REG_CHIP_CMD = 0x37;
constexpr uint16_t REG_TX_POLL = 0x38;
constexpr uint16_t REG_INTR_MASK = 0x3C;
constexpr uint16_t REG_INTR_STATUS = 0x3E;
constexpr uint16_t REG_TX_CONFIG = 0x40;
constexpr uint16_t REG_RX_CONFIG = 0x44;
constexpr uint16_t REG_CFG9346 = 0x50;
constexpr uint16_t REG_CONFIG2 = 0x53;
constexpr uint16_t REG_CONFIG5 = 0x56;
constexpr uint16_t REG_PHYAR = 0x60;
constexpr uint16_t REG_PHY_STATUS = 0x6C;
constexpr uint16_t REG_PMCH = 0x6F;
constexpr uint16_t REG_ERIDR = 0x70;
constexpr uint16_t REG_ERIAR = 0x74;
constexpr uint16_t REG_OCPDR = 0xB0;
constexpr uint16_t REG_GPHY_OCP = 0xB8;
constexpr uint16_t REG_MCU = 0xD3;
constexpr uint16_t REG_RX_MAX_SIZE = 0xDA;
constexpr uint16_t REG_CPLUS_CMD = 0xE0;
constexpr uint16_t REG_INTR_MITIGATE = 0xE2;
constexpr uint16_t REG_RX_DESC_LOW = 0xE4;
constexpr uint16_t REG_RX_DESC_HIGH = 0xE8;
constexpr uint16_t REG_MAX_TX_PACKET = 0xEC;   // EarlyTxThres on the 8169
constexpr uint16_t REG_MISC = 0xF0;
constexpr uint16_t REG_CONFIG1 = 0x52;
constexpr uint16_t REG_CONFIG3 = 0x54;
constexpr uint16_t REG_EPHYAR = 0x80;
// RTL8125
constexpr uint16_t REG_INTR_MASK_8125 = 0x38;
constexpr uint16_t REG_INTR_STATUS_8125 = 0x3C;
constexpr uint16_t REG_TX_POLL_8125 = 0x90;
constexpr uint16_t REG_MAC0_BKP = 0x19E0;
constexpr uint16_t REG_EEE_TXIDLE_TIMER_8125 = 0x6048;
constexpr uint32_t RX_FETCH_DFLT_8125 = 8u << 27;
constexpr uint8_t CMD_STOP_REQ = 0x80;
constexpr uint8_t CONFIG3_RDY_TO_L23 = 1 << 1;
constexpr uint32_t EPHYAR_FLAG = 1u << 31;

constexpr uint8_t CMD_RESET = 0x10;
constexpr uint8_t CMD_RX_ENABLE = 0x08;
constexpr uint8_t CMD_TX_ENABLE = 0x04;
constexpr uint8_t TX_POLL_NORMAL = 0x40;        // NPQ
constexpr uint8_t CFG9346_UNLOCK = 0xC0;
constexpr uint8_t CFG9346_LOCK = 0x00;
constexpr uint8_t CONFIG2_CLKREQ = 1 << 7;
constexpr uint8_t CONFIG5_ASPM = 1 << 0;
constexpr uint8_t PHY_STATUS_LINK = 0x02;

constexpr uint16_t INT_RX_OK = 0x0001;
constexpr uint16_t INT_RX_ERR = 0x0002;
constexpr uint16_t INT_RX_OVERFLOW = 0x0010;
constexpr uint16_t INT_LINK_CHANGE = 0x0020;
constexpr uint16_t INT_RX_FIFO_OVER = 0x0040;
constexpr uint16_t INT_SYS_ERR = 0x8000;
constexpr uint16_t INT_RECEIVE =
    INT_RX_OK | INT_RX_ERR | INT_RX_OVERFLOW | INT_LINK_CHANGE | INT_RX_FIFO_OVER | INT_SYS_ERR;

constexpr uint32_t RX_ACCEPT_MY_PHYS = 0x02;
constexpr uint32_t RX_ACCEPT_MULTICAST = 0x04;
constexpr uint32_t RX_ACCEPT_BROADCAST = 0x08;
constexpr uint32_t RX_ACCEPT_MASK = 0x3F;
constexpr uint32_t RX_128_INT = 1 << 15;
constexpr uint32_t RX_MULTI = 1 << 14;
constexpr uint32_t RX_FIFO_THRESH = 7 << 13;
constexpr uint32_t RX_EARLY_OFF = 1 << 11;
constexpr uint32_t RX_DMA_BURST = 7 << 8;
constexpr uint32_t TX_DMA_BURST = 7 << 8;
constexpr uint32_t TX_IFG = 3 << 24;
constexpr uint32_t TX_AUTO_FIFO = 1 << 7;
constexpr uint32_t TX_CONFIG_EMPTY = 1 << 11;

constexpr uint16_t CPLUS_PCI_MUL_RW = 1 << 3;
constexpr uint16_t CPLUS_KEEP = (1 << 13) | 0x3;   // Normal_mode | INTT

constexpr uint32_t MISC_RXDV_GATED = 1 << 19;
constexpr uint8_t MCU_NOW_IS_OOB = 1 << 7;
constexpr uint8_t MCU_TX_EMPTY = 1 << 5;
constexpr uint8_t MCU_RX_EMPTY = 1 << 4;
constexpr uint8_t MCU_LINK_LIST_READY = 1 << 1;

constexpr uint32_t OCP_FLAG = 1u << 31;
constexpr uint16_t OCP_STD_PHY_BASE = 0xA400;
constexpr uint32_t ERIAR_FLAG = 1u << 31;
constexpr uint32_t ERIAR_MASK_ALL = 0xFu << 12;

constexpr uint32_t DESC_OWN = 1u << 31;
constexpr uint32_t DESC_EOR = 1u << 30;
constexpr uint32_t DESC_FIRST = 1u << 29;
constexpr uint32_t DESC_LAST = 1u << 28;
constexpr uint32_t DESC_LENGTH = 0x3FFF;
constexpr uint32_t RX_STATUS_ERROR = 1u << 21;   // RxRES
constexpr size_t ETH_CRC = 4;
constexpr size_t ETH_MIN_FRAME = 60;

constexpr uint16_t MII_BMCR = 0;
constexpr uint16_t MII_ADVERTISE = 4;
constexpr uint16_t MII_CTRL1000 = 9;
constexpr uint16_t BMCR_ANENABLE = 0x1000;
constexpr uint16_t BMCR_ANRESTART = 0x0200;
constexpr uint16_t ADVERTISE_ALL = 0x01E1 | 0x0C00;   // 10/100 half+full, pause
constexpr uint16_t ADVERTISE_1000 = 0x0300;

constexpr uint32_t TX_OWN_SPIN_LIMIT = 1000000;

// The chips' XID (TxConfig bits 31:20) -> Linux's mac version, first match.
struct ChipEntry {
    uint16_t mask;
    uint16_t value;
    uint8_t version;
    const char* name;
};
constexpr ChipEntry kChips[] = {
    {0x7cf, 0x641, 63, "rtl8125b"}, {0x7cf, 0x609, 61, "rtl8125a"},
    {0x7cf, 0x54b, 53, "rtl8117"}, {0x7cf, 0x54a, 52, "rtl8117"},
    {0x7cf, 0x502, 51, "rtl8168ep"}, {0x7cf, 0x501, 50, "rtl8168ep"}, {0x7cf, 0x500, 49, "rtl8168ep"},
    {0x7cf, 0x541, 46, "rtl8168h"}, {0x7cf, 0x540, 45, "rtl8168h"},
    {0x7cf, 0x5c8, 44, "rtl8411b"}, {0x7cf, 0x509, 42, "rtl8168gu"},
    {0x7cf, 0x4c1, 41, "rtl8168g"}, {0x7cf, 0x4c0, 40, "rtl8168g"},
    {0x7c8, 0x488, 38, "rtl8411"}, {0x7cf, 0x481, 36, "rtl8168f"}, {0x7cf, 0x480, 35, "rtl8168f"},
    {0x7c8, 0x2c8, 34, "rtl8168evl"}, {0x7cf, 0x2c1, 32, "rtl8168e"}, {0x7c8, 0x2c0, 33, "rtl8168e"},
    {0x7cf, 0x281, 25, "rtl8168d"}, {0x7c8, 0x280, 26, "rtl8168d"},
    {0x7cf, 0x28a, 28, "rtl8168dp"}, {0x7cf, 0x28b, 31, "rtl8168dp"},
    {0x7cf, 0x3c9, 23, "rtl8168cp"}, {0x7cf, 0x3c8, 18, "rtl8168cp"}, {0x7c8, 0x3c8, 24, "rtl8168cp"},
    {0x7cf, 0x3c0, 19, "rtl8168c"}, {0x7cf, 0x3c2, 20, "rtl8168c"}, {0x7cf, 0x3c3, 21, "rtl8168c"},
    {0x7c8, 0x3c0, 22, "rtl8168c"},
    {0x7c8, 0x380, 17, "rtl8168b"}, {0x7c8, 0x300, 11, "rtl8168b"},
    {0x7c8, 0x448, 39, "rtl8106e"}, {0x7c8, 0x440, 37, "rtl8402"},
    {0x7cf, 0x409, 29, "rtl8105e"}, {0x7c8, 0x408, 30, "rtl8105e"},
    {0x7cf, 0x349, 8, "rtl8102e"}, {0x7cf, 0x249, 8, "rtl8102e"},
    {0x7cf, 0x348, 7, "rtl8102e"}, {0x7cf, 0x248, 7, "rtl8102e"},
    {0x7cf, 0x340, 13, "rtl8101e"}, {0x7cf, 0x240, 14, "rtl8401"},
    {0x7cf, 0x343, 10, "rtl8101e"}, {0x7cf, 0x342, 16, "rtl8101e"},
    {0x7c8, 0x348, 9, "rtl8102e"}, {0x7c8, 0x248, 9, "rtl8102e"}, {0x7c8, 0x340, 16, "rtl8101e"},
    {0xfc8, 0x980, 6, "rtl8169sc"}, {0xfc8, 0x180, 5, "rtl8169sc"}, {0xfc8, 0x100, 4, "rtl8169sb"},
    {0xfc8, 0x040, 3, "rtl8110s"}, {0xfc8, 0x008, 2, "rtl8169s"}, {0xfc8, 0x000, 1, "rtl8169"},
};

struct PciId {
    uint16_t vendor;
    uint16_t device;
};
constexpr PciId kPciIds[] = {
    {0x10EC, 0x8168},   // RTL8111/8168 (B through H, EP, 8411)
    {0x10EC, 0x8161},   // RTL8168 variant
    {0x10EC, 0x8167},   // RTL8110SC/8169SC
    {0x10EC, 0x8169},   // RTL8169
    {0x10EC, 0x8136},   // RTL8101E/8102E/8105E/8106E (fast ethernet)
    {0x10EC, 0x2502},   // RTL8168 variant
    {0x10EC, 0x2600},   // RTL8168 variant
    {0x10EC, 0x8125},   // RTL8125A/B 2.5G
    {0x10EC, 0x3000},   // RTL8125 (Killer E3000)
    {0x1186, 0x4300},   // D-Link DGE-528T (8169)
    {0x1186, 0x4302},   // D-Link DGE-530T rev C
    {0x1259, 0xC107},   // Corega CG-LAPCIGT (8169)
    {0x1737, 0x1032},   // Linksys EG1032 (8169)
};

bool isFastEthernet(int version) {
    switch (version) {
    case 7: case 8: case 9: case 10: case 13: case 14: case 16: case 29: case 30: case 37: case 39:
        return true;
    default:
        return false;
    }
}

// 8168G and later: PHY through GPHY_OCP, receive gate, MCU.
bool is8168gUp(int version) {
    return version >= 40 && version <= 53;
}

// The 2.5G RTL8125A (61) and B (63): the 8168G model plus its own registers.
bool is8125(int version) {
    return version >= 61;
}

// Reach the PHY through GPHY_OCP, gate the receiver, have an MCU.
bool hasGphyOcp(int version) {
    return is8168gUp(version) || is8125(version);
}

// 8168E-VL and later.
bool is8168evlUp(int version) {
    return version >= 34 && version != 39 && version <= 53;
}

void barrier() {
    idrv_barrier();
}

void delayUs(uint64_t us) {
    gApi->delayUs(us);
}

void writeHex(uint64_t value, int digits) {
    char text[17];
    for (int i = digits - 1; i >= 0; --i) {
        text[i] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    }
    text[digits] = '\0';
    log(text);
}

int irqClaim(void* context);
uint32_t irqHandle(void*) { return IDRV_IRQ_NET_RECEIVE; }
const IdrvIrqOps kIrqOps = {irqClaim, irqHandle};

class RTL8169 {
public:
    bool bind(const IdrvPciDevice* dev);
    void getMacAddress(uint8_t* mac);
    bool sendPacket(const void* data, size_t len);
    int receivePacket(void* buffer, size_t maxLen);
    bool isLinkUp() const;
    bool claimInterrupt();

    bool bound;
    uint32_t irqMode;   // IDRV_IRQ_*
    const char* chipName;

private:
    void identifyChip();
    void hwInit8168g();
    void hwInit8125();
    void start8125();
    void waitTxRxFifoEmpty();
    uint16_t ephyRead(uint8_t reg);
    void ephyWrite(uint8_t reg, uint16_t value);
    void irqMask(uint32_t mask);
    uint32_t irqStatus() const;
    void irqAck(uint32_t bits);
    void enableRxdvGate();
    void reset();
    void readMacAddress();
    void writeMacAddress();
    void startPhy();
    bool setupRings();
    void start();
    void setupInterrupts();

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

    IdrvPciDevice pci;
    volatile uint8_t* mmio;
    uint32_t xid;
    int macVersion;   // Linux RTL_GIGA_MAC_VER_*, 0 = unknown

    RTL8169Desc* rxRing;   // the CPU's view (direct map)
    uint64_t rxRingPhys;
    uint8_t* rxBuffers;
    uint64_t rxBuffersPhys;
    uint16_t rxCurrent;

    RTL8169Desc* txRing;
    uint64_t txRingPhys;
    uint8_t* txBuffers;
    uint64_t txBuffersPhys;
    uint16_t txCurrent;

    uint8_t macAddr[6];
};

RTL8169 gNic;

bool RTL8169::bind(const IdrvPciDevice* dev) {
    pci = *dev;
    chipName = "rtl8169";
    const uint32_t command = gApi->pciRead(&pci, IDRV_PCI_COMMAND, 2);
    gApi->pciWrite(&pci, IDRV_PCI_COMMAND, 2, command | IDRV_PCI_COMMAND_MEMORY | IDRV_PCI_COMMAND_BUS_MASTER);

    // The registers are the first memory BAR: BAR1 on the 8169, BAR2 on the
    // 8168/8101 (BAR0 is the same window in I/O space).
    uint64_t base = 0;
    for (uint8_t bar = 0; bar < 6 && base == 0; ++bar) {
        base = gApi->pciMapBar(&pci, bar, 0x1000);
    }
    if (base == 0) {
        log("[net] rtl8169: no memory BAR\n");
        return false;
    }
    mmio = reinterpret_cast<volatile uint8_t*>(base);
    if (read32(REG_TX_CONFIG) == 0xFFFFFFFFu) {
        log("[net] rtl8169: registers read all ones\n");
        return false;
    }

    identifyChip();
    if (macVersion == 0) {
        log("[net] rtl8169: unknown chip, XID ");
        writeHex(xid, 3);
        log("\n");
        return false;
    }

    if (is8125(macVersion)) {
        // Its registers span 64 KiB (MAC0_BKP at 0x19e0, the coalescing table).
        uint64_t wide = 0;
        for (uint8_t bar = 0; bar < 6 && wide == 0; ++bar) {
            wide = gApi->pciMapBar(&pci, bar, 0x10000);
        }
        if (wide != 0) {
            mmio = reinterpret_cast<volatile uint8_t*>(wide);
        }
    }

    irqMask(0);
    irqAck(0xFFFFFFFFu);
    if (is8125(macVersion)) {
        hwInit8125();
    } else if (is8168gUp(macVersion)) {
        hwInit8168g();
    }
    readMacAddress();
    reset();
    if (!setupRings()) {
        return false;
    }
    startPhy();
    start();
    setupInterrupts();

    log("[net] rtl8169: ");
    log(chipName);
    log(" (XID ");
    writeHex(xid, 3);
    log(", mac version ");
    writeHex(static_cast<uint64_t>(macVersion), 2);
    log(") MAC ");
    for (int i = 0; i < 6; ++i) {
        writeHex(macAddr[i], 2);
        log(i < 5 ? ":" : "");
    }
    // The PHY's ID tells boards apart in a report from real hardware.
    log(" PHY ");
    writeHex(static_cast<uint16_t>(phyRead(2)), 4);
    log(":");
    writeHex(static_cast<uint16_t>(phyRead(3)), 4);
    log("\n");
    return true;
}

void RTL8169::identifyChip() {
    xid = (read32(REG_TX_CONFIG) >> 20) & 0xFCF;
    for (const ChipEntry& chip : kChips) {
        if ((xid & chip.mask) == chip.value) {
            macVersion = chip.version;
            chipName = chip.name;
            return;
        }
    }
}

// rtl_hw_init_8168g: take the MAC back from the out-of-band firmware (DASH /
// boot ROM) and rebuild its link list, with the receiver gated meanwhile.
void RTL8169::hwInit8168g() {
    enableRxdvGate();
    write8(REG_CHIP_CMD, static_cast<uint8_t>(read8(REG_CHIP_CMD) & ~(CMD_TX_ENABLE | CMD_RX_ENABLE)));
    delayUs(1000);
    write8(REG_MCU, static_cast<uint8_t>(read8(REG_MCU) & ~MCU_NOW_IS_OOB));
    macOcpModify(0xE8DE, 1 << 14, 0);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
    macOcpModify(0xE8DE, 0, 1 << 15);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
}

// rtl_hw_init_8125: as for the 8168G, plus three MAC OCP settings.
void RTL8169::hwInit8125() {
    enableRxdvGate();
    write8(REG_CHIP_CMD, static_cast<uint8_t>(read8(REG_CHIP_CMD) & ~(CMD_TX_ENABLE | CMD_RX_ENABLE)));
    delayUs(1000);
    write8(REG_MCU, static_cast<uint8_t>(read8(REG_MCU) & ~MCU_NOW_IS_OOB));
    macOcpModify(0xE8DE, 1 << 14, 0);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
    macOcpWrite(0xC0AA, 0x07D0);
    macOcpWrite(0xC0A6, 0x0150);
    macOcpWrite(0xC01E, 0x5555);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
}

void RTL8169::enableRxdvGate() {
    write32(REG_MISC, read32(REG_MISC) | MISC_RXDV_GATED);
    delayUs(2000);
    waitTxRxFifoEmpty();
}

// rtl_wait_txrx_fifo_empty
void RTL8169::waitTxRxFifoEmpty() {
    if (macVersion == 63) {
        write8(REG_CHIP_CMD, static_cast<uint8_t>(read8(REG_CHIP_CMD) | CMD_STOP_REQ));
        waitFor(REG_MCU, MCU_TX_EMPTY | MCU_RX_EMPTY, true, 42, 100);
        // IntrMitigate reports the FIFOs on the 8125B.
        for (int i = 0; i < 42 && (read16(REG_INTR_MITIGATE) & 0x0103) != 0x0103; ++i) {
            delayUs(100);
        }
        return;
    }
    if (macVersion == 61) {
        waitFor(REG_MCU, MCU_TX_EMPTY | MCU_RX_EMPTY, true, 42, 100);
        return;
    }
    waitFor(REG_TX_CONFIG, TX_CONFIG_EMPTY, true, 42, 100);
    waitFor(REG_MCU, MCU_TX_EMPTY | MCU_RX_EMPTY, true, 42, 100);
}

// rtl8169_cleanup + rtl_hw_reset.
void RTL8169::reset() {
    write32(REG_RX_CONFIG, read32(REG_RX_CONFIG) & ~RX_ACCEPT_MASK);
    if (hasGphyOcp(macVersion)) {
        enableRxdvGate();
    }
    write8(REG_CHIP_CMD, CMD_RESET);
    if (!waitFor(REG_CHIP_CMD, CMD_RESET, false, 100, 100)) {
        log("[net] rtl8169: reset did not finish\n");
    }
    irqMask(0);
    irqAck(0xFFFFFFFFu);
}

void RTL8169::readMacAddress() {
    auto valid = [](const uint8_t* mac) {
        bool zero = true;
        for (int i = 0; i < 6; ++i) {
            zero = zero && mac[i] == 0;
        }
        return !zero && (mac[0] & 1) == 0;
    };
    // 8168E-VL and later keep the factory address at ERI 0xe0 too, which
    // survives a boot ROM that changed MAC0.
    if (is8168evlUp(macVersion) && macVersion != 34) {
        const uint32_t low = eriRead(0xE0);
        const uint32_t high = eriRead(0xE4);
        const uint8_t mac[6] = {
            static_cast<uint8_t>(low), static_cast<uint8_t>(low >> 8),
            static_cast<uint8_t>(low >> 16), static_cast<uint8_t>(low >> 24),
            static_cast<uint8_t>(high), static_cast<uint8_t>(high >> 8),
        };
        if (valid(mac)) {
            memcpy(macAddr, mac, 6);
            return;
        }
    }
    // The 8125 keeps its factory address in MAC0_BKP.
    const uint16_t macReg = is8125(macVersion) ? REG_MAC0_BKP : REG_MAC0;
    for (int i = 0; i < 6; ++i) {
        macAddr[i] = read8(static_cast<uint16_t>(macReg + i));
    }
    if (!valid(macAddr) && is8125(macVersion)) {
        for (int i = 0; i < 6; ++i) {
            macAddr[i] = read8(static_cast<uint16_t>(REG_MAC0 + i));
        }
    }
    if (!valid(macAddr)) {
        // A locally administered address rather than none at all.
        if (IDRV_API_HAS(gApi, random)) {
            gApi->random(macAddr, sizeof(macAddr));
        }
        macAddr[0] = static_cast<uint8_t>((macAddr[0] & 0xFE) | 0x02);
    }
}

void RTL8169::writeMacAddress() {
    write32(REG_MAC4, static_cast<uint32_t>(macAddr[4]) | (static_cast<uint32_t>(macAddr[5]) << 8));
    (void)read32(REG_MAC4);
    write32(REG_MAC0, static_cast<uint32_t>(macAddr[0]) | (static_cast<uint32_t>(macAddr[1]) << 8) |
                          (static_cast<uint32_t>(macAddr[2]) << 16) |
                          (static_cast<uint32_t>(macAddr[3]) << 24));
    (void)read32(REG_MAC0);
}

// Power the PHY up (a board may hand it over powered down) and (re)start
// autonegotiation with everything advertised.
void RTL8169::startPhy() {
    if (hasGphyOcp(macVersion)) {
        write8(REG_PMCH, static_cast<uint8_t>(read8(REG_PMCH) | 0xC0));   // PLL power up
    } else if (macVersion >= 25) {
        write8(REG_PMCH, static_cast<uint8_t>(read8(REG_PMCH) | 0x80));
    }
    if (!hasGphyOcp(macVersion)) {
        phyWrite(0x1F, 0);   // page 0
    }
    phyWrite(MII_ADVERTISE, ADVERTISE_ALL);
    if (!isFastEthernet(macVersion)) {
        phyWrite(MII_CTRL1000, ADVERTISE_1000);
    }
    phyWrite(MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

bool RTL8169::setupRings() {
    // One page per ring (256 descriptors of 16 bytes, 256-byte aligned) and
    // a block of 2 KiB buffers each way, all below 4 GiB: chips before the
    // 8168C cannot address more.
    constexpr uint64_t kBelow4G = 0x100000000ULL;
    void* virt = nullptr;
    rxRingPhys = gApi->dmaAlloc(1, kBelow4G, &virt);
    rxRing = static_cast<RTL8169Desc*>(virt);
    txRingPhys = gApi->dmaAlloc(1, kBelow4G, &virt);
    txRing = static_cast<RTL8169Desc*>(virt);
    rxBuffersPhys = gApi->dmaAlloc(RX_DESCRIPTORS * BUFFER_SIZE / 4096, kBelow4G, &virt);
    rxBuffers = static_cast<uint8_t*>(virt);
    txBuffersPhys = gApi->dmaAlloc(TX_DESCRIPTORS * BUFFER_SIZE / 4096, kBelow4G, &virt);
    txBuffers = static_cast<uint8_t*>(virt);
    if (rxRingPhys == 0 || txRingPhys == 0 || rxBuffersPhys == 0 || txBuffersPhys == 0) {
        log("[net] rtl8169: no memory below 4 GiB for the rings\n");
        return false;
    }
    for (size_t i = 0; i < RX_DESCRIPTORS; ++i) {
        rxRing[i].addr = rxBuffersPhys + i * BUFFER_SIZE;
        rxRing[i].opts2 = 0;
        rxRing[i].opts1 = DESC_OWN | (i == RX_DESCRIPTORS - 1 ? DESC_EOR : 0) | static_cast<uint32_t>(BUFFER_SIZE);
    }
    for (size_t i = 0; i < TX_DESCRIPTORS; ++i) {
        txRing[i].addr = txBuffersPhys + i * BUFFER_SIZE;
        txRing[i].opts1 = i == TX_DESCRIPTORS - 1 ? DESC_EOR : 0;
    }
    rxCurrent = 0;
    txCurrent = 0;
    barrier();
    return true;
}

// rtl_hw_start, without the per-chip tuning tables (ePHY, ERI, PHY patches).
void RTL8169::start() {
    write8(REG_CFG9346, CFG9346_UNLOCK);
    if (macVersion > 6) {
        // ASPM and clock requests off: the classic cause of a receiver that
        // stops after a while.
        write8(REG_CONFIG2, static_cast<uint8_t>(read8(REG_CONFIG2) & ~CONFIG2_CLKREQ));
        write8(REG_CONFIG5, static_cast<uint8_t>(read8(REG_CONFIG5) & ~CONFIG5_ASPM));
    }
    uint16_t cplus = static_cast<uint16_t>(read16(REG_CPLUS_CMD) & CPLUS_KEEP);
    if (macVersion <= 6) {
        cplus |= CPLUS_PCI_MUL_RW;
    }
    write16(REG_CPLUS_CMD, cplus);
    writeMacAddress();
    if (is8125(macVersion)) {
        start8125();   // ends with the receive gate opened
        macOcpModify(0xC0AC, 0, 0x1F80);   // rtl_enable_exit_l1
    } else {
        write8(REG_MAX_TX_PACKET, is8168evlUp(macVersion) ? 0x27 : 0x3F);
        if (is8168gUp(macVersion)) {
            write32(REG_MISC, read32(REG_MISC) & ~MISC_RXDV_GATED);
        }
        write16(REG_INTR_MITIGATE, 0);
    }
    write16(REG_RX_MAX_SIZE, BUFFER_SIZE);

    const uint64_t txPhys = txRingPhys;
    const uint64_t rxPhys = rxRingPhys;
    write32(REG_TX_DESC_HIGH, static_cast<uint32_t>(txPhys >> 32));
    write32(REG_TX_DESC_LOW, static_cast<uint32_t>(txPhys));
    write32(REG_RX_DESC_HIGH, static_cast<uint32_t>(rxPhys >> 32));
    write32(REG_RX_DESC_LOW, static_cast<uint32_t>(rxPhys));
    write8(REG_CFG9346, CFG9346_LOCK);
    (void)read8(REG_CHIP_CMD);

    write8(REG_CHIP_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    // rtl_init_rxcfg
    uint32_t rxConfig;
    if (is8125(macVersion)) {
        rxConfig = RX_FETCH_DFLT_8125 | RX_DMA_BURST;
    } else if (macVersion >= 40 && macVersion <= 53) {
        rxConfig = RX_128_INT | RX_MULTI | RX_DMA_BURST | RX_EARLY_OFF;
    } else if ((macVersion >= 18 && macVersion <= 24) || (macVersion >= 34 && macVersion <= 36) ||
               macVersion == 38) {
        rxConfig = RX_128_INT | RX_MULTI | RX_DMA_BURST;
    } else if (macVersion <= 6 || (macVersion >= 10 && macVersion <= 17) ||
               macVersion == 25 || macVersion == 26) {
        rxConfig = RX_FIFO_THRESH | RX_DMA_BURST;
    } else {
        rxConfig = RX_128_INT | RX_DMA_BURST;
    }
    write32(REG_TX_CONFIG, TX_DMA_BURST | TX_IFG | (is8168evlUp(macVersion) ? TX_AUTO_FIFO : 0));
    // Every multicast group (the hash filter all ones), broadcast, our address.
    write32(REG_MAR0 + 4, 0xFFFFFFFFu);
    write32(REG_MAR0, 0xFFFFFFFFu);
    write32(REG_RX_CONFIG, rxConfig | RX_ACCEPT_BROADCAST | RX_ACCEPT_MULTICAST | RX_ACCEPT_MY_PHYS);
}

void RTL8169::setupInterrupts() {
    irqMode = gApi->irqRegister(&pci, IDRV_IRQ_ALLOW_MSI | IDRV_IRQ_ALLOW_LEGACY, &kIrqOps, this);
    if (irqMode != IDRV_IRQ_NONE) {
        irqAck(0xFFFFFFFFu);
        irqMask(INT_RECEIVE);
    }
    log(irqMode == IDRV_IRQ_MSI ? "[net] rtl8169: MSI receive interrupt\n"
        : irqMode == IDRV_IRQ_LEGACY ? "[net] rtl8169: INTx receive interrupt\n"
        : "[net] rtl8169: no interrupt, polling\n");
}

bool RTL8169::claimInterrupt() {
    if (!mmio) {
        return false;
    }
    // Write-one-to-clear; all ones means the device is gone.
    const uint32_t status = irqStatus();
    const uint32_t gone = is8125(macVersion) ? 0xFFFFFFFFu : 0xFFFFu;
    if (status == 0 || status == gone) {
        return false;
    }
    irqAck(status);
    return true;
}

void RTL8169::getMacAddress(uint8_t* mac) {
    memcpy(mac, macAddr, sizeof(macAddr));
}

bool RTL8169::sendPacket(const void* data, size_t len) {
    if (!data || len == 0 || len > MTU) {
        return false;
    }
    RTL8169Desc& desc = txRing[txCurrent];
    for (uint32_t spin = 0; (desc.opts1 & DESC_OWN) != 0; ++spin) {
        if (spin == TX_OWN_SPIN_LIMIT) {
            return false;   // the ring is full and the chip is not draining it
        }
        barrier();
    }

    uint8_t* buffer = txBuffers + txCurrent * BUFFER_SIZE;
    memcpy(buffer, data, len);
    // Some chips do not pad short frames themselves.
    if (len < ETH_MIN_FRAME) {
        memset(buffer + len, 0, ETH_MIN_FRAME - len);
        len = ETH_MIN_FRAME;
    }
    const bool last = txCurrent == TX_DESCRIPTORS - 1;
    desc.opts2 = 0;
    barrier();
    desc.opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (last ? DESC_EOR : 0) | static_cast<uint32_t>(len);
    barrier();
    txCurrent = static_cast<uint16_t>((txCurrent + 1) % TX_DESCRIPTORS);
    // rtl8169_doorbell
    if (is8125(macVersion)) {
        write16(REG_TX_POLL_8125, 1);
    } else {
        write8(REG_TX_POLL, TX_POLL_NORMAL);
    }
    return true;
}

int RTL8169::receivePacket(void* buffer, size_t maxLen) {
    if (!buffer || maxLen == 0) {
        return -1;
    }
    for (;;) {
        RTL8169Desc& desc = rxRing[rxCurrent];
        const uint32_t status = desc.opts1;
        if ((status & DESC_OWN) != 0) {
            return -1;   // nothing (more) received
        }
        barrier();
        int result = -1;
        // Only whole, error-free frames; one spread over several buffers
        // (larger than any the stack sends or expects) is dropped.
        const size_t length = status & DESC_LENGTH;
        if ((status & (DESC_FIRST | DESC_LAST)) == (DESC_FIRST | DESC_LAST) &&
            (status & RX_STATUS_ERROR) == 0 && length > ETH_CRC) {
            const size_t frame = length - ETH_CRC;
            const size_t copied = frame < maxLen ? frame : maxLen;
            memcpy(buffer, rxBuffers + rxCurrent * BUFFER_SIZE, copied);
            result = static_cast<int>(copied);
        }
        const bool last = rxCurrent == RX_DESCRIPTORS - 1;
        desc.opts2 = 0;
        barrier();
        desc.opts1 = DESC_OWN | (last ? DESC_EOR : 0) | static_cast<uint32_t>(BUFFER_SIZE);
        barrier();
        rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % RX_DESCRIPTORS);
        if (result >= 0) {
            return result;
        }
    }
}

bool RTL8169::isLinkUp() const {
    return (read8(REG_PHY_STATUS) & PHY_STATUS_LINK) != 0;
}

int RTL8169::phyRead(uint8_t reg) {
    if (hasGphyOcp(macVersion)) {
        write32(REG_GPHY_OCP, static_cast<uint32_t>(OCP_STD_PHY_BASE + reg * 2) << 15);
        if (!waitFor(REG_GPHY_OCP, OCP_FLAG, true, 25, 10)) {
            return -1;
        }
        return static_cast<int>(read32(REG_GPHY_OCP) & 0xFFFF);
    }
    write32(REG_PHYAR, static_cast<uint32_t>(reg & 0x1F) << 16);
    if (!waitFor(REG_PHYAR, 1u << 31, true, 20, 25)) {
        return -1;
    }
    const int value = static_cast<int>(read32(REG_PHYAR) & 0xFFFF);
    delayUs(20);   // the chip needs a breather between MDIO accesses
    return value;
}

void RTL8169::phyWrite(uint8_t reg, uint16_t value) {
    if (hasGphyOcp(macVersion)) {
        write32(REG_GPHY_OCP, OCP_FLAG | (static_cast<uint32_t>(OCP_STD_PHY_BASE + reg * 2) << 15) | value);
        waitFor(REG_GPHY_OCP, OCP_FLAG, false, 25, 10);
        return;
    }
    write32(REG_PHYAR, (1u << 31) | (static_cast<uint32_t>(reg & 0x1F) << 16) | value);
    waitFor(REG_PHYAR, 1u << 31, false, 20, 25);
    delayUs(20);
}

// rtl_hw_start_8125 + rtl_hw_start_8125a_2 / _8125b + rtl_hw_start_8125_common.
void RTL8169::start8125() {
    // Interrupt coalescing off.
    for (uint16_t reg = 0xA00; reg < 0xB00; reg += 4) {
        write32(reg, 0);
    }
    // ASPM entry latency (PCIe config 0x70f; dropped without ECAM) and the
    // PCIe PHY tables.
    gApi->pciWrite(&pci, 0x070F, 1, 0x27);
    struct Ephy { uint8_t reg; uint16_t mask; uint16_t bits; };
    static constexpr Ephy k8125a[] = {
        {0x04, 0xffff, 0xd000}, {0x0a, 0xffff, 0x8653}, {0x23, 0xffff, 0xab66},
        {0x20, 0xffff, 0x9455}, {0x21, 0xffff, 0x99ff}, {0x29, 0xffff, 0xfe04},
        {0x44, 0xffff, 0xd000}, {0x4a, 0xffff, 0x8653}, {0x63, 0xffff, 0xab66},
        {0x60, 0xffff, 0x9455}, {0x61, 0xffff, 0x99ff}, {0x69, 0xffff, 0xfe04},
    };
    static constexpr Ephy k8125b[] = {
        {0x0b, 0xffff, 0xa908}, {0x1e, 0xffff, 0x20eb}, {0x4b, 0xffff, 0xa908},
        {0x5e, 0xffff, 0x20eb}, {0x22, 0x0030, 0x0020}, {0x62, 0x0030, 0x0020},
    };
    const bool b = macVersion == 63;
    const Ephy* table = b ? k8125b : k8125a;
    const size_t count = b ? sizeof(k8125b) / sizeof(k8125b[0]) : sizeof(k8125a) / sizeof(k8125a[0]);
    for (size_t i = 0; i < count; ++i) {
        ephyWrite(table[i].reg, static_cast<uint16_t>((ephyRead(table[i].reg) & ~table[i].mask) | table[i].bits));
    }

    // rtl_hw_start_8125_common
    write8(REG_CONFIG3, static_cast<uint8_t>(read8(REG_CONFIG3) & ~CONFIG3_RDY_TO_L23));
    write16(0x382, 0x221B);
    write8(0x4500, 0);
    write16(0x4800, 0);
    macOcpModify(0xD40A, 0x0010, 0x0000);   // UPS off
    write8(REG_CONFIG1, static_cast<uint8_t>(read8(REG_CONFIG1) & ~0x10));
    macOcpWrite(0xC140, 0xFFFF);
    macOcpWrite(0xC142, 0xFFFF);
    macOcpModify(0xD3E2, 0x0FFF, 0x03A9);
    macOcpModify(0xD3E4, 0x00FF, 0x0000);
    macOcpModify(0xE860, 0x0000, 0x0080);
    macOcpModify(0xEB58, 0x0001, 0x0000);   // the old TX descriptor format
    macOcpModify(0xE614, 0x0700, b ? 0x0200 : 0x0400);
    macOcpModify(0xE63E, 0x0C30, b ? 0x0000 : 0x0020);
    macOcpModify(0xC0B4, 0x0000, 0x000C);
    macOcpModify(0xEB6A, 0x00FF, 0x0033);
    macOcpModify(0xEB50, 0x03E0, 0x0040);
    macOcpModify(0xE056, 0x00F0, 0x0030);
    macOcpModify(0xE040, 0x1000, 0x0000);
    macOcpModify(0xEA1C, 0x0003, 0x0001);
    macOcpModify(0xE0C0, 0x4F0F, 0x4403);
    macOcpModify(0xE052, 0x0080, 0x0068);
    macOcpModify(0xD430, 0x0FFF, 0x047F);
    macOcpModify(0xEA1C, 0x0004, 0x0000);
    macOcpModify(0xEB54, 0x0000, 0x0001);
    delayUs(1);
    macOcpModify(0xEB54, 0x0001, 0x0000);
    write16(0x1880, static_cast<uint16_t>(read16(0x1880) & ~0x0030));
    macOcpWrite(0xE098, 0xC302);
    for (int i = 0; i < 10 && (macOcpRead(0xE00E) & (1 << 13)) != 0; ++i) {
        delayUs(1000);
    }
    // EEE MAC settings (rtl8125a/b_config_eee_mac)
    if (b) {
        write16(REG_EEE_TXIDLE_TIMER_8125, static_cast<uint16_t>(1500 + 14 + 0x20));
        macOcpModify(0xE040, 0, (1 << 1) | (1 << 0));
    } else {
        macOcpModify(0xE040, 0, (1 << 1) | (1 << 0));
        macOcpModify(0xEB62, 0, (1 << 2) | (1 << 1));
    }
    write32(REG_MISC, read32(REG_MISC) & ~MISC_RXDV_GATED);
}

uint16_t RTL8169::ephyRead(uint8_t reg) {
    write32(REG_EPHYAR, static_cast<uint32_t>(reg & 0x3F) << 16);
    if (!waitFor(REG_EPHYAR, EPHYAR_FLAG, true, 100, 10)) {
        return 0;
    }
    return static_cast<uint16_t>(read32(REG_EPHYAR));
}

void RTL8169::ephyWrite(uint8_t reg, uint16_t value) {
    write32(REG_EPHYAR, EPHYAR_FLAG | (static_cast<uint32_t>(reg & 0x3F) << 16) | value);
    waitFor(REG_EPHYAR, EPHYAR_FLAG, false, 100, 10);
    delayUs(10);
}

// rtl_irq_enable/disable, rtl_get_events, rtl_ack_events: 16 bits at
// 0x3c/0x3e before the 8125, 32 bits at 0x38/0x3c on it.
void RTL8169::irqMask(uint32_t mask) {
    if (is8125(macVersion)) {
        write32(REG_INTR_MASK_8125, mask);
    } else {
        write16(REG_INTR_MASK, static_cast<uint16_t>(mask));
    }
}

uint32_t RTL8169::irqStatus() const {
    return is8125(macVersion) ? read32(REG_INTR_STATUS_8125) : read16(REG_INTR_STATUS);
}

void RTL8169::irqAck(uint32_t bits) {
    if (is8125(macVersion)) {
        write32(REG_INTR_STATUS_8125, bits);
    } else {
        write16(REG_INTR_STATUS, static_cast<uint16_t>(bits));
    }
}

uint16_t RTL8169::macOcpRead(uint16_t reg) {
    write32(REG_OCPDR, static_cast<uint32_t>(reg) << 15);
    return static_cast<uint16_t>(read32(REG_OCPDR));
}

void RTL8169::macOcpWrite(uint16_t reg, uint16_t value) {
    write32(REG_OCPDR, OCP_FLAG | (static_cast<uint32_t>(reg) << 15) | value);
}

void RTL8169::macOcpModify(uint16_t reg, uint16_t clear, uint16_t set) {
    macOcpWrite(reg, static_cast<uint16_t>((macOcpRead(reg) & ~clear) | set));
}

uint32_t RTL8169::eriRead(uint16_t addr) {
    write32(REG_ERIAR, ERIAR_MASK_ALL | addr);   // read, ExGMAC space
    if (!waitFor(REG_ERIAR, ERIAR_FLAG, true, 100, 100)) {
        return 0;
    }
    return read32(REG_ERIDR);
}

bool RTL8169::waitFor(uint16_t reg, uint32_t mask, bool high, uint32_t polls,
                            uint32_t intervalUs) const {
    for (uint32_t i = 0; i < polls; ++i) {
        const uint32_t value = (mask & 0xFFFFFF00u) ? read32(reg) : read8(reg);
        if (high ? (value & mask) == mask : (value & mask) == 0) {
            return true;
        }
        delayUs(intervalUs);
    }
    return false;
}

uint8_t RTL8169::read8(uint16_t reg) const {
    return *reinterpret_cast<volatile uint8_t*>(mmio + reg);
}

uint16_t RTL8169::read16(uint16_t reg) const {
    return *reinterpret_cast<volatile uint16_t*>(mmio + reg);
}

uint32_t RTL8169::read32(uint16_t reg) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio + reg);
}

void RTL8169::write8(uint16_t reg, uint8_t value) const {
    *reinterpret_cast<volatile uint8_t*>(mmio + reg) = value;
}

void RTL8169::write16(uint16_t reg, uint16_t value) const {
    *reinterpret_cast<volatile uint16_t*>(mmio + reg) = value;
}

void RTL8169::write32(uint16_t reg, uint32_t value) const {
    *reinterpret_cast<volatile uint32_t*>(mmio + reg) = value;
}

// ---- The kernel's callbacks ------------------------------------------------

int irqClaim(void* context) {
    return static_cast<RTL8169*>(context)->claimInterrupt();
}

void macAddress(void* context, uint8_t mac[6]) {
    static_cast<RTL8169*>(context)->getMacAddress(mac);
}

int send(void* context, const void* frame, size_t length) {
    return static_cast<RTL8169*>(context)->sendPacket(frame, length);
}

int receive(void* context, void* buffer, size_t maxLength) {
    return static_cast<RTL8169*>(context)->receivePacket(buffer, maxLength);
}

int linkUp(void* context) {
    return static_cast<RTL8169*>(context)->isLinkUp();
}

int interruptDriven(void* context) {
    return static_cast<RTL8169*>(context)->irqMode != IDRV_IRQ_NONE;
}

// netcfg shows the chip ("rtl8168h"): the name is filled in at probe.
IdrvNetOps gNetOps = {
    "rtl8169", 60, 0, macAddress, send, receive, linkUp, interruptDriven,
};

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    gApi = api;
    if (gNic.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    if (!gNic.bind(dev)) {
        return IDRV_ENODEV;
    }
    gNic.bound = true;
    gNetOps.name = gNic.chipName;
    return api->netRegister(&gNetOps, &gNic);
}

constexpr size_t kPciIdCount = sizeof(kPciIds) / sizeof(kPciIds[0]);

struct MatchTable {
    IdrvPciMatch entries[kPciIdCount];
};

constexpr MatchTable makeMatches() {
    MatchTable t = {};
    for (size_t i = 0; i < kPciIdCount; ++i) {
        t.entries[i] = {kPciIds[i].vendor, kPciIds[i].device, 0, 0};
    }
    return t;
}

constexpr MatchTable kMatches = makeMatches();

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "rtl8169",
    "1.0",
    "Realtek RTL8169/8111/8168/8101 and RTL8125 Ethernet",
    "InstantOS",
    kMatches.entries,
    kPciIdCount,
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
