#include <graphics/virtio_gpu.hpp>
#include <cpu/acpi/pci.hpp>
#include <memory/pmm.hpp>
#include <common/string.hpp>

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

constexpr uint32_t kControlQueueIndex = 0;
constexpr uint32_t kCursorQueueIndex = 1;
constexpr uint16_t kQueueSize = 64;
constexpr uint64_t kCommandTimeout = 1000000ULL;

static void virtio_barrier() {
    asm volatile("" ::: "memory");
}

static void cpu_pause() {
    asm volatile("pause" ::: "memory");
}

static uint16_t min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}

static uint64_t dma_address(const void* ptr) {
    if (!ptr) {
        return 0;
    }

    const uint64_t virt = reinterpret_cast<uint64_t>(ptr);
    return virt;
}
}

VirtIOGPUDriver& VirtIOGPUDriver::get() {
    static VirtIOGPUDriver instance;
    return instance;
}

VirtIOGPUDriver::VirtIOGPUDriver()
    : initialized(false),
      deviceFound(false),
      bus(0),
      device(0),
      function(0),
      commonCfg(nullptr),
      notifyBase(nullptr),
      notifyOffMultiplier(0),
      deviceCfg(nullptr),
      framebuffer(nullptr),
      framebufferPhys(0),
      maxWidth(0),
      maxHeight(0),
      currentWidth(0),
      currentHeight(0),
      fbSize(0),
      resourceId(1),
      commandLock(0) {
}

bool VirtIOGPUDriver::initialize() {
    if (initialized) {
        return true;
    }

    if (!detectDevice()) {
        return false;
    }

    if (!initDevice()) {
        return false;
    }

    if (!getDisplayInfo()) {
        return false;
    }

    if (currentWidth == 0 || currentHeight == 0) {
        return false;
    }

    if (!createResource()) {
        return false;
    }

    if (!attachBacking()) {
        return false;
    }

    if (!setScanout()) {
        return false;
    }

    flush(0, 0, currentWidth, currentHeight);
    initialized = true;
    return true;
}

