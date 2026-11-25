// VMware's paravirtual NIC, vmxnet3 (15ad:07b0), as a loadable module
// (vmxnet3.idrv). It was the kernel's src/drivers/net/vmxnet3.cpp; the
// hardware side is unchanged. VMware's default for modern guests, also
// emulated by QEMU (-device vmxnet3).
//
// The driver hands the device one "driver shared" block (DSAL/DSAH) that
// points at a table of queue descriptors, each naming its rings; descriptors
// carry a generation bit that flips each time a ring wraps, so the owner of a
// slot is whoever's generation it shows. One TX queue (TX ring + completion
// ring) and one RX queue (a head ring for whole frames, a small body ring the
// device may use for overflow, one completion ring for both). Register numbers,
// commands and layouts follow Linux's vmxnet3_defs.h (QEMU's hw/net/vmxnet3.h
// is a copy). MSI, else INTx (ICR tells whether it was us).

#include "../sdk/idrv_sdk.h"

namespace {

constexpr uint16_t VMWARE_VENDOR = 0x15AD;
constexpr uint16_t VMXNET3_DEVICE = 0x07B0;

constexpr size_t TX_RING = 256;
constexpr size_t RX_RING = 256;
constexpr size_t RX_RING2 = 32;     // body buffers; sizes are multiples of 32
constexpr size_t BUFFER = 2048;
constexpr size_t MTU = 1514;

// BAR0 (PT)
constexpr uint32_t REG_IMR = 0x0;
constexpr uint32_t REG_TXPROD = 0x600;
constexpr uint32_t REG_RXPROD = 0x800;
constexpr uint32_t REG_RXPROD2 = 0xA00;
// BAR1 (VD)
constexpr uint32_t REG_VRRS = 0x0;
constexpr uint32_t REG_UVRS = 0x8;
constexpr uint32_t REG_DSAL = 0x10;
constexpr uint32_t REG_DSAH = 0x18;
constexpr uint32_t REG_CMD = 0x20;
constexpr uint32_t REG_MACL = 0x28;
constexpr uint32_t REG_MACH = 0x30;
constexpr uint32_t REG_ICR = 0x38;
constexpr uint32_t REG_ECR = 0x40;

constexpr uint32_t CMD_ACTIVATE_DEV = 0xCAFE0000;
constexpr uint32_t CMD_RESET_DEV = 0xCAFE0002;
constexpr uint32_t CMD_UPDATE_RX_MODE = 0xCAFE0003;
constexpr uint32_t CMD_UPDATE_MAC_FILTERS = 0xCAFE0004;
constexpr uint32_t CMD_GET_LINK = 0xF00D0002;

constexpr uint32_t REV1_MAGIC = 0xBABEFEE1;
constexpr uint32_t RXM_UCAST = 0x01;
constexpr uint32_t RXM_BCAST = 0x04;
constexpr uint32_t RXM_ALL_MULTI = 0x08;
constexpr uint32_t GOS_BITS_64 = 2;
constexpr uint32_t GOS_TYPE_LINUX = 1;   // the closest the device knows; it only adjusts RX for Windows

constexpr uint32_t TXD_EOP = 1u << 12;
constexpr uint32_t TXD_CQ = 1u << 13;
constexpr uint32_t TXD_GEN = 1u << 14;
constexpr uint32_t RXD_BTYPE_BODY = 1u << 14;
constexpr uint32_t RXD_GEN = 1u << 31;
constexpr uint32_t COMP_GEN = 1u << 31;   // last word of both completion descriptors
constexpr size_t ETH_MIN_FRAME = 60;

// The device's descriptor formats (16 bytes each) and shared structures.
struct TxDesc { uint64_t addr; uint32_t word2; uint32_t word3; } __attribute__((packed));
struct TxCompDesc { uint32_t word0; uint32_t ext2; uint32_t ext3; uint32_t word3; } __attribute__((packed));
struct RxDesc { uint64_t addr; uint32_t word2; uint32_t ext1; } __attribute__((packed));
struct RxCompDesc { uint32_t word0; uint32_t rssHash; uint32_t word2; uint32_t word3; } __attribute__((packed));
static_assert(sizeof(TxDesc) == 16 && sizeof(TxCompDesc) == 16 && sizeof(RxDesc) == 16 && sizeof(RxCompDesc) == 16);

struct DriverInfo { uint32_t version; uint32_t gos; uint32_t vmxnet3RevSpt; uint32_t uptVerSpt; } __attribute__((packed));
struct MiscConf {
    DriverInfo driverInfo;
    uint64_t uptFeatures;
    uint64_t ddPA;
    uint64_t queueDescPA;
    uint32_t ddLen;
    uint32_t queueDescLen;
    uint32_t mtu;
    uint16_t maxNumRxSG;
    uint8_t numTxQueues;
    uint8_t numRxQueues;
    uint32_t reserved[4];
} __attribute__((packed));
struct IntrConf {
    uint8_t autoMask;
    uint8_t numIntrs;
    uint8_t eventIntrIdx;
    uint8_t modLevels[25];
    uint32_t intrCtrl;
    uint32_t reserved[2];
} __attribute__((packed));
struct RxFilterConf {
    uint32_t rxMode;
    uint16_t mfTableLen;
    uint16_t pad1;
    uint64_t mfTablePA;
    uint32_t vfTable[128];
} __attribute__((packed));
struct VarLenConfDesc { uint32_t confVer; uint32_t confLen; uint64_t confPA; } __attribute__((packed));
struct DriverShared {
    uint32_t magic;
    uint32_t pad;
    MiscConf misc;
    IntrConf intrConf;
    RxFilterConf rxFilterConf;
    VarLenConfDesc rssConfDesc;
    VarLenConfDesc pmConfDesc;
    VarLenConfDesc pluginConfDesc;
    uint32_t ecr;
    uint32_t reserved[5];
} __attribute__((packed));
static_assert(sizeof(MiscConf) == 72 && sizeof(IntrConf) == 40 && sizeof(RxFilterConf) == 528);
static_assert(sizeof(DriverShared) == 720);

struct QueueStatus { uint8_t stopped; uint8_t pad[3]; uint32_t error; } __attribute__((packed));
struct TxQueueDesc {
    uint32_t txNumDeferred;
    uint32_t txThreshold;
    uint64_t ctrlReserved;
    uint64_t txRingBasePA;
    uint64_t dataRingBasePA;
    uint64_t compRingBasePA;
    uint64_t ddPA;
    uint64_t confReserved;
    uint32_t txRingSize;
    uint32_t dataRingSize;
    uint32_t compRingSize;
    uint32_t ddLen;
    uint8_t intrIdx;
    uint8_t confPad[7];
    QueueStatus status;
    uint64_t stats[10];
    uint8_t pad[88];
} __attribute__((packed));
struct RxQueueDesc {
    uint8_t updateRxProd;
    uint8_t ctrlPad[7];
    uint64_t ctrlReserved;
    uint64_t rxRingBasePA[2];
    uint64_t compRingBasePA;
    uint64_t ddPA;
    uint64_t confReserved;
    uint32_t rxRingSize[2];
    uint32_t compRingSize;
    uint32_t ddLen;
    uint8_t intrIdx;
    uint8_t confPad[7];
    QueueStatus status;
    uint64_t stats[10];
    uint8_t pad[88];
} __attribute__((packed));
static_assert(sizeof(TxQueueDesc) == 256 && sizeof(RxQueueDesc) == 256);

// The DMA block: shared area, queue descriptors, rings, then the buffers.
constexpr size_t OFF_SHARED = 0;
constexpr size_t OFF_TXQ = 1024;
constexpr size_t OFF_RXQ = OFF_TXQ + sizeof(TxQueueDesc);
constexpr size_t OFF_TX_RING = 2048;
constexpr size_t OFF_TX_COMP = OFF_TX_RING + TX_RING * 16;
constexpr size_t OFF_TX_DATA = OFF_TX_COMP + TX_RING * 16;   // data ring (unused, but configured)
constexpr size_t OFF_RX_RING = OFF_TX_DATA + TX_RING * 128;
constexpr size_t OFF_RX_RING2 = OFF_RX_RING + RX_RING * 16;
constexpr size_t OFF_RX_COMP = OFF_RX_RING2 + RX_RING2 * 16;
constexpr size_t OFF_BUFFERS = (OFF_RX_COMP + (RX_RING + RX_RING2) * 16 + 4095) & ~size_t(4095);
constexpr size_t OFF_TX_BUF = OFF_BUFFERS;
constexpr size_t OFF_RX_BUF = OFF_TX_BUF + TX_RING * BUFFER;
constexpr size_t OFF_RX_BUF2 = OFF_RX_BUF + RX_RING * BUFFER;
constexpr size_t DMA_BYTES = OFF_RX_BUF2 + RX_RING2 * BUFFER;

struct Nic {
    bool bound;
    uint32_t irqMode;   // IDRV_IRQ_*
    uint64_t pt;        // BAR0: interrupt masks, producer indices
    uint64_t vd;        // BAR1: version, shared block, commands, MAC

