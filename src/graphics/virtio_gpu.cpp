#include <graphics/virtio_gpu.hpp>
#include <cpu/acpi/pci.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
#include <interrupts/timer.hpp>
#include <common/string.hpp>
#include <graphics/console.hpp>

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

static uint32_t min_u32(uint32_t a, uint32_t b) {
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

    bool shouldDispatch() override {
        return owner.claimPendingInterrupt();
    }

    void Run(InterruptFrame* frame) override {
        (void)frame;
        owner.handleInterrupt();
    }

private:
    VirtIOGPUDriver& owner;
};

VirtIOGPUInterruptHandler& getVirtIOGPUInterruptHandler() {
    static VirtIOGPUInterruptHandler handler(VirtIOGPUDriver::get());
    return handler;
}
}

VirtIOGPUDriver& VirtIOGPUDriver::get() {
    static VirtIOGPUDriver instance;
    return instance;
}

const char* VirtIOGPUDriver::describeControlType(uint32_t type) {
    switch (type) {
        case VIRTIO_GPU_CMD_GET_DISPLAY_INFO: return "GET_DISPLAY_INFO";
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: return "RESOURCE_CREATE_2D";
        case VIRTIO_GPU_CMD_RESOURCE_UNREF: return "RESOURCE_UNREF";
        case VIRTIO_GPU_CMD_SET_SCANOUT: return "SET_SCANOUT";
        case VIRTIO_GPU_CMD_RESOURCE_FLUSH: return "RESOURCE_FLUSH";
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: return "TRANSFER_TO_HOST_2D";
        case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: return "RESOURCE_ATTACH_BACKING";
        case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: return "RESOURCE_DETACH_BACKING";
        case VIRTIO_GPU_CMD_GET_CAPSET_INFO: return "GET_CAPSET_INFO";
        case VIRTIO_GPU_CMD_GET_CAPSET: return "GET_CAPSET";
        case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID: return "RESOURCE_ASSIGN_UUID";
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: return "RESOURCE_CREATE_BLOB";
        case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB: return "SET_SCANOUT_BLOB";
        case VIRTIO_GPU_CMD_CTX_CREATE: return "CTX_CREATE";
        case VIRTIO_GPU_CMD_CTX_DESTROY: return "CTX_DESTROY";
        case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE: return "CTX_ATTACH_RESOURCE";
        case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: return "CTX_DETACH_RESOURCE";
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: return "RESOURCE_CREATE_3D";
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D: return "TRANSFER_TO_HOST_3D";
        case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: return "TRANSFER_FROM_HOST_3D";
        case VIRTIO_GPU_CMD_SUBMIT_3D: return "SUBMIT_3D";
        case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: return "RESOURCE_MAP_BLOB";
        case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: return "RESOURCE_UNMAP_BLOB";
        default: return "UNKNOWN_CMD";
    }
}

const char* VirtIOGPUDriver::describeResponseType(uint32_t type) {
    switch (type) {
        case VIRTIO_GPU_RESP_OK_NODATA: return "OK_NODATA";
        case VIRTIO_GPU_RESP_OK_DISPLAY_INFO: return "OK_DISPLAY_INFO";
        case VIRTIO_GPU_RESP_OK_CAPSET_INFO: return "OK_CAPSET_INFO";
        case VIRTIO_GPU_RESP_OK_CAPSET: return "OK_CAPSET";
        case VIRTIO_GPU_RESP_OK_EDID: return "OK_EDID";
        case VIRTIO_GPU_RESP_OK_RESOURCE_UUID: return "OK_RESOURCE_UUID";
        case VIRTIO_GPU_RESP_OK_MAP_INFO: return "OK_MAP_INFO";
        case VIRTIO_GPU_RESP_ERR_UNSPEC: return "ERR_UNSPEC";
        case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY: return "ERR_OUT_OF_MEMORY";
        case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID: return "ERR_INVALID_SCANOUT_ID";
        case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID: return "ERR_INVALID_RESOURCE_ID";
        case VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID: return "ERR_INVALID_CONTEXT_ID";
        case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER: return "ERR_INVALID_PARAMETER";
        default: return "UNKNOWN_RESP";
    }
}

bool VirtIOGPUDriver::waitForFence(uint64_t fenceId, uint64_t spinLimit, uint64_t* completedFence, uint32_t* responseType) {
    if (fenceId == 0) {
        return false;
    }

    for (uint64_t i = 0; i < spinLimit; ++i) {
        processControlQueueCompletions();
        if (lastCompletedFence >= fenceId) {
            if (completedFence) {
                *completedFence = lastCompletedFence;
            }
            if (responseType) {
                *responseType = lastResponseType;
            }
            return true;
        }

        if (irqRegistered && isrCfg && interrupts_enabled()) {
            cpu_wait_for_interrupt();
        } else {
            cpu_pause();
        }
    }

    if (completedFence) {
        *completedFence = lastCompletedFence;
    }
    if (responseType) {
        *responseType = lastResponseType;
    }
    return false;
}

VirtIOGPUDriver::VirtIOGPUDriver()
    : initialized(false),
      deviceFound(false),
      virglSupported(false),
      contextInitSupported(false),
      resourceBlobSupported(false),
      resourceUUIDSupported(false),
      blobScanoutActive(false),
      bus(0),
      device(0),
      function(0),
      commonCfg(nullptr),
      notifyBase(nullptr),
      notifyOffMultiplier(0),
      isrCfg(nullptr),
      deviceCfg(nullptr),
      hostVisibleShmBase(nullptr),
      hostVisibleShmLength(0),
      nextHostVisibleOffset(0),
      framebuffer(nullptr),
      framebufferPhys(0),
      maxWidth(0),
      maxHeight(0),
      currentWidth(0),
      currentHeight(0),
      fallbackWidth(0),
      fallbackHeight(0),
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
      irqMode(IRQMode::None),
      interruptLatched(false),
      commandLock(0) {
    memset(fenceWaiters, 0, sizeof(fenceWaiters));
    memset(resourceRecords, 0, sizeof(resourceRecords));
    memset(contextRecords, 0, sizeof(contextRecords));
}

