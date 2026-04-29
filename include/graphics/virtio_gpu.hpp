#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/virtio/virtio.hpp>

// VirtIO GPU Device IDs
constexpr uint16_t VIRTIO_GPU_DEVICE_ID = 0x1050;  // Modern VirtIO GPU

// VirtIO GPU 2D Commands
enum VirtIOGPUCtrlType : uint32_t {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
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

struct VirtIOGPURect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct VirtIOGPUCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct VirtIOGPUDisplayOne {
    VirtIOGPURect r;
    uint32_t enabled;
    uint32_t flags;
};

struct VirtIOGPURespDisplayInfo {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPUDisplayOne pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct VirtIOGPUResourceCreate2D {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct VirtIOGPUSetScanout {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct VirtIOGPUTransferToHost2D {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtIOGPUResourceFlush {
    VirtIOGPUCtrlHdr hdr;
    VirtIOGPURect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtIOGPUMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct VirtIOGPUResourceAttachBacking {
    VirtIOGPUCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

class VirtIOGPUDriver {
public:
    static VirtIOGPUDriver& get();
    
    bool initialize();
    bool isInitialized() const { return initialized; }
    bool isAvailable() const { return deviceFound; }
    
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
    
    // Queue operations
    bool sendCommand(void* cmd, size_t cmdSize, void* resp, size_t respSize);
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
    
    // PCI location
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    
    // VirtIO structures
    volatile VirtioPCICommonCfg* commonCfg;
    volatile uint32_t* notifyBase;
    uint32_t notifyOffMultiplier;
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
    uint32_t fbSize;
    uint32_t resourceId;
    int commandLock;
};
