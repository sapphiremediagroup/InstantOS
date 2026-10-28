#include <drivers/net/rtl8169.hpp>
#include <cpu/acpi/pci.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/cereal/cereal.hpp>
#include <time/tsc_timer.hpp>
#include <common/krandom.hpp>

// Register names and bits follow Linux's drivers/net/ethernet/realtek/r8169.

namespace {
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

// 8168E-VL and later.
bool is8168evlUp(int version) {
    return version >= 34 && version != 39 && version <= 53;
}

void barrier() {
    asm volatile("" ::: "memory");
}

void delayUs(uint64_t us) {
    // The uptime runs on the TSC; the guard ends the wait should it not move.
    const uint64_t end = time_get_uptime_ns() + us * 1000;
    for (uint64_t guard = 0; time_get_uptime_ns() < end && guard < us * 1000; ++guard) {
        asm volatile("pause");
    }
}

void writeHex(uint64_t value, int digits) {
    char text[17];
    for (int i = digits - 1; i >= 0; --i) {
        text[i] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    }
    text[digits] = '\0';
    Cereal::get().write(text);
}

class RTL8169InterruptHandler : public Interrupt {
public:
    void initialize() override {
    }

    bool shouldDispatch() override {
        return RTL8169Driver::get().claimInterrupt();
    }

    void Run(InterruptFrame* frame) override {
        // As for e1000: an MSI vector is ours and is EOId here, the shared
        // INTx dispatcher EOIs after all its handlers.
        RTL8169Driver& nic = RTL8169Driver::get();
        const bool msi = nic.usesMsi();
        if (msi) {
            nic.claimInterrupt();   // acknowledge before draining
            sendEOI();
        }
        netDeviceInterrupt(frame, msi);
    }
};

RTL8169InterruptHandler& interruptHandler() {
    static RTL8169InterruptHandler handler;
    return handler;
}
}

RTL8169Driver& RTL8169Driver::get() {
    static RTL8169Driver instance;
    return instance;
}

bool RTL8169Driver::initialize() {
    if (initialized) {
        return true;
    }
    if (!detectDevice() || !initDevice()) {
        return false;
    }
    initialized = true;
    return true;
}