bool VirtIOGPUDriver::initialize() {
    if (initialized) {
        return true;
    }

    if (!detectDevice()) {
        Console::get().drawText("[VGPU:init] detectDevice failed\n");
        return false;
    }

    if (!initDevice()) {
        Console::get().drawText("[VGPU:init] initDevice failed\n");
        return false;
    }

    if (!getDisplayInfo()) {
        if (fallbackWidth != 0 && fallbackHeight != 0) {
            Console::get().drawText("[VGPU:init] getDisplayInfo failed, forcing boot fallback\n");
            maxWidth = fallbackWidth;
            maxHeight = fallbackHeight;
            currentWidth = fallbackWidth;
            currentHeight = fallbackHeight;
        } else {
            Console::get().drawText("[VGPU:init] getDisplayInfo failed\n");
            return false;
        }
    }

    if (currentWidth == 0 || currentHeight == 0) {
        Console::get().drawText("[VGPU:init] invalid mode\n");
        return false;
    }

    if (!createResource()) {
        Console::get().drawText("[VGPU:init] createResource failed\n");
        releaseFramebufferMemory();
        return false;
    }

    if (!attachBacking()) {
        if (!blobScanoutActive) {
            Console::get().drawText("[VGPU:init] attachBacking failed\n");
            destroyFramebufferResource();
            releaseFramebufferMemory();
            return false;
        }
        Console::get().drawText("[VGPU:init] blob attachBacking skipped\n");
    }

    if (!blobScanoutActive && !setScanout()) {
        Console::get().drawText("[VGPU:init] legacy setScanout failed\n");
        destroyFramebufferResource();
        releaseFramebufferMemory();
        return false;
    }

    if (blobScanoutActive && !setScanout()) {
        Console::get().drawText("[VGPU:init] blob setScanout failed, falling back\n");
        destroyFramebufferResource();
        releaseFramebufferMemory();
        if (!createResource(false)) {
            Console::get().drawText("[VGPU:init] fallback createResource failed\n");
            releaseFramebufferMemory();
            return false;
        }
        if (!attachBacking() || !setScanout()) {
            Console::get().drawText("[VGPU:init] fallback scanout path failed\n");
            destroyFramebufferResource();
            releaseFramebufferMemory();
            return false;
        }
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
    hostVisibleShmBase = nullptr;
    hostVisibleShmLength = 0;
    nextHostVisibleOffset = 0;

    const uint16_t status = readConfig16(PCI_STATUS_REG);
    if ((status & PCI_STATUS_CAP_LIST) == 0) {
        return false;
    }

    uint8_t capPtr = readConfig8(PCI_CAP_PTR_REG) & 0xFC;
    for (uint32_t guard = 0; capPtr != 0 && guard < 64; ++guard) {
        const uint8_t capId = readConfig8(capPtr);
        const uint8_t next = readConfig8(capPtr + 1) & 0xFC;
        const uint8_t capLen = readConfig8(capPtr + 2);

        const uint8_t cfgType = (capId == PCI_CAP_ID_VENDOR_SPECIFIC) ? readConfig8(capPtr + 3) : 0;
        if (capId == PCI_CAP_ID_VENDOR_SPECIFIC &&
            cfgType != VIRTIO_PCI_CAP_SHARED_MEMORY_CFG &&
            capLen >= sizeof(VirtioPCICapability)) {
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
        } else if (capId == PCI_CAP_ID_VENDOR_SPECIFIC &&
                   capLen >= sizeof(VirtioPCISharedMemoryCap) &&
                   cfgType == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG) {
            const uint8_t bar = readConfig8(capPtr + 4);
            const uint8_t shmId = readConfig8(capPtr + 5);
            const uint64_t offset = static_cast<uint64_t>(readConfig32(capPtr + 8)) |
                                    (static_cast<uint64_t>(readConfig32(capPtr + 16)) << 32);
            const uint64_t length = static_cast<uint64_t>(readConfig32(capPtr + 12)) |
                                    (static_cast<uint64_t>(readConfig32(capPtr + 20)) << 32);
            if (shmId == VIRTIO_GPU_SHM_ID_HOST_VISIBLE && length != 0) {
                void* barBase = mapBar(bar, static_cast<size_t>(offset + length));
                if (barBase) {
                    hostVisibleShmBase = reinterpret_cast<uint8_t*>(barBase) + offset;
                    hostVisibleShmLength = length;
                    nextHostVisibleOffset = 0;
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

    pciIrqLine = 0xFF;
    pciIrqVector = 0;
    irqMode = IRQMode::None;
    irqRegistered = PCI::get().registerMSIInterrupt(0, bus, device, function,
        &getVirtIOGPUInterruptHandler(), &pciIrqVector);
    if (irqRegistered) {
        irqMode = IRQMode::MSI;
    }
    if (!irqRegistered) {
        irqRegistered = PCI::get().registerLegacyInterrupt(0, bus, device, function,
            &getVirtIOGPUInterruptHandler(), &pciIrqLine, &pciIrqVector);
        if (irqRegistered) {
            irqMode = IRQMode::LegacyINTx;
        }
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

    const uint64_t requiredFeatures = VIRTIO_F_VERSION_1;
    if ((deviceFeatures & requiredFeatures) != requiredFeatures) {
        return false;
    }

    virglSupported = (deviceFeatures & VIRTIO_GPU_F_VIRGL) != 0;
    contextInitSupported = (deviceFeatures & VIRTIO_GPU_F_CONTEXT_INIT) != 0;
    resourceBlobSupported = (deviceFeatures & VIRTIO_GPU_F_RESOURCE_BLOB) != 0;
    resourceUUIDSupported = (deviceFeatures & VIRTIO_GPU_F_RESOURCE_UUID) != 0;

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
    if (resourceUUIDSupported) {
        wantedFeatures |= VIRTIO_GPU_F_RESOURCE_UUID;
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
        Console::get().drawText("[VGPU:init] display info resp=");
        Console::get().drawText(describeResponseType(response.hdr.type));
        Console::get().drawText("\n");
        return false;
    }

    uint32_t pmodeFallbackWidth = 0;
    uint32_t pmodeFallbackHeight = 0;
    for (uint32_t i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i) {
        const VirtIOGPUDisplayOne& mode = response.pmodes[i];
        if (mode.r.width > 0 && mode.r.height > 0 && pmodeFallbackWidth == 0 && pmodeFallbackHeight == 0) {
            pmodeFallbackWidth = mode.r.width;
            pmodeFallbackHeight = mode.r.height;
        }

        if (mode.enabled && mode.r.width > 0 && mode.r.height > 0) {
            maxWidth = mode.r.width;
            maxHeight = mode.r.height;
            currentWidth = mode.r.width;
            currentHeight = mode.r.height;
            return true;
        }
    }

    if (pmodeFallbackWidth != 0 && pmodeFallbackHeight != 0) {
        Console::get().drawText("[VGPU:init] using pmodes fallback\n");
        maxWidth = pmodeFallbackWidth;
        maxHeight = pmodeFallbackHeight;
        currentWidth = pmodeFallbackWidth;
        currentHeight = pmodeFallbackHeight;
        return true;
    }

    if (fallbackWidth != 0 && fallbackHeight != 0) {
        Console::get().drawText("[VGPU:init] using boot fallback mode\n");
        maxWidth = fallbackWidth;
        maxHeight = fallbackHeight;
        currentWidth = fallbackWidth;
        currentHeight = fallbackHeight;
        return true;
    }

    Console::get().drawText("[VGPU:init] fallback dims=");
    Console::get().drawNumber(static_cast<int64_t>(fallbackWidth));
    Console::get().drawText("x");
    Console::get().drawNumber(static_cast<int64_t>(fallbackHeight));
    Console::get().drawText("\n");
    Console::get().drawText("[VGPU:init] no usable scanout modes\n");
    return false;
}

void VirtIOGPUDriver::setFallbackDisplayMode(uint32_t width, uint32_t height) {
    fallbackWidth = width;
    fallbackHeight = height;
}

bool VirtIOGPUDriver::createResource(bool allowBlobScanout) {
    fbSize = currentWidth * currentHeight * 4;
    blobScanoutActive = false;

    if (allowBlobScanout && resourceBlobSupported && hostVisibleShmBase && hostVisibleShmLength >= fbSize) {
        void* mapping = nullptr;
        uint32_t blobResourceId = 0;
        if (allocateHostVisibleBlob(fbSize, &blobResourceId, &mapping)) {
            framebuffer = mapping;
            resourceId = blobResourceId;
            memset(framebuffer, 0, fbSize);
            blobScanoutActive = true;
            if (ResourceRecord* record = findResourceRecord(resourceId)) {
                record->guestPtr = framebuffer;
                record->guestLength = fbSize;
                framebufferPhys = record->guestPhys;
            } else {
                framebufferPhys = 0;
            }
            return true;
        }
    }

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

    ResourceRecord* record = ensureResourceRecord(resourceId);
    if (!record) {
        unrefResource(resourceId);
        releaseFramebufferMemory();
        return false;
    }

    record->backingType = ResourceBackingType::GuestBacking;
    record->size = fbSize;
    record->guestPtr = framebuffer;
    record->guestPhys = framebufferPhys;
    record->guestLength = fbSize;

    return true;
}

bool VirtIOGPUDriver::attachBacking() {
    if (blobScanoutActive) {
        return true;
    }
    return attachBacking(resourceId, framebufferPhys, fbSize);
}

bool VirtIOGPUDriver::setScanout() {
    if (blobScanoutActive) {
        VirtIOGPUSetScanoutBlob request = {};
        request.r = { 0, 0, currentWidth, currentHeight };
        request.scanout_id = 0;
        request.resource_id = resourceId;
        request.width = currentWidth;
        request.height = currentHeight;
        request.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        request.strides[0] = currentWidth * 4;
        request.offsets[0] = 0;
        return setScanoutBlob(request);
    }

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

    if (!blobScanoutActive) {
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

bool VirtIOGPUDriver::runVirglProbe(VirtIOGPUVirglProbeResult* result) {
    VirtIOGPUVirglProbeResult localResult = {};
    if (!virglSupported) {
        if (result) {
            *result = localResult;
        }
        return false;
    }

    VirtIOGPUCapsetInfo chosenCapset = {};
    bool foundCapset = false;
    for (uint32_t i = 0; i < numCapsets; ++i) {
        VirtIOGPUCapsetInfo info = {};
        if (!getCapsetInfo(i, &info)) {
            continue;
        }
        localResult.capsetInfoOk = true;
        if (!foundCapset &&
            (info.capset_id == VIRTIO_GPU_CAPSET_VIRGL || info.capset_id == VIRTIO_GPU_CAPSET_VIRGL2)) {
            chosenCapset = info;
            foundCapset = true;
        }
    }

    if (!foundCapset) {
        if (result) {
            *result = localResult;
        }
        return false;
    }

    localResult.capsetId = chosenCapset.capset_id;
    localResult.capsetVersion = chosenCapset.capset_max_version;
    localResult.capsetSize = chosenCapset.capset_max_size;

    uint32_t actualCapsetSize = 0;
    uint8_t* capsetData = nullptr;
    if (chosenCapset.capset_max_size != 0) {
        capsetData = reinterpret_cast<uint8_t*>(kmalloc(chosenCapset.capset_max_size));
    }
    if (capsetData &&
        getCapset(chosenCapset.capset_id, chosenCapset.capset_max_version, capsetData,
                  chosenCapset.capset_max_size, &actualCapsetSize)) {
        localResult.capsetFetchOk = true;
        localResult.capsetSize = actualCapsetSize;
    }
    if (capsetData) {
        kfree(capsetData);
    }

    if (!localResult.capsetFetchOk) {
        if (result) {
            *result = localResult;
        }
        return false;
    }

    uint32_t ctxId = 0;
    bool contextOk = false;
    if (contextInitSupported) {
        contextOk = createContextWithCapset(&ctxId, chosenCapset.capset_id, "virgl-probe");
    } else {
        contextOk = createContext(&ctxId, "virgl-probe");
    }
    if (!contextOk) {
        if (result) {
            *result = localResult;
        }
        return false;
    }

    localResult.contextCreateOk = true;
    localResult.ctxId = ctxId;

    uint32_t resourceIdValue = 0;
    VirtIOGPUResourceCreate3D resource = {};
    resource.hdr.ctx_id = ctxId;
    resource.target = 2;
    resource.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    resource.bind = 0;
    resource.width = 64;
    resource.height = 64;
    resource.depth = 1;
    resource.array_size = 1;
    resource.last_level = 0;
    resource.nr_samples = 1;
    if (createResource3D(resource, &resourceIdValue)) {
        localResult.resourceCreateOk = true;
        localResult.resourceId = resourceIdValue;
    }

    if (localResult.resourceCreateOk && attachResourceToContext(ctxId, resourceIdValue)) {
        localResult.resourceAttachOk = true;
    }

    uint32_t minimalCommands[1] = {0};
    const bool submitTransportOk = submit3D(ctxId, minimalCommands, sizeof(minimalCommands));
    const VirtIOGPUCommandStatus submitStatus = getLastCommandStatus();
    localResult.submitTransportOk = submitTransportOk;
    localResult.submitResponseOk = submitStatus.responseOk;
    localResult.responseType = submitStatus.responseType;
    localResult.submittedFence = submitStatus.submittedFence;
    localResult.completedFence = submitStatus.completedFence;
    localResult.fenceCompleted = submitStatus.completedFence >= submitStatus.submittedFence &&
                                 submitStatus.submittedFence != 0;

    if (resourceIdValue != 0) {
        destroyResource3D(ctxId, resourceIdValue);
    }
    destroyContext(ctxId);

    if (result) {
        *result = localResult;
    }

    return localResult.capsetInfoOk &&
           localResult.capsetFetchOk &&
           localResult.contextCreateOk &&
           localResult.resourceCreateOk &&
           localResult.resourceAttachOk &&
           localResult.submitTransportOk &&
           localResult.fenceCompleted;
}

bool VirtIOGPUDriver::createContext(uint32_t* ctxId, const char* debugName, uint32_t contextInit,
                                    uint8_t ringIdx, bool useRingIdx) {
    if (!virglSupported || !ctxId) {
        return false;
    }

    VirtIOGPUCtxCreate request = {};
    VirtIOGPUCtrlHdr response = {};
    const uint32_t newCtxId = allocateContextId();
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_CREATE, newCtxId, ringIdx, useRingIdx);
    request.context_init = sanitizeContextInit(contextInit);

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

    ContextRecord* record = ensureContextRecord(newCtxId);
    if (!record) {
        destroyContext(newCtxId);
        return false;
    }

    record->contextInit = request.context_init;
    record->capsetId = request.context_init & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    record->ringIdx = ringIdx;
    record->useRingIdx = useRingIdx;

    *ctxId = newCtxId;
    return true;
}

bool VirtIOGPUDriver::createContextWithCapset(uint32_t* ctxId, uint32_t capsetId, const char* debugName,
                                              uint32_t contextInitFlags, uint8_t ringIdx, bool useRingIdx) {
    if (!contextInitSupported || capsetId > VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK) {
        return false;
    }

    const uint32_t contextInit = sanitizeContextInit(contextInitFlags, capsetId);
    return createContext(ctxId, debugName, contextInit, ringIdx, useRingIdx);
}

bool VirtIOGPUDriver::destroyContext(uint32_t ctxId) {
    if (!virglSupported || ctxId == 0) {
        return false;
    }

    VirtIOGPUCtxDestroy request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_DESTROY, ctxId);

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    releaseContextRecord(ctxId);
    return true;
}

bool VirtIOGPUDriver::configureContextRing(uint32_t ctxId, uint8_t ringIdx, bool useRingIdx) {
    ContextRecord* record = requireContextRecord(ctxId);
    if (!record) {
        return false;
    }

    record->ringIdx = ringIdx;
    record->useRingIdx = useRingIdx;
    return true;
}

bool VirtIOGPUDriver::attachResourceToContext(uint32_t ctxId, uint32_t resourceIdValue) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0 || !requireContextRecord(ctxId)) {
        return false;
    }

    VirtIOGPUCtxResource request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctxId);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    return bindResourceToContextRecord(ctxId, resourceIdValue);
}

bool VirtIOGPUDriver::detachResourceFromContext(uint32_t ctxId, uint32_t resourceIdValue) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0 || !requireContextRecord(ctxId)) {
        return false;
    }

    VirtIOGPUCtxResource request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctxId);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    unbindResourceFromContextRecord(ctxId, resourceIdValue);
    return true;
}

