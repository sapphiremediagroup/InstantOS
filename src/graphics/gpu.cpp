#include <graphics/gpu.hpp>
#include <graphics/virtio_gpu.hpp>

namespace {

class FramebufferGPUBackend : public GPUBackend {
public:
    const char* name() const override { return "framebuffer"; }
    GPUBackendKind kind() const override { return GPUBackendKind::Framebuffer; }
    bool probe() override { return true; }

    bool initialize(iFramebuffer& framebufferVal) override {
        framebuffer = &framebufferVal;
        return framebuffer != nullptr && framebuffer->getRaw() != nullptr;
    }

    bool submitCommand(const GPUCommand& command, GPUCommandResult* result) override {
        switch (command.type) {
            case GPUCommandType::Nop:
            case GPUCommandType::Flush:
                setResult(result, true);
                return true;
            case GPUCommandType::SetMode:
                if (framebuffer &&
                    command.width == framebuffer->getWidth() &&
                    command.height == framebuffer->getHeight() &&
                    (command.bpp == 0 || command.bpp == 32)) {
                    setResult(result, true);
                    return true;
                }
                setResult(result, false, 1);
                return false;
            default:
                setResult(result, false, 2);
                return false;
        }
    }

    void* getFramebuffer() override { return framebuffer ? framebuffer->getRaw() : nullptr; }
    uint32_t getWidth() const override { return framebuffer ? static_cast<uint32_t>(framebuffer->getWidth()) : 0; }
    uint32_t getHeight() const override { return framebuffer ? static_cast<uint32_t>(framebuffer->getHeight()) : 0; }
    uint32_t getPitch() const override { return framebuffer ? static_cast<uint32_t>(framebuffer->getPitch() * 4) : 0; }
    uint32_t getFramebufferSize() const override { return framebuffer ? static_cast<uint32_t>(framebuffer->getFBSize()) : 0; }
    bool isHardwareAccelerated() const override { return false; }

private:
    iFramebuffer* framebuffer = nullptr;

    void setResult(GPUCommandResult* result, bool ok, uint32_t status = 0) {
        if (!result) {
            return;
        }
        result->ok = ok;
        result->status = status;
        result->value32 = 0;
        result->value64 = 0;
    }
};

class VirtIOGPUBackend : public GPUBackend {
public:
    const char* name() const override { return "virtio-gpu"; }
    GPUBackendKind kind() const override { return GPUBackendKind::VirtIO; }
    bool probe() override { return true; }

    bool initialize(iFramebuffer& framebufferVal) override {
        framebuffer = &framebufferVal;
        VirtIOGPUDriver& driver = VirtIOGPUDriver::get();
        driver.setFallbackDisplayMode(static_cast<uint32_t>(framebufferVal.getWidth()),
                                      static_cast<uint32_t>(framebufferVal.getHeight()));
        if (!driver.initialize()) {
            return false;
        }

        uint32_t width = 0;
        uint32_t height = 0;
        driver.getMode(&width, &height);
        framebufferVal.switchToVirtIO(driver.getFramebuffer(), width, height, driver.getPitch());
        return true;
    }

    bool submitCommand(const GPUCommand& command, GPUCommandResult* result) override {
        VirtIOGPUDriver& driver = VirtIOGPUDriver::get();
        if (!driver.isInitialized()) {
            setResult(result, false, 1);
            return false;
        }

        bool ok = false;
        uint32_t value32 = 0;
        uint64_t value64 = 0;
        uint32_t status = 0;

        switch (command.type) {
            case GPUCommandType::Nop:
                ok = true;
                break;
            case GPUCommandType::Flush:
                ok = driver.flush(command.x, command.y, command.width, command.height);
                break;
            case GPUCommandType::SetMode:
                ok = driver.setMode(command.width, command.height);
                if (ok && framebuffer) {
                    framebuffer->switchToVirtIO(driver.getFramebuffer(),
                                                command.width,
                                                command.height,
                                                driver.getPitch());
                }
                break;
            case GPUCommandType::CreateContext:
                ok = driver.createContext(&value32,
                                          static_cast<const char*>(command.data),
                                          static_cast<uint32_t>(command.value),
                                          static_cast<uint8_t>(command.resourceId),
                                          command.bpp != 0);
                break;
            case GPUCommandType::DestroyContext:
                ok = driver.destroyContext(command.contextId);
                break;
            case GPUCommandType::Submit3D:
                ok = driver.submit3D(command.contextId, command.data, command.size);
                status = driver.getLastCommandStatus().responseType;
                value64 = driver.getLastSubmittedFence();
                break;
            case GPUCommandType::WaitFence:
                ok = driver.waitForFence(command.value,
                                         command.value2 != 0 ? command.value2 : 1000000ULL,
                                         &value64,
                                         &status);
                break;
        }

        setResult(result, ok, status, value32, value64);
        return ok;
    }

