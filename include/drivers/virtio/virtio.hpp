#pragma once

#include <stdint.h>

constexpr uint16_t VIRTIO_VENDOR_ID = 0x1AF4;

// VirtIO PCI Capability Types
constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG = 1;
constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG = 2;
constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG = 3;
constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG = 4;
constexpr uint8_t VIRTIO_PCI_CAP_PCI_CFG = 5;

// VirtIO Device Status
constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
constexpr uint8_t VIRTIO_STATUS_DRIVER = 2;
constexpr uint8_t VIRTIO_STATUS_DRIVER_OK = 4;
constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;
constexpr uint8_t VIRTIO_STATUS_DEVICE_NEEDS_RESET = 64;
constexpr uint8_t VIRTIO_STATUS_FAILED = 128;

// VirtIO Feature Bits
constexpr uint64_t VIRTIO_F_VERSION_1 = (1ULL << 32);
constexpr uint64_t VIRTIO_F_RING_EVENT_IDX = (1ULL << 29);

// Virtqueue descriptor flags
constexpr uint16_t VIRTQ_DESC_F_NEXT = 1;
constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;
constexpr uint16_t VIRTQ_DESC_F_INDIRECT = 4;

// Virtqueue used flags
constexpr uint16_t VIRTQ_USED_F_NO_NOTIFY = 1;

// Virtqueue avail flags
constexpr uint16_t VIRTQ_AVAIL_F_NO_INTERRUPT = 1;

struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct VirtqUsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct VirtqUsed {
    uint16_t flags;
    uint16_t idx;
    VirtqUsedElem ring[];
} __attribute__((packed));

// VirtIO PCI Common Configuration Structure
struct VirtioPCICommonCfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct VirtioPCICapability {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t padding[3];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed));

struct VirtioPCINotifyCap {
    VirtioPCICapability cap;
    uint32_t notify_off_multiplier;
} __attribute__((packed));

class Virtqueue {
public:
    Virtqueue();
    ~Virtqueue();
    
    bool init(uint16_t queueSize);
    void reset();
    
    // Descriptor management
    int allocDesc();
    void freeDesc(int idx);
    void setDesc(int idx, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next);
    
    // Queue operations
    void addAvail(uint16_t descIdx);
    bool hasUsed();
    uint32_t getUsed(uint32_t* len);
    
    // Getters for queue addresses
    uint64_t getDescAddr() const { return reinterpret_cast<uint64_t>(desc); }
    uint64_t getAvailAddr() const { return reinterpret_cast<uint64_t>(avail); }
    uint64_t getUsedAddr() const { return reinterpret_cast<uint64_t>(used); }
    
    uint16_t getSize() const { return queueSize; }
    
private:
    uint16_t queueSize;
    uint16_t lastUsedIdx;
    uint16_t numFree;
    
    VirtqDesc* desc;
    VirtqAvail* avail;
    VirtqUsed* used;
    uint64_t descPages;
    uint64_t availPages;
    uint64_t usedPages;
    
    uint16_t* freeList;
};