bool VirtIOGPUDriver::createResource3D(const VirtIOGPUResourceCreate3D& resource, uint32_t* outResourceId) {
    if (!virglSupported) {
        return false;
    }

    if (resource.hdr.ctx_id != 0 && !requireContextRecord(resource.hdr.ctx_id)) {
        return false;
    }

    VirtIOGPUResourceCreate3D request = resource;
    VirtIOGPUCtrlHdr response = {};
    const uint32_t newResourceId = allocateResourceId();
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, request.hdr.ctx_id,
                     request.hdr.ring_idx, (request.hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) != 0);
    request.resource_id = newResourceId;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    ResourceRecord* record = ensureResourceRecord(newResourceId);
    if (!record) {
        unrefResource(newResourceId);
        return false;
    }
    record->size = static_cast<uint64_t>(request.width) * request.height * request.depth;

    if (outResourceId) {
        *outResourceId = newResourceId;
    }
    return true;
}

bool VirtIOGPUDriver::createBlobResource(const VirtIOGPUResourceCreateBlob& resource, uint32_t* outResourceId,
                                         uint64_t guestAddress, uint32_t guestLength) {
    if (!resourceBlobSupported) {
        return false;
    }

    if (resource.hdr.ctx_id != 0 && !requireContextRecord(resource.hdr.ctx_id)) {
        return false;
    }

    const bool needsGuestEntry = resource.nr_entries != 0;
    if (needsGuestEntry && (guestAddress == 0 || guestLength == 0)) {
        return false;
    }
    if (!needsGuestEntry && (guestAddress != 0 || guestLength != 0)) {
        return false;
    }

    struct BlobCreateWithEntry {
        VirtIOGPUResourceCreateBlob create;
        VirtIOGPUMemEntry entry;
    };

    const uint32_t newResourceId = allocateResourceId();
    VirtIOGPUCtrlHdr response = {};
    bool ok = false;

    if (needsGuestEntry) {
        BlobCreateWithEntry request = {};
        request.create = resource;
        initializeHeader(&request.create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, resource.hdr.ctx_id,
                         resource.hdr.ring_idx, (resource.hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) != 0);
        request.create.resource_id = newResourceId;
        request.create.nr_entries = 1;
        request.entry.addr = guestAddress;
        request.entry.length = guestLength;
        ok = sendCommand(&request, sizeof(request), &response, sizeof(response));
    } else {
        VirtIOGPUResourceCreateBlob request = resource;
        initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, resource.hdr.ctx_id,
                         resource.hdr.ring_idx, (resource.hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) != 0);
        request.resource_id = newResourceId;
        request.nr_entries = 0;
        ok = sendCommand(&request, sizeof(request), &response, sizeof(response));
    }

    if (!ok || response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    ResourceRecord* record = ensureResourceRecord(newResourceId);
    if (!record) {
        unrefResource(newResourceId);
        return false;
    }

    record->size = resource.size;
    if (resource.blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST) {
        record->backingType = ResourceBackingType::BlobGuest;
    } else if (resource.blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D) {
        record->backingType = ResourceBackingType::BlobHost3D;
    } else if (resource.blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST) {
        record->backingType = ResourceBackingType::BlobHost3DGuest;
    } else {
        record->backingType = ResourceBackingType::None;
    }
    record->attachedBacking = needsGuestEntry;
    record->guestPtr = reinterpret_cast<void*>(guestAddress);
    record->guestPhys = guestAddress;
    record->guestLength = guestLength;

    if (outResourceId) {
        *outResourceId = newResourceId;
    }
    return true;
}

