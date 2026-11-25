// AMD PCnet-PCI II/III (Am79C970A/Am79C973): VirtualBox's and older VMware's
// default NIC, as a loadable module (pcnet.idrv). It was the kernel's
// src/drivers/net/pcnet.cpp; the hardware side is unchanged. Register numbers
// and bits: AMD's data sheets (Linux's pcnet32.c uses the same names).

#include "../sdk/idrv_sdk.h"

namespace {

constexpr uint16_t AMD_VENDOR = 0x1022;
constexpr uint16_t PCNET_DEVICE = 0x2000;

// Powers of two, at most 512. A frame that finds no free receive descriptor
// is dropped (QEMU's model too), and a 64 KiB UDP datagram comes back as 45
// fragments at once: with 32 the suite's large echo failed now and then.
constexpr size_t RX_RING = 128;
constexpr size_t TX_RING = 32;
constexpr size_t BUFFER = 1536;
constexpr size_t MTU = 1514;

// 16-bit (WIO) port layout.
constexpr uint16_t PORT_APROM = 0x00;   // the station address PROM, 16 bytes
constexpr uint16_t PORT_RDP = 0x10;
constexpr uint16_t PORT_RAP = 0x12;
constexpr uint16_t PORT_RESET = 0x14;   // reading resets the chip
constexpr uint16_t PORT_BDP = 0x16;
constexpr uint16_t PORT_RESET32 = 0x18; // the same in 32-bit (DWIO) mode

constexpr uint16_t CSR0_INIT = 1 << 0;
constexpr uint16_t CSR0_STRT = 1 << 1;
constexpr uint16_t CSR0_STOP = 1 << 2;
constexpr uint16_t CSR0_TDMD = 1 << 3;
constexpr uint16_t CSR0_IENA = 1 << 6;
constexpr uint16_t CSR0_INTR = 1 << 7;
constexpr uint16_t CSR0_IDON = 1 << 8;
constexpr uint16_t CSR0_STATUS = 0x7F00;   // IDON TINT RINT MERR MISS CERR BABL: write one to clear
constexpr uint16_t CSR3_IDONM = 1 << 8;    // no interrupt for init done
constexpr uint16_t CSR3_TINTM = 1 << 9;    // nor for each frame sent
constexpr uint16_t CSR4_APAD_XMT = 1 << 11;   // the chip pads short frames
constexpr uint16_t CSR4_ASTRP_RCV = 1 << 10;  // and strips the padding of received ones
constexpr uint16_t BCR20_SWSTYLE2 = 2;     // 32-bit PCnet-PCI descriptors
constexpr uint16_t BCR4_LEDOUT = 1 << 15;  // LED0 (link status by default)

constexpr uint32_t DESC_OWN = 1u << 31;
constexpr uint32_t DESC_ERR = 1u << 30;
constexpr uint32_t DESC_STP = 1u << 25;
constexpr uint32_t DESC_ENP = 1u << 24;
constexpr uint32_t DESC_ONES = 0xF000;     // bits 15:12 of the byte count field
constexpr size_t ETH_CRC = 4;

// SWSTYLE 2: 16-byte descriptors, the buffer address first.
struct Desc {
    uint32_t addr;
    uint32_t flags;    // OWN, ERR, STP, ENP, ..., BCNT (negative buffer size, low 12 bits)
    uint32_t misc;     // RX: message length (low 12 bits); TX: error bits
    uint32_t user;
} __attribute__((packed));
static_assert(sizeof(Desc) == 16);

// The initialization block for SSIZE32 (SWSTYLE 2).
struct InitBlock {
    uint16_t mode;
    uint8_t rlen;      // log2(RX descriptors) << 4
    uint8_t tlen;      // log2(TX descriptors) << 4
    uint8_t mac[6];
    uint16_t reserved;
    uint8_t ladrf[8];  // multicast hash filter
    uint32_t rdra;
    uint32_t tdra;
} __attribute__((packed));
static_assert(sizeof(InitBlock) == 28);

constexpr size_t OFF_INIT = 0;
constexpr size_t OFF_RX_RING = 64;
constexpr size_t OFF_TX_RING = OFF_RX_RING + RX_RING * sizeof(Desc);
constexpr size_t OFF_RX_BUF = 4096;
constexpr size_t OFF_TX_BUF = OFF_RX_BUF + RX_RING * BUFFER;
constexpr size_t DMA_BYTES = OFF_TX_BUF + TX_RING * BUFFER;
static_assert(OFF_TX_RING + TX_RING * sizeof(Desc) <= OFF_RX_BUF, "the rings share the first page");

struct Nic {
    const IdrvKernelApi* api;
    bool bound;
    bool interruptHooked;
    uint16_t ioBase;
    uint64_t dmaPhys;
    uint8_t* dma;
    Desc* rxRing;
    Desc* txRing;
    uint32_t rxNext;
    uint32_t txNext;
    uint8_t mac[6];
};

Nic gNic;

uint8_t log2Of(size_t n) {
    uint8_t bits = 0;
    while ((size_t(1) << bits) < n) {
        ++bits;
    }
    return bits;
}

uint32_t byteCount(size_t bytes) {
    return DESC_ONES | (static_cast<uint32_t>(-static_cast<int32_t>(bytes)) & 0x0FFF);
}

uint16_t readCsr(const Nic& nic, uint16_t index) {
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_RAP), index);
    return idrv_inw(static_cast<uint16_t>(nic.ioBase + PORT_RDP));
}

