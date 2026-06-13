#include <drivers/net/rtl8139.hpp>
#include <cpu/acpi/pci.hpp>
#include <common/ports.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>

namespace {
// PCI config-space register offsets.
constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;
constexpr uint16_t PCI_BAR0_REG = 0x10;

constexpr uint16_t PCI_COMMAND_IO = 1 << 0;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;

// RTL8139 register offsets (relative to the I/O BAR base).
constexpr uint16_t REG_IDR0 = 0x00;       // MAC address (6 bytes)
constexpr uint16_t REG_TSD0 = 0x10;       // Transmit Status of Descriptor 0..3
constexpr uint16_t REG_TSAD0 = 0x20;      // Transmit Start Address of Desc 0..3
constexpr uint16_t REG_RBSTART = 0x30;    // Receive Buffer Start (physical addr)
constexpr uint16_t REG_CR = 0x37;         // Command Register
constexpr uint16_t REG_CAPR = 0x38;       // Current Address of Packet Read
constexpr uint16_t REG_CBR = 0x3A;        // Current Buffer Address
constexpr uint16_t REG_IMR = 0x3C;        // Interrupt Mask Register
constexpr uint16_t REG_ISR = 0x3E;        // Interrupt Status Register
constexpr uint16_t REG_TCR = 0x40;        // Transmit Configuration Register
constexpr uint16_t REG_RCR = 0x44;        // Receive Configuration Register
constexpr uint16_t REG_CONFIG1 = 0x52;    // Configuration Register 1
constexpr uint16_t REG_MSR = 0x58;        // Media Status Register

// Command register bits.
constexpr uint8_t CR_BUFE = 1 << 0;       // Receive Buffer Empty
constexpr uint8_t CR_TE = 1 << 2;         // Transmitter Enable
constexpr uint8_t CR_RE = 1 << 3;         // Receiver Enable
constexpr uint8_t CR_RST = 1 << 4;        // Reset

// Transmit Status of Descriptor bits.
constexpr uint32_t TSD_OWN = 1 << 13;     // 0 while DMA in progress, 1 when done
constexpr uint32_t TSD_TOK = 1 << 15;     // Transmit OK
constexpr uint32_t TSD_SIZE_MASK = 0x1FFF;

// Receive Configuration Register bits.
constexpr uint32_t RCR_AAP = 1 << 0;      // Accept All Packets (promiscuous)
constexpr uint32_t RCR_APM = 1 << 1;      // Accept Physical Match
constexpr uint32_t RCR_AM = 1 << 2;       // Accept Multicast
constexpr uint32_t RCR_AB = 1 << 3;       // Accept Broadcast
constexpr uint32_t RCR_WRAP = 1 << 7;     // Wrap (no chip-side wrap; we pad)
// RBLEN = 00 -> 8 KiB + 16 receive buffer.

// Receive packet header status bits (first 16-bit word of each RX entry).
constexpr uint16_t RX_STATUS_ROK = 1 << 0;  // Receive OK

// Media Status Register bits.
constexpr uint8_t MSR_LINKB = 1 << 2;     // 0 = link up, 1 = link down (inverse)

constexpr size_t ETH_MIN_FRAME = 60;      // Minimum frame size (pad with zeros)
constexpr uint32_t TX_OWN_SPIN_LIMIT = 1000000;

void io_delay() {
    asm volatile("" ::: "memory");
}
}

RTL8139Driver& RTL8139Driver::get() {
    static RTL8139Driver instance;
    return instance;
}

RTL8139Driver::RTL8139Driver()
    : initialized(false),
      deviceFound(false),
      bus(0),
      device(0),
      function(0),
      ioBase(0),
      rxBufferPhys(0),
      rxBuffer(nullptr),
      txBufferPhys{},
      txBuffer{},
      rxOffset(0),
      txCurrent(0),
      macAddr{} {}

bool RTL8139Driver::initialize() {
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

bool RTL8139Driver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_VENDOR_ID_REG);
                if (vendor == 0xFFFF) {
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_DEVICE_ID_REG);
                if (vendor == RTL8139_VENDOR_ID && devId == RTL8139_DEVICE_ID) {
                    bus = static_cast<uint8_t>(b);
                    device = d;
                    function = f;
                    deviceFound = true;
                    return true;
                }
            }
        }
    }
    return false;
}

