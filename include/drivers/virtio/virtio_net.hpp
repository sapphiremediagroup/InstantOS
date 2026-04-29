#pragma once

#include <stdint.h>
#include <stddef.h>
#include <drivers/virtio/virtio.hpp>

// VirtIO Network Device ID
constexpr uint16_t VIRTIO_NET_DEVICE_ID_LEGACY = 0x1000;  // Legacy VirtIO Network
constexpr uint16_t VIRTIO_NET_DEVICE_ID = 0x1041;         // Modern VirtIO Network

// VirtIO Network Feature Bits
constexpr uint64_t VIRTIO_NET_F_CSUM = (1ULL << 0);
constexpr uint64_t VIRTIO_NET_F_GUEST_CSUM = (1ULL << 1);
constexpr uint64_t VIRTIO_NET_F_MAC = (1ULL << 5);
constexpr uint64_t VIRTIO_NET_F_STATUS = (1ULL << 16);

// Network packet header
struct VirtIONetHdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

// Device configuration
struct VirtIONetConfig {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
} __attribute__((packed));

constexpr size_t VIRTIO_NET_MTU = 1514;  // Ethernet frame size
constexpr size_t VIRTIO_NET_BUFFER_SIZE = sizeof(VirtIONetHdr) + VIRTIO_NET_MTU;
constexpr size_t VIRTIO_NET_RX_BUFFERS = 32;

class VirtIONetDriver {
public:
    static VirtIONetDriver& get();
    
    bool initialize();
    bool isInitialized() const { return initialized; }
    bool isAvailable() const { return deviceFound; }
    
    // MAC address
    void getMacAddress(uint8_t* mac);
    
    // Send/receive
    bool sendPacket(const void* data, size_t len);
    int receivePacket(void* buffer, size_t maxLen);
    
    // Status
    bool isLinkUp() const;
    
private:
    VirtIONetDriver();
    
    bool detectDevice();
    bool initDevice();
    bool negotiateFeatures();
    bool setupQueues();
    bool fillRxQueue();
    
    // Queue operations
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
    VirtioPCICommonCfg* commonCfg;
    volatile uint32_t* notifyBase;
    uint32_t notifyOffMultiplier;
    VirtIONetConfig* deviceCfg;
    
    // Queues
    Virtqueue rxQueue;  // Queue 0: receive
    Virtqueue txQueue;  // Queue 1: transmit
    
    // RX buffers
    void* rxBuffers[VIRTIO_NET_RX_BUFFERS];
    uint64_t rxBuffersPhys[VIRTIO_NET_RX_BUFFERS];
    
    // MAC address
    uint8_t macAddr[6];
};