bool VirtIOGPUDriver::detectDevice() {
    PCI& pci = PCI::get();

    for (uint16_t b = 0; b < 256; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                const uint16_t vendor = pci.readConfig16(0, b, d, f, PCI_VENDOR_ID_REG);
                if (vendor == 0xFFFF) {
                    continue;
                }

                const uint16_t devId = pci.readConfig16(0, b, d, f, PCI_DEVICE_ID_REG);
                if (vendor == VIRTIO_VENDOR_ID && devId == VIRTIO_GPU_DEVICE_ID) {
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

bool VirtIOGPUDriver::initDevice() {
    PCI& pci = PCI::get();

    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND_REG);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    pci.writeConfig16(0, bus, device, function, PCI_COMMAND_REG, command);

    commonCfg = nullptr;
    notifyBase = nullptr;
    deviceCfg = nullptr;
    notifyOffMultiplier = 0;

    const uint16_t status = readConfig16(PCI_STATUS_REG);
    if ((status & PCI_STATUS_CAP_LIST) == 0) {
        return false;
    }

    uint8_t capPtr = readConfig8(PCI_CAP_PTR_REG) & 0xFC;
    for (uint32_t guard = 0; capPtr != 0 && guard < 64; ++guard) {
        const uint8_t capId = readConfig8(capPtr);
        const uint8_t next = readConfig8(capPtr + 1) & 0xFC;
        const uint8_t capLen = readConfig8(capPtr + 2);

        if (capId == PCI_CAP_ID_VENDOR_SPECIFIC && capLen >= sizeof(VirtioPCICapability)) {
            const uint8_t cfgType = readConfig8(capPtr + 3);
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
                    deviceCfg = mapped;
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

    commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_DRIVER_OK;
    virtio_barrier();
    return true;
}

bool VirtIOGPUDriver::negotiateFeatures() {
    commonCfg->device_feature_select = 0;
    virtio_barrier();
    const uint64_t featuresLow = commonCfg->device_feature;
    commonCfg->device_feature_select = 1;
    virtio_barrier();
    const uint64_t featuresHigh = commonCfg->device_feature;
    const uint64_t deviceFeatures = featuresLow | (featuresHigh << 32);

    const uint64_t wantedFeatures = VIRTIO_F_VERSION_1;
    if ((deviceFeatures & wantedFeatures) != wantedFeatures) {
        return false;
    }

    commonCfg->driver_feature_select = 0;
    commonCfg->driver_feature = static_cast<uint32_t>(wantedFeatures);
    commonCfg->driver_feature_select = 1;
    commonCfg->driver_feature = static_cast<uint32_t>(wantedFeatures >> 32);
    virtio_barrier();

    commonCfg->device_status = commonCfg->device_status | VIRTIO_STATUS_FEATURES_OK;
    virtio_barrier();
    return (commonCfg->device_status & VIRTIO_STATUS_FEATURES_OK) != 0;
}

bool VirtIOGPUDriver::setupQueues() {
    if (commonCfg->num_queues <= kControlQueueIndex) {
        return false;
    }

    commonCfg->queue_select = kControlQueueIndex;
    virtio_barrier();

    uint16_t controlSize = commonCfg->queue_size;
    if (controlSize == 0) {
        return false;
    }
    controlSize = min_u16(controlSize, kQueueSize);
    if (!controlQueue.init(controlSize)) {
        return false;
    }

    commonCfg->queue_size = controlSize;
    commonCfg->queue_desc = dma_address(reinterpret_cast<void*>(controlQueue.getDescAddr()));
    commonCfg->queue_driver = dma_address(reinterpret_cast<void*>(controlQueue.getAvailAddr()));
    commonCfg->queue_device = dma_address(reinterpret_cast<void*>(controlQueue.getUsedAddr()));
    virtio_barrier();
    commonCfg->queue_enable = 1;

    if (commonCfg->num_queues > kCursorQueueIndex) {
        commonCfg->queue_select = kCursorQueueIndex;
        virtio_barrier();
        uint16_t cursorSize = commonCfg->queue_size;
        if (cursorSize != 0) {
            cursorSize = min_u16(cursorSize, kQueueSize);
            if (cursorQueue.init(cursorSize)) {
                commonCfg->queue_size = cursorSize;
                commonCfg->queue_desc = dma_address(reinterpret_cast<void*>(cursorQueue.getDescAddr()));
                commonCfg->queue_driver = dma_address(reinterpret_cast<void*>(cursorQueue.getAvailAddr()));
                commonCfg->queue_device = dma_address(reinterpret_cast<void*>(cursorQueue.getUsedAddr()));
                virtio_barrier();
                commonCfg->queue_enable = 1;
            }
        }
    }

    return true;
}

bool VirtIOGPUDriver::getDisplayInfo() {
    VirtIOGPUCtrlHdr request = {};
    VirtIOGPURespDisplayInfo response = {};
    request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response))) {
        return false;
    }

    if (response.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        return false;
    }

    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i) {
        const VirtIOGPUDisplayOne& mode = response.pmodes[i];
        if (mode.enabled && mode.r.width > 0 && mode.r.height > 0) {
            maxWidth = mode.r.width;
            maxHeight = mode.r.height;
            currentWidth = mode.r.width;
            currentHeight = mode.r.height;
            return true;
        }
    }

    return false;
}

bool VirtIOGPUDriver::createResource() {
    fbSize = currentWidth * currentHeight * 4;
    const uint64_t pages = (static_cast<uint64_t>(fbSize) + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    framebufferPhys = PMM::AllocFrames(pages);
    if (!framebufferPhys) {
        return false;
    }

    framebuffer = reinterpret_cast<void*>(framebufferPhys);
    memset(framebuffer, 0, pages * PMM::PAGE_SIZE);

    VirtIOGPUResourceCreate2D request = {};
    VirtIOGPUCtrlHdr response = {};
    request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    request.resource_id = resourceId;
    request.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    request.width = currentWidth;
    request.height = currentHeight;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response))) {
        return false;
    }

    return response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::attachBacking() {
    struct AttachBackingRequest {
        VirtIOGPUResourceAttachBacking attach;
        VirtIOGPUMemEntry entry;
    } __attribute__((packed));

    AttachBackingRequest request = {};
    VirtIOGPUCtrlHdr response = {};
    request.attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    request.attach.resource_id = resourceId;
    request.attach.nr_entries = 1;
    request.entry.addr = framebufferPhys;
    request.entry.length = fbSize;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response))) {
        return false;
    }

    return response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::setScanout() {
    VirtIOGPUSetScanout request = {};
    VirtIOGPUCtrlHdr response = {};
    request.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    request.r = { 0, 0, currentWidth, currentHeight };
    request.scanout_id = 0;
    request.resource_id = resourceId;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response))) {
        return false;
    }

    return response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::setMode(uint32_t width, uint32_t height) {
    if (width == currentWidth && height == currentHeight) {
        return true;
    }

    return false;
}

void VirtIOGPUDriver::getMode(uint32_t* width, uint32_t* height) {
    if (width) {
        *width = currentWidth;
    }
    if (height) {
        *height = currentHeight;
    }
}

