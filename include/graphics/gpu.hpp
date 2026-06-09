#pragma once

#include <stddef.h>
#include <stdint.h>
#include <graphics/framebuffer.hpp>

enum class GPUBackendKind : uint32_t {
    Framebuffer = 0,
    VirtIO = 1,
    Custom = 0x80000000U
};

enum class GPUCommandType : uint32_t {
    Nop = 0,
    Flush,
    SetMode,
    CreateContext,
    DestroyContext,
    Submit3D,
    WaitFence
};

struct GPUCommand {
    GPUCommandType type;
    uint32_t contextId;
    uint32_t resourceId;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t size;
    uint64_t value;
    uint64_t value2;
    const void* data;
};

struct GPUCommandResult {
    bool ok;
    uint32_t status;
    uint32_t value32;
    uint64_t value64;
};

class GPUBackend {
public:
    virtual ~GPUBackend() = default;

    virtual const char* name() const = 0;
    virtual GPUBackendKind kind() const = 0;
    virtual bool probe() = 0;
    virtual bool initialize(iFramebuffer& framebuffer) = 0;
    virtual bool submitCommand(const GPUCommand& command, GPUCommandResult* result) = 0;

    virtual void* getFramebuffer() = 0;
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual uint32_t getPitch() const = 0;
    virtual uint32_t getFramebufferSize() const = 0;
    virtual bool isHardwareAccelerated() const = 0;
};

class GPU {
public:
    static GPU& get();

    bool initialize(iFramebuffer* framebuffer);
    bool registerBackend(GPUBackend* backend);

    bool submitCommand(const GPUCommand& command, GPUCommandResult* result = nullptr);
    bool flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    bool setMode(uint32_t width, uint32_t height, uint32_t bpp = 32);
    bool submit3D(uint32_t contextId, const void* commands, uint32_t size);
    bool createContext(uint32_t* contextId, const char* debugName = nullptr,
                       uint32_t contextInit = 0, uint8_t ringIdx = 0, bool useRingIdx = false);
    bool destroyContext(uint32_t contextId);
    bool waitForFence(uint64_t fenceId, uint64_t spinLimit = 1000000ULL,
                      uint64_t* completedFence = nullptr, uint32_t* responseType = nullptr);

    iFramebuffer* getFramebuffer() { return framebuffer; }
    GPUBackend* getActiveBackend() { return activeBackend; }
    GPUBackendKind getActiveBackendKind() const;
    const char* getActiveBackendName() const;
    bool isInitialized() const { return initialized; }
    bool isHardwareAccelerated() const;

private:
    static constexpr size_t MaxBackends = 8;

    GPU();

    void registerBuiltinBackends();
    bool setActiveBackend(GPUBackend* backend);
    void clearResult(GPUCommandResult* result, bool ok, uint32_t status = 0,
                     uint32_t value32 = 0, uint64_t value64 = 0);

    iFramebuffer* framebuffer;
    GPUBackend* backends[MaxBackends];
    size_t backendCount;
    GPUBackend* activeBackend;
    bool initialized;
    bool builtinsRegistered;
};
