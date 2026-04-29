#include <graphics/virtio_gpu.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/idt/interrupt.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
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

static bool interrupts_enabled() {
    uint64_t flags = 0;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static void cpu_wait_for_interrupt() {
    asm volatile("hlt");
}

static uint16_t min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}

struct DMABuffer {
    void* ptr;
    uint64_t phys;
    uint64_t pages;
};

static DMABuffer allocate_dma_buffer(size_t size) {
    DMABuffer buffer = {};
    if (size == 0) {
        return buffer;
    }

    const uint64_t pages = (static_cast<uint64_t>(size) + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t phys = PMM::AllocFrames(pages);
    if (!phys) {
        return buffer;
    }

    buffer.ptr = reinterpret_cast<void*>(phys);
    buffer.phys = phys;
    buffer.pages = pages;
    memset(buffer.ptr, 0, pages * PMM::PAGE_SIZE);
    return buffer;
}

static void free_dma_buffer(DMABuffer* buffer) {
    if (!buffer || !buffer->phys || buffer->pages == 0) {
        return;
    }

    for (uint64_t i = 0; i < buffer->pages; ++i) {
        PMM::FreeFrame(buffer->phys + i * PMM::PAGE_SIZE);
    }

    buffer->ptr = nullptr;
    buffer->phys = 0;
    buffer->pages = 0;
}

struct VirtIOGPUCapsetResponse {
    VirtIOGPUCtrlHdr hdr;
    uint8_t data[];
};

struct VirtIOGPUSubmit3DRequest {
    VirtIOGPUCmdSubmit3D submit;
    uint8_t commands[];
};

class VirtIOGPUInterruptHandler : public Interrupt {
public:
    explicit VirtIOGPUInterruptHandler(VirtIOGPUDriver& ownerIn) : owner(ownerIn) {
    }

    void initialize() override {
    }

    void Run(InterruptFrame* frame) override {
        (void)frame;
        owner.handleInterrupt();
    }

private:
    VirtIOGPUDriver& owner;
};

VirtIOGPUInterruptHandler g_virtioGpuInterruptHandler(VirtIOGPUDriver::get());
}

VirtIOGPUDriver& VirtIOGPUDriver::get() {
    static VirtIOGPUDriver instance;
    return instance;
}

VirtIOGPUDriver::VirtIOGPUDriver()
    : initialized(false),
      deviceFound(false),
      virglSupported(false),
      contextInitSupported(false),
      resourceBlobSupported(false),
      bus(0),
      device(0),
      function(0),
      commonCfg(nullptr),
      notifyBase(nullptr),
      notifyOffMultiplier(0),
      isrCfg(nullptr),
      deviceCfg(nullptr),
      framebuffer(nullptr),
      framebufferPhys(0),
      maxWidth(0),
      maxHeight(0),
      currentWidth(0),
      currentHeight(0),
      numCapsets(0),
      fbSize(0),
      resourceId(1),
      nextResourceId(2),
      nextContextId(1),
      nextFenceId(1),
      lastSubmittedFence(0),
      lastCompletedFence(0),
      lastResponseFence(0),
      lastResponseType(VIRTIO_GPU_UNDEFINED),
      lastUsedLength(0),
      lastCommandStatus{},
      pciIrqLine(0xFF),
      pciIrqVector(0),
      lastInterruptStatus(0),
      interruptCount(0),
      queueInterruptCount(0),
      configInterruptCount(0),
      irqRegistered(false),
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
        releaseFramebufferMemory();
        return false;
    }

    if (!attachBacking()) {
        destroyFramebufferResource();
        releaseFramebufferMemory();
        return false;
    }

    if (!setScanout()) {
        destroyFramebufferResource();
        releaseFramebufferMemory();
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
    isrCfg = nullptr;

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
                } else if (cfgType == VIRTIO_PCI_CAP_ISR_CFG) {
                    isrCfg = reinterpret_cast<volatile uint8_t*>(mapped);
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

    irqRegistered = PCI::get().registerLegacyInterrupt(0, bus, device, function,
        &g_virtioGpuInterruptHandler, &pciIrqLine, &pciIrqVector);

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

    const uint64_t requiredFeatures = VIRTIO_F_VERSION_1;
    if ((deviceFeatures & requiredFeatures) != requiredFeatures) {
        return false;
    }

    virglSupported = (deviceFeatures & VIRTIO_GPU_F_VIRGL) != 0;
    contextInitSupported = (deviceFeatures & VIRTIO_GPU_F_CONTEXT_INIT) != 0;
    resourceBlobSupported = (deviceFeatures & VIRTIO_GPU_F_RESOURCE_BLOB) != 0;

    uint64_t wantedFeatures = requiredFeatures;
    if (virglSupported) {
        wantedFeatures |= VIRTIO_GPU_F_VIRGL;
    }
    if (contextInitSupported) {
        wantedFeatures |= VIRTIO_GPU_F_CONTEXT_INIT;
    }
    if (resourceBlobSupported) {
        wantedFeatures |= VIRTIO_GPU_F_RESOURCE_BLOB;
    }

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
    commonCfg->queue_desc = controlQueue.getDescAddr();
    commonCfg->queue_driver = controlQueue.getAvailAddr();
    commonCfg->queue_device = controlQueue.getUsedAddr();
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
                commonCfg->queue_desc = cursorQueue.getDescAddr();
                commonCfg->queue_driver = cursorQueue.getAvailAddr();
                commonCfg->queue_device = cursorQueue.getUsedAddr();
                virtio_barrier();
                commonCfg->queue_enable = 1;
            }
        }
    }

    return true;
}

