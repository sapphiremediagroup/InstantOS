#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/virtio/virtio.hpp>

// VirtIO GPU Device IDs
constexpr uint16_t VIRTIO_GPU_DEVICE_ID = 0x1050;  // Modern VirtIO GPU
constexpr uint64_t VIRTIO_GPU_F_VIRGL = (1ULL << 0);
constexpr uint64_t VIRTIO_GPU_F_EDID = (1ULL << 1);
constexpr uint64_t VIRTIO_GPU_F_RESOURCE_UUID = (1ULL << 2);
constexpr uint64_t VIRTIO_GPU_F_RESOURCE_BLOB = (1ULL << 3);
constexpr uint64_t VIRTIO_GPU_F_CONTEXT_INIT = (1ULL << 4);

constexpr uint32_t VIRTIO_GPU_FLAG_FENCE = (1U << 0);
constexpr uint32_t VIRTIO_GPU_FLAG_INFO_RING_IDX = (1U << 1);

// VirtIO GPU 2D Commands
enum VirtIOGPUCtrlType : uint32_t {
    VIRTIO_GPU_UNDEFINED = 0,
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID = 0x010A,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID = 0x010B,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x010C,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB = 0x010D,

    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D = 0x0207,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB = 0x0208,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB = 0x0209,
    
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID = 0x1104,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID = 0x1105,
    VIRTIO_GPU_RESP_OK_MAP_INFO = 0x1106,

    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY = 0x1201,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
};

// VirtIO GPU Formats
enum VirtIOGPUFormats : uint32_t {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134,
};

constexpr uint32_t VIRTIO_GPU_MAX_SCANOUTS = 16;
constexpr uint32_t VIRTIO_GPU_CONTEXT_NAME_MAX = 64;
constexpr uint32_t VIRTIO_GPU_CAPSET_VIRGL = 1;
constexpr uint32_t VIRTIO_GPU_CAPSET_VIRGL2 = 2;

constexpr uint32_t VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK = 0x000000FFU;
constexpr uint32_t VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP = (1U << 0);

// VirtIO GPU command/response payloads are plain wire structs. Keep their
// layout explicit and guard it with compile-time size assertions below.
struct alignas(4) VirtIOGPURect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct alignas(8) VirtIOGPUCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct alignas(4) VirtIOGPUDisplayOne {
    VirtIOGPURect r;
    uint32_t enabled;
    uint32_t flags;
};

