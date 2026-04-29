#pragma once

#include <cpu/syscall/syscall.hpp>
#include <cpu/process/process.hpp>
#include <memory/vmm.hpp>
#include <stddef.h>
#include <stdint.h>

struct IPCMessageHeader {
    uint64_t id;
    uint32_t senderPID;
    uint16_t flags;
    uint16_t reserved;
    uint64_t size;
};

enum IPCMessageFlags : uint16_t {
    IPCMessageFlagNone = 0,
    IPCMessageFlagRequest = 1 << 0,
    IPCMessageFlagEvent = 1 << 1
};

inline constexpr size_t IPCMaxServiceName = 64;

class SharedMemoryObject {
public:
    static constexpr size_t MaxMappings = 32;

    SharedMemoryObject(uint64_t size, uint64_t alignedSize, uint64_t physBase);

    uint64_t getSize() const { return size; }
    uint64_t getAlignedSize() const { return alignedSize; }

    void retain();
    void release();

    uint64_t mapInto(Process* process);
    bool unmapFrom(Process* process);
    void unmapAllFrom(Process* process);

private:
    struct Mapping {
        uint32_t pid;
        uint64_t address;
    };

    uint64_t size;
    uint64_t alignedSize;
    uint64_t physBase;
    uint32_t refCount;
    Mapping mappings[MaxMappings];
    size_t mappingCount;

    int findMappingSlot(uint32_t pid) const;
    bool canDestroy() const;
    void destroy();
};

class MessageQueueObject;

class SurfaceObject {
public:
    SurfaceObject(uint64_t idValue, uint32_t widthValue, uint32_t heightValue, uint32_t formatValue, SharedMemoryObject* backingObject);
    ~SurfaceObject();

    void retain();
    void release();

    uint64_t getID() const { return id; }
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    uint32_t getFormat() const { return format; }
    uint32_t getPitch() const { return width * 4; }
    uint64_t getSize() const { return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL; }

    uint64_t mapInto(Process* process);
    SharedMemoryObject* getBacking() const { return backing; }

    void commit(uint32_t x, uint32_t y, uint32_t widthValue, uint32_t heightValue);
    bool consumeCommit();
    uint32_t getDirtyX() const { return dirtyX; }
    uint32_t getDirtyY() const { return dirtyY; }
    uint32_t getDirtyWidth() const { return dirtyWidth; }
    uint32_t getDirtyHeight() const { return dirtyHeight; }

private:
    uint64_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    SharedMemoryObject* backing;
    uint32_t refCount;
    bool committed;
    uint32_t dirtyX;
    uint32_t dirtyY;
    uint32_t dirtyWidth;
    uint32_t dirtyHeight;
};

class WindowObject {
public:
    WindowObject(uint64_t idValue, uint32_t ownerPIDValue, uint32_t flagsValue, int32_t widthValue, int32_t heightValue, MessageQueueObject* eventQueue);
    ~WindowObject();

    void retain();
    void release();

    uint64_t getID() const { return id; }
    uint32_t getOwnerPID() const { return ownerPID; }
    uint32_t getFlags() const { return flags; }
    uint32_t getState() const { return state; }
    int32_t getX() const { return x; }
    int32_t getY() const { return y; }
    int32_t getWidth() const { return width; }
    int32_t getHeight() const { return height; }
    uint64_t getSurfaceID() const { return surface ? surface->getID() : 0; }
    uint32_t getZOrder() const { return zOrder; }
    MessageQueueObject* getEventQueue() const { return queue; }
    const char* getTitle() const { return title; }

    void setTitle(const char* value);
    void attachSurface(SurfaceObject* value);
    void setPosition(int32_t newX, int32_t newY);
    void setSize(int32_t newWidth, int32_t newHeight);
    void setFocused(bool focused);
    void setZOrder(uint32_t order) { zOrder = order; }
    void control(WindowControlAction action, int32_t maxWidth, int32_t maxHeight);
    bool enqueueWindowEvent(WindowEventAction action);

    void snapshot(WindowInfo* out) const;

private:
    uint64_t id;
    uint32_t ownerPID;
    uint32_t flags;
    uint32_t refCount;
    uint32_t state;
    uint32_t zOrder;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t restoreX;
    int32_t restoreY;
    int32_t restoreWidth;
    int32_t restoreHeight;
    MessageQueueObject* queue;
    SurfaceObject* surface;
    char title[64];
};

class MessageQueueObject {
public:
    static constexpr size_t MaxMessages = 64;
    static constexpr size_t MaxPayloadSize = 256;