bool VirtIOGPUDriver::assignResourceUUID(uint32_t resourceIdValue, uint8_t outUUID[16]) {
    if (!resourceUUIDSupported || resourceIdValue == 0 || !outUUID) {
        return false;
    }

    VirtIOGPUResourceAssignUUID request = {};
    VirtIOGPURespResourceUUID response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.hdr.type != VIRTIO_GPU_RESP_OK_RESOURCE_UUID) {
        return false;
    }

    memcpy(outUUID, response.uuid, sizeof(response.uuid));
    if (ResourceRecord* record = ensureResourceRecord(resourceIdValue)) {
        record->hasUUID = true;
        memcpy(record->uuid, response.uuid, sizeof(response.uuid));
    }
    return true;
}

bool VirtIOGPUDriver::destroyResource3D(uint32_t ctxId, uint32_t resourceIdValue, bool hasBacking) {
    if (!virglSupported || resourceIdValue == 0) {
        return false;
    }

    const ResourceRecord* record = findResourceRecord(resourceIdValue);
    bool ok = true;
    if (ctxId != 0) {
        ok = detachResourceFromContext(ctxId, resourceIdValue) && ok;
    } else if (record) {
        while (record->boundContextCount != 0) {
            const uint32_t boundCtxId = record->boundContexts[0];
            ok = detachResourceFromContext(boundCtxId, resourceIdValue) && ok;
            record = findResourceRecord(resourceIdValue);
            if (!record) {
                break;
            }
        }
    }
    if (record && record->mapped) {
        ok = unmapBlobResource(resourceIdValue) && ok;
    }
    if (hasBacking || (record && record->attachedBacking)) {
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

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    ResourceRecord* record = ensureResourceRecord(resourceIdValue);
    if (!record) {
        return false;
    }

    record->backingType = ResourceBackingType::GuestBacking;
    record->attachedBacking = true;
    record->guestPtr = reinterpret_cast<void*>(guestAddress);
    record->guestPhys = guestAddress;
    record->guestLength = length;
    if (record->size == 0) {
        record->size = length;
    }
    return true;
}

bool VirtIOGPUDriver::mapBlobResource(uint32_t resourceIdValue, void** outPtr, uint64_t* outOffset, uint32_t* outMapInfo) {
    if (!resourceBlobSupported || !outPtr || resourceIdValue == 0 || !hostVisibleShmBase || hostVisibleShmLength == 0) {
        return false;
    }

    ResourceRecord* record = findResourceRecord(resourceIdValue);
    if (!record || record->size == 0) {
        return false;
    }

    if (!record->mapped) {
        const uint64_t mapOffset = allocateHostVisibleOffset(record->size);
        if (mapOffset == UINT64_MAX) {
            return false;
        }

        VirtIOGPUResourceMapBlob request = {};
        VirtIOGPURespMapInfo response = {};
        initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB);
        request.resource_id = resourceIdValue;
        request.offset = mapOffset;

        if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
            response.hdr.type != VIRTIO_GPU_RESP_OK_MAP_INFO) {
            return false;
        }

        record->mapped = true;
        record->mapInfo = response.map_info;
        record->hostVisibleOffset = mapOffset;
    }

    *outPtr = reinterpret_cast<uint8_t*>(hostVisibleShmBase) + record->hostVisibleOffset;
    if (outOffset) {
        *outOffset = record->hostVisibleOffset;
    }
    if (outMapInfo) {
        *outMapInfo = record->mapInfo;
    }
    return true;
}