void writeCsr(const Nic& nic, uint16_t index, uint16_t value) {
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_RAP), index);
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_RDP), value);
}

uint16_t readBcr(const Nic& nic, uint16_t index) {
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_RAP), index);
    return idrv_inw(static_cast<uint16_t>(nic.ioBase + PORT_BDP));
}

void writeBcr(const Nic& nic, uint16_t index, uint16_t value) {
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_RAP), index);
    idrv_outw(static_cast<uint16_t>(nic.ioBase + PORT_BDP), value);
}

void writeHex2(const IdrvKernelApi* api, uint8_t value) {
    char text[3] = {"0123456789abcdef"[value >> 4], "0123456789abcdef"[value & 0xF], '\0'};
    api->log(text);
}

// ---- The kernel's callbacks ------------------------------------------------

int claimInterrupt(void* context) {
    Nic& nic = *static_cast<Nic*>(context);
    const uint16_t csr0 = readCsr(nic, 0);
    if ((csr0 & CSR0_INTR) == 0) {
        return 0;   // the shared line was raised by another device
    }
    // Clear what is pending; keep interrupts enabled. The control bits
    // (INIT, STRT, STOP, TDMD) are written as zero, which changes nothing.
    writeCsr(nic, 0, static_cast<uint16_t>((csr0 & CSR0_STATUS) | CSR0_IENA));
    return 1;
}

uint32_t handleInterrupt(void*) {
    return IDRV_IRQ_NET_RECEIVE;
}

const IdrvIrqOps kIrqOps = {claimInterrupt, handleInterrupt};

void macAddress(void* context, uint8_t mac[6]) {
    memcpy(mac, static_cast<Nic*>(context)->mac, 6);
}

int send(void* context, const void* data, size_t len) {
    Nic& nic = *static_cast<Nic*>(context);
    if (!data || len == 0 || len > MTU) {
        return 0;
    }
    Desc& desc = nic.txRing[nic.txNext];
    if ((desc.flags & DESC_OWN) != 0) {
        return 0;   // the chip has not sent the frame in this slot yet: ring full
    }
    memcpy(nic.dma + OFF_TX_BUF + nic.txNext * BUFFER, data, len);
    desc.misc = 0;
    idrv_barrier();
    desc.flags = DESC_OWN | DESC_STP | DESC_ENP | byteCount(len);
    idrv_barrier();
    nic.txNext = static_cast<uint32_t>((nic.txNext + 1) % TX_RING);
    writeCsr(nic, 0, static_cast<uint16_t>(CSR0_TDMD | (nic.interruptHooked ? CSR0_IENA : 0)));
    return 1;
}

int receive(void* context, void* buffer, size_t maxLen) {
    Nic& nic = *static_cast<Nic*>(context);
    for (;;) {
        Desc& desc = nic.rxRing[nic.rxNext];
        const uint32_t flags = desc.flags;
        if ((flags & DESC_OWN) != 0) {
            return -1;   // nothing (more) received
        }
        idrv_barrier();
        int result = -1;
        const size_t length = desc.misc & 0x0FFF;   // includes the FCS
        if ((flags & (DESC_STP | DESC_ENP)) == (DESC_STP | DESC_ENP) && (flags & DESC_ERR) == 0 &&
            length > ETH_CRC) {
            const size_t frame = length - ETH_CRC;
            const size_t copied = frame < maxLen ? frame : maxLen;
            memcpy(buffer, nic.dma + OFF_RX_BUF + nic.rxNext * BUFFER, copied);
            result = static_cast<int>(copied);
        }
        desc.misc = 0;
        idrv_barrier();
        desc.flags = DESC_OWN | byteCount(BUFFER);
        idrv_barrier();
        nic.rxNext = static_cast<uint32_t>((nic.rxNext + 1) % RX_RING);
        if (result >= 0) {
            return result;
        }
    }
}

int linkUp(void* context) {
    return (readBcr(*static_cast<Nic*>(context), 4) & BCR4_LEDOUT) != 0;
}

int interruptDriven(void* context) {
    return static_cast<Nic*>(context)->interruptHooked;
}

const IdrvNetOps kNetOps = {
    "pcnet", 70, 0, macAddress, send, receive, linkUp, interruptDriven,
};