bool RTL8139Driver::initDevice() {
    PCI& pci = PCI::get();

    // Enable I/O space and bus mastering.
    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND_REG);
    command |= PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER;
    pci.writeConfig16(0, bus, device, function, PCI_COMMAND_REG, command);

    // BAR0 is an I/O BAR; bit 0 set indicates I/O space.
    const uint32_t bar0 = pci.readConfig32(0, bus, device, function, PCI_BAR0_REG);
    if ((bar0 & 0x1) == 0) {
        return false;  // expected an I/O BAR
    }
    ioBase = static_cast<uint16_t>(bar0 & ~0x3U);
    if (ioBase == 0) {
        return false;
    }

    // Power on the device (clear CONFIG1 LWAKE + PMEn -> 0x00).
    write8(REG_CONFIG1, 0x00);

    // Software reset: set RST and wait for it to clear.
    write8(REG_CR, CR_RST);
    for (uint32_t i = 0; i < 1000000; ++i) {
        if ((read8(REG_CR) & CR_RST) == 0) {
            break;
        }
        io_delay();
    }
    if ((read8(REG_CR) & CR_RST) != 0) {
        return false;
    }

    // Allocate the receive ring.  RBLEN=00 needs 8 KiB+16, plus headroom so a
    // packet that starts near the end can be written without WRAP support.
    const uint64_t rxFrames = (RTL8139_RX_BUFFER_SIZE + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    rxBufferPhys = PMM::AllocFrames(rxFrames);
    if (rxBufferPhys == 0) {
        return false;
    }
    rxBuffer = reinterpret_cast<uint8_t*>(rxBufferPhys);
    memset(rxBuffer, 0, rxFrames * PMM::PAGE_SIZE);

    // Allocate transmit buffers (one page each, well above the 1514 MTU).
    for (size_t i = 0; i < RTL8139_TX_DESCRIPTORS; ++i) {
        const uint64_t phys = PMM::AllocFrames(1);
        if (phys == 0) {
            return false;
        }
        txBufferPhys[i] = phys;
        txBuffer[i] = reinterpret_cast<uint8_t*>(phys);
        memset(txBuffer[i], 0, PMM::PAGE_SIZE);
    }

    // Program the receive buffer physical address (32-bit register; the
    // identity-mapped physical address must fit in 32 bits).
    write32(REG_RBSTART, static_cast<uint32_t>(rxBufferPhys));

    // Mask all interrupts: this driver polls.
    write16(REG_IMR, 0x0000);

    // Receive configuration: accept broadcast/multicast/physical-match packets,
    // 8 KiB buffer (RBLEN=00), unlimited DMA burst, no WRAP (we provide
    // headroom so the chip never needs to wrap a single packet).
    write32(REG_RCR, RCR_APM | RCR_AB | RCR_AM | (0u << 11) | (7u << 8) | (7u << 13));

    // Enable receiver and transmitter.
    write8(REG_CR, CR_RE | CR_TE);

    // Clear any pending interrupt status.
    write16(REG_ISR, 0xFFFF);

    // Read the MAC address from IDR0..IDR5.
    for (uint8_t i = 0; i < 6; ++i) {
        macAddr[i] = read8(REG_IDR0 + i);
    }

    rxOffset = 0;
    txCurrent = 0;
    return true;
}

void RTL8139Driver::getMacAddress(uint8_t* mac) {
    if (!mac) {
        return;
    }
    memcpy(mac, macAddr, sizeof(macAddr));
}