bool VirtIOGPUDriver::flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!deviceFound || !framebuffer || currentWidth == 0 || currentHeight == 0) {
        return false;
    }

    if (x >= currentWidth || y >= currentHeight || w == 0 || h == 0) {
        return true;
    }

    if (x + w < x || x + w > currentWidth) {
        w = currentWidth - x;
    }
    if (y + h < y || y + h > currentHeight) {
        h = currentHeight - y;
    }

    VirtIOGPUTransferToHost2D transfer = {};
    VirtIOGPUCtrlHdr transferResponse = {};
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r = { x, y, w, h };
    transfer.offset = static_cast<uint64_t>(y) * currentWidth * 4 + static_cast<uint64_t>(x) * 4;
    transfer.resource_id = resourceId;

    if (!sendCommand(&transfer, sizeof(transfer), &transferResponse, sizeof(transferResponse)) ||
        transferResponse.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    VirtIOGPUResourceFlush flushRequest = {};
    VirtIOGPUCtrlHdr flushResponse = {};
    flushRequest.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flushRequest.r = { x, y, w, h };
    flushRequest.resource_id = resourceId;

    if (!sendCommand(&flushRequest, sizeof(flushRequest), &flushResponse, sizeof(flushResponse))) {
        return false;
    }

    return flushResponse.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::sendCommand(void* cmd, size_t cmdSize, void* resp, size_t respSize) {
    if (!cmd || cmdSize == 0 || !resp || respSize == 0 || !notifyBase) {
        return false;
    }

    while (__sync_lock_test_and_set(&commandLock, 1)) {
        cpu_pause();
    }

    const int cmdDesc = controlQueue.allocDesc();
    const int respDesc = controlQueue.allocDesc();
    if (cmdDesc < 0 || respDesc < 0) {
        if (cmdDesc >= 0) {
            controlQueue.freeDesc(cmdDesc);
        }
        if (respDesc >= 0) {
            controlQueue.freeDesc(respDesc);
        }
        __sync_lock_release(&commandLock);
        return false;
    }

    controlQueue.setDesc(
        cmdDesc,
        dma_address(cmd),
        static_cast<uint32_t>(cmdSize),
        VIRTQ_DESC_F_NEXT,
        static_cast<uint16_t>(respDesc)
    );
    controlQueue.setDesc(
        respDesc,
        dma_address(resp),
        static_cast<uint32_t>(respSize),
        VIRTQ_DESC_F_WRITE,
        0
    );

    controlQueue.addAvail(static_cast<uint16_t>(cmdDesc));
    notifyQueue(kControlQueueIndex);

    bool completed = false;
    for (uint64_t i = 0; i < kCommandTimeout; ++i) {
        if (controlQueue.hasUsed()) {
            uint32_t usedLen = 0;
            controlQueue.getUsed(&usedLen);
            completed = true;
            break;
        }
        cpu_pause();
    }

    controlQueue.freeDesc(respDesc);
    controlQueue.freeDesc(cmdDesc);
    __sync_lock_release(&commandLock);
    return completed;
}

void VirtIOGPUDriver::notifyQueue(uint16_t queueIdx) {
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

void* VirtIOGPUDriver::mapBar(uint8_t barIdx, size_t size) {
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

    if (size == 0) {
        size = PMM::PAGE_SIZE;
    }

    return reinterpret_cast<void*>(base);
}

uint8_t VirtIOGPUDriver::readConfig8(uint16_t offset) {
    return PCI::get().readConfig8(0, bus, device, function, offset);
}

uint16_t VirtIOGPUDriver::readConfig16(uint16_t offset) {
    return PCI::get().readConfig16(0, bus, device, function, offset);
}

uint32_t VirtIOGPUDriver::readConfig32(uint16_t offset) {
    return static_cast<uint32_t>(readConfig8(offset)) |
           (static_cast<uint32_t>(readConfig8(offset + 1)) << 8) |
           (static_cast<uint32_t>(readConfig8(offset + 2)) << 16) |
           (static_cast<uint32_t>(readConfig8(offset + 3)) << 24);
}

void VirtIOGPUDriver::writeConfig8(uint16_t offset, uint8_t value) {
    PCI::get().writeConfig8(0, bus, device, function, offset, value);
}

void VirtIOGPUDriver::writeConfig16(uint16_t offset, uint16_t value) {
    PCI::get().writeConfig16(0, bus, device, function, offset, value);
}

void VirtIOGPUDriver::writeConfig32(uint16_t offset, uint32_t value) {
    PCI::get().writeConfig32(0, bus, device, function, offset, value);
}