    struct Message {
        IPCMessageHeader header;
        uint8_t payload[MaxPayloadSize];
    };

    MessageQueueObject();

    void retain();
    void release();

    bool enqueue(const IPCMessageHeader& header, const void* payload);
    bool dequeue(Message* outMessage);
    bool hasMessages() const;
    size_t pendingCount() const { return count; }

private:
    uint32_t refCount;
    Message messages[MaxMessages];
    size_t head;
    size_t count;
};

class ServiceObject {
public:
    ServiceObject(const char* serviceName, uint32_t ownerPID, MessageQueueObject* serviceQueue);

    void retain();
    void release();

    const char* getName() const { return name; }
    uint32_t getOwnerPID() const { return ownerPID; }
    MessageQueueObject* getQueue() const { return queue; }

private:
    char name[IPCMaxServiceName];
    uint32_t ownerPID;
    uint32_t refCount;
    MessageQueueObject* queue;
};

class IPCManager {
public:
    static constexpr size_t MaxSharedObjects = 64;
    static constexpr size_t MaxSurfaces = 64;
    static constexpr size_t MaxWindows = 64;
    static constexpr size_t MaxQueues = 64;
    static constexpr size_t MaxServices = 32;
    static constexpr size_t MaxPendingRequests = 64;
    static constexpr size_t MaxServiceName = IPCMaxServiceName;

    static IPCManager& get();

    SharedMemoryObject* createSharedMemory(uint64_t size);
    SurfaceObject* createSurface(uint32_t width, uint32_t height, uint32_t format);
    WindowObject* createWindow(Process* owner, uint32_t flags, int32_t width, int32_t height);
    MessageQueueObject* createQueue();
    SurfaceObject* lookupSurface(uint64_t id);
    WindowObject* lookupWindow(uint64_t id);
    SurfaceObject* pollCommittedSurface();
    size_t listWindows(WindowInfo* entries, size_t capacity) const;
    bool focusWindow(uint64_t id);
    bool moveWindow(uint64_t id, int32_t x, int32_t y);
    bool resizeWindow(uint64_t id, int32_t width, int32_t height);
    bool controlWindow(uint64_t id, WindowControlAction action, int32_t maxWidth, int32_t maxHeight);
    bool postEventToFocusedWindow(const Event& event);
    bool postKeyEventToFocusedWindow(const Event& event);

    uint64_t registerService(Process* owner, const char* name, MessageQueueObject* queue);
    ServiceObject* connectService(const char* name);
    bool postServiceEvent(const char* name, const void* payload, uint64_t size);

    uint64_t nextRequestID();
    bool beginRequest(uint64_t requestID, Process* owner);
    bool completeRequest(uint64_t requestID, const void* data, uint64_t size);
    bool waitForRequest(Process* owner, uint64_t requestID, void* data, uint64_t* size);

    void wakeQueueWaiters();
    void cleanupProcess(Process* process);

    void forgetSharedObject(SharedMemoryObject* object);
    void forgetSurface(SurfaceObject* object);
    void forgetWindow(WindowObject* object);
    void forgetQueue(MessageQueueObject* queue);

private:
    IPCManager();

    struct ServiceEntry {
        bool used;
        ServiceObject* service;
    };

    struct PendingRequest {
        bool used;
        uint64_t id;
        uint32_t ownerPID;
        bool completed;
        uint64_t responseSize;
        uint8_t response[MessageQueueObject::MaxPayloadSize];
    };

    SharedMemoryObject* sharedObjects[MaxSharedObjects];
    SurfaceObject* surfaces[MaxSurfaces];
    WindowObject* windows[MaxWindows];
    MessageQueueObject* queues[MaxQueues];
    ServiceEntry services[MaxServices];
    PendingRequest requests[MaxPendingRequests];
    uint64_t requestCounter;
    uint64_t surfaceIDCounter;
    uint64_t windowIDCounter;
    uint64_t focusedWindowID;
    uint32_t nextWindowZOrder;

    void registerSharedObject(SharedMemoryObject* object);
    void registerSurface(SurfaceObject* object);
    void registerWindow(WindowObject* object);
    void registerQueue(MessageQueueObject* queue);
    WindowObject* topVisibleWindow() const;
};

void retainSharedMemoryHandle(void* object);
void releaseSharedMemoryHandle(void* object);
void retainSurfaceHandle(void* object);
void releaseSurfaceHandle(void* object);
void retainWindowHandle(void* object);
void releaseWindowHandle(void* object);
void retainMessageQueueHandle(void* object);
void releaseMessageQueueHandle(void* object);
void retainServiceHandle(void* object);
void releaseServiceHandle(void* object);