bool VirtIOGPUDriver::unmapBlobResource(uint32_t resourceIdValue) {
    if (!resourceBlobSupported || resourceIdValue == 0) {
        return false;
    }

    ResourceRecord* record = findResourceRecord(resourceIdValue);
    if (!record || !record->mapped) {
        return false;
    }

    VirtIOGPUResourceUnmapBlob request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    record->mapped = false;
    record->mapInfo = 0;
    record->hostVisibleOffset = 0;
    return true;
}

bool VirtIOGPUDriver::setScanoutBlob(const VirtIOGPUSetScanoutBlob& scanoutRequest) {
    if (!resourceBlobSupported) {
        return false;
    }

    VirtIOGPUSetScanoutBlob request = scanoutRequest;
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_SET_SCANOUT_BLOB, scanoutRequest.hdr.ctx_id);

    return sendCommand(&request, sizeof(request), &response, sizeof(response)) &&
           response.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool VirtIOGPUDriver::allocateHostVisibleBlob(uint64_t size, uint32_t* outResourceId, void** outPtr,
                                              uint64_t* outOffset, uint32_t* outMapInfo, uint32_t blobFlags) {
    if (!resourceBlobSupported || !outResourceId || !outPtr || size == 0) {
        return false;
    }

    const uint64_t pages = (size + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    const uint64_t guestPhys = PMM::AllocFrames(pages);
    if (!guestPhys) {
        return false;
    }

    void* guestPtr = reinterpret_cast<void*>(guestPhys);
    memset(guestPtr, 0, pages * PMM::PAGE_SIZE);

    VirtIOGPUResourceCreateBlob request = {};
    request.blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST;
    request.blob_flags = blobFlags | VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
    request.nr_entries = 1;
    request.size = size;

    uint32_t resourceIdValue = 0;
    if (!createBlobResource(request, &resourceIdValue, guestPhys,
                            static_cast<uint32_t>(pages * PMM::PAGE_SIZE))) {
        for (uint64_t i = 0; i < pages; ++i) {
            PMM::FreeFrame(guestPhys + i * PMM::PAGE_SIZE);
        }
        return false;
    }

    *outResourceId = resourceIdValue;
    *outPtr = guestPtr;
    if (outOffset) {
        *outOffset = 0;
    }
    if (outMapInfo) {
        *outMapInfo = 0;
    }
    if (ResourceRecord* record = findResourceRecord(resourceIdValue)) {
        record->guestPtr = guestPtr;
        record->guestPhys = guestPhys;
        record->guestLength = static_cast<uint32_t>(pages * PMM::PAGE_SIZE);
    }
    return true;
}

bool VirtIOGPUDriver::detachResourceBacking(uint32_t resourceIdValue) {
    if (resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUResourceDetachBacking request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    if (ResourceRecord* record = findResourceRecord(resourceIdValue)) {
        record->attachedBacking = false;
        record->guestPtr = nullptr;
        record->guestPhys = 0;
        record->guestLength = 0;
    }
    return true;
}

bool VirtIOGPUDriver::unrefResource(uint32_t resourceIdValue) {
    if (resourceIdValue == 0) {
        return false;
    }

    VirtIOGPUResourceUnref request = {};
    VirtIOGPUCtrlHdr response = {};
    initializeHeader(&request.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    request.resource_id = resourceIdValue;

    if (!sendCommand(&request, sizeof(request), &response, sizeof(response)) ||
        response.type != VIRTIO_GPU_RESP_OK_NODATA) {
        return false;
    }

    releaseResourceRecord(resourceIdValue);
    return true;
}

bool VirtIOGPUDriver::transferToHost3D(uint32_t ctxId, uint32_t resourceIdValue, const VirtIOGPUBox& box,
                                       uint64_t offset, uint32_t level, uint32_t stride, uint32_t layerStride) {
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0 || !requireContextRecord(ctxId)) {
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
    if (!virglSupported || ctxId == 0 || resourceIdValue == 0 || !requireContextRecord(ctxId)) {
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
    if (!virglSupported || ctxId == 0 || !commands || size == 0 || !requireContextRecord(ctxId)) {
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

uint32_t VirtIOGPUDriver::sanitizeContextInit(uint32_t contextInit, uint32_t capsetId) const {
    if (!contextInitSupported) {
        return 0;
    }

    uint32_t sanitized = contextInit;
    if (capsetId != 0) {
        sanitized &= ~VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
        sanitized |= (capsetId & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK);
    }
    return sanitized;
}

void VirtIOGPUDriver::initializeHeader(VirtIOGPUCtrlHdr* hdr, uint32_t type, uint32_t ctxId,
                                       uint8_t ringIdx, bool useRingIdx) {
    if (!hdr) {
        return;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->ctx_id = ctxId;
    hdr->flags = VIRTIO_GPU_FLAG_FENCE;
    if (ctxId != 0 && !useRingIdx) {
        if (const ContextRecord* context = findContextRecord(ctxId)) {
            ringIdx = context->ringIdx;
            useRingIdx = context->useRingIdx;
        }
    }
    if (useRingIdx) {
        hdr->flags |= VIRTIO_GPU_FLAG_INFO_RING_IDX;
        hdr->ring_idx = ringIdx;
    }
    hdr->fence_id = nextFenceId++;
    lastSubmittedFence = hdr->fence_id;
    lastCommandStatus.requestType = type;
    lastCommandStatus.submittedFence = hdr->fence_id;
}

bool VirtIOGPUDriver::claimPendingInterrupt() {
    if (!irqRegistered || !isrCfg) {
        return true;
    }

    const uint8_t status = *isrCfg;
    if (status == 0) {
        return false;
    }

    lastInterruptStatus = status;
    interruptLatched = true;
    return true;
}

void VirtIOGPUDriver::handleInterrupt() {
    if (interruptLatched) {
        interruptLatched = false;
    } else if (isrCfg) {
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

    processControlQueueCompletions();
}

VirtIOGPUDriver::FenceWaiter* VirtIOGPUDriver::allocateFenceWaiter() {
    for (size_t i = 0; i < MaxFenceWaiters; ++i) {
        if (fenceWaiters[i].used) {
            continue;
        }

        memset(&fenceWaiters[i], 0, sizeof(fenceWaiters[i]));
        fenceWaiters[i].used = true;
        return &fenceWaiters[i];
    }

    return nullptr;
}

void VirtIOGPUDriver::releaseFenceWaiter(FenceWaiter* waiter) {
    if (!waiter) {
        return;
    }

    memset(waiter, 0, sizeof(*waiter));
}

void VirtIOGPUDriver::processControlQueueCompletions() {
    while (controlQueue.hasUsed()) {
        uint32_t usedLen = 0;
        const uint32_t usedId = controlQueue.getUsed(&usedLen);
        if (usedId == 0xFFFFFFFFU) {
            return;
        }

        lastUsedLength = usedLen;
        lastCommandStatus.usedLength = usedLen;

        FenceWaiter* matched = nullptr;
        for (size_t i = 0; i < MaxFenceWaiters; ++i) {
            if (!fenceWaiters[i].used || fenceWaiters[i].headDesc != usedId) {
                continue;
            }

            matched = &fenceWaiters[i];
            break;
        }

        if (!matched) {
            continue;
        }

        matched->usedLength = usedLen;
        matched->transportOk = true;

        if (matched->responseSize >= sizeof(VirtIOGPUCtrlHdr) && matched->responseBuffer) {
            auto* responseHdr = reinterpret_cast<VirtIOGPUCtrlHdr*>(matched->responseBuffer);
            matched->responseType = responseHdr->type;
            matched->completedFence = responseHdr->fence_id;

            lastResponseFence = responseHdr->fence_id;
            lastResponseType = responseHdr->type;
            lastCommandStatus.responseType = responseHdr->type;

            if (responseHdr->fence_id != matched->expectedFence) {
                matched->transportOk = false;
            } else {
                lastCompletedFence = responseHdr->fence_id;
                lastCommandStatus.completedFence = responseHdr->fence_id;
            }
        } else {
            matched->completedFence = matched->expectedFence;
            matched->responseType = VIRTIO_GPU_UNDEFINED;
            lastCompletedFence = matched->expectedFence;
            lastCommandStatus.completedFence = matched->expectedFence;
        }

        matched->completed = true;

        if (matched->ownerPID != 0) {
            Process* owner = Scheduler::get().getProcessByPID(matched->ownerPID);
            if (owner && owner->getState() == ProcessState::Blocked) {
                owner->setState(ProcessState::Ready);
                Scheduler::get().wakeProcess(owner);
            }
        }
    }
}

bool VirtIOGPUDriver::awaitFence(FenceWaiter& waiter) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (current && waiter.ownerPID == current->getPID() && irqRegistered) {
        while (waiter.used && !waiter.completed) {
            processControlQueueCompletions();
            if (waiter.completed) {
                break;
            }
            current->setState(ProcessState::Blocked);
            Scheduler::get().scheduleFromSyscall();
            processControlQueueCompletions();
        }

        return waiter.completed && waiter.transportOk;
    }

    for (uint64_t i = 0; i < kCommandTimeout; ++i) {
        processControlQueueCompletions();
        if (!waiter.used || waiter.completed) {
            return waiter.completed && waiter.transportOk;
        }

        if (irqRegistered && isrCfg && interrupts_enabled()) {
            cpu_wait_for_interrupt();
        } else {
            cpu_pause();
        }
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

    FenceWaiter* waiter = allocateFenceWaiter();
    const int cmdDesc = controlQueue.allocDesc();
    const int respDesc = controlQueue.allocDesc();
    if (!waiter || cmdDesc < 0 || respDesc < 0) {
        if (cmdDesc >= 0) {
            controlQueue.freeDesc(cmdDesc);
        }
        if (respDesc >= 0) {
            controlQueue.freeDesc(respDesc);
        }
        if (waiter) {
            releaseFenceWaiter(waiter);
        }
        __sync_lock_release(&commandLock);
        free_dma_buffer(&respBuffer);
        free_dma_buffer(&cmdBuffer);
        return false;
    }

    Process* current = Scheduler::get().getCurrentProcess();
    waiter->headDesc = static_cast<uint16_t>(cmdDesc);
    waiter->expectedFence = expectedFence;
    waiter->responseBuffer = respBuffer.ptr;
    waiter->responseSize = respSize;
    waiter->ownerPID = current ? current->getPID() : 0;

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
    __sync_lock_release(&commandLock);

    const bool completed = awaitFence(*waiter);
    if (completed) {
        memcpy(resp, respBuffer.ptr, respSize);
        lastCommandStatus.transportOk = waiter->transportOk;
        lastCommandStatus.usedLength = waiter->usedLength;
        if (respSize >= sizeof(VirtIOGPUCtrlHdr)) {
            const auto* responseHdr = reinterpret_cast<const VirtIOGPUCtrlHdr*>(resp);
            lastCommandStatus.responseType = responseHdr->type;
            lastCommandStatus.completedFence = responseHdr->fence_id;
            lastCommandStatus.responseOk = responseHdr->type < VIRTIO_GPU_RESP_ERR_UNSPEC;
            if (!lastCommandStatus.responseOk) {
                logCommandError(commandHdr->type, responseHdr->type, commandHdr->ctx_id, responseHdr->fence_id);
            }
        } else {
            lastCommandStatus.responseOk = true;
        }
    }

    controlQueue.freeDesc(respDesc);
    controlQueue.freeDesc(cmdDesc);
    releaseFenceWaiter(waiter);
    free_dma_buffer(&respBuffer);
    free_dma_buffer(&cmdBuffer);
    return completed;
}

bool VirtIOGPUDriver::destroyFramebufferResource() {
    bool ok = true;
    if (blobScanoutActive) {
        if (const ResourceRecord* record = findResourceRecord(resourceId)) {
            if (record->mapped) {
                ok = unmapBlobResource(resourceId) && ok;
            }
        }
    }
    if (!blobScanoutActive && fbSize != 0) {
        ok = detachResourceBacking(resourceId) && ok;
    }
    ok = unrefResource(resourceId) && ok;
    blobScanoutActive = false;
    return ok;
}

void VirtIOGPUDriver::releaseFramebufferMemory() {
    if (blobScanoutActive) {
        if (framebufferPhys && fbSize != 0) {
            const uint64_t pages = (static_cast<uint64_t>(fbSize) + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
            for (uint64_t i = 0; i < pages; ++i) {
                PMM::FreeFrame(framebufferPhys + i * PMM::PAGE_SIZE);
            }
        }
        framebuffer = nullptr;
        framebufferPhys = 0;
        fbSize = 0;
        blobScanoutActive = false;
        return;
    }

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

VirtIOGPUDriver::ResourceRecord* VirtIOGPUDriver::findResourceRecord(uint32_t resourceIdValue) {
    for (size_t i = 0; i < MaxTrackedResources; ++i) {
        if (resourceRecords[i].used && resourceRecords[i].resourceId == resourceIdValue) {
            return &resourceRecords[i];
        }
    }

    return nullptr;
}

const VirtIOGPUDriver::ResourceRecord* VirtIOGPUDriver::findResourceRecord(uint32_t resourceIdValue) const {
    for (size_t i = 0; i < MaxTrackedResources; ++i) {
        if (resourceRecords[i].used && resourceRecords[i].resourceId == resourceIdValue) {
            return &resourceRecords[i];
        }
    }

    return nullptr;
}

VirtIOGPUDriver::ResourceRecord* VirtIOGPUDriver::ensureResourceRecord(uint32_t resourceIdValue) {
    if (resourceIdValue == 0) {
        return nullptr;
    }

    if (ResourceRecord* existing = findResourceRecord(resourceIdValue)) {
        return existing;
    }

    for (size_t i = 0; i < MaxTrackedResources; ++i) {
        if (resourceRecords[i].used) {
            continue;
        }

        memset(&resourceRecords[i], 0, sizeof(resourceRecords[i]));
        resourceRecords[i].used = true;
        resourceRecords[i].resourceId = resourceIdValue;
        return &resourceRecords[i];
    }

    return nullptr;
}

void VirtIOGPUDriver::releaseResourceRecord(uint32_t resourceIdValue) {
    ResourceRecord* record = findResourceRecord(resourceIdValue);
    if (!record) {
        return;
    }

    unbindResourceFromAllContexts(resourceIdValue);
    memset(record, 0, sizeof(*record));
}

VirtIOGPUDriver::ContextRecord* VirtIOGPUDriver::findContextRecord(uint32_t ctxIdValue) {
    for (size_t i = 0; i < MaxTrackedContexts; ++i) {
        if (contextRecords[i].used && contextRecords[i].ctxId == ctxIdValue) {
            return &contextRecords[i];
        }
    }

    return nullptr;
}

const VirtIOGPUDriver::ContextRecord* VirtIOGPUDriver::findContextRecord(uint32_t ctxIdValue) const {
    for (size_t i = 0; i < MaxTrackedContexts; ++i) {
        if (contextRecords[i].used && contextRecords[i].ctxId == ctxIdValue) {
            return &contextRecords[i];
        }
    }

    return nullptr;
}

VirtIOGPUDriver::ContextRecord* VirtIOGPUDriver::ensureContextRecord(uint32_t ctxIdValue) {
    if (ctxIdValue == 0) {
        return nullptr;
    }

    if (ContextRecord* existing = findContextRecord(ctxIdValue)) {
        return existing;
    }

    for (size_t i = 0; i < MaxTrackedContexts; ++i) {
        if (contextRecords[i].used) {
            continue;
        }

        memset(&contextRecords[i], 0, sizeof(contextRecords[i]));
        contextRecords[i].used = true;
        contextRecords[i].ctxId = ctxIdValue;
        return &contextRecords[i];
    }

    return nullptr;
}

VirtIOGPUDriver::ContextRecord* VirtIOGPUDriver::requireContextRecord(uint32_t ctxIdValue) {
    if (ctxIdValue == 0) {
        return nullptr;
    }

    return findContextRecord(ctxIdValue);
}

void VirtIOGPUDriver::releaseContextRecord(uint32_t ctxIdValue) {
    ContextRecord* context = findContextRecord(ctxIdValue);
    if (!context) {
        return;
    }

    for (uint32_t i = 0; i < context->attachedResourceCount; ++i) {
        if (ResourceRecord* resource = findResourceRecord(context->attachedResources[i])) {
            for (uint32_t j = 0; j < resource->boundContextCount; ++j) {
                if (resource->boundContexts[j] != ctxIdValue) {
                    continue;
                }

                for (uint32_t k = j + 1; k < resource->boundContextCount; ++k) {
                    resource->boundContexts[k - 1] = resource->boundContexts[k];
                }
                --resource->boundContextCount;
                break;
            }
        }
    }

    memset(context, 0, sizeof(*context));
}

bool VirtIOGPUDriver::bindResourceToContextRecord(uint32_t ctxIdValue, uint32_t resourceIdValue) {
    ContextRecord* context = ensureContextRecord(ctxIdValue);
    ResourceRecord* resource = ensureResourceRecord(resourceIdValue);
    if (!context || !resource) {
        return false;
    }

    for (uint32_t i = 0; i < context->attachedResourceCount; ++i) {
        if (context->attachedResources[i] == resourceIdValue) {
            return true;
        }
    }
    if (context->attachedResourceCount >= MaxContextResources || resource->boundContextCount >= MaxResourceContexts) {
        return false;
    }

    context->attachedResources[context->attachedResourceCount++] = resourceIdValue;
    resource->boundContexts[resource->boundContextCount++] = ctxIdValue;
    return true;
}

void VirtIOGPUDriver::unbindResourceFromContextRecord(uint32_t ctxIdValue, uint32_t resourceIdValue) {
    if (ContextRecord* context = findContextRecord(ctxIdValue)) {
        for (uint32_t i = 0; i < context->attachedResourceCount; ++i) {
            if (context->attachedResources[i] != resourceIdValue) {
                continue;
            }

            for (uint32_t j = i + 1; j < context->attachedResourceCount; ++j) {
                context->attachedResources[j - 1] = context->attachedResources[j];
            }
            --context->attachedResourceCount;
            break;
        }
    }

    if (ResourceRecord* resource = findResourceRecord(resourceIdValue)) {
        for (uint32_t i = 0; i < resource->boundContextCount; ++i) {
            if (resource->boundContexts[i] != ctxIdValue) {
                continue;
            }

            for (uint32_t j = i + 1; j < resource->boundContextCount; ++j) {
                resource->boundContexts[j - 1] = resource->boundContexts[j];
            }
            --resource->boundContextCount;
            break;
        }
    }
}

void VirtIOGPUDriver::unbindResourceFromAllContexts(uint32_t resourceIdValue) {
    ResourceRecord* resource = findResourceRecord(resourceIdValue);
    if (!resource) {
        return;
    }

    while (resource->boundContextCount != 0) {
        unbindResourceFromContextRecord(resource->boundContexts[0], resourceIdValue);
    }
}

uint64_t VirtIOGPUDriver::allocateHostVisibleOffset(uint64_t size) {
    if (!hostVisibleShmBase || size == 0) {
        return UINT64_MAX;
    }

    const uint64_t alignedOffset = (nextHostVisibleOffset + 0xFFFULL) & ~0xFFFULL;
    if (alignedOffset > hostVisibleShmLength || size > (hostVisibleShmLength - alignedOffset)) {
        return UINT64_MAX;
    }

    nextHostVisibleOffset = alignedOffset + size;
    return alignedOffset;
}

void VirtIOGPUDriver::logCommandError(uint32_t requestType, uint32_t responseType, uint32_t ctxId, uint64_t fenceId) const {
    Console::get().drawText("[VGPU] cmd=");
    Console::get().drawText(describeControlType(requestType));
    Console::get().drawText(" resp=");
    Console::get().drawText(describeResponseType(responseType));
    Console::get().drawText(" ctx=");
    Console::get().drawHex(ctxId);
    Console::get().drawText(" fence=");
    Console::get().drawHex(fenceId);
    Console::get().drawText("\n");
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
