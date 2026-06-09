#include <drivers/virtio/virtio_net.hpp>
#include <cpu/acpi/pci.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>

namespace {
constexpr uint16_t PCI_VENDOR_ID_REG = 0x00;
constexpr uint16_t PCI_DEVICE_ID_REG = 0x02;
constexpr uint16_t PCI_COMMAND_REG = 0x04;
constexpr uint16_t PCI_STATUS_REG = 0x06;
constexpr uint16_t PCI_CAP_PTR_REG = 0x34;
constexpr uint16_t PCI_BAR0_REG = 0x10;

constexpr uint16_t PCI_STATUS_CAP_LIST = 1 << 4;
constexpr uint16_t PCI_COMMAND_MEMORY = 1 << 1;
constexpr uint16_t PCI_COMMAND_BUS_MASTER = 1 << 2;
constexpr uint8_t PCI_CAP_ID_VENDOR_SPECIFIC = 0x09;

constexpr uint16_t kReceiveQueueIndex = 0;
constexpr uint16_t kTransmitQueueIndex = 1;
constexpr uint16_t kQueueSize = 32;
constexpr uint16_t kVirtioNetStatusLinkUp = 1;
constexpr uint32_t kTxCompletionSpinLimit = 1000000;
constexpr uint32_t kInvalidUsedDescriptor = 0xFFFFFFFFU;

static void virtio_barrier() {
    asm volatile("" ::: "memory");
}

static void cpu_pause() {
    asm volatile("pause");
}

uint16_t min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}
}

VirtIONetDriver& VirtIONetDriver::get() {
    static VirtIONetDriver instance;
    return instance;
}

VirtIONetDriver::VirtIONetDriver()
    : initialized(false),
      deviceFound(false),
      bus(0),
      device(0),
      function(0),
      commonCfg(nullptr),
      notifyBase(nullptr),
      notifyOffMultiplier(0),
      deviceCfg(nullptr),
      negotiatedFeatures(0),
      rxBuffers{},
      rxBuffersPhys{},
      rxDesc{},
      txBuffers{},
      macAddr{} {
    for (size_t i = 0; i < VIRTIO_NET_RX_BUFFERS; ++i) {
        rxDesc[i] = -1;
    }
}