struct alignas(8) VirtIOGPURespDisplayInfo {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPUDisplayOne pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct alignas(8) VirtIOGPUResourceCreate2D {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct alignas(8) VirtIOGPUSetScanout {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct alignas(8) VirtIOGPUTransferToHost2D {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUResourceFlush {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUResourceAttachBacking {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct alignas(4) VirtIOGPUBox {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct alignas(8) VirtIOGPUTransferHost3D {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPUBox box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
};

struct alignas(8) VirtIOGPUResourceCreate3D {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUCtxCreate {
    VirtIOGPUCtrlHdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[VIRTIO_GPU_CONTEXT_NAME_MAX];
};

struct alignas(8) VirtIOGPUCtxDestroy {
    VirtIOGPUCtrlHdr hdr;
};

struct alignas(8) VirtIOGPUCtxResource {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUCmdSubmit3D {
    VirtIOGPUCtrlHdr hdr;
    uint32_t size;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUGetCapsetInfo {
    VirtIOGPUCtrlHdr hdr;
    uint32_t capset_index;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUCapsetInfo {
    VirtIOGPUCtrlHdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUGetCapset {
    VirtIOGPUCtrlHdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
};

struct alignas(4) VirtIOGPUConfig {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

struct alignas(8) VirtIOGPUResourceDetachBacking {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct alignas(8) VirtIOGPUResourceUnref {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtIOGPUCommandStatus {
    bool transportOk;
    bool responseOk;
    uint32_t requestType;
    uint32_t responseType;
    uint64_t submittedFence;
    uint64_t completedFence;
    uint32_t usedLength;
};

static_assert(sizeof(VirtIOGPURect) == 16, "VirtIOGPURect layout mismatch");
static_assert(sizeof(VirtIOGPUCtrlHdr) == 24, "VirtIOGPUCtrlHdr layout mismatch");
static_assert(offsetof(VirtIOGPUCtrlHdr, fence_id) == 8, "VirtIOGPUCtrlHdr fence_id offset mismatch");
static_assert(offsetof(VirtIOGPUCtrlHdr, ctx_id) == 16, "VirtIOGPUCtrlHdr ctx_id offset mismatch");
static_assert(offsetof(VirtIOGPUCtrlHdr, ring_idx) == 20, "VirtIOGPUCtrlHdr ring_idx offset mismatch");
static_assert(sizeof(VirtIOGPUDisplayOne) == 24, "VirtIOGPUDisplayOne layout mismatch");
static_assert(sizeof(VirtIOGPURespDisplayInfo) == 408, "VirtIOGPURespDisplayInfo layout mismatch");
static_assert(sizeof(VirtIOGPUResourceCreate2D) == 40, "VirtIOGPUResourceCreate2D layout mismatch");
static_assert(sizeof(VirtIOGPUSetScanout) == 48, "VirtIOGPUSetScanout layout mismatch");
static_assert(sizeof(VirtIOGPUTransferToHost2D) == 56, "VirtIOGPUTransferToHost2D layout mismatch");
static_assert(sizeof(VirtIOGPUResourceFlush) == 48, "VirtIOGPUResourceFlush layout mismatch");
static_assert(sizeof(VirtIOGPUMemEntry) == 16, "VirtIOGPUMemEntry layout mismatch");
static_assert(sizeof(VirtIOGPUResourceAttachBacking) == 32, "VirtIOGPUResourceAttachBacking layout mismatch");
static_assert(sizeof(VirtIOGPUBox) == 24, "VirtIOGPUBox layout mismatch");
static_assert(sizeof(VirtIOGPUTransferHost3D) == 72, "VirtIOGPUTransferHost3D layout mismatch");
static_assert(sizeof(VirtIOGPUResourceCreate3D) == 72, "VirtIOGPUResourceCreate3D layout mismatch");
static_assert(sizeof(VirtIOGPUCtxCreate) == 96, "VirtIOGPUCtxCreate layout mismatch");
static_assert(sizeof(VirtIOGPUCtxDestroy) == 24, "VirtIOGPUCtxDestroy layout mismatch");
static_assert(sizeof(VirtIOGPUCtxResource) == 32, "VirtIOGPUCtxResource layout mismatch");
static_assert(sizeof(VirtIOGPUCmdSubmit3D) == 32, "VirtIOGPUCmdSubmit3D layout mismatch");
static_assert(sizeof(VirtIOGPUGetCapsetInfo) == 32, "VirtIOGPUGetCapsetInfo layout mismatch");
static_assert(sizeof(VirtIOGPUCapsetInfo) == 40, "VirtIOGPUCapsetInfo layout mismatch");
static_assert(sizeof(VirtIOGPUGetCapset) == 32, "VirtIOGPUGetCapset layout mismatch");
static_assert(sizeof(VirtIOGPUConfig) == 16, "VirtIOGPUConfig layout mismatch");
static_assert(sizeof(VirtIOGPUResourceDetachBacking) == 32, "VirtIOGPUResourceDetachBacking layout mismatch");
static_assert(sizeof(VirtIOGPUResourceUnref) == 32, "VirtIOGPUResourceUnref layout mismatch");

class VirtIOGPUDriver {
public:
    static VirtIOGPUDriver& get();
    
    bool initialize();
    bool isInitialized() const { return initialized; }
    bool isAvailable() const { return deviceFound; }
    bool supportsVirgl() const { return virglSupported; }
    bool supportsContextInit() const { return contextInitSupported; }
    bool supportsBlobResources() const { return resourceBlobSupported; }
    uint32_t getNumCapsets() const { return numCapsets; }
    VirtIOGPUCommandStatus getLastCommandStatus() const { return lastCommandStatus; }
    
    // Display info
    uint32_t getMaxWidth() const { return maxWidth; }
    uint32_t getMaxHeight() const { return maxHeight; }
    
    // Display mode management
    bool setMode(uint32_t width, uint32_t height);
    void getMode(uint32_t* width, uint32_t* height);
    
    // Framebuffer access
    void* getFramebuffer() { return framebuffer; }
    uint32_t getFBSize() const { return fbSize; }
    uint32_t getPitch() const { return currentWidth * 4; }
    
    // Operations
    bool flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // VirGL / 3D transport plumbing
    bool getCapsetInfo(uint32_t capsetIndex, VirtIOGPUCapsetInfo* info);
    bool getCapset(uint32_t capsetId, uint32_t capsetVersion, void* buffer, uint32_t bufferSize, uint32_t* actualSize = nullptr);
    bool createContext(uint32_t* ctxId, const char* debugName = nullptr, uint32_t contextInit = 0);
    bool destroyContext(uint32_t ctxId);
    bool attachResourceToContext(uint32_t ctxId, uint32_t resourceId);
    bool detachResourceFromContext(uint32_t ctxId, uint32_t resourceId);
    bool createResource3D(const VirtIOGPUResourceCreate3D& resource, uint32_t* outResourceId);
    bool destroyResource3D(uint32_t ctxId, uint32_t resourceId, bool hasBacking = false);
    bool destroyContextWithResource(uint32_t ctxId, uint32_t resourceId = 0, bool resourceHasBacking = false);
    bool attachBacking(uint32_t resourceId, uint64_t guestAddress, uint32_t length);
    bool transferToHost3D(uint32_t ctxId, uint32_t resourceId, const VirtIOGPUBox& box,
                          uint64_t offset, uint32_t level, uint32_t stride, uint32_t layerStride);
    bool transferFromHost3D(uint32_t ctxId, uint32_t resourceId, const VirtIOGPUBox& box,
                            uint64_t offset, uint32_t level, uint32_t stride, uint32_t layerStride);
    bool submit3D(uint32_t ctxId, const void* commands, uint32_t size);
    bool detachResourceBacking(uint32_t resourceId);
    bool unrefResource(uint32_t resourceId);
    uint32_t allocateResourceId();
    uint32_t allocateContextId();
    void handleInterrupt();
    
private:
    VirtIOGPUDriver();
    
    bool detectDevice();
    bool initDevice();
    bool negotiateFeatures();
    bool setupQueues();
    bool getDisplayInfo();
    bool createResource();
    bool attachBacking();
    bool setScanout();
    void resetCommandStatus();
    void initializeHeader(VirtIOGPUCtrlHdr* hdr, uint32_t type, uint32_t ctxId = 0);
    bool waitForFence(uint16_t headDesc, uint64_t expectedFence, void* resp, size_t respSize);
    bool destroyFramebufferResource();
    void releaseFramebufferMemory();
    
    // Queue operations
    bool sendCommand(const void* cmd, size_t cmdSize, void* resp, size_t respSize);
    void notifyQueue(uint16_t queueIdx);
    
    // PCI BAR access
    void* mapBar(uint8_t barIdx, size_t size);
    uint8_t readConfig8(uint16_t offset);
    uint16_t readConfig16(uint16_t offset);
    uint32_t readConfig32(uint16_t offset);
    void writeConfig8(uint16_t offset, uint8_t value);
    void writeConfig16(uint16_t offset, uint16_t value);
    void writeConfig32(uint16_t offset, uint32_t value);
    
    bool initialized;
    bool deviceFound;
    bool virglSupported;
    bool contextInitSupported;
    bool resourceBlobSupported;
    
    // PCI location
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    
    // VirtIO structures
    volatile VirtioPCICommonCfg* commonCfg;
    volatile uint32_t* notifyBase;
    uint32_t notifyOffMultiplier;
    volatile uint8_t* isrCfg;
    volatile void* deviceCfg;
    
    // Queues
    Virtqueue controlQueue;
    Virtqueue cursorQueue;
    
    // Resources
    void* framebuffer;
    uint64_t framebufferPhys;
    
    // Display info
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t currentWidth;
    uint32_t currentHeight;
    uint32_t numCapsets;
    uint32_t fbSize;
    uint32_t resourceId;
    uint32_t nextResourceId;
    uint32_t nextContextId;
    uint64_t nextFenceId;
    uint64_t lastSubmittedFence;
    uint64_t lastCompletedFence;
    uint64_t lastResponseFence;
    uint32_t lastResponseType;
    uint32_t lastUsedLength;
    VirtIOGPUCommandStatus lastCommandStatus;
    uint8_t pciIrqLine;
    uint8_t pciIrqVector;
    uint8_t lastInterruptStatus;
    uint32_t interruptCount;
    uint32_t queueInterruptCount;
    uint32_t configInterruptCount;
    bool irqRegistered;
    int commandLock;
};