    uint64_t dmaPhys;
    uint8_t* dma;
    TxDesc* txRing;
    TxCompDesc* txComp;
    RxDesc* rxRing;
    RxDesc* rxRing2;
    RxCompDesc* rxComp;

    uint32_t txNext;      // next TX slot to fill
    uint32_t txGen;
    uint32_t txInFlight;
    uint32_t txCompNext;
    uint32_t txCompGen;

    uint32_t rxFill;      // head ring: next slot to give back (in order)
    uint32_t rxFillGen;
    uint32_t rxFill2;
    uint32_t rxFill2Gen;
    uint32_t rxCompNext;
    uint32_t rxCompGen;

    uint8_t mac[6];
};

Nic gNic;

uint32_t readVd(const Nic& n, uint32_t reg) { return idrv_mmio_read32(n.vd, reg); }
void writeVd(const Nic& n, uint32_t reg, uint32_t v) { idrv_mmio_write32(n.vd, reg, v); }
void writePt(const Nic& n, uint32_t reg, uint32_t v) { idrv_mmio_write32(n.pt, reg, v); }

uint32_t command(const Nic& n, uint32_t cmd) {
    writeVd(n, REG_CMD, cmd);
    return readVd(n, REG_CMD);
}

void logHex(const IdrvKernelApi* api, uint64_t value, int digits) {
    char text[17];
    for (int i = digits - 1; i >= 0; --i) {
        text[i] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    }
    text[digits] = '\0';
    api->log(text);
}

void allocate(Nic& n) {
    const uint64_t p = n.dmaPhys;
    uint8_t* d = n.dma;
    n.txRing = reinterpret_cast<TxDesc*>(d + OFF_TX_RING);
    n.txComp = reinterpret_cast<TxCompDesc*>(d + OFF_TX_COMP);
    n.rxRing = reinterpret_cast<RxDesc*>(d + OFF_RX_RING);
    n.rxRing2 = reinterpret_cast<RxDesc*>(d + OFF_RX_RING2);
    n.rxComp = reinterpret_cast<RxCompDesc*>(d + OFF_RX_COMP);

    auto* shared = reinterpret_cast<DriverShared*>(d + OFF_SHARED);
    shared->magic = REV1_MAGIC;
    shared->misc.driverInfo.version = 1;
    shared->misc.driverInfo.gos = GOS_BITS_64 | (GOS_TYPE_LINUX << 2);
    shared->misc.driverInfo.vmxnet3RevSpt = 1;
    shared->misc.driverInfo.uptVerSpt = 1;
    shared->misc.uptFeatures = 0;   // no checksum offload, LRO, RSS or VLAN stripping
    shared->misc.queueDescPA = p + OFF_TXQ;
    shared->misc.queueDescLen = sizeof(TxQueueDesc) + sizeof(RxQueueDesc);
    shared->misc.mtu = 1500;
    shared->misc.maxNumRxSG = 1;
    shared->misc.numTxQueues = 1;
    shared->misc.numRxQueues = 1;
    shared->intrConf.autoMask = 0;   // the vector stays unmasked; INTx is cleared by reading ICR
    shared->intrConf.numIntrs = 1;
    shared->intrConf.eventIntrIdx = 0;
    shared->rxFilterConf.rxMode = RXM_UCAST | RXM_BCAST | RXM_ALL_MULTI;

    auto* txq = reinterpret_cast<TxQueueDesc*>(d + OFF_TXQ);
    txq->txRingBasePA = p + OFF_TX_RING;
    txq->dataRingBasePA = p + OFF_TX_DATA;
    txq->compRingBasePA = p + OFF_TX_COMP;
    txq->txRingSize = TX_RING;
    txq->dataRingSize = TX_RING;
    txq->compRingSize = TX_RING;
    txq->intrIdx = 0;

    auto* rxq = reinterpret_cast<RxQueueDesc*>(d + OFF_RXQ);
    rxq->rxRingBasePA[0] = p + OFF_RX_RING;
    rxq->rxRingBasePA[1] = p + OFF_RX_RING2;
    rxq->compRingBasePA = p + OFF_RX_COMP;
    rxq->rxRingSize[0] = RX_RING;
    rxq->rxRingSize[1] = RX_RING2;
    rxq->compRingSize = RX_RING + RX_RING2;
    rxq->intrIdx = 0;

    // Hand every RX buffer to the device: one pass with generation 1, so the
    // next refill of each slot (its second pass) carries generation 0.
    for (size_t i = 0; i < RX_RING; ++i) {
        n.rxRing[i].addr = p + OFF_RX_BUF + i * BUFFER;
        n.rxRing[i].word2 = static_cast<uint32_t>(BUFFER) | RXD_GEN;
    }
    for (size_t i = 0; i < RX_RING2; ++i) {
        n.rxRing2[i].addr = p + OFF_RX_BUF2 + i * BUFFER;
        n.rxRing2[i].word2 = static_cast<uint32_t>(BUFFER) | RXD_BTYPE_BODY | RXD_GEN;
    }
    n.txNext = 0;
    n.txGen = 1;
    n.txInFlight = 0;
    n.txCompNext = 0;
    n.txCompGen = 1;
    n.rxFill = 0;
    n.rxFillGen = 0;
    n.rxFill2 = 0;
    n.rxFill2Gen = 0;
    n.rxCompNext = 0;
    n.rxCompGen = 1;
    idrv_barrier();
}

// ---- The kernel's callbacks ------------------------------------------------

int claimInterrupt(void* context) {
    Nic& n = *static_cast<Nic*>(context);
    if (n.irqMode == IDRV_IRQ_LEGACY && readVd(n, REG_ICR) == 0) {
        return 0;   // the shared line was raised by another device
    }
    // Events (link changes, queue errors): acknowledge what the device posted.
    const uint32_t events = readVd(n, REG_ECR);
    if (events != 0) {
        writeVd(n, REG_ECR, events);
    }
    return 1;
}

uint32_t handleInterrupt(void*) {
    return IDRV_IRQ_NET_RECEIVE;
}

const IdrvIrqOps kIrqOps = {claimInterrupt, handleInterrupt};

void macAddress(void* context, uint8_t mac[6]) {
    memcpy(mac, static_cast<Nic*>(context)->mac, 6);
}

// Count the frames the device has finished sending (one completion per frame).
void reclaimTx(Nic& n) {
    for (;;) {
        TxCompDesc& comp = n.txComp[n.txCompNext];
        if (((comp.word3 & COMP_GEN) != 0) != (n.txCompGen != 0)) {
            return;
        }
        idrv_barrier();
        if (n.txInFlight > 0) {
            --n.txInFlight;
        }
        if (++n.txCompNext == TX_RING) {
            n.txCompNext = 0;
            n.txCompGen ^= 1;
        }
    }
}

int send(void* context, const void* data, size_t len) {
    Nic& n = *static_cast<Nic*>(context);
    if (!data || len == 0 || len > MTU) {
        return 0;
    }
    reclaimTx(n);
    if (n.txInFlight >= TX_RING - 1) {
        return 0;   // ring full and nothing completed
    }
    uint8_t* buffer = n.dma + OFF_TX_BUF + n.txNext * BUFFER;
    memcpy(buffer, data, len);
    if (len < ETH_MIN_FRAME) {
        memset(buffer + len, 0, ETH_MIN_FRAME - len);
        len = ETH_MIN_FRAME;
    }
    TxDesc& desc = n.txRing[n.txNext];
    desc.addr = n.dmaPhys + OFF_TX_BUF + n.txNext * BUFFER;
    desc.word3 = TXD_EOP | TXD_CQ;
    idrv_barrier();
    // The generation bit hands the slot over, so it goes in last.
    desc.word2 = static_cast<uint32_t>(len) | (n.txGen ? TXD_GEN : 0);
    idrv_barrier();
    ++n.txInFlight;
    if (++n.txNext == TX_RING) {
        n.txNext = 0;
        n.txGen ^= 1;
    }
    writePt(n, REG_TXPROD, n.txNext);
    return 1;
}

int receive(void* context, void* buffer, size_t maxLen) {
    Nic& n = *static_cast<Nic*>(context);
    for (;;) {
        RxCompDesc& comp = n.rxComp[n.rxCompNext];
        if (((comp.word3 & COMP_GEN) != 0) != (n.rxCompGen != 0)) {
            return -1;   // nothing (more) received
        }
        idrv_barrier();
        const uint32_t index = comp.word0 & 0xFFF;
        const uint32_t ring = (comp.word0 >> 16) & 0x3FF;   // rqID: 0 the head ring, 1 the body ring
        const bool eop = (comp.word0 & (1u << 14)) != 0;
        const bool sop = (comp.word0 & (1u << 15)) != 0;
        const uint32_t length = comp.word2 & 0x3FFF;
        const bool error = (comp.word2 & (1u << 14)) != 0;
        int result = -1;
        if (ring == 0 && sop && eop && !error && length > 0 && index < RX_RING) {
            const size_t copied = length < maxLen ? length : maxLen;
            memcpy(buffer, n.dma + OFF_RX_BUF + index * BUFFER, copied);
            result = static_cast<int>(copied);
        }
        // Give the slot back. The device fills each ring in order, so the
        // slot to refill is the next one in it (frames spread over several
        // buffers are dropped; none fits that description at this MTU).
        if (ring == 0) {
            n.rxRing[n.rxFill].word2 = static_cast<uint32_t>(BUFFER) | (n.rxFillGen ? RXD_GEN : 0);
            if (++n.rxFill == RX_RING) {
                n.rxFill = 0;
                n.rxFillGen ^= 1;
            }
        } else {
            n.rxRing2[n.rxFill2].word2 =
                static_cast<uint32_t>(BUFFER) | RXD_BTYPE_BODY | (n.rxFill2Gen ? RXD_GEN : 0);
            if (++n.rxFill2 == RX_RING2) {
                n.rxFill2 = 0;
                n.rxFill2Gen ^= 1;
            }
        }
        idrv_barrier();
        if (++n.rxCompNext == RX_RING + RX_RING2) {
            n.rxCompNext = 0;
            n.rxCompGen ^= 1;
        }
        writePt(n, ring == 0 ? REG_RXPROD : REG_RXPROD2, ring == 0 ? n.rxFill : n.rxFill2);
        if (result >= 0) {
            return result;
        }
    }
}

int linkUp(void* context) {
    return (command(*static_cast<Nic*>(context), CMD_GET_LINK) & 1) != 0;
}

int interruptDriven(void* context) {
    return static_cast<Nic*>(context)->irqMode != IDRV_IRQ_NONE;
}

// Paravirtual: after virtio, before the emulated chips.
const IdrvNetOps kNetOps = {
    "vmxnet3", 40, 0, macAddress, send, receive, linkUp, interruptDriven,
};

// ---- Bring-up ----------------------------------------------------------------

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    Nic& n = gNic;
    if (n.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    const uint32_t cmd = api->pciRead(dev, IDRV_PCI_COMMAND, 2);
    api->pciWrite(dev, IDRV_PCI_COMMAND, 2, cmd | IDRV_PCI_COMMAND_MEMORY | IDRV_PCI_COMMAND_BUS_MASTER);
    n.pt = api->pciMapBar(dev, 0, 0x1000);
    n.vd = api->pciMapBar(dev, 1, 0x1000);
    if (n.pt == 0 || n.vd == 0) {
        api->log("[net] vmxnet3: BAR0/BAR1 not mapped\n");
        return IDRV_ENODEV;
    }

    // Revision 1 of the device and of UPT, which every version supports.
    if ((readVd(n, REG_VRRS) & 1) == 0 || (readVd(n, REG_UVRS) & 1) == 0) {
        api->log("[net] vmxnet3: revision 1 not offered\n");
        return IDRV_ENODEV;
    }
    writeVd(n, REG_VRRS, 1);
    writeVd(n, REG_UVRS, 1);
    writeVd(n, REG_CMD, CMD_RESET_DEV);

    const uint32_t macLow = readVd(n, REG_MACL);
    const uint32_t macHigh = readVd(n, REG_MACH);
    for (int i = 0; i < 4; ++i) {
        n.mac[i] = static_cast<uint8_t>(macLow >> (8 * i));
    }
    n.mac[4] = static_cast<uint8_t>(macHigh);
    n.mac[5] = static_cast<uint8_t>(macHigh >> 8);

    void* dma = nullptr;
    const uint64_t pages = (DMA_BYTES + 4095) / 4096;
    n.dmaPhys = api->dmaAlloc(pages, 0, &dma);
    if (n.dmaPhys == 0) {
        api->log("[net] vmxnet3: no memory for the rings\n");
        return IDRV_ENOMEM;
    }
    n.dma = static_cast<uint8_t*>(dma);
    allocate(n);

    writeVd(n, REG_DSAL, static_cast<uint32_t>(n.dmaPhys + OFF_SHARED));
    writeVd(n, REG_DSAH, static_cast<uint32_t>((n.dmaPhys + OFF_SHARED) >> 32));
    if (command(n, CMD_ACTIVATE_DEV) != 0) {
        api->log("[net] vmxnet3: the device refused the configuration\n");
        api->dmaFree(n.dmaPhys, pages);
        return IDRV_EIO;
    }
    // Every RX descriptor is the device's already; the producer indices say so.
    writePt(n, REG_RXPROD, 0);
    writePt(n, REG_RXPROD2, 0);
    // Set commands have no result to read back (only ACTIVATE and the gets do).
    writeVd(n, REG_CMD, CMD_UPDATE_MAC_FILTERS);
    writeVd(n, REG_CMD, CMD_UPDATE_RX_MODE);

    n.irqMode = api->irqRegister(dev, IDRV_IRQ_ALLOW_MSI | IDRV_IRQ_ALLOW_LEGACY, &kIrqOps, &n);
    if (n.irqMode != IDRV_IRQ_NONE) {
        writePt(n, REG_IMR, 0);   // 0 unmasks vector 0
    }
    api->log(n.irqMode == IDRV_IRQ_MSI ? "[net] vmxnet3: MSI receive interrupt\n"
             : n.irqMode == IDRV_IRQ_LEGACY ? "[net] vmxnet3: INTx receive interrupt\n"
             : "[net] vmxnet3: no interrupt, polling\n");

    api->log("[net] vmxnet3: MAC ");
    for (int i = 0; i < 6; ++i) {
        logHex(api, n.mac[i], 2);
        api->log(i < 5 ? ":" : "");
    }
    const uint32_t link = command(n, CMD_GET_LINK);
    api->log((link & 1) ? ", link up " : ", link down ");
    logHex(api, link >> 16, 4);
    api->log(" Mbit/s (hex)\n");

    n.bound = true;
    return api->netRegister(&kNetOps, &n);
}

const IdrvPciMatch kMatches[] = {
    {VMWARE_VENDOR, VMXNET3_DEVICE, 0, 0},
};

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "vmxnet3",
    "1.0",
    "VMware vmxnet3 paravirtual Ethernet",
    "InstantOS",
    kMatches,
    sizeof(kMatches) / sizeof(kMatches[0]),
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
