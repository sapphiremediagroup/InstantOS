// Realtek RTL8139 (RTL8139C/8139D) Fast Ethernet, as a loadable module
// (rtl8139.idrv). It was the kernel's src/drivers/net/rtl8139.cpp; the
// hardware side is unchanged. Emulated by QEMU (-device rtl8139), VirtualBox
// and others: the PCI I/O BAR, one linear receive ring and four transmit
// descriptors. Polled (the interrupt mask stays zero).

#include "../sdk/idrv_sdk.h"

namespace {

constexpr uint16_t RTL_VENDOR = 0x10EC;
constexpr uint16_t RTL8139_DEVICE = 0x8139;

// Receive buffer: 8 KiB ring + 16 byte slack for the WRAP overflow + room for
// a full frame past the wrap point.
constexpr size_t RX_BUFFER_SIZE = 8192 + 16 + 1536;
constexpr size_t TX_DESCRIPTORS = 4;   // fixed by the chip
constexpr size_t MTU = 1514;

// Registers (relative to the I/O BAR).
constexpr uint16_t REG_IDR0 = 0x00;       // MAC address (6 bytes)
constexpr uint16_t REG_TSD0 = 0x10;       // Transmit Status of Descriptor 0..3
constexpr uint16_t REG_TSAD0 = 0x20;      // Transmit Start Address of Desc 0..3
constexpr uint16_t REG_RBSTART = 0x30;    // Receive Buffer Start (physical addr)
constexpr uint16_t REG_CR = 0x37;         // Command Register
constexpr uint16_t REG_CAPR = 0x38;       // Current Address of Packet Read
constexpr uint16_t REG_IMR = 0x3C;        // Interrupt Mask Register
constexpr uint16_t REG_ISR = 0x3E;        // Interrupt Status Register
constexpr uint16_t REG_RCR = 0x44;        // Receive Configuration Register
constexpr uint16_t REG_CONFIG1 = 0x52;    // Configuration Register 1
constexpr uint16_t REG_MSR = 0x58;        // Media Status Register

constexpr uint8_t CR_BUFE = 1 << 0;       // Receive Buffer Empty
constexpr uint8_t CR_TE = 1 << 2;         // Transmitter Enable
constexpr uint8_t CR_RE = 1 << 3;         // Receiver Enable
constexpr uint8_t CR_RST = 1 << 4;        // Reset

constexpr uint32_t TSD_OWN = 1 << 13;     // 0 while DMA in progress, 1 when done
constexpr uint32_t TSD_SIZE_MASK = 0x1FFF;

constexpr uint32_t RCR_APM = 1 << 1;      // Accept Physical Match
constexpr uint32_t RCR_AM = 1 << 2;       // Accept Multicast
constexpr uint32_t RCR_AB = 1 << 3;       // Accept Broadcast
constexpr uint32_t RCR_WRAP = 1 << 7;     // write a frame past the ring's end (into the headroom)

constexpr uint16_t RX_STATUS_ROK = 1 << 0;  // Receive OK
constexpr uint8_t MSR_LINKB = 1 << 2;       // 0 = link up, 1 = link down (inverse)

constexpr size_t ETH_MIN_FRAME = 60;
constexpr uint32_t TX_OWN_SPIN_LIMIT = 1000000;

struct Nic {
    bool bound;
    uint16_t ioBase;
    uint64_t rxBufferPhys;
    uint8_t* rxBuffer;
    uint64_t txBufferPhys[TX_DESCRIPTORS];
    uint8_t* txBuffer[TX_DESCRIPTORS];
    uint16_t rxOffset;   // software read offset into the RX ring (mirrors CAPR)
    uint8_t txCurrent;   // next TX descriptor (round-robin 0..3)
    uint8_t mac[6];
};

Nic gNic;

uint8_t read8(const Nic& n, uint16_t reg) { return idrv_inb(static_cast<uint16_t>(n.ioBase + reg)); }
uint32_t read32(const Nic& n, uint16_t reg) { return idrv_inl(static_cast<uint16_t>(n.ioBase + reg)); }
void write8(const Nic& n, uint16_t reg, uint8_t v) { idrv_outb(static_cast<uint16_t>(n.ioBase + reg), v); }
void write16(const Nic& n, uint16_t reg, uint16_t v) { idrv_outw(static_cast<uint16_t>(n.ioBase + reg), v); }
void write32(const Nic& n, uint16_t reg, uint32_t v) { idrv_outl(static_cast<uint16_t>(n.ioBase + reg), v); }

void macAddress(void* context, uint8_t mac[6]) {
    memcpy(mac, static_cast<Nic*>(context)->mac, 6);
}

int send(void* context, const void* data, size_t len) {
    Nic& n = *static_cast<Nic*>(context);
    if (!data || len == 0 || len > MTU) {
        return 0;
    }
    const uint8_t desc = n.txCurrent;
    const uint16_t tsd = static_cast<uint16_t>(REG_TSD0 + desc * 4);
    // OWN == 1: the previous send from this descriptor completed.
    for (uint32_t spin = 0; (read32(n, tsd) & TSD_OWN) == 0; ++spin) {
        if (spin + 1 == TX_OWN_SPIN_LIMIT) {
            return 0;   // descriptor never freed
        }
        idrv_barrier();
    }
    uint8_t* buf = n.txBuffer[desc];
    memcpy(buf, data, len);
    size_t frameLen = len;
    if (frameLen < ETH_MIN_FRAME) {
        memset(buf + frameLen, 0, ETH_MIN_FRAME - frameLen);
        frameLen = ETH_MIN_FRAME;
    }
    // Start address, then the size (clearing OWN starts the DMA).
    write32(n, static_cast<uint16_t>(REG_TSAD0 + desc * 4), static_cast<uint32_t>(n.txBufferPhys[desc]));
    write32(n, tsd, static_cast<uint32_t>(frameLen) & TSD_SIZE_MASK);
    n.txCurrent = static_cast<uint8_t>((desc + 1) % TX_DESCRIPTORS);
    return 1;
}

int receive(void* context, void* buffer, size_t maxLen) {
    Nic& n = *static_cast<Nic*>(context);
    if ((read8(n, REG_CR) & CR_BUFE) != 0) {
        return -1;   // the receive buffer is empty
    }
    // Each entry: 2-byte status, 2-byte length, then the frame and its CRC.
    uint8_t* entry = n.rxBuffer + n.rxOffset;
    uint16_t status = 0;
    uint16_t length = 0;
    memcpy(&status, entry, sizeof(status));
    memcpy(&length, entry + 2, sizeof(length));
    if (length < 4 || length > RX_BUFFER_SIZE) {
        // A desync: start over rather than loop forever.
        n.rxOffset = 0;
        write16(n, REG_CAPR, static_cast<uint16_t>(n.rxOffset - 16));
        return -1;
    }
    int result = -1;
    if ((status & RX_STATUS_ROK) != 0) {
        const size_t payload = static_cast<size_t>(length) - 4;   // without the CRC
        const size_t copied = payload < maxLen ? payload : maxLen;
        if (copied != 0) {
            memcpy(buffer, entry + 4, copied);
        }
        result = static_cast<int>(copied);
    }
    // Header + length, 4-aligned, wrapping in the 8 KiB ring; CAPR lags by 16.
    n.rxOffset = static_cast<uint16_t>(((n.rxOffset + length + 4 + 3) & ~3u) % 8192);
    write16(n, REG_CAPR, static_cast<uint16_t>(n.rxOffset - 16));
    return result;
}

int linkUp(void* context) {
    return (read8(*static_cast<Nic*>(context), REG_MSR) & MSR_LINKB) == 0;
}

int polled(void*) {
    return 0;
}

const IdrvNetOps kNetOps = {
    "rtl8139", 60, 0, macAddress, send, receive, linkUp, polled,
};

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    Nic& n = gNic;
    if (n.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    const uint32_t command = api->pciRead(dev, IDRV_PCI_COMMAND, 2);
    api->pciWrite(dev, IDRV_PCI_COMMAND, 2, command | IDRV_PCI_COMMAND_IO | IDRV_PCI_COMMAND_BUS_MASTER);
    const uint32_t bar0 = api->pciRead(dev, IDRV_PCI_BAR0, 4);
    if ((bar0 & 1) == 0 || (bar0 & ~0x3u) == 0) {
        api->log("[net] rtl8139: BAR0 is not an I/O BAR\n");
        return IDRV_ENODEV;
    }
    n.ioBase = static_cast<uint16_t>(bar0 & ~0x3u);

    write8(n, REG_CONFIG1, 0x00);   // power on (LWAKE and PMEn off)
    write8(n, REG_CR, CR_RST);
    for (uint32_t i = 0; i < 1000000 && (read8(n, REG_CR) & CR_RST) != 0; ++i) {
        idrv_barrier();
    }
    if ((read8(n, REG_CR) & CR_RST) != 0) {
        api->log("[net] rtl8139: reset did not finish\n");
        return IDRV_EIO;
    }

    // RBSTART and TSAD are 32-bit: every buffer below 4 GiB.
    void* virt = nullptr;
    n.rxBufferPhys = api->dmaAlloc((RX_BUFFER_SIZE + 4095) / 4096, 0x100000000ULL, &virt);
    n.rxBuffer = static_cast<uint8_t*>(virt);
    void* txVirt = nullptr;
    const uint64_t txPhys = api->dmaAlloc(TX_DESCRIPTORS, 0x100000000ULL, &txVirt);   // a page each
    if (n.rxBufferPhys == 0 || txPhys == 0) {
        api->log("[net] rtl8139: no memory below 4 GiB for the buffers\n");
        return IDRV_ENOMEM;
    }
    for (size_t i = 0; i < TX_DESCRIPTORS; ++i) {
        n.txBufferPhys[i] = txPhys + i * 4096;
        n.txBuffer[i] = static_cast<uint8_t*>(txVirt) + i * 4096;
    }

    write32(n, REG_RBSTART, static_cast<uint32_t>(n.rxBufferPhys));
    write16(n, REG_IMR, 0x0000);   // polled
    // Broadcast, multicast and our address; 8 KiB buffer (RBLEN=00),
    // unlimited DMA bursts, and WRAP set: a frame that reaches the ring's end
    // runs on into the headroom after it, so each frame is contiguous. With
    // WRAP clear the chip continues it at the ring's start, which the copy
    // below did not expect: every frame across the end came out garbled (the
    // kernel's driver had that bug; the suite's large transfers caught it).
    write32(n, REG_RCR, RCR_APM | RCR_AB | RCR_AM | RCR_WRAP | (0u << 11) | (7u << 8) | (7u << 13));
    write8(n, REG_CR, CR_RE | CR_TE);
    write16(n, REG_ISR, 0xFFFF);
    for (uint8_t i = 0; i < 6; ++i) {
        n.mac[i] = read8(n, static_cast<uint16_t>(REG_IDR0 + i));
    }
    n.rxOffset = 0;
    n.txCurrent = 0;

    api->log("[net] rtl8139: MAC ");
    for (int i = 0; i < 6; ++i) {
        char text[3] = {"0123456789abcdef"[n.mac[i] >> 4], "0123456789abcdef"[n.mac[i] & 0xF], '\0'};
        api->log(text);
        api->log(i < 5 ? ":" : ", polled\n");
    }
    n.bound = true;
    return api->netRegister(&kNetOps, &n);
}

const IdrvPciMatch kMatches[] = {
    {RTL_VENDOR, RTL8139_DEVICE, 0, 0},
};

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "rtl8139",
    "1.0",
    "Realtek RTL8139 Fast Ethernet",
    "InstantOS",
    kMatches,
    sizeof(kMatches) / sizeof(kMatches[0]),
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