bool RTL8169Driver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint8_t busNumber = static_cast<uint8_t>(b);
                const uint16_t vendor = pci.readConfig16(0, busNumber, d, f, PCI_VENDOR_ID_REG);
                if (vendor == 0xFFFF) {
                    if (f == 0) {
                        break;
                    }
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, busNumber, d, f, PCI_DEVICE_ID_REG);
                for (const PciId& id : kPciIds) {
                    if (id.vendor == vendor && id.device == devId) {
                        bus = busNumber;
                        device = d;
                        function = f;
                        pciDeviceId = devId;
                        deviceFound = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool RTL8169Driver::initDevice() {
    PCI& pci = PCI::get();
    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND_REG);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    pci.writeConfig16(0, bus, device, function, PCI_COMMAND_REG, command);

    // The registers are the first memory BAR: BAR1 on the 8169, BAR2 on the
    // 8168/8101 (BAR0 is the same window in I/O space).
    uint64_t base = 0;
    for (uint8_t bar = 0; bar < 6 && base == 0; ++bar) {
        base = pci.mapMemoryBar(0, bus, device, function, bar, 0x1000);
    }
    if (base == 0) {
        Cereal::get().write("[net] rtl8169: no memory BAR\n");
        return false;
    }
    mmio = reinterpret_cast<volatile uint8_t*>(base);
    if (read32(REG_TX_CONFIG) == 0xFFFFFFFFu) {
        Cereal::get().write("[net] rtl8169: registers read all ones\n");
        return false;
    }

    identifyChip();
    if (macVersion == 0) {
        Cereal::get().write("[net] rtl8169: unknown chip, XID ");
        writeHex(xid, 3);
        Cereal::get().write("\n");
        return false;
    }

    write16(REG_INTR_MASK, 0);
    write16(REG_INTR_STATUS, 0xFFFF);
    if (is8168gUp(macVersion)) {
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

    Cereal::get().write("[net] rtl8169: ");
    Cereal::get().write(chipName);
    Cereal::get().write(" (XID ");
    writeHex(xid, 3);
    Cereal::get().write(", mac version ");
    writeHex(static_cast<uint64_t>(macVersion), 2);
    Cereal::get().write(") MAC ");
    for (int i = 0; i < 6; ++i) {
        writeHex(macAddr[i], 2);
        Cereal::get().write(i < 5 ? ":" : "");
    }
    // The PHY's ID tells boards apart in a report from real hardware.
    Cereal::get().write(" PHY ");
    writeHex(static_cast<uint16_t>(phyRead(2)), 4);
    Cereal::get().write(":");
    writeHex(static_cast<uint16_t>(phyRead(3)), 4);
    Cereal::get().write("\n");
    return true;
}

void RTL8169Driver::identifyChip() {
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
void RTL8169Driver::hwInit8168g() {
    enableRxdvGate();
    write8(REG_CHIP_CMD, static_cast<uint8_t>(read8(REG_CHIP_CMD) & ~(CMD_TX_ENABLE | CMD_RX_ENABLE)));
    delayUs(1000);
    write8(REG_MCU, static_cast<uint8_t>(read8(REG_MCU) & ~MCU_NOW_IS_OOB));
    macOcpModify(0xE8DE, 1 << 14, 0);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
    macOcpModify(0xE8DE, 0, 1 << 15);
    waitFor(REG_MCU, MCU_LINK_LIST_READY, true, 42, 100);
}

void RTL8169Driver::enableRxdvGate() {
    write32(REG_MISC, read32(REG_MISC) | MISC_RXDV_GATED);
    delayUs(2000);
    // rtl_wait_txrx_fifo_empty
    waitFor(REG_TX_CONFIG, TX_CONFIG_EMPTY, true, 42, 100);
    waitFor(REG_MCU, MCU_TX_EMPTY | MCU_RX_EMPTY, true, 42, 100);
}

// rtl8169_cleanup + rtl_hw_reset.
void RTL8169Driver::reset() {
    write32(REG_RX_CONFIG, read32(REG_RX_CONFIG) & ~RX_ACCEPT_MASK);
    if (is8168gUp(macVersion)) {
        enableRxdvGate();
    }
    write8(REG_CHIP_CMD, CMD_RESET);
    if (!waitFor(REG_CHIP_CMD, CMD_RESET, false, 100, 100)) {
        Cereal::get().write("[net] rtl8169: reset did not finish\n");
    }
    write16(REG_INTR_MASK, 0);
    write16(REG_INTR_STATUS, 0xFFFF);
}

void RTL8169Driver::readMacAddress() {
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
    for (int i = 0; i < 6; ++i) {
        macAddr[i] = read8(static_cast<uint16_t>(REG_MAC0 + i));
    }
    if (!valid(macAddr)) {
        // A locally administered address rather than none at all.
        kernel_fill_entropy(macAddr, sizeof(macAddr));
        macAddr[0] = static_cast<uint8_t>((macAddr[0] & 0xFE) | 0x02);
    }
}

void RTL8169Driver::writeMacAddress() {
    write32(REG_MAC4, static_cast<uint32_t>(macAddr[4]) | (static_cast<uint32_t>(macAddr[5]) << 8));
    (void)read32(REG_MAC4);
    write32(REG_MAC0, static_cast<uint32_t>(macAddr[0]) | (static_cast<uint32_t>(macAddr[1]) << 8) |
                          (static_cast<uint32_t>(macAddr[2]) << 16) |
                          (static_cast<uint32_t>(macAddr[3]) << 24));
    (void)read32(REG_MAC0);
}

// Power the PHY up (a board may hand it over powered down) and (re)start
// autonegotiation with everything advertised.
void RTL8169Driver::startPhy() {
    if (is8168gUp(macVersion)) {
        write8(REG_PMCH, static_cast<uint8_t>(read8(REG_PMCH) | 0xC0));   // PLL power up
    } else if (macVersion >= 25) {
        write8(REG_PMCH, static_cast<uint8_t>(read8(REG_PMCH) | 0x80));
    }
    if (!is8168gUp(macVersion)) {
        phyWrite(0x1F, 0);   // page 0
    }
    phyWrite(MII_ADVERTISE, ADVERTISE_ALL);
    if (!isFastEthernet(macVersion)) {
        phyWrite(MII_CTRL1000, ADVERTISE_1000);
    }
    phyWrite(MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
}

bool RTL8169Driver::setupRings() {
    // One page per ring (256 descriptors of 16 bytes, 256-byte aligned) and
    // per buffer, all below 4 GiB: chips before the 8168C cannot address more.
    constexpr uint64_t kBelow4G = 0x100000000ULL;
    const uint64_t rxRingPhys = PMM::AllocFramesBelow(1, kBelow4G);
    const uint64_t txRingPhys = PMM::AllocFramesBelow(1, kBelow4G);
    if (rxRingPhys == 0 || txRingPhys == 0) {
        return false;
    }
    rxRing = reinterpret_cast<RTL8169Desc*>(rxRingPhys);
    txRing = reinterpret_cast<RTL8169Desc*>(txRingPhys);
    memset(rxRing, 0, PMM::PAGE_SIZE);
    memset(txRing, 0, PMM::PAGE_SIZE);

    for (size_t i = 0; i < RTL8169_RX_DESCRIPTORS; ++i) {
        const uint64_t phys = PMM::AllocFramesBelow(1, kBelow4G);
        if (phys == 0) {
            return false;
        }
        rxBuffers[i] = reinterpret_cast<uint8_t*>(phys);
        rxRing[i].addr = phys;
        rxRing[i].opts2 = 0;
        rxRing[i].opts1 = DESC_OWN | (i == RTL8169_RX_DESCRIPTORS - 1 ? DESC_EOR : 0) |
                          static_cast<uint32_t>(RTL8169_RX_BUFFER_SIZE);
    }
    for (size_t i = 0; i < RTL8169_TX_DESCRIPTORS; ++i) {
        const uint64_t phys = PMM::AllocFramesBelow(1, kBelow4G);
        if (phys == 0) {
            return false;
        }
        txBuffers[i] = reinterpret_cast<uint8_t*>(phys);
        txRing[i].addr = phys;
        txRing[i].opts1 = i == RTL8169_TX_DESCRIPTORS - 1 ? DESC_EOR : 0;
    }
    rxCurrent = 0;
    txCurrent = 0;
    barrier();
    return true;
}

// rtl_hw_start, without the per-chip tuning tables (ePHY, ERI, PHY patches).
void RTL8169Driver::start() {
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
    write8(REG_MAX_TX_PACKET, is8168evlUp(macVersion) ? 0x27 : 0x3F);
    if (is8168gUp(macVersion)) {
        write32(REG_MISC, read32(REG_MISC) & ~MISC_RXDV_GATED);
    }
    write16(REG_INTR_MITIGATE, 0);
    write16(REG_RX_MAX_SIZE, RTL8169_RX_BUFFER_SIZE);

    const uint64_t txPhys = reinterpret_cast<uint64_t>(txRing);
    const uint64_t rxPhys = reinterpret_cast<uint64_t>(rxRing);
    write32(REG_TX_DESC_HIGH, static_cast<uint32_t>(txPhys >> 32));
    write32(REG_TX_DESC_LOW, static_cast<uint32_t>(txPhys));
    write32(REG_RX_DESC_HIGH, static_cast<uint32_t>(rxPhys >> 32));
    write32(REG_RX_DESC_LOW, static_cast<uint32_t>(rxPhys));
    write8(REG_CFG9346, CFG9346_LOCK);
    (void)read8(REG_CHIP_CMD);

    write8(REG_CHIP_CMD, CMD_TX_ENABLE | CMD_RX_ENABLE);

    // rtl_init_rxcfg
    uint32_t rxConfig;
    if (macVersion >= 40 && macVersion <= 53) {
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

void RTL8169Driver::setupInterrupts() {
    irqMode = IRQMode::None;
    if (PCI::get().registerMSIInterrupt(0, bus, device, function, &interruptHandler())) {
        irqMode = IRQMode::Msi;
    } else if (PCI::get().registerLegacyInterrupt(0, bus, device, function, &interruptHandler())) {
        irqMode = IRQMode::Legacy;
    }
    if (irqMode != IRQMode::None) {
        write16(REG_INTR_STATUS, 0xFFFF);
        write16(REG_INTR_MASK, INT_RECEIVE);
    }
    Cereal::get().write(irqMode == IRQMode::Msi ? "[net] rtl8169: MSI receive interrupt\n"
                        : irqMode == IRQMode::Legacy ? "[net] rtl8169: INTx receive interrupt\n"
                        : "[net] rtl8169: no interrupt, polling\n");
}

bool RTL8169Driver::claimInterrupt() {
    if (!mmio) {
        return false;
    }
    // Write-one-to-clear; all ones means the device is gone.
    const uint16_t status = read16(REG_INTR_STATUS);
    if (status == 0 || status == 0xFFFF) {
        return false;
    }
    write16(REG_INTR_STATUS, status);
    return true;
}

void RTL8169Driver::getMacAddress(uint8_t* mac) {
    if (mac) {
        memcpy(mac, macAddr, sizeof(macAddr));
    }
}

bool RTL8169Driver::sendPacket(const void* data, size_t len) {
    if (!initialized || !data || len == 0 || len > NET_DEVICE_MTU) {
        return false;
    }
    RTL8169Desc& desc = txRing[txCurrent];
    for (uint32_t spin = 0; (desc.opts1 & DESC_OWN) != 0; ++spin) {
        if (spin == TX_OWN_SPIN_LIMIT) {
            return false;   // the ring is full and the chip is not draining it
        }
        barrier();
    }

    uint8_t* buffer = txBuffers[txCurrent];
    memcpy(buffer, data, len);
    // Some chips do not pad short frames themselves.
    if (len < ETH_MIN_FRAME) {
        memset(buffer + len, 0, ETH_MIN_FRAME - len);
        len = ETH_MIN_FRAME;
    }
    const bool last = txCurrent == RTL8169_TX_DESCRIPTORS - 1;
    desc.opts2 = 0;
    barrier();
    desc.opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (last ? DESC_EOR : 0) | static_cast<uint32_t>(len);
    barrier();
    txCurrent = static_cast<uint16_t>((txCurrent + 1) % RTL8169_TX_DESCRIPTORS);
    write8(REG_TX_POLL, TX_POLL_NORMAL);
    return true;
}

int RTL8169Driver::receivePacket(void* buffer, size_t maxLen) {
    if (!initialized || !buffer || maxLen == 0) {
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
            memcpy(buffer, rxBuffers[rxCurrent], copied);
            result = static_cast<int>(copied);
        }
        const bool last = rxCurrent == RTL8169_RX_DESCRIPTORS - 1;
        desc.opts2 = 0;
        barrier();
        desc.opts1 = DESC_OWN | (last ? DESC_EOR : 0) | static_cast<uint32_t>(RTL8169_RX_BUFFER_SIZE);
        barrier();
        rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % RTL8169_RX_DESCRIPTORS);
        if (result >= 0) {
            return result;
        }
    }
}

bool RTL8169Driver::isLinkUp() const {
    return initialized && (read8(REG_PHY_STATUS) & PHY_STATUS_LINK) != 0;
}

int RTL8169Driver::phyRead(uint8_t reg) {
    if (is8168gUp(macVersion)) {
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

void RTL8169Driver::phyWrite(uint8_t reg, uint16_t value) {
    if (is8168gUp(macVersion)) {
        write32(REG_GPHY_OCP, OCP_FLAG | (static_cast<uint32_t>(OCP_STD_PHY_BASE + reg * 2) << 15) | value);
        waitFor(REG_GPHY_OCP, OCP_FLAG, false, 25, 10);
        return;
    }
    write32(REG_PHYAR, (1u << 31) | (static_cast<uint32_t>(reg & 0x1F) << 16) | value);
    waitFor(REG_PHYAR, 1u << 31, false, 20, 25);
    delayUs(20);
}

uint16_t RTL8169Driver::macOcpRead(uint16_t reg) {
    write32(REG_OCPDR, static_cast<uint32_t>(reg) << 15);
    return static_cast<uint16_t>(read32(REG_OCPDR));
}

void RTL8169Driver::macOcpWrite(uint16_t reg, uint16_t value) {
    write32(REG_OCPDR, OCP_FLAG | (static_cast<uint32_t>(reg) << 15) | value);
}

void RTL8169Driver::macOcpModify(uint16_t reg, uint16_t clear, uint16_t set) {
    macOcpWrite(reg, static_cast<uint16_t>((macOcpRead(reg) & ~clear) | set));
}

uint32_t RTL8169Driver::eriRead(uint16_t addr) {
    write32(REG_ERIAR, ERIAR_MASK_ALL | addr);   // read, ExGMAC space
    if (!waitFor(REG_ERIAR, ERIAR_FLAG, true, 100, 100)) {
        return 0;
    }
    return read32(REG_ERIDR);
}

bool RTL8169Driver::waitFor(uint16_t reg, uint32_t mask, bool high, uint32_t polls,
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

uint8_t RTL8169Driver::read8(uint16_t reg) const {
    return *reinterpret_cast<volatile uint8_t*>(mmio + reg);
}

uint16_t RTL8169Driver::read16(uint16_t reg) const {
    return *reinterpret_cast<volatile uint16_t*>(mmio + reg);
}

uint32_t RTL8169Driver::read32(uint16_t reg) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio + reg);
}

void RTL8169Driver::write8(uint16_t reg, uint8_t value) const {
    *reinterpret_cast<volatile uint8_t*>(mmio + reg) = value;
}

void RTL8169Driver::write16(uint16_t reg, uint16_t value) const {
    *reinterpret_cast<volatile uint16_t*>(mmio + reg) = value;
}

void RTL8169Driver::write32(uint16_t reg, uint32_t value) const {
    *reinterpret_cast<volatile uint32_t*>(mmio + reg) = value;
}
