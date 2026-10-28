#include <drivers/net/e1000.hpp>
#include <cpu/acpi/pci.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/cereal/cereal.hpp>
#include <time/tsc_timer.hpp>

// Register names and bits follow Linux's e1000, e1000e and igb drivers.

namespace {
// PCI config-space register offsets.
constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;

constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

constexpr uint16_t INTEL_VENDOR_ID = 0x8086;

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

// Device Status bits.
constexpr uint32_t STATUS_LU = 1 << 1;    // Link Up
constexpr uint32_t STATUS_LAN_INIT_DONE = 1 << 9;
constexpr uint32_t STATUS_PHYRA = 1 << 10;

// Device Control bits.
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

// Receive Control bits.
constexpr uint32_t RCTL_EN = 1 << 1;      // Receiver Enable
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

// Interrupt causes (ICR/IMS bits): receive timer, RX descriptor minimum
// threshold, receiver overrun, link status change.
constexpr uint32_t INT_LSC = 1 << 2;
constexpr uint32_t INT_RXDMT0 = 1 << 4;
constexpr uint32_t INT_RXO = 1 << 6;
constexpr uint32_t INT_RXT0 = 1 << 7;
constexpr uint32_t INT_RECEIVE = INT_LSC | INT_RXDMT0 | INT_RXO | INT_RXT0;

using Family = E1000Driver::Family;

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
};

void mem_barrier() {
    asm volatile("" ::: "memory");
}

void delayUs(uint64_t us) {
    // The uptime runs on the TSC; the guard ends the wait should it not move.
    const uint64_t end = time_get_uptime_ns() + us * 1000;
    for (uint64_t guard = 0; time_get_uptime_ns() < end && guard < us * 1000; ++guard) {
        asm volatile("pause");
    }
}

class E1000InterruptHandler : public Interrupt {
public:
    void initialize() override {
    }

    bool shouldDispatch() override {
        return E1000Driver::get().claimInterrupt();
    }

    void Run(InterruptFrame* frame) override {
        // Same split as virtio-net: an MSI vector is ours and is EOId here,
        // the shared INTx dispatcher EOIs after all its handlers.
        E1000Driver& nic = E1000Driver::get();
        const bool msi = nic.usesMsi();
        if (msi) {
            nic.claimInterrupt();   // read ICR to acknowledge the causes
            sendEOI();
        }
        netDeviceInterrupt(frame, msi);
    }
};

E1000InterruptHandler& interruptHandler() {
    static E1000InterruptHandler handler;
    return handler;
}
}

E1000Driver& E1000Driver::get() {
    static E1000Driver instance;
    return instance;
}

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

bool E1000Driver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_VENDOR_ID_REG);
                if (vendor != INTEL_VENDOR_ID) {
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_DEVICE_ID_REG);
                for (size_t i = 0; i < sizeof(kDevices) / sizeof(kDevices[0]); ++i) {
                    if (kDevices[i].id != devId) {
                        continue;
                    }
                    bus = static_cast<uint8_t>(b);
                    device = d;
                    function = f;
                    pciDeviceId = devId;
                    family = kDevices[i].family;
                    chipName = kDevices[i].name;
                    // The I217, I218 and I219 are the PCH parts named "i...".
                    pchLpt = family == Family::Pch && chipName[0] == 'i';
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

    // BAR0 is the 128 KiB register space.
    const uint64_t base = pci.mapMemoryBar(0, bus, device, function, 0, 0x20000);
    if (base == 0) {
        return false;
    }
    mmio = reinterpret_cast<volatile uint8_t*>(base);
    rxBase = family == Family::Igb ? 0xC000 : 0x2800;
    txBase = family == Family::Igb ? 0xE000 : 0x3800;

    // Mask all interrupts until the rings are set up (setupInterrupts).
    mmioWrite(REG_IMC, 0xFFFFFFFFu);
    (void)mmioRead(REG_ICR);  // clear pending causes

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

    if (!setupRx()) {
        return false;
    }
    if (!setupTx()) {
        return false;
    }

    rxCurrent = 0;
    txCurrent = 0;
    setupInterrupts();
    return true;
}