// ---- Bring-up ----------------------------------------------------------------

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    Nic& nic = gNic;
    if (nic.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    nic.api = api;
    const uint32_t bar0 = api->pciRead(dev, IDRV_PCI_BAR0, 4);
    if ((bar0 & 1) == 0) {
        api->log("[net] pcnet: BAR0 is not an I/O BAR\n");
        return IDRV_ENODEV;
    }
    nic.ioBase = static_cast<uint16_t>(bar0 & ~0x3u);
    const uint32_t cmd = api->pciRead(dev, IDRV_PCI_COMMAND, 2);
    api->pciWrite(dev, IDRV_PCI_COMMAND, 2, cmd | IDRV_PCI_COMMAND_IO | IDRV_PCI_COMMAND_BUS_MASTER);

    // Reset (either width, whichever mode the firmware left it in), which
    // also puts it back in 16-bit mode, and stop it.
    (void)idrv_inl(static_cast<uint16_t>(nic.ioBase + PORT_RESET32));
    (void)idrv_inw(static_cast<uint16_t>(nic.ioBase + PORT_RESET));
    api->delayUs(10);
    writeCsr(nic, 0, CSR0_STOP);

    for (int i = 0; i < 6; ++i) {
        nic.mac[i] = idrv_inb(static_cast<uint16_t>(nic.ioBase + PORT_APROM + i));
    }

    const uint64_t pages = (DMA_BYTES + 4095) / 4096;
    void* dma = nullptr;
    nic.dmaPhys = api->dmaAlloc(pages, 0x100000000ULL, &dma);   // 32-bit addresses
    if (nic.dmaPhys == 0) {
        api->log("[net] pcnet: no memory below 4 GiB for the rings\n");
        return IDRV_ENOMEM;
    }
    nic.dma = static_cast<uint8_t*>(dma);
    nic.rxRing = reinterpret_cast<Desc*>(nic.dma + OFF_RX_RING);
    nic.txRing = reinterpret_cast<Desc*>(nic.dma + OFF_TX_RING);
    for (size_t i = 0; i < RX_RING; ++i) {
        nic.rxRing[i].addr = static_cast<uint32_t>(nic.dmaPhys + OFF_RX_BUF + i * BUFFER);
        nic.rxRing[i].misc = 0;
        nic.rxRing[i].flags = DESC_OWN | byteCount(BUFFER);
    }
    for (size_t i = 0; i < TX_RING; ++i) {
        nic.txRing[i].addr = static_cast<uint32_t>(nic.dmaPhys + OFF_TX_BUF + i * BUFFER);
        nic.txRing[i].flags = 0;   // the driver's
    }

    auto* init = reinterpret_cast<InitBlock*>(nic.dma + OFF_INIT);
    init->mode = 0;   // normal: receive our address and broadcast, transmit with FCS
    init->rlen = static_cast<uint8_t>(log2Of(RX_RING) << 4);
    init->tlen = static_cast<uint8_t>(log2Of(TX_RING) << 4);
    memcpy(init->mac, nic.mac, 6);
    memset(init->ladrf, 0xFF, sizeof(init->ladrf));   // every multicast group
    init->rdra = static_cast<uint32_t>(nic.dmaPhys + OFF_RX_RING);
    init->tdra = static_cast<uint32_t>(nic.dmaPhys + OFF_TX_RING);
    idrv_barrier();

    writeBcr(nic, 20, BCR20_SWSTYLE2);
    writeCsr(nic, 1, static_cast<uint16_t>(nic.dmaPhys + OFF_INIT));
    writeCsr(nic, 2, static_cast<uint16_t>((nic.dmaPhys + OFF_INIT) >> 16));
    writeCsr(nic, 3, CSR3_IDONM | CSR3_TINTM);
    writeCsr(nic, 4, static_cast<uint16_t>(readCsr(nic, 4) | CSR4_APAD_XMT | CSR4_ASTRP_RCV));
    writeCsr(nic, 0, CSR0_INIT);
    const uint64_t deadline = api->uptimeNs() + 100'000'000ULL;
    while ((readCsr(nic, 0) & CSR0_IDON) == 0) {
        if (api->uptimeNs() > deadline) {
            api->log("[net] pcnet: initialization did not finish\n");
            api->dmaFree(nic.dmaPhys, pages);
            return IDRV_EIO;
        }
        idrv_pause();
    }

    nic.interruptHooked = api->irqRegister(dev, IDRV_IRQ_ALLOW_LEGACY, &kIrqOps, &nic) == IDRV_IRQ_LEGACY;
    nic.rxNext = 0;
    nic.txNext = 0;
    writeCsr(nic, 0, static_cast<uint16_t>(CSR0_IDON | CSR0_STRT | (nic.interruptHooked ? CSR0_IENA : 0)));

    api->log("[net] pcnet: MAC ");
    for (int i = 0; i < 6; ++i) {
        writeHex2(api, nic.mac[i]);
        api->log(i < 5 ? ":" : "");
    }
    api->log(nic.interruptHooked ? ", INTx receive interrupt\n" : ", polling\n");

    nic.bound = true;
    return api->netRegister(&kNetOps, &nic);
}

const IdrvPciMatch kMatches[] = {
    {AMD_VENDOR, PCNET_DEVICE, 0, 0},
};

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "pcnet",
    "1.0",
    "AMD PCnet-PCI II/III Ethernet (Am79C970A/Am79C973)",
    "InstantOS",
    kMatches,
    sizeof(kMatches) / sizeof(kMatches[0]),
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