bool VirtIOGPUDriver::getDisplayInfo() {
    if (deviceCfg) {
        const auto* gpuCfg = reinterpret_cast<volatile const VirtIOGPUConfig*>(deviceCfg);
        numCapsets = gpuCfg->num_capsets;
    }

    VirtIOGPUCtrlHdr request = {};
    VirtIOGPURespDisplayInfo response = {};
    initializeHeader(&request, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

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
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    request.resource_id = resourceId;
    request.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    request.width = currentWidth;
    request.height = currentHeight;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response))) {
        releaseFramebufferMemory();
        return false;
    }

    if (response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        releaseFramebufferMemory();
        return false;
    }

    return true;
}

bool VirtIOGPUDriver::attachBacking() {
    return attachBacking(resourceId, framebufferPhys, fbSize);
}

bool VirtIOGPUDriver::setScanout() {
    VirtIOGPUSetScanout request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
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
    initializeHeader(&transfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    transfer.r = { x, y, w, h };
    transfer.offset = static_cast<uint64_t>(y) * currentWidth * 4 + static_cast<uint64_t>(x) * 4;
    transfer.resource_id = resourceId;

    if (!sendCommand(&transfer, sizeof(transfer), &transferResponse, sizeof(transferResponse)) ||
        transferResponse.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    VirtIOGPUResourceFlush flushRequest = {};
    VirtIOGPUCtrlHdr flushResponse = {};
    initializeHeader(&flushRequest.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flushRequest.r = { x, y, w, h };
    flushRequest.resource_id = resourceId;

    if (!sendCommand(&flushRequest, sizeof(flushRequest), &flushResponse, sizeof(flushResponse))) {
        return false;
    }

    return flushResponse.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::getCapsetInfo(uint32_t capsetIndex, VirtIOGPUCapsetInfo* info) {
    if (!virglSupported || !info || capsetIndex >= numCapsets) {
        return false;
    }

    VirtIOGPUGetCapsetInfo request = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    request.capset_index = capsetIndex;

    if (!sendCommand(&request, sizeof(request), info, sizeof(*info))) {
        return false;
    }

    return info->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO;
}

bool VirtIOGPUDriver::getCapset(uint32_t capsetId, uint32_t capsetVersion, void* buffer, uint32_t bufferSize, uint32_t* actualSize) {
    if (!virglSupported || !buffer || bufferSize == 0) {
        return false;
    }

    const size_t responseSize = sizeof(VirtIOGPUCtrlHdr) + bufferSize;
    auto* response = reinterpret_cast<VirtIOGPUCapsetResponse*>(kmalloc(responseSize));
    if (!response) {
        return false;
    }

    VirtIOGPUGetCapset request = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_GET_CAPSET);
    request.capset_id = capsetId;
    request.capset_version = capsetVersion;

    const bool ok = sendCommand(&request, sizeof(request), response, responseSize) &&
                    response->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET;
    if (ok) {
        memcpy(buffer, response->data, bufferSize);
        if (actualSize) {
            *actualSize = bufferSize;
        }
    }

    kfree(response);
    return ok;
}

bool VirtIOGPUDriver::createContext(uint32_t* ctxId, const char* debugName, uint32_t contextInit) {
    if (!virglSupported || !ctxId) {
        return false;
    }

    VirtIOGPUCtxCreate request = {};
    VirtIOGPUCtrlHdr response = {};
    const uint32_t newCtxId = allocateContextId();
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_CREATE, newCtxId);
    request.context_init = contextInitSupported ? contextInit : 0;

    if (debugName) {
        const size_t nameLength = strlen(debugName);
        const uint32_t copyLength = static_cast<uint32_t>(
            nameLength < (VIRTIO_GPU_CONTEXT_NAME_MAX - 1) ? nameLength : (VIRTIO_GPU_CONTEXT_NAME_MAX - 1));
        request.nlen = copyLength;
        memcpy(request.debug_name, debugName, copyLength);
        request.debug_name[copyLength] = '\0';
    }

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    *ctxId = newCtxId;
    return true;
}

bool VirtIOGPUDriver::destroyContext(uint32_t ctxId) {
    if (!virglSupported || ctxId == 0) {
        return false;
    }

    VirtIOGPUCtxDestroy request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_DESTROY, ctxId);

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::attachResourceToContext(uint32_t ctxId, uint32_t resourceIdValue) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUCtxResource request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctxId);
    request.resource_id = resourceIdValue;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::detachResourceFromContext(uint32_t ctxId, uint32_t resourceIdValue) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUCtxResource request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctxId);
    request.resource_id = resourceIdValue;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::createResource3D(const VirtIOGPUResourceCreate3D& resource, uint32_t* outResourceId) {
    if (!virglSupported) {
        return false;
    }

    VirtIOGPUResourceCreate3D request = resource;
    VirtIOGPUCtrlHdr response = {};
    const uint32_t newResourceId = allocateResourceId();
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, request.hdr.ctx_id);
    request.resource_id = newResourceId;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    if (outResourceId) {
        *outResourceId = newResourceId;
    }
    return true;
}