bool RTL8139Driver::sendPacket(const void* data, size_t len) {
    if (!initialized || !data || len == 0 || len > NET_DEVICE_MTU) {
        return false;
    }

    const uint8_t desc = txCurrent;
    const uint16_t tsd = static_cast<uint16_t>(REG_TSD0 + desc * 4);

    // Wait until this descriptor is free (OWN == 1 means the previous send
    // completed and the descriptor is available again).
    for (uint32_t spin = 0; spin < TX_OWN_SPIN_LIMIT; ++spin) {
        const uint32_t status = read32(tsd);
        if ((status & TSD_OWN) != 0) {
            break;
        }
        if (spin + 1 == TX_OWN_SPIN_LIMIT) {
            return false;  // descriptor never freed
        }
        io_delay();
    }

    // Copy the frame into the transmit buffer, zero-padding to the minimum
    // Ethernet frame size.
    uint8_t* buf = txBuffer[desc];
    memcpy(buf, data, len);
    size_t frameLen = len;
    if (frameLen < ETH_MIN_FRAME) {
        memset(buf + frameLen, 0, ETH_MIN_FRAME - frameLen);
        frameLen = ETH_MIN_FRAME;
    }

    // Program the descriptor start address, then write the size (clearing OWN
    // kicks off the DMA).  Early TX threshold = 0 (whole packet).
    write32(static_cast<uint16_t>(REG_TSAD0 + desc * 4), static_cast<uint32_t>(txBufferPhys[desc]));
    write32(tsd, static_cast<uint32_t>(frameLen) & TSD_SIZE_MASK);

    txCurrent = static_cast<uint8_t>((desc + 1) % RTL8139_TX_DESCRIPTORS);
    return true;
}

int RTL8139Driver::receivePacket(void* buffer, size_t maxLen) {
    if (!initialized || !buffer || maxLen == 0) {
        return -1;
    }

    // BUFE set means the receive buffer is empty.
    if ((read8(REG_CR) & CR_BUFE) != 0) {
        return -1;
    }

    // Each RX entry: 2-byte status, 2-byte length, then the frame (incl. the
    // 4-byte CRC appended by the chip).
    uint8_t* entry = rxBuffer + rxOffset;
    uint16_t status = 0;
    uint16_t length = 0;
    memcpy(&status, entry, sizeof(status));
    memcpy(&length, entry + 2, sizeof(length));

    // A reported length below the header or above the ring is a desync; reset
    // the read pointer so we recover rather than loop forever.
    if (length < 4 || length > RTL8139_RX_BUFFER_SIZE) {
        rxOffset = 0;
        write16(REG_CAPR, static_cast<uint16_t>(rxOffset - 16));
        return -1;
    }

    int result = -1;
    if ((status & RX_STATUS_ROK) != 0) {
        // Strip the 4-byte CRC the chip appends.
        const size_t payloadLength = length >= 4 ? static_cast<size_t>(length) - 4 : 0;
        const size_t copied = payloadLength < maxLen ? payloadLength : maxLen;
        if (copied != 0) {
            memcpy(buffer, entry + 4, copied);
        }
        result = static_cast<int>(copied);
    }

    // Advance past this packet: header (4) + length, aligned up to 4 bytes,
    // wrapping within the 8 KiB ring.
    rxOffset = static_cast<uint16_t>((rxOffset + length + 4 + 3) & ~3u);
    rxOffset %= 8192;

    // Tell the chip where we have read up to (CAPR lags CBR by 16 bytes).
    write16(REG_CAPR, static_cast<uint16_t>(rxOffset - 16));

    return result;
}

bool RTL8139Driver::isLinkUp() const {
    if (!initialized) {
        return false;
    }
    // MSR LINKB: 0 = link present, 1 = link down.
    return (read8(REG_MSR) & MSR_LINKB) == 0;
}

uint8_t RTL8139Driver::read8(uint16_t reg) const {
    return inb(static_cast<uint16_t>(ioBase + reg));
}

uint16_t RTL8139Driver::read16(uint16_t reg) const {
    return inw(static_cast<uint16_t>(ioBase + reg));
}

uint32_t RTL8139Driver::read32(uint16_t reg) const {
    return inl(static_cast<uint16_t>(ioBase + reg));
}

void RTL8139Driver::write8(uint16_t reg, uint8_t value) const {
    outb(static_cast<uint16_t>(ioBase + reg), value);
}

void RTL8139Driver::write16(uint16_t reg, uint16_t value) const {
    outw(static_cast<uint16_t>(ioBase + reg), value);
}

void RTL8139Driver::write32(uint16_t reg, uint32_t value) const {
    outl(static_cast<uint16_t>(ioBase + reg), value);
}