    void* getFramebuffer() override { return VirtIOGPUDriver::get().getFramebuffer(); }
    uint32_t getWidth() const override {
        uint32_t width = 0;
        uint32_t height = 0;
        VirtIOGPUDriver::get().getMode(&width, &height);
        return width;
    }
    uint32_t getHeight() const override {
        uint32_t width = 0;
        uint32_t height = 0;
        VirtIOGPUDriver::get().getMode(&width, &height);
        return height;
    }
    uint32_t getPitch() const override { return VirtIOGPUDriver::get().getPitch(); }
    uint32_t getFramebufferSize() const override { return VirtIOGPUDriver::get().getFBSize(); }
    bool isHardwareAccelerated() const override { return true; }

private:
    iFramebuffer* framebuffer = nullptr;

    void setResult(GPUCommandResult* result, bool ok, uint32_t status = 0,
                   uint32_t value32 = 0, uint64_t value64 = 0) {
        if (!result) {
            return;
        }
        result->ok = ok;
        result->status = status;
        result->value32 = value32;
        result->value64 = value64;
    }
};

FramebufferGPUBackend framebufferBackend;
VirtIOGPUBackend virtioBackend;

}

GPU& GPU::get() {
    static GPU instance;
    return instance;
}

GPU::GPU()
    : framebuffer(nullptr),
      backends{},
      backendCount(0),
      activeBackend(nullptr),
      initialized(false),
      builtinsRegistered(false) {
}

bool GPU::initialize(iFramebuffer* framebufferVal) {
    if (!framebufferVal) {
        return false;
    }

    framebuffer = framebufferVal;
    registerBuiltinBackends();

    activeBackend = nullptr;
    for (size_t i = 0; i < backendCount; ++i) {
        GPUBackend* backend = backends[i];
        if (!backend || !backend->probe()) {
            continue;
        }
        if (backend->initialize(*framebuffer)) {
            setActiveBackend(backend);
            initialized = true;
            return true;
        }
    }

    initialized = false;
    return false;
}

bool GPU::registerBackend(GPUBackend* backend) {
    if (!backend || backendCount >= MaxBackends) {
        return false;
    }

    for (size_t i = 0; i < backendCount; ++i) {
        if (backends[i] == backend) {
            return true;
        }
    }

    backends[backendCount++] = backend;
    return true;
}

bool GPU::submitCommand(const GPUCommand& command, GPUCommandResult* result) {
    if (!activeBackend) {
        clearResult(result, false, 1);
        return false;
    }
    return activeBackend->submitCommand(command, result);
}

bool GPU::flush(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    GPUCommand command = {};
    command.type = GPUCommandType::Flush;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    return submitCommand(command);
}

bool GPU::setMode(uint32_t width, uint32_t height, uint32_t bpp) {
    GPUCommand command = {};
    command.type = GPUCommandType::SetMode;
    command.width = width;
    command.height = height;
    command.bpp = bpp;
    return submitCommand(command);
}

bool GPU::submit3D(uint32_t contextId, const void* commands, uint32_t size) {
    GPUCommand command = {};
    command.type = GPUCommandType::Submit3D;
    command.contextId = contextId;
    command.data = commands;
    command.size = size;
    return submitCommand(command);
}

bool GPU::createContext(uint32_t* contextId, const char* debugName,
                        uint32_t contextInit, uint8_t ringIdx, bool useRingIdx) {
    GPUCommand command = {};
    GPUCommandResult result = {};
    command.type = GPUCommandType::CreateContext;
    command.data = debugName;
    command.value = contextInit;
    command.resourceId = ringIdx;
    command.bpp = useRingIdx ? 1 : 0;

    if (!submitCommand(command, &result)) {
        return false;
    }
    if (contextId) {
        *contextId = result.value32;
    }
    return true;
}

bool GPU::destroyContext(uint32_t contextId) {
    GPUCommand command = {};
    command.type = GPUCommandType::DestroyContext;
    command.contextId = contextId;
    return submitCommand(command);
}

bool GPU::waitForFence(uint64_t fenceId, uint64_t spinLimit, uint64_t* completedFence, uint32_t* responseType) {
    GPUCommand command = {};
    GPUCommandResult result = {};
    command.type = GPUCommandType::WaitFence;
    command.value = fenceId;
    command.value2 = spinLimit;

    if (!submitCommand(command, &result)) {
        return false;
    }
    if (completedFence) {
        *completedFence = result.value64;
    }
    if (responseType) {
        *responseType = result.status;
    }
    return true;
}

GPUBackendKind GPU::getActiveBackendKind() const {
    return activeBackend ? activeBackend->kind() : GPUBackendKind::Framebuffer;
}

const char* GPU::getActiveBackendName() const {
    return activeBackend ? activeBackend->name() : "none";
}

bool GPU::isHardwareAccelerated() const {
    return activeBackend && activeBackend->isHardwareAccelerated();
}

void GPU::registerBuiltinBackends() {
    if (builtinsRegistered) {
        return;
    }

    registerBackend(&virtioBackend);
    registerBackend(&framebufferBackend);
    builtinsRegistered = true;
}

bool GPU::setActiveBackend(GPUBackend* backend) {
    activeBackend = backend;
    return activeBackend != nullptr;
}

void GPU::clearResult(GPUCommandResult* result, bool ok, uint32_t status,
                      uint32_t value32, uint64_t value64) {
    if (!result) {
        return;
    }
    result->ok = ok;
    result->status = status;
    result->value32 = value32;
    result->value64 = value64;
}