bool VirtIOGPUDriver::destroyResource3D(uint32_t ctxId, uint32_t resourceIdValue, bool hasBacking) {
    if (!virglSupported || resourceIdValue == 0) {
        return false;
    }

    bool ok = true;
    if (ctxId != 0) {
        ok = detachResourceFromContext(ctxId, resourceIdValue) && ok;
    }
    if (hasBacking) {
        ok = detachResourceBacking(resourceIdValue) && ok;
    }
    ok = unrefResource(resourceIdValue) && ok;
    return ok;
}

bool VirtIOGPUDriver::destroyContextWithResource(uint32_t ctxId, uint32_t resourceIdValue, bool resourceHasBacking) {
    if (!virglSupported || ctxId == 0) {
        return false;
    }

    bool ok = true;
    if (resourceIdValue != 0) {
        ok = destroyResource3D(ctxId, resourceIdValue, resourceHasBacking) && ok;
    }
    ok = destroyContext(ctxId) && ok;
    return ok;
}

bool VirtIOGPUDriver::attachBacking(uint32_t resourceIdValue, uint64_t guestAddress, uint32_t length) {
    if (resourceIdValue == 0 || guestAddress == 0 || length == 0) {
        return false;
    }

    struct AttachBackingRequest {
        VirtIOGPUResourceAttachBacking attach;
        VirtIOGPUMemEntry entry;
    };

    AttachBackingRequest request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    request.attach.resource_id = resourceIdValue;
    request.attach.nr_entries = 1;
    request.entry.addr = guestAddress;
    request.entry.length = length;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::detachResourceBacking(uint32_t resourceIdValue) {
    if (resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUResourceDetachBacking request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    request.resource_id = resourceIdValue;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::unrefResource(uint32_t resourceIdValue) {
    if (resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUResourceUnref request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    request.resource_id = resourceIdValue;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::transferToHost3D(uint32_t ctxId, uint32_t resourceIdValue, const VirtIOGPUBox& box,
                                       uint64_t offset, uint32_t level, uint32_t stride, uint32_t layerStride) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUTransferHost3D request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, ctxId);
    request.box = box;
    request.offset = offset;
    request.resource_id = resourceIdValue;
    request.level = level;
    request.stride = stride;
    request.layer_stride = layerStride;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::transferFromHost3D(uint32_t ctxId, uint32_t resourceIdValue, const VirtIOGPUBox& box,
                                         uint64_t offset, uint32_t level, uint32_t stride, uint32_t layerStride) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUTransferHost3D request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, ctxId);
    request.box = box;
    request.offset = offset;
    request.resource_id = resourceIdValue;
    request.level = level;
    request.stride = stride;
    request.layer_stride = layerStride;

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::submit3D(uint32_t ctxId, const void* commands, uint32_t size) {
    if (!virglSupported || ctxId == 0 || !commands || size == 0) {
        return false;
    }

    const size_t requestSize = sizeof(VirtIOGPUCmdSubmit3D) + size;
    auto* request = reinterpret_cast<VirtIOGPUSubmit3DRequest*>(kmalloc(requestSize));
    if (!request) {
        return false;
    }

    VirtIOGPUCtrlHdr response = {};
    memset(request, 0, requestSize);
    initializeHeader(&request->submit.hdr, VIRTIO_GPU_CMD_SUBMIT_3D, ctxId);
    request->submit.size = size;
    memcpy(request->commands, commands, size);

    const bool ok = sendCommand(request, requestSize, &response, sizeof(response)) &&
                    response.type == VIRTIO_GPU_RESP_OK_NODATA;
    kfree(request);
    return ok;
}

uint32_t VirtIOGPUDriver::allocateResourceId() {
    return nextResourceId++;
}

uint32_t VirtIOGPUDriver::allocateContextId() {
    return nextContextId++;
}

void VirtIOGPUDriver::resetCommandStatus() {
    lastCommandStatus = {};
    lastCommandStatus.requestType = VIRTIO_GPU_UNDEFINED;
    lastCommandStatus.responseType = VIRTIO_GPU_UNDEFINED;
}

void VirtIOGPUDriver::initializeHeader(VirtIOGPUCtrlHdr* hdr, uint32_t type, uint32_t ctxId) {
    if (!hdr) {
        return;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->ctx_id = ctxId;
    hdr->flags = VIRTIO_GPU_FLAG_FENCE;
    hdr->fence_id = nextFenceId++;
    lastSubmittedFence = hdr->fence_id;
    lastCommandStatus.requestType = type;
    lastCommandStatus.submittedFence = hdr->fence_id;
}

void VirtIOGPUDriver::handleInterrupt() {
    if (isrCfg) {
        lastInterruptStatus = *isrCfg;
    } else {
        lastInterruptStatus = 0;
    }

    ++interruptCount;
    if (lastInterruptStatus & 0x1U) {
        ++queueInterruptCount;
    }
    if (lastInterruptStatus & 0x2U) {
        ++configInterruptCount;
    }
}

bool VirtIOGPUDriver::waitForFence(uint16_t headDesc, uint64_t expectedFence, void* resp, size_t respSize) {
    for (uint64_t i = 0; i < kCommandTimeout; ++i) {
        if (!controlQueue.hasUsed()) {
            if (irqRegistered && isrCfg && interrupts_enabled()) {
                cpu_wait_for_interrupt();
            } else {
                cpu_pause();
            }
            continue;
        }

        uint32_t usedLen = 0;
        const uint32_t usedId = controlQueue.getUsed(&usedLen);
        lastUsedLength = usedLen;
        lastCommandStatus.usedLength = usedLen;
        if (usedId != headDesc) {
            return false;
        }

        if (respSize < sizeof(VirtIOGPUCtrlHdr)) {
            lastCompletedFence = expectedFence;
            lastCommandStatus.completedFence = expectedFence;
            return true;
        }

        auto* responseHdr = reinterpret_cast<VirtIOGPUCtrlHdr*>(resp);
        lastResponseFence = responseHdr->fence_id;
        lastResponseType = responseHdr->type;
        lastCommandStatus.responseType = responseHdr->type;
        if (responseHdr->fence_id != expectedFence) {
            return false;
        }

        lastCompletedFence = responseHdr->fence_id;
        lastCommandStatus.completedFence = responseHdr->fence_id;
        return true;
    }

    return false;
}

bool VirtIOGPUDriver::sendCommand(const void* cmd, size_t cmdSize, void* resp, size_t respSize) {
    if (!cmd || cmdSize == 0 || !resp || respSize == 0 || !notifyBase) {
        return false;
    }

    resetCommandStatus();
    const auto* commandHdr = reinterpret_cast<const VirtIOGPUCtrlHdr*>(cmd);
    const uint64_t expectedFence = cmdSize >= sizeof(VirtIOGPUCtrlHdr) ? commandHdr->fence_id : 0;
    if (cmdSize >= sizeof(VirtIOGPUCtrlHdr)) {
        lastCommandStatus.requestType = commandHdr->type;
        lastCommandStatus.submittedFence = expectedFence;
    }

    DMABuffer cmdBuffer = allocate_dma_buffer(cmdSize);
    DMABuffer respBuffer = allocate_dma_buffer(respSize);
    if (!cmdBuffer.ptr || !respBuffer.ptr) {
        free_dma_buffer(&respBuffer);
        free_dma_buffer(&cmdBuffer);
        return false;
    }

    memcpy(cmdBuffer.ptr, cmd, cmdSize);

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
        cmdBuffer.phys,
        static_cast<uint32_t>(cmdSize),
        VIRTQ_DESC_F_NEXT,
        static_cast<uint16_t>(respDesc)
    );
    controlQueue.setDesc(
        respDesc,
        respBuffer.phys,
        static_cast<uint32_t>(respSize),
        VIRTQ_DESC_F_WRITE,
        0
    );

    controlQueue.addAvail(static_cast<uint16_t>(cmdDesc));
    notifyQueue(kControlQueueIndex);

    const bool completed = waitForFence(static_cast<uint16_t>(cmdDesc), expectedFence, resp, respSize);
    if (completed) {
        memcpy(resp, respBuffer.ptr, respSize);
        lastCommandStatus.transportOk = true;
        if (respSize >= sizeof(VirtIOGPUCtrlHdr)) {
            const auto* responseHdr = reinterpret_cast<const VirtIOGPUCtrlHdr*>(resp);
            lastCommandStatus.responseType = responseHdr->type;
            lastCommandStatus.completedFence = responseHdr->fence_id;
            lastCommandStatus.responseOk = responseHdr->type < VIRTIO_GPU_RESP_ERR_UNSPEC;
        } else {
            lastCommandStatus.responseOk = true;
        }
    }

    controlQueue.freeDesc(respDesc);
    controlQueue.freeDesc(cmdDesc);
    __sync_lock_release(&commandLock);
    free_dma_buffer(&respBuffer);
    free_dma_buffer(&cmdBuffer);
    return completed;
}

bool VirtIOGPUDriver::destroyFramebufferResource() {
    bool ok = true;
    if (fbSize != 0) {
        ok = detachResourceBacking(resourceId) && ok;
    }
    ok = unrefResource(resourceId) && ok;
    return ok;
}

void VirtIOGPUDriver::releaseFramebufferMemory() {
    if (!framebufferPhys || fbSize == 0) {
        framebuffer = nullptr;
        framebufferPhys = 0;
        fbSize = 0;
        return;
    }

    const uint64_t pages = (static_cast<uint64_t>(fbSize) + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    for (uint64_t i = 0; i < pages; ++i) {
        PMM::FreeFrame(framebufferPhys + i * PMM::PAGE_SIZE);
    }

    framebuffer = nullptr;
    framebufferPhys = 0;
    fbSize = 0;
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