void E1000Driver::resetMac() {
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
void E1000Driver::pchPhyWorkarounds() {
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
            Cereal::get().write("[net] e1000: PHY unreachable and its reset is blocked by the ME\n");
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
    Cereal::get().write(accessible ? "[net] e1000: PHY answers\n"
                                   : "[net] e1000: PHY does not answer\n");
}

// Cycle the LANPHYPC pin: the PHY powers back up and the interconnect comes up
// in PCIe mode.
void E1000Driver::togglePhyPowerPin() {
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
bool E1000Driver::phyAccessible() {
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

bool E1000Driver::acquireSwFlag() {
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
    Cereal::get().write("[net] e1000: firmware holds the PHY semaphore\n");
    return false;
}

void E1000Driver::releaseSwFlag() {
    mmioWrite(REG_EXTCNF_CTRL, mmioRead(REG_EXTCNF_CTRL) & ~EXTCNF_CTRL_SWFLAG);
}

// A PHY the firmware powered down or isolated, or without a link after the
// reset (the igb's, QEMU's too, waits for this), gets autonegotiation
// (re)started; one with a link is left alone, since a restart costs seconds.
void E1000Driver::startPhy() {
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
        Cereal::get().write("[net] e1000: PHY autonegotiating\n");
    }
    if (flag) {
        releaseSwFlag();
    }
}

bool E1000Driver::mdicRead(uint8_t reg, uint16_t& value) {
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

bool E1000Driver::mdicWrite(uint8_t reg, uint16_t value) {
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

void E1000Driver::setupInterrupts() {
    irqMode = IRQMode::None;
    if (PCI::get().registerMSIInterrupt(0, bus, device, function, &interruptHandler())) {
        irqMode = IRQMode::Msi;
    } else if (PCI::get().registerLegacyInterrupt(0, bus, device, function, &interruptHandler())) {
        irqMode = IRQMode::Legacy;
    }
    if (irqMode != IRQMode::None) {
        if (family != Family::Igb) {
            mmioWrite(REG_RDTR, 0);   // interrupt as soon as a frame lands
        }
        (void)mmioRead(REG_ICR);
        mmioWrite(REG_IMS, INT_RECEIVE);
    }
    Cereal::get().write("[net] e1000: ");
    Cereal::get().write(chipName);
    Cereal::get().write(irqMode == IRQMode::Msi ? ", MSI receive interrupt\n"
                        : irqMode == IRQMode::Legacy ? ", INTx receive interrupt\n"
                        : ", no interrupt, polling\n");
}

bool E1000Driver::claimInterrupt() {
    // ICR is read-to-clear: it both says whether we raised a (possibly
    // shared) INTx and deasserts it.
    return mmio && mmioRead(REG_ICR) != 0;
}

void E1000Driver::readMacAddress() {
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
bool E1000Driver::enableQueue(uint32_t reg) {
    mmioWrite(reg, mmioRead(reg) | QUEUE_ENABLE);
    for (uint32_t i = 0; i < 100; ++i) {
        if ((mmioRead(reg) & QUEUE_ENABLE) != 0) {
            return true;
        }
        delayUs(100);
    }
    Cereal::get().write("[net] e1000: queue did not enable\n");
    return false;
}

bool E1000Driver::setupRx() {
    // Allocate the descriptor ring (must be 16-byte aligned; a page is fine).
    const uint64_t ringBytes = E1000_RX_DESCRIPTORS * sizeof(E1000RxDesc);
    const uint64_t ringFrames = (ringBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t rxDescsPhys = PMM::AllocFrames(ringFrames);
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
        rxBuffers[i] = reinterpret_cast<uint8_t*>(phys);
        rxDescs[i].addr = phys;
        rxDescs[i].status = 0;   // igb: the header address, 0
    }

    if (family == Family::Igb) {
        mmioWrite(rxBase + Q_DCTL, 0);
    }
    mmioWrite(rxBase + Q_BAL, static_cast<uint32_t>(rxDescsPhys & 0xFFFFFFFFu));
    mmioWrite(rxBase + Q_BAH, static_cast<uint32_t>(rxDescsPhys >> 32));
    mmioWrite(rxBase + Q_LEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(rxBase + Q_HEAD, 0);
    if (family == Family::Igb) {
        mmioWrite(rxBase + Q_TAIL, 0);
        mmioWrite(rxBase + Q_SRRCTL, SRRCTL_BSIZE_2K | SRRCTL_DESCTYPE_ADV_ONEBUF | SRRCTL_DROP_EN);
        if (!enableQueue(rxBase + Q_DCTL)) {
            return false;
        }
    }
    mmioWrite(rxBase + Q_TAIL, E1000_RX_DESCRIPTORS - 1);

    // Enable receiver: broadcast accept, strip CRC, 2 KiB buffers.
    mmioWrite(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    return true;
}

bool E1000Driver::setupTx() {
    const uint64_t ringBytes = E1000_TX_DESCRIPTORS * sizeof(E1000TxDesc);
    const uint64_t ringFrames = (ringBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t txDescsPhys = PMM::AllocFrames(ringFrames);
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
        // Mark all free initially (DD; bit 0 of the advanced write-back too).
        if (family == Family::Igb) {
            reinterpret_cast<E1000AdvTxDesc*>(&txDescs[i])->olinfoStatus = ADV_TX_STATUS_DD;
        } else {
            txDescs[i].status = TX_STATUS_DD;
        }
    }

    if (family == Family::Igb) {
        mmioWrite(txBase + Q_DCTL, 0);
    }
    mmioWrite(txBase + Q_BAL, static_cast<uint32_t>(txDescsPhys & 0xFFFFFFFFu));
    mmioWrite(txBase + Q_BAH, static_cast<uint32_t>(txDescsPhys >> 32));
    mmioWrite(txBase + Q_LEN, static_cast<uint32_t>(ringBytes));
    mmioWrite(txBase + Q_HEAD, 0);
    mmioWrite(txBase + Q_TAIL, 0);
    if (family == Family::Igb && !enableQueue(txBase + Q_DCTL)) {
        return false;
    }

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
    auto* adv = reinterpret_cast<volatile E1000AdvTxDesc*>(&txDescs[desc]);
    const bool igb = family == Family::Igb;

    // Wait for the descriptor to be free (DD set) before reusing it.
    for (uint32_t spin = 0; spin < TX_DD_SPIN_LIMIT; ++spin) {
        const bool done = igb ? (adv->olinfoStatus & ADV_TX_STATUS_DD) != 0
                              : (txDescs[desc].status & TX_STATUS_DD) != 0;
        if (done) {
            break;
        }
        if (spin + 1 == TX_DD_SPIN_LIMIT) {
            return false;
        }
        mem_barrier();
    }

    memcpy(txBuffers[desc], data, len);
    if (igb) {
        adv->addr = txBuffersPhys[desc];
        adv->cmdTypeLen = static_cast<uint32_t>(len) | ADV_TX_DTYP_DATA | ADV_TX_CMD_DEXT |
                          ADV_TX_CMD_EOP | ADV_TX_CMD_IFCS | ADV_TX_CMD_RS;
        adv->olinfoStatus = static_cast<uint32_t>(len) << ADV_TX_PAYLEN_SHIFT;
    } else {
        txDescs[desc].addr = txBuffersPhys[desc];
        txDescs[desc].length = static_cast<uint16_t>(len);
        txDescs[desc].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
        txDescs[desc].status = 0;
    }
    mem_barrier();

    // Advance the tail to hand the descriptor to the NIC.
    txCurrent = static_cast<uint16_t>((desc + 1) % E1000_TX_DESCRIPTORS);
    mmioWrite(txBase + Q_TAIL, txCurrent);
    return true;
}

int E1000Driver::receivePacket(void* buffer, size_t maxLen) {
    if (!initialized || !buffer || maxLen == 0) {
        return -1;
    }
    if (family == Family::Igb) {
        return receiveAdvanced(buffer, maxLen);
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
    mmioWrite(rxBase + Q_TAIL, rxCurrent);
    rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % E1000_RX_DESCRIPTORS);
    return result;
}

// igb: advanced descriptors (QEMU's igb writes back nothing else). The
// write-back overwrites the buffer address, so each one is handed back whole.
int E1000Driver::receiveAdvanced(void* buffer, size_t maxLen) {
    auto* desc = reinterpret_cast<volatile E1000AdvRxDesc*>(&rxDescs[rxCurrent]);
    const uint32_t status = desc->statusError;
    if ((status & ADV_RX_STATUS_DD) == 0) {
        return -1;  // no packet ready
    }
    mem_barrier();
    int result = -1;
    if ((status & ADV_RX_STATUS_EOP) != 0) {
        const size_t length = desc->length;
        const size_t copied = length < maxLen ? length : maxLen;
        if (copied != 0) {
            memcpy(buffer, rxBuffers[rxCurrent], copied);
        }
        result = static_cast<int>(copied);
    }
    desc->packetAddr = reinterpret_cast<uint64_t>(rxBuffers[rxCurrent]);
    desc->headerAddr = 0;
    mem_barrier();
    mmioWrite(rxBase + Q_TAIL, rxCurrent);
    rxCurrent = static_cast<uint16_t>((rxCurrent + 1) % E1000_RX_DESCRIPTORS);
    return result;
}

bool E1000Driver::isLinkUp() const {
    if (!initialized) {
        return false;
    }
    return (mmioRead(REG_STATUS) & STATUS_LU) != 0;
}

uint32_t E1000Driver::mmioRead(uint32_t reg) const {
    return *reinterpret_cast<volatile uint32_t*>(mmio + reg);
}

void E1000Driver::mmioWrite(uint32_t reg, uint32_t value) const {
    *reinterpret_cast<volatile uint32_t*>(mmio + reg) = value;
}
