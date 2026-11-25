// virtio-net (modern, 1af4:1041; or a transitional 1af4:1000 through its
// modern interface), as a loadable module (virtio-net.idrv). It was the
// kernel's src/drivers/virtio/virtio_net.cpp; the device side is unchanged.
// One RX and one TX queue; the RX queue takes MSI-X entry 0 (INTx through the
// ISR status otherwise), TX completions are reclaimed lazily with no interrupt.
// The split-virtqueue layout and the PCI capabilities follow the virtio 1.x
// specification (the kernel's include/drivers/virtio/virtio.hpp, which
// virtio-gpu still uses, has the same definitions).

#include "../sdk/idrv_sdk.h"

namespace {

constexpr uint16_t VIRTIO_VENDOR = 0x1AF4;
constexpr uint16_t VIRTIO_NET_DEVICE = 0x1041;
constexpr uint16_t VIRTIO_NET_DEVICE_TRANSITIONAL = 0x1000;

constexpr uint16_t PCI_STATUS_REG = 0x06;
constexpr uint16_t PCI_CAP_PTR_REG = 0x34;
constexpr uint16_t PCI_STATUS_CAP_LIST = 1 << 4;
constexpr uint8_t PCI_CAP_ID_VENDOR_SPECIFIC = 0x09;

constexpr uint8_t CAP_COMMON_CFG = 1;
constexpr uint8_t CAP_NOTIFY_CFG = 2;
constexpr uint8_t CAP_ISR_CFG = 3;
constexpr uint8_t CAP_DEVICE_CFG = 4;

constexpr uint8_t STATUS_ACKNOWLEDGE = 1;
constexpr uint8_t STATUS_DRIVER = 2;
constexpr uint8_t STATUS_DRIVER_OK = 4;
constexpr uint8_t STATUS_FEATURES_OK = 8;
constexpr uint8_t STATUS_FAILED = 128;

constexpr uint64_t F_VERSION_1 = 1ULL << 32;
constexpr uint64_t NET_F_MAC = 1ULL << 5;
constexpr uint64_t NET_F_STATUS = 1ULL << 16;

constexpr uint16_t DESC_F_WRITE = 2;
constexpr uint16_t AVAIL_F_NO_INTERRUPT = 1;
constexpr uint16_t NO_VECTOR = 0xFFFF;

constexpr uint16_t RX_QUEUE = 0;
constexpr uint16_t TX_QUEUE = 1;
constexpr uint16_t QUEUE_SIZE = 256;   // at most; the device may offer less
constexpr uint16_t LINK_UP = 1;
constexpr uint32_t TX_COMPLETION_SPIN_LIMIT = 100000;
constexpr uint16_t RX_NOTIFY_BATCH = 32;   // RX buffers handed back per notify
constexpr size_t MTU = 1514;
// One 2 KiB slot per descriptor (header + frame is 1526 bytes). 32 RX
// buffers overflowed whenever a TCP peer had more than 32 segments in flight.
constexpr size_t SLOT = 2048;

struct NetHdr {
    uint8_t flags;
    uint8_t gsoType;
    uint16_t hdrLen;
    uint16_t gsoSize;
    uint16_t csumStart;
    uint16_t csumOffset;
    uint16_t numBuffers;
} __attribute__((packed));

struct NetConfig {
    uint8_t mac[6];
    uint16_t status;
    uint16_t maxVirtqueuePairs;
} __attribute__((packed));

struct CommonCfg {
    uint32_t deviceFeatureSelect;
    uint32_t deviceFeature;
    uint32_t driverFeatureSelect;
    uint32_t driverFeature;
    uint16_t msixConfig;
    uint16_t numQueues;
    uint8_t deviceStatus;
    uint8_t configGeneration;
    uint16_t queueSelect;
    uint16_t queueSize;
    uint16_t queueMsixVector;
    uint16_t queueEnable;
    uint16_t queueNotifyOff;
    uint64_t queueDesc;
    uint64_t queueDriver;
    uint64_t queueDevice;
} __attribute__((packed));

struct Desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct UsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

// A split virtqueue in three pages: descriptors, the available ring, the used
// ring. The device gets physical addresses, the CPU the direct map's.
struct Queue {
    uint16_t size;
    uint16_t lastUsed;
    uint16_t numFree;
    uint64_t phys;
    Desc* desc;
    volatile uint16_t* avail;   // flags, idx, ring[size]
    volatile uint16_t* used;    // flags, idx, then UsedElem ring[size]
    uint16_t freeList[QUEUE_SIZE];
};

struct Nic {
    const IdrvKernelApi* api;
    bool bound;
    uint32_t irqMode;   // IDRV_IRQ_*
    volatile CommonCfg* common;
    uint64_t notifyBase;
    uint32_t notifyMultiplier;
    volatile NetConfig* deviceCfg;
    volatile uint8_t* isrStatus;   // read-to-clear, for INTx
    uint64_t features;
    Queue rx;
    Queue tx;
    uint64_t rxPhys;   // RX slots
    uint8_t* rxSlots;
    uint64_t txPhys;   // TX slots, one per descriptor
    uint8_t* txSlots;
    uint16_t rxRequeuedSinceNotify;
    uint8_t mac[6];
};

Nic gNic;

inline void barrier() {
    idrv_barrier();
}

bool queueInit(const IdrvKernelApi* api, Queue& q, uint16_t size) {
    void* virt = nullptr;
    q.phys = api->dmaAlloc(3, 0, &virt);
    if (q.phys == 0) {
        return false;
    }
    auto* base = static_cast<uint8_t*>(virt);
    q.size = size;
    q.desc = reinterpret_cast<Desc*>(base);
    q.avail = reinterpret_cast<volatile uint16_t*>(base + 4096);
    q.used = reinterpret_cast<volatile uint16_t*>(base + 8192);
    for (uint16_t i = 0; i < size; ++i) {
        q.freeList[i] = i;
    }
    q.numFree = size;
    q.lastUsed = 0;
    return true;
}

int allocDesc(Queue& q) {
    return q.numFree == 0 ? -1 : q.freeList[--q.numFree];
}

void freeDesc(Queue& q, int idx) {
    if (idx < 0 || idx >= q.size || q.numFree >= q.size) {
        return;
    }
    q.desc[idx] = {0, 0, 0, 0xFFFF};
    q.freeList[q.numFree++] = static_cast<uint16_t>(idx);
}

void addAvail(Queue& q, uint16_t descIdx) {
    const uint16_t idx = q.avail[1];
    q.avail[2 + idx % q.size] = descIdx;
    barrier();
    q.avail[1] = static_cast<uint16_t>(idx + 1);
    barrier();
}

bool hasUsed(Queue& q) {
    barrier();
    return q.used[1] != q.lastUsed;
}

// The next used element's descriptor, or -1.
int getUsed(Queue& q, uint32_t* len) {
    if (!hasUsed(q)) {
        return -1;
    }
    const auto* ring = reinterpret_cast<const volatile UsedElem*>(q.used + 2);
    const uint32_t id = ring[q.lastUsed % q.size].id;
    *len = ring[q.lastUsed % q.size].len;
    ++q.lastUsed;
    return id < q.size ? static_cast<int>(id) : -1;
}

uint8_t cfg8(const IdrvKernelApi* api, const IdrvPciDevice* dev, uint16_t off) {
    return static_cast<uint8_t>(api->pciRead(dev, off, 1));
}

uint32_t cfg32(const IdrvKernelApi* api, const IdrvPciDevice* dev, uint16_t off) {
    return api->pciRead(dev, off, 4);
}

void notifyQueue(Nic& n, uint16_t queue) {
    n.common->queueSelect = queue;
    barrier();
    const uint16_t off = n.common->queueNotifyOff;
    *reinterpret_cast<volatile uint16_t*>(n.notifyBase + static_cast<uint64_t>(off) * n.notifyMultiplier) = queue;
}

// RX descriptor i always holds RX slot i: the queue hands descriptors back
// in any order, and each goes straight back with its own slot.
void queueRx(Nic& n, int desc) {
    n.rx.desc[desc] = {n.rxPhys + static_cast<uint64_t>(desc) * SLOT, static_cast<uint32_t>(SLOT), DESC_F_WRITE, 0};
    addAvail(n.rx, static_cast<uint16_t>(desc));
}

// ---- The kernel's callbacks ------------------------------------------------

int claimInterrupt(void* context) {
    Nic& n = *static_cast<Nic*>(context);
    if (n.irqMode == IDRV_IRQ_MSIX) {
        return 1;
    }
    // INTx may be shared: reading the ISR status says (and acknowledges)
    // whether this device raised it. A config change also holds the line.
    return n.isrStatus && *n.isrStatus != 0;
}

uint32_t handleInterrupt(void*) {
    return IDRV_IRQ_NET_RECEIVE;
}

const IdrvIrqOps kIrqOps = {claimInterrupt, handleInterrupt};

void macAddress(void* context, uint8_t mac[6]) {
    memcpy(mac, static_cast<Nic*>(context)->mac, 6);
}

void reclaimTx(Nic& n) {
    uint32_t len = 0;
    while (hasUsed(n.tx)) {
        const int desc = getUsed(n.tx, &len);
        if (desc >= 0) {
            freeDesc(n.tx, desc);
        }
    }
}

int send(void* context, const void* data, size_t len) {
    Nic& n = *static_cast<Nic*>(context);
    if (!data || len == 0 || len > MTU) {
        return 0;
    }
    // Completions are reclaimed lazily: waiting for the device to consume each
    // frame made every send cost a full round trip through the host.
    reclaimTx(n);
    int desc = allocDesc(n.tx);
    for (uint32_t spin = 0; desc < 0 && spin < TX_COMPLETION_SPIN_LIMIT; ++spin) {
        idrv_pause();
        reclaimTx(n);
        desc = allocDesc(n.tx);
    }
    if (desc < 0) {
        return 0;
    }
    uint8_t* packet = n.txSlots + static_cast<size_t>(desc) * SLOT;
    memset(packet, 0, sizeof(NetHdr));
    memcpy(packet + sizeof(NetHdr), data, len);
    n.tx.desc[desc] = {n.txPhys + static_cast<uint64_t>(desc) * SLOT,
                       static_cast<uint32_t>(sizeof(NetHdr) + len), 0, 0};
    addAvail(n.tx, static_cast<uint16_t>(desc));
    notifyQueue(n, TX_QUEUE);
    return 1;
}

int receive(void* context, void* buffer, size_t maxLen) {
    Nic& n = *static_cast<Nic*>(context);
    uint32_t usedLength = 0;
    const int desc = getUsed(n.rx, &usedLength);
    if (desc < 0) {
        return -1;
    }
    const size_t payload = usedLength > sizeof(NetHdr) ? usedLength - sizeof(NetHdr) : 0;
    const size_t copied = payload < maxLen ? payload : maxLen;
    if (copied != 0) {
        memcpy(buffer, n.rxSlots + static_cast<size_t>(desc) * SLOT + sizeof(NetHdr), copied);
    }
    // Hand the buffer back, but notify only once per batch or when the ring
    // has been drained: each notify is a trap to the host.
    queueRx(n, desc);
    if (++n.rxRequeuedSinceNotify >= RX_NOTIFY_BATCH || !hasUsed(n.rx)) {
        n.rxRequeuedSinceNotify = 0;
        notifyQueue(n, RX_QUEUE);
    }
    return static_cast<int>(copied);
}

int linkUp(void* context) {
    Nic& n = *static_cast<Nic*>(context);
    if ((n.features & NET_F_STATUS) == 0 || !n.deviceCfg) {
        return 1;
    }
    return (n.deviceCfg->status & LINK_UP) != 0;
}

int interruptDriven(void* context) {
    return static_cast<Nic*>(context)->irqMode != IDRV_IRQ_NONE;
}

// Paravirtualized: preferred over the emulated physical cards.
const IdrvNetOps kNetOps = {
    "virtio-net", 10, 0, macAddress, send, receive, linkUp, interruptDriven,
};

// ---- Bring-up ----------------------------------------------------------------

bool findCapabilities(const IdrvKernelApi* api, const IdrvPciDevice* dev, Nic& n) {
    if ((api->pciRead(dev, PCI_STATUS_REG, 2) & PCI_STATUS_CAP_LIST) == 0) {
        return false;
    }
    uint8_t cap = cfg8(api, dev, PCI_CAP_PTR_REG) & 0xFC;
    for (uint32_t guard = 0; cap != 0 && guard < 64; ++guard) {
        const uint8_t id = cfg8(api, dev, cap);
        const uint8_t next = cfg8(api, dev, static_cast<uint16_t>(cap + 1)) & 0xFC;
        const uint8_t len = cfg8(api, dev, static_cast<uint16_t>(cap + 2));
        if (id == PCI_CAP_ID_VENDOR_SPECIFIC && len >= 16) {
            const uint8_t type = cfg8(api, dev, static_cast<uint16_t>(cap + 3));
            const uint8_t bar = cfg8(api, dev, static_cast<uint16_t>(cap + 4));
            const uint32_t offset = cfg32(api, dev, static_cast<uint16_t>(cap + 8));
            const uint32_t length = cfg32(api, dev, static_cast<uint16_t>(cap + 12));
            const uint64_t base = bar <= 5 ? api->pciMapBar(dev, bar, static_cast<uint64_t>(offset) + length) : 0;
            if (base != 0) {
                const uint64_t at = base + offset;
                if (type == CAP_COMMON_CFG) {
                    n.common = reinterpret_cast<volatile CommonCfg*>(at);
                } else if (type == CAP_NOTIFY_CFG && len >= 20) {
                    n.notifyBase = at;
                    n.notifyMultiplier = cfg32(api, dev, static_cast<uint16_t>(cap + 16));
                } else if (type == CAP_DEVICE_CFG) {
                    n.deviceCfg = reinterpret_cast<volatile NetConfig*>(at);
                } else if (type == CAP_ISR_CFG) {
                    n.isrStatus = reinterpret_cast<volatile uint8_t*>(at);
                }
            }
        }
        cap = next;
    }
    return n.common && n.notifyBase;
}

bool negotiateFeatures(Nic& n) {
    n.common->deviceFeatureSelect = 0;
    barrier();
    const uint64_t low = n.common->deviceFeature;
    n.common->deviceFeatureSelect = 1;
    barrier();
    const uint64_t offered = low | (static_cast<uint64_t>(n.common->deviceFeature) << 32);
    if ((offered & F_VERSION_1) == 0) {
        return false;   // legacy only: not spoken here
    }
    const uint64_t wanted = F_VERSION_1 | (offered & (NET_F_MAC | NET_F_STATUS));
    n.common->driverFeatureSelect = 0;
    n.common->driverFeature = static_cast<uint32_t>(wanted);
    n.common->driverFeatureSelect = 1;
    n.common->driverFeature = static_cast<uint32_t>(wanted >> 32);
    barrier();
    n.common->deviceStatus = n.common->deviceStatus | STATUS_FEATURES_OK;
    barrier();
    if ((n.common->deviceStatus & STATUS_FEATURES_OK) == 0) {
        return false;
    }
    n.features = wanted;
    return true;
}

bool setupQueue(const IdrvKernelApi* api, Nic& n, uint16_t index, Queue& q, bool interrupts) {
    n.common->queueSelect = index;
    barrier();
    uint16_t size = n.common->queueSize;
    if (size == 0) {
        return false;
    }
    size = size < QUEUE_SIZE ? size : QUEUE_SIZE;
    if (!queueInit(api, q, size)) {
        return false;
    }
    n.common->queueSize = size;
    if (!interrupts) {
        q.avail[0] = AVAIL_F_NO_INTERRUPT;
        n.common->queueMsixVector = NO_VECTOR;
    } else if (n.irqMode == IDRV_IRQ_MSIX) {
        n.common->queueMsixVector = 0;
        barrier();
        if (n.common->queueMsixVector != 0) {
            // The device refused the vector: fall back to polling.
            n.irqMode = IDRV_IRQ_NONE;
            api->log("[net] virtio-net: RX queue rejected its MSI-X vector, polling\n");
        }
    }
    n.common->queueDesc = q.phys;
    n.common->queueDriver = q.phys + 4096;
    n.common->queueDevice = q.phys + 8192;
    barrier();
    n.common->queueEnable = 1;
    return true;
}

int fail(Nic& n) {
    n.common->deviceStatus = n.common->deviceStatus | STATUS_FAILED;
    return IDRV_EIO;
}

int probe(const IdrvKernelApi* api, const IdrvPciDevice* dev) {
    Nic& n = gNic;
    if (n.bound) {
        return IDRV_ENODEV;   // one is enough for the stack
    }
    n.api = api;
    const uint32_t command = api->pciRead(dev, IDRV_PCI_COMMAND, 2);
    api->pciWrite(dev, IDRV_PCI_COMMAND, 2, command | IDRV_PCI_COMMAND_MEMORY | IDRV_PCI_COMMAND_BUS_MASTER);
    if (!findCapabilities(api, dev, n)) {
        api->log("[net] virtio-net: no modern virtio interface\n");
        return IDRV_ENODEV;
    }

    n.common->deviceStatus = 0;   // reset
    barrier();
    for (uint64_t i = 0; i < 1000000 && n.common->deviceStatus != 0; ++i) {
        idrv_pause();
    }
    if (n.common->deviceStatus != 0) {
        return IDRV_EIO;
    }
    n.common->deviceStatus = STATUS_ACKNOWLEDGE;
    barrier();
    n.common->deviceStatus = n.common->deviceStatus | STATUS_DRIVER;
    barrier();
    if (!negotiateFeatures(n) || n.common->numQueues <= TX_QUEUE) {
        return fail(n);
    }

    // Before the queues: their MSI-X vector numbers only stick once MSI-X is on.
    n.irqMode = IDRV_IRQ_NONE;
    if (IDRV_API_HAS(api, irqRegisterMsix) &&
        api->irqRegisterMsix(dev, 0, &kIrqOps, &n) == IDRV_IRQ_MSIX) {
        n.irqMode = IDRV_IRQ_MSIX;
        n.common->msixConfig = NO_VECTOR;   // no config-change interrupts
    } else if (n.isrStatus) {
        n.irqMode = api->irqRegister(dev, IDRV_IRQ_ALLOW_LEGACY, &kIrqOps, &n);
    }
    api->log(n.irqMode == IDRV_IRQ_MSIX ? "[net] virtio-net: MSI-X receive interrupt\n"
             : n.irqMode == IDRV_IRQ_LEGACY ? "[net] virtio-net: INTx receive interrupt\n"
             : "[net] virtio-net: no interrupt, polling\n");

    if (!setupQueue(api, n, RX_QUEUE, n.rx, true) || !setupQueue(api, n, TX_QUEUE, n.tx, false)) {
        return fail(n);
    }
    void* virt = nullptr;
    n.rxPhys = api->dmaAlloc((n.rx.size * SLOT + 4095) / 4096, 0, &virt);
    n.rxSlots = static_cast<uint8_t*>(virt);
    n.txPhys = api->dmaAlloc((n.tx.size * SLOT + 4095) / 4096, 0, &virt);
    n.txSlots = static_cast<uint8_t*>(virt);
    if (n.rxPhys == 0 || n.txPhys == 0) {
        api->log("[net] virtio-net: no memory for the buffers\n");
        return fail(n);
    }
    // Every RX descriptor gets its slot (allocated in order, so descriptor i
    // is slot i), then one notify.
    for (uint16_t i = 0; i < n.rx.size; ++i) {
        const int desc = allocDesc(n.rx);
        if (desc < 0) {
            return fail(n);
        }
        queueRx(n, desc);
    }
    notifyQueue(n, RX_QUEUE);

    if ((n.features & NET_F_MAC) != 0 && n.deviceCfg) {
        for (int i = 0; i < 6; ++i) {
            n.mac[i] = n.deviceCfg->mac[i];
        }
    } else if (IDRV_API_HAS(api, random)) {
        api->random(n.mac, 6);
        n.mac[0] = static_cast<uint8_t>((n.mac[0] & 0xFE) | 0x02);   // locally administered unicast
    }
    n.common->deviceStatus = n.common->deviceStatus | STATUS_DRIVER_OK;
    barrier();

    n.bound = true;
    return api->netRegister(&kNetOps, &n);
}

const IdrvPciMatch kMatches[] = {
    {VIRTIO_VENDOR, VIRTIO_NET_DEVICE, 0, 0},
    {VIRTIO_VENDOR, VIRTIO_NET_DEVICE_TRANSITIONAL, 0, 0},
};

}  // namespace

extern "C" IDRV_EXPORT const IdrvModule idrv_module = {
    IDRV_MODULE_MAGIC,
    IDRV_API_VERSION,
    "virtio-net",
    "1.0",
    "virtio network device (modern interface)",
    "InstantOS",
    kMatches,
    sizeof(kMatches) / sizeof(kMatches[0]),
    0,
    nullptr,
    probe,
    nullptr,
    IDRV_RUNTIME_API,
};