bool VirtIONetDriver::initialize() {
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

bool VirtIONetDriver::detectDevice() {
    PCI& pci = PCI::get();
    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_VENDOR_ID_REG);
                if (vendor == 0xFFFF) {
                    continue;
                }
                const uint16_t devId = pci.readConfig16(0, static_cast<uint8_t>(b), d, f, PCI_DEVICE_ID_REG);
                if (vendor == VIRTIO_VENDOR_ID && devId == VIRTIO_NET_DEVICE_ID) {
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

bool VirtIONetDriver::initDevice() {
    PCI& pci = PCI::get();
    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND_REG);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    pci.writeConfig16(0, bus, device, function, PCI_COMMAND_REG, command);

    commonCfg = nullptr;
    notifyBase = nullptr;
    deviceCfg = nullptr;
    notifyOffMultiplier = 0;
    negotiatedFeatures = 0;

    const uint16_t status = readConfig16(PCI_STATUS_REG);
    if ((status & PCI_STATUS_CAP_LIST) == 0) {
        return false;
    }

    uint8_t capPtr = readConfig8(PCI_CAP_PTR_REG) & 0xFC;
    for (uint32_t guard = 0; capPtr != 0 && guard < 64; ++guard) {
        const uint8_t capId = readConfig8(capPtr);
        const uint8_t next = readConfig8(capPtr + 1) & 0xFC;
        const uint8_t capLen = readConfig8(capPtr + 2);
        const uint8_t cfgType = capId == PCI_CAP_ID_VENDOR_SPECIFIC ? readConfig8(capPtr + 3) : 0;

        if (capId == PCI_CAP_ID_VENDOR_SPECIFIC && capLen >= sizeof(VirtioPCICapability)) {
            const uint8_t bar = readConfig8(capPtr + 4);
            const uint32_t offset = readConfig32(capPtr + 8);
            const uint32_t length = readConfig32(capPtr + 12);
            void* barBase = mapBar(bar, offset + length);
            if (barBase) {
                auto* mapped = reinterpret_cast<uint8_t*>(barBase) + offset;
                if (cfgType == VIRTIO_PCI_CAP_COMMON_CFG) {
                    commonCfg = reinterpret_cast<volatile VirtioPCICommonCfg*>(mapped);
                } else if (cfgType == VIRTIO_PCI_CAP_NOTIFY_CFG && capLen >= sizeof(VirtioPCINotifyCap)) {
                    notifyBase = reinterpret_cast<volatile uint32_t*>(mapped);
                    notifyOffMultiplier = readConfig32(capPtr + 16);
                } else if (cfgType == VIRTIO_PCI_CAP_DEVICE_CFG) {
                    deviceCfg = reinterpret_cast<volatile VirtIONetConfig*>(mapped);
                }
            }
        }

        capPtr = next;
    }

    if (!commonCfg || !notifyBase) {
        return false;
    }

    commonCfg->device_status = 0;
    virtio_barrier();
    for (uint64_t i = 0; i < 1000000 && commonCfg->device_status != 0; ++i) {
        cpu_pause();
    }
    if (commonCfg->device_status != 0) {
        return false;
    }

    commonCfg->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    virtio_barrier();
    commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_DRIVER;
    virtio_barrier();

    if (!negotiateFeatures()) {
        commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_FAILED;
        return false;
    }
    if (!setupQueues()) {
        commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_FAILED;
        return false;
    }
    if (!fillRxQueue()) {
        commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_FAILED;
        return false;
    }

    if ((negotiatedFeatures & VIRTIO_NET_F_MAC) != 0 && deviceCfg) {
        for (uint8_t i = 0; i < 6; ++i) {
            macAddr[i] = deviceCfg->mac[i];
        }
    }

    commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_DRIVER_OK;
    virtio_barrier();
    return true;
}

bool VirtIONetDriver::negotiateFeatures() {
    commonCfg->device_feature_select = 0;
    virtio_barrier();
    const uint64_t featuresLow = commonCfg->device_feature;
    commonCfg->device_feature_select = 1;
    virtio_barrier();
    const uint64_t featuresHigh = commonCfg->device_feature;
    const uint64_t deviceFeatures = featuresLow | (featuresHigh << 32);

    const uint64_t requiredFeatures = VIRTIO_F_VERSION_1;
    if ((deviceFeatures & requiredFeatures) != requiredFeatures) {
        return false;
    }

    uint64_t wantedFeatures = requiredFeatures;
    if ((deviceFeatures & VIRTIO_NET_F_MAC) != 0) {
        wantedFeatures |= VIRTIO_NET_F_MAC;
    }
    if ((deviceFeatures & VIRTIO_NET_F_STATUS) != 0) {
        wantedFeatures |= VIRTIO_NET_F_STATUS;
    }

    commonCfg->driver_feature_select = 0;
    commonCfg->driver_feature = static_cast<uint32_t>(wantedFeatures);
    commonCfg->driver_feature_select = 1;
    commonCfg->driver_feature = static_cast<uint32_t>(wantedFeatures >> 32);
    virtio_barrier();

    commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_FEATURES_OK;
    virtio_barrier();
    if ((commonCfg->device_status & VIRTIO_STATUS_FEATURES_OK) == 0) {
        return false;
    }

    negotiatedFeatures = wantedFeatures;
    return true;
}

bool VirtIONetDriver::setupQueues() {
    if (commonCfg->num_queues <= kTransmitQueueIndex) {
        return false;
    }

    commonCfg->queue_select = kReceiveQueueIndex;
    virtio_barrier();
    uint16_t rxSize = commonCfg->queue_size;
    if (rxSize == 0) {
        return false;
    }
    rxSize = min_u16(rxSize, kQueueSize);
    if (!rxQueue.init(rxSize)) {
        return false;
    }
    commonCfg->queue_size = rxSize;
    commonCfg->queue_desc = rxQueue.getDescAddr();
    commonCfg->queue_driver = rxQueue.getAvailAddr();
    commonCfg->queue_device = rxQueue.getUsedAddr();
    virtio_barrier();
    commonCfg->queue_enable = 1;

    commonCfg->queue_select = kTransmitQueueIndex;
    virtio_barrier();
    uint16_t txSize = commonCfg->queue_size;
    if (txSize == 0) {
        return false;
    }
    txSize = min_u16(txSize, kQueueSize);
    if (!txQueue.init(txSize)) {
        return false;
    }
    commonCfg->queue_size = txSize;
    commonCfg->queue_desc = txQueue.getDescAddr();
    commonCfg->queue_driver = txQueue.getAvailAddr();
    commonCfg->queue_device = txQueue.getUsedAddr();
    virtio_barrier();
    commonCfg->queue_enable = 1;
    return true;
}

bool VirtIONetDriver::fillRxQueue() {
    const size_t count = rxQueue.getSize() < VIRTIO_NET_RX_BUFFERS ? rxQueue.getSize() : VIRTIO_NET_RX_BUFFERS;
    for (size_t i = 0; i < count; ++i) {
        if (!queueRxBuffer(i)) {
            return false;
        }
    }
    notifyQueue(kReceiveQueueIndex);
    return true;
}

bool VirtIONetDriver::queueRxBuffer(size_t index) {
    if (index >= VIRTIO_NET_RX_BUFFERS) {
        return false;
    }
    if (!rxBuffers[index]) {
        const uint64_t phys = PMM::AllocFrames(1);
        if (phys == 0) {
            return false;
        }
        rxBuffers[index] = reinterpret_cast<void*>(phys);
        rxBuffersPhys[index] = phys;
        memset(rxBuffers[index], 0, PMM::PAGE_SIZE);
    }

    const int desc = rxQueue.allocDesc();
    if (desc < 0) {
        return false;
    }
    rxDesc[index] = desc;
    rxQueue.setDesc(desc, rxBuffersPhys[index], VIRTIO_NET_BUFFER_SIZE, VIRTQ_DESC_F_WRITE, 0);
    rxQueue.addAvail(static_cast<uint16_t>(desc));
    return true;
}

void VirtIONetDriver::getMacAddress(uint8_t* mac) {
    if (!mac) {
        return;
    }
    memcpy(mac, macAddr, sizeof(macAddr));
}

bool VirtIONetDriver::sendPacket(const void* data, size_t len) {
    if (!initialized || !data || len == 0 || len > VIRTIO_NET_MTU) {
        return false;
    }

    reclaimTxCompletions();

    const int desc = txQueue.allocDesc();
    if (desc < 0) {
        return false;
    }
    if (static_cast<size_t>(desc) >= VIRTIO_NET_TX_BUFFERS) {
        txQueue.freeDesc(desc);
        return false;
    }
    const uint64_t phys = PMM::AllocFrames(1);
    if (phys == 0) {
        txQueue.freeDesc(desc);
        return false;
    }
    txBuffers[desc] = phys;

    auto* packet = reinterpret_cast<uint8_t*>(phys);
    memset(packet, 0, sizeof(VirtIONetHdr));
    memcpy(packet + sizeof(VirtIONetHdr), data, len);

    txQueue.setDesc(desc, phys, static_cast<uint32_t>(sizeof(VirtIONetHdr) + len), 0, 0);
    txQueue.addAvail(static_cast<uint16_t>(desc));
    notifyQueue(kTransmitQueueIndex);

    uint32_t usedLength = 0;
    for (uint32_t spin = 0; spin < kTxCompletionSpinLimit; ++spin) {
        if (!txQueue.hasUsed()) {
            cpu_pause();
            continue;
        }
        const uint32_t usedDesc = txQueue.getUsed(&usedLength);
        if (usedDesc != kInvalidUsedDescriptor) {
            if (usedDesc < VIRTIO_NET_TX_BUFFERS && txBuffers[usedDesc] != 0) {
                PMM::FreeFrames(txBuffers[usedDesc], 1);
                txBuffers[usedDesc] = 0;
            }
            txQueue.freeDesc(static_cast<int>(usedDesc));
        }
        if (usedDesc == static_cast<uint32_t>(desc)) {
            return true;
        }
    }

    return true;
}

void VirtIONetDriver::reclaimTxCompletions() {
    uint32_t usedLength = 0;
    while (txQueue.hasUsed()) {
        const uint32_t usedDesc = txQueue.getUsed(&usedLength);
        if (usedDesc == kInvalidUsedDescriptor) {
            continue;
        }
        if (usedDesc < VIRTIO_NET_TX_BUFFERS && txBuffers[usedDesc] != 0) {
            PMM::FreeFrames(txBuffers[usedDesc], 1);
            txBuffers[usedDesc] = 0;
        }
        txQueue.freeDesc(static_cast<int>(usedDesc));
    }
}

int VirtIONetDriver::receivePacket(void* buffer, size_t maxLen) {
    if (!initialized || !buffer || maxLen == 0) {
        return -1;
    }
    if (!rxQueue.hasUsed()) {
        return -1;
    }

    uint32_t usedLength = 0;
    const uint32_t usedDesc = rxQueue.getUsed(&usedLength);
    if (usedDesc == kInvalidUsedDescriptor) {
        return -1;
    }

    size_t index = VIRTIO_NET_RX_BUFFERS;
    for (size_t i = 0; i < VIRTIO_NET_RX_BUFFERS; ++i) {
        if (rxDesc[i] == static_cast<int>(usedDesc)) {
            index = i;
            break;
        }
    }
    if (index == VIRTIO_NET_RX_BUFFERS || !rxBuffers[index]) {
        rxQueue.freeDesc(static_cast<int>(usedDesc));
        return -1;
    }

    const size_t payloadLength = usedLength > sizeof(VirtIONetHdr) ? usedLength - sizeof(VirtIONetHdr) : 0;
    const size_t copied = payloadLength < maxLen ? payloadLength : maxLen;
    if (copied != 0) {
        memcpy(buffer, reinterpret_cast<uint8_t*>(rxBuffers[index]) + sizeof(VirtIONetHdr), copied);
    }

    rxQueue.freeDesc(static_cast<int>(usedDesc));
    rxDesc[index] = -1;
    if (queueRxBuffer(index)) {
        notifyQueue(kReceiveQueueIndex);
    }
    return static_cast<int>(copied);
}

bool VirtIONetDriver::isLinkUp() const {
    if (!initialized) {
        return false;
    }
    if ((negotiatedFeatures & VIRTIO_NET_F_STATUS) == 0 || !deviceCfg) {
        return true;
    }
    return (deviceCfg->status & kVirtioNetStatusLinkUp) != 0;
}

void VirtIONetDriver::notifyQueue(uint16_t queueIdx) {
    if (!commonCfg || !notifyBase) {
        return;
    }
    commonCfg->queue_select = queueIdx;
    virtio_barrier();
    const uint16_t notifyOffset = commonCfg->queue_notify_off;
    auto* notify = reinterpret_cast<volatile uint16_t*>(
        reinterpret_cast<uintptr_t>(notifyBase) +
        static_cast<uintptr_t>(notifyOffset) * notifyOffMultiplier
    );
    *notify = queueIdx;
}

void* VirtIONetDriver::mapBar(uint8_t barIdx, size_t size) {
    (void)size;
    if (barIdx > 5) {
        return nullptr;
    }
    const uint16_t barOffset = PCI_BAR0_REG + static_cast<uint16_t>(barIdx) * 4;
    const uint32_t barLow = readConfig32(barOffset);
    if (barLow == 0 || barLow == 0xFFFFFFFF) {
        return nullptr;
    }
    if (barLow & 0x1) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(barLow & ~0x3U));
    }
    uint64_t base = barLow & ~0xFULL;
    const uint32_t barType = (barLow >> 1) & 0x3;
    if (barType == 0x2 && barIdx < 5) {
        const uint32_t barHigh = readConfig32(barOffset + 4);
        base |= static_cast<uint64_t>(barHigh) << 32;
    }
    if (base == 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(base);
}

uint8_t VirtIONetDriver::readConfig8(uint16_t offset) {
    return PCI::get().readConfig8(0, bus, device, function, offset);
}

uint16_t VirtIONetDriver::readConfig16(uint16_t offset) {
    return PCI::get().readConfig16(0, bus, device, function, offset);
}

uint32_t VirtIONetDriver::readConfig32(uint16_t offset) {
    return static_cast<uint32_t>(readConfig8(offset)) |
           (static_cast<uint32_t>(readConfig8(offset + 1)) << 8) |
           (static_cast<uint32_t>(readConfig8(offset + 2)) << 16) |
           (static_cast<uint32_t>(readConfig8(offset + 3)) << 24);
}

void VirtIONetDriver::writeConfig8(uint16_t offset, uint8_t value) {
    PCI::get().writeConfig8(0, bus, device, function, offset, value);
}

void VirtIONetDriver::writeConfig16(uint16_t offset, uint16_t value) {
    PCI::get().writeConfig16(0, bus, device, function, offset, value);
}

void VirtIONetDriver::writeConfig32(uint16_t offset, uint32_t value) {
    PCI::get().writeConfig32(0, bus, device, function, offset, value);
}
