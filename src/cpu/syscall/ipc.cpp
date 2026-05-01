#include <cpu/syscall/syscall.hpp>

#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/console.hpp>
#include <interrupts/keyboard.hpp>
#include <ipc/ipc.hpp>

namespace {
constexpr uint32_t kSharedHandleRights = HandleRightMap | HandleRightDuplicate;
constexpr uint32_t kSurfaceHandleRights =
    HandleRightMap | HandleRightRead | HandleRightWrite | HandleRightDuplicate;
constexpr uint32_t kWindowHandleRights =
    HandleRightRead | HandleRightWrite | HandleRightSignal | HandleRightWait | HandleRightControl | HandleRightDuplicate;
constexpr uint32_t kQueueHandleRights =
    HandleRightRead | HandleRightWrite | HandleRightSignal | HandleRightWait | HandleRightControl | HandleRightDuplicate;
constexpr uint32_t kConnectedQueueRights =
    HandleRightRead | HandleRightWrite | HandleRightSignal | HandleRightWait | HandleRightDuplicate;
constexpr uint32_t kServiceHandleRights =
    HandleRightRead | HandleRightWrite | HandleRightSignal | HandleRightWait | HandleRightDuplicate;

void traceStr(const char* text) {
    Cereal::get().write(text);
}

void traceDec(uint64_t value) {
    char buf[21];
    int pos = 0;

    if (value == 0) {
        Cereal::get().write('0');
        return;
    }

    while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        Cereal::get().write(buf[--pos]);
    }
}

void traceSignedDec(int64_t value) {
    if (value < 0) {
        Cereal::get().write('-');
        traceDec(static_cast<uint64_t>(-value));
        return;
    }

    traceDec(static_cast<uint64_t>(value));
}

void traceHex(uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    Cereal::get().write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        Cereal::get().write(kDigits[(value >> shift) & 0xFULL]);
    }
}

bool isInputManagerName(const char* name) {
    return name && strncmp(name, "input.manager", IPCManager::MaxServiceName) == 0;
}

void traceProcess(Process* process) {
    traceStr(" pid=");
    traceDec(process ? process->getPID() : 0);
    traceStr(" name=");
    traceStr(process ? process->getName() : "<none>");
}

void traceInputEvent(const Event& event) {
    traceStr(" event_type=");
    traceDec(static_cast<uint64_t>(event.type));
    if (event.type == EventType::Key) {
        traceStr(" key_action=");
        traceDec(static_cast<uint64_t>(event.key.action));
        traceStr(" keycode=");
        traceDec(event.key.keycode);
        traceStr(" modifiers=");
        traceHex(event.key.modifiers);
        traceStr(" text0=");
        traceHex(static_cast<uint8_t>(event.key.text[0]));
    } else if (event.type == EventType::Pointer) {
        traceStr(" pointer_action=");
        traceDec(static_cast<uint64_t>(event.pointer.action));
        traceStr(" buttons=");
        traceHex(event.pointer.buttons);
        traceStr(" x=");
        traceDec(event.pointer.x);
        traceStr(" y=");
        traceDec(event.pointer.y);
        traceStr(" dx=");
        traceSignedDec(event.pointer.deltaX);
        traceStr(" dy=");
        traceSignedDec(event.pointer.deltaY);
    }
}

bool copyIPCMessageFromUser(uint64_t userPtr, IPCMessage* outMessage) {
    if (!outMessage || !Syscall::isValidUserPointer(userPtr, sizeof(IPCMessage))) {
        return false;
    }
    return Syscall::copyFromUser(outMessage, userPtr, sizeof(IPCMessage));
}

bool copyIPCMessageToUser(uint64_t userPtr, const IPCMessage& message) {
    if (!Syscall::isValidUserPointer(userPtr, sizeof(IPCMessage))) {
        return false;
    }
    return Syscall::copyToUser(userPtr, &message, sizeof(IPCMessage));
}

bool isCompositorProcess(Process* process) {
    return process && strcmp(process->getName(), "/bin/graphics-compositor.exe") == 0;
}

ServiceObject* resolveServiceHandle(Process* process, uint64_t handle, uint32_t requiredRights) {
    if (!process) {
        return nullptr;
    }

    return reinterpret_cast<ServiceObject*>(
        process->getHandleObject(handle, HandleType::Service, requiredRights)
    );
}

MessageQueueObject* resolveQueueHandle(Process* process, uint64_t handle, uint32_t requiredRights) {
    if (!process) {
        return nullptr;
    }

    auto* queue = reinterpret_cast<MessageQueueObject*>(
        process->getHandleObject(handle, HandleType::EventQueue, requiredRights)
    );
    if (queue) {
        return queue;
    }

    auto* service = reinterpret_cast<ServiceObject*>(
        process->getHandleObject(handle, HandleType::Service, requiredRights)
    );
    if (!service) {
        return nullptr;
    }

    return service->getQueue();
}

ServiceObject* resolveInputManagerServiceHandle(Process* process, uint64_t handle, uint32_t requiredRights) {
    auto* service = resolveServiceHandle(process, handle, requiredRights);
    if (!service || !isInputManagerName(service->getName())) {
        return nullptr;
    }
    return service;
}
}

uint64_t Syscall::sys_shared_alloc(uint64_t size) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || size == 0) {
        return static_cast<uint64_t>(-1);
    }

    SharedMemoryObject* object = IPCManager::get().createSharedMemory(size);
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    uint64_t handleValue = current->allocateHandle(
        HandleType::SharedMemory,
        kSharedHandleRights,
        object,
        retainSharedMemoryHandle,
        releaseSharedMemoryHandle
    );
    if (handleValue == static_cast<uint64_t>(-1)) {
        object->release();
    }
    return handleValue;
}

uint64_t Syscall::sys_shared_map(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* object = reinterpret_cast<SharedMemoryObject*>(
        current->getHandleObject(handle, HandleType::SharedMemory, HandleRightMap)
    );
    if (!object) {
        auto* surface = reinterpret_cast<SurfaceObject*>(
            current->getHandleObject(handle, HandleType::Surface, HandleRightMap)
        );
        if (!surface) {
            return static_cast<uint64_t>(-1);
        }

        uint64_t address = surface->mapInto(current);
        return address ? address : static_cast<uint64_t>(-1);
    }

    uint64_t address = object->mapInto(current);
    return address ? address : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_shared_free(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* object = reinterpret_cast<SharedMemoryObject*>(current->getHandleObject(handle, HandleType::SharedMemory));
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    object->unmapAllFrom(current);
    return current->closeHandle(handle, HandleType::SharedMemory) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_surface_create(uint64_t width, uint64_t height, uint64_t format) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    SurfaceObject* object = IPCManager::get().createSurface(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(format));
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    uint64_t handleValue = current->allocateHandle(
        HandleType::Surface,
        kSurfaceHandleRights,
        object,
        retainSurfaceHandle,
        releaseSurfaceHandle
    );
    if (handleValue == static_cast<uint64_t>(-1)) {
        object->release();
    }
    return handleValue;
}

uint64_t Syscall::sys_surface_map(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* object = reinterpret_cast<SurfaceObject*>(
        current->getHandleObject(handle, HandleType::Surface, HandleRightMap)
    );
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    uint64_t address = object->mapInto(current);
    return address ? address : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_surface_commit(uint64_t handle, uint64_t x, uint64_t y, uint64_t packedWH) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* object = reinterpret_cast<SurfaceObject*>(
        current->getHandleObject(handle, HandleType::Surface, HandleRightWrite)
    );
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    const uint32_t width = static_cast<uint32_t>(packedWH >> 32);
    const uint32_t height = static_cast<uint32_t>(packedWH & 0xFFFFFFFFULL);
    object->commit(static_cast<uint32_t>(x), static_cast<uint32_t>(y), width, height);

    Event event = {};
    event.type = EventType::Window;
    event.window.action = WindowEventAction::None;
    IPCManager::get().postServiceEvent("graphics.compositor", &event, sizeof(event));
    IPCManager::get().wakeQueueWaiters();
    return 0;
}

uint64_t Syscall::sys_surface_poll(uint64_t infoPtr) {
    if (!isValidUserPointer(infoPtr, sizeof(SurfaceInfo))) {
        return static_cast<uint64_t>(-1);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current)) {
        return static_cast<uint64_t>(-1);
    }

    SurfaceObject* object = IPCManager::get().pollCommittedSurface();
    if (!object) {
        return static_cast<uint64_t>(-1);
    }

    SurfaceInfo info = {};
    info.id = object->getID();
    info.width = object->getWidth();
    info.height = object->getHeight();
    info.format = object->getFormat();
    info.pitch = object->getPitch();
    info.dirtyX = object->getDirtyX();
    info.dirtyY = object->getDirtyY();
    info.dirtyWidth = object->getDirtyWidth();
    info.dirtyHeight = object->getDirtyHeight();
    info.address = object->mapInto(current);
    if (info.address == 0) {
        return static_cast<uint64_t>(-1);
    }

    return copyToUser(infoPtr, &info, sizeof(info)) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_compositor_create_window(uint64_t compositorHandle, uint64_t width, uint64_t height, uint64_t flags) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    ServiceObject* compositor = resolveServiceHandle(current, compositorHandle, HandleRightWrite);
    if (!compositor || strcmp(compositor->getName(), "graphics.compositor") != 0) {
        return static_cast<uint64_t>(-1);
    }

    WindowObject* window = IPCManager::get().createWindow(current, static_cast<uint32_t>(flags), static_cast<int32_t>(width), static_cast<int32_t>(height));
    if (!window) {
        return static_cast<uint64_t>(-1);
    }

    const uint64_t handle = current->allocateHandle(
        HandleType::Window,
        kWindowHandleRights,
        window,
        retainWindowHandle,
        releaseWindowHandle
    );
    if (handle == static_cast<uint64_t>(-1)) {
        window->release();
        return static_cast<uint64_t>(-1);
    }

    IPCManager::get().focusWindow(window->getID());
    return handle;
}

uint64_t Syscall::sys_window_event_queue(uint64_t windowHandle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* window = reinterpret_cast<WindowObject*>(
        current->getHandleObject(windowHandle, HandleType::Window, HandleRightRead)
    );
    if (!window || !window->getEventQueue()) {
        return static_cast<uint64_t>(-1);
    }

    window->getEventQueue()->retain();
    const uint64_t handle = current->allocateHandle(
        HandleType::EventQueue,
        kQueueHandleRights,
        window->getEventQueue(),
        retainMessageQueueHandle,
        releaseMessageQueueHandle
    );
    if (handle == static_cast<uint64_t>(-1)) {
        window->getEventQueue()->release();
    }
    return handle;
}

uint64_t Syscall::sys_window_set_title(uint64_t windowHandle, uint64_t titlePtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* window = reinterpret_cast<WindowObject*>(
        current->getHandleObject(windowHandle, HandleType::Window, HandleRightWrite)
    );
    if (!window) {
        return static_cast<uint64_t>(-1);
    }

    char title[64];
    if (!copyUserString(titlePtr, title, sizeof(title))) {
        return static_cast<uint64_t>(-1);
    }

    window->setTitle(title);
    return 0;
}

uint64_t Syscall::sys_window_attach_surface(uint64_t windowHandle, uint64_t surfaceHandle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* window = reinterpret_cast<WindowObject*>(
        current->getHandleObject(windowHandle, HandleType::Window, HandleRightWrite)
    );
    auto* surface = reinterpret_cast<SurfaceObject*>(
        current->getHandleObject(surfaceHandle, HandleType::Surface, HandleRightRead)
    );
    if (!window || !surface) {
        return static_cast<uint64_t>(-1);
    }

    window->attachSurface(surface);
    return 0;
}

uint64_t Syscall::sys_window_list(uint64_t entriesPtr, uint64_t capacity) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current) || capacity == 0 || capacity > IPCManager::MaxWindows) {
        return static_cast<uint64_t>(-1);
    }

    const size_t bytes = static_cast<size_t>(capacity) * sizeof(WindowInfo);
    if (!isValidUserPointer(entriesPtr, bytes)) {
        return static_cast<uint64_t>(-1);
    }

    WindowInfo* entries = new WindowInfo[capacity];
    if (!entries) {
        return static_cast<uint64_t>(-1);
    }

    memset(entries, 0, static_cast<size_t>(capacity) * sizeof(WindowInfo));
    const size_t count = IPCManager::get().listWindows(entries, static_cast<size_t>(capacity));

    for (size_t i = 0; i < count; i++) {
        if (!copyToUser(entriesPtr + (i * sizeof(WindowInfo)), &entries[i], sizeof(entries[i]))) {
            delete[] entries;
            return static_cast<uint64_t>(-1);
        }
    }

    delete[] entries;
    return count;
}

uint64_t Syscall::sys_window_focus(uint64_t windowId) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current)) {
        return static_cast<uint64_t>(-1);
    }

    return IPCManager::get().focusWindow(windowId) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_window_move(uint64_t windowId, uint64_t x, uint64_t y) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current)) {
        return static_cast<uint64_t>(-1);
    }

    return IPCManager::get().moveWindow(windowId, static_cast<int32_t>(x), static_cast<int32_t>(y))
        ? 0
        : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_window_resize(uint64_t windowId, uint64_t width, uint64_t height) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current)) {
        return static_cast<uint64_t>(-1);
    }

    return IPCManager::get().resizeWindow(windowId, static_cast<int32_t>(width), static_cast<int32_t>(height))
        ? 0
        : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_window_control(uint64_t windowId, uint64_t action) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!isCompositorProcess(current)) {
        return static_cast<uint64_t>(-1);
    }

    iFramebuffer* framebuffer = Console::get().getFramebuffer();
    const int32_t maxWidth = framebuffer ? static_cast<int32_t>(framebuffer->getWidth()) : 1024;
    const int32_t maxHeight = framebuffer ? static_cast<int32_t>(framebuffer->getHeight()) : 768;
    return IPCManager::get().controlWindow(windowId, static_cast<WindowControlAction>(action), maxWidth, maxHeight)
        ? 0
        : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_queue_create() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    if (strcmp(current->getName(), "/bin/input-manager.exe") == 0) {
        traceStr("[ipc:input] sys_queue_create begin");
        traceProcess(current);
        traceStr("\n");
    }

    MessageQueueObject* queue = IPCManager::get().createQueue();
    if (!queue) {
        if (strcmp(current->getName(), "/bin/input-manager.exe") == 0) {
            traceStr("[ipc:input] sys_queue_create createQueue failed");
            traceProcess(current);
            traceStr("\n");
        }
        return static_cast<uint64_t>(-1);
    }

    uint64_t handleValue = current->allocateHandle(
        HandleType::EventQueue,
        kQueueHandleRights,
        queue,
        retainMessageQueueHandle,
        releaseMessageQueueHandle
    );
    if (handleValue == static_cast<uint64_t>(-1)) {
        queue->release();
        if (strcmp(current->getName(), "/bin/input-manager.exe") == 0) {
            traceStr("[ipc:input] sys_queue_create allocateHandle failed");
            traceProcess(current);
            traceStr("\n");
        }
    } else if (strcmp(current->getName(), "/bin/input-manager.exe") == 0) {
        traceStr("[ipc:input] sys_queue_create ok handle=");
        traceHex(handleValue);
        traceProcess(current);
        traceStr("\n");
    }
    return handleValue;
}

uint64_t Syscall::sys_queue_send(uint64_t handle, uint64_t messagePtr, uint64_t wait) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* queue = resolveQueueHandle(current, handle, HandleRightWrite);
    if (!queue) {
        return static_cast<uint64_t>(-1);
    }

    IPCMessage message {};
    if (!copyIPCMessageFromUser(messagePtr, &message) || message.size > MessageQueueObject::MaxPayloadSize) {
        return static_cast<uint64_t>(-1);
    }

    IPCMessageHeader header {};
    header.id = message.id;
    header.senderPID = current->getPID();
    header.flags = message.flags;
    header.reserved = 0;
    header.size = message.size;

    while (!queue->enqueue(header, message.data)) {
        if (!wait) {
            return static_cast<uint64_t>(-1);
        }

        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
    }

    IPCManager::get().wakeQueueWaiters();
    return 0;
}

uint64_t Syscall::sys_queue_receive(uint64_t handle, uint64_t messagePtr, uint64_t wait) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    ServiceObject* inputService = resolveInputManagerServiceHandle(current, handle, HandleRightRead);
    auto* queue = resolveQueueHandle(current, handle, HandleRightRead);
    if (!queue) {
        if (inputService || strcmp(current->getName(), "/bin/login.exe") == 0) {
            traceStr("[ipc:queue] receive invalid handle=");
            traceHex(handle);
            traceProcess(current);
            traceStr(" wait=");
            traceDec(wait);
            traceStr("\n");
        }
        return static_cast<uint64_t>(-1);
    }

    MessageQueueObject::Message kernelMessage {};
    while (!queue->dequeue(&kernelMessage)) {
        if (!wait) {
            if (inputService) {
                traceStr("[ipc:input] receive poll empty handle=");
                traceHex(handle);
                traceProcess(current);
                traceStr("\n");
            }
            return static_cast<uint64_t>(-1);
        }

        if (inputService) {
            traceStr("[ipc:input] receive blocking handle=");
            traceHex(handle);
            traceProcess(current);
            traceStr(" pending=");
            traceDec(queue->pendingCount());
            traceStr("\n");
        }
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        if (inputService) {
            traceStr("[ipc:input] receive woke handle=");
            traceHex(handle);
            traceProcess(current);
            traceStr(" pending=");
            traceDec(queue->pendingCount());
            traceStr("\n");
        }
    }

    IPCMessage userMessage {};
    userMessage.id = kernelMessage.header.id;
    userMessage.senderPID = kernelMessage.header.senderPID;
    userMessage.flags = kernelMessage.header.flags;
    userMessage.reserved = 0;
    userMessage.size = kernelMessage.header.size;
    if (userMessage.size > 0) {
        memcpy(userMessage.data, kernelMessage.payload, static_cast<size_t>(userMessage.size));
    }

    if (inputService) {
        traceStr("[ipc:input] receive dequeued handle=");
        traceHex(handle);
        traceProcess(current);
        traceStr(" flags=");
        traceHex(kernelMessage.header.flags);
        traceStr(" size=");
        traceDec(kernelMessage.header.size);
        traceStr(" pending=");
        traceDec(queue->pendingCount());
        if ((kernelMessage.header.flags & IPCMessageFlagEvent) != 0 && kernelMessage.header.size >= sizeof(Event)) {
            traceInputEvent(*reinterpret_cast<const Event*>(kernelMessage.payload));
        }
        traceStr("\n");
    }

    IPCManager::get().wakeQueueWaiters();
    if (!copyIPCMessageToUser(messagePtr, userMessage)) {
        if (inputService) {
            traceStr("[ipc:input] receive copy_to_user failed ptr=");
            traceHex(messagePtr);
            traceProcess(current);
            traceStr("\n");
        }
        return static_cast<uint64_t>(-1);
    }
    return 0;
}

uint64_t Syscall::sys_queue_reply(uint64_t handle, uint64_t requestID, uint64_t dataPtr, uint64_t size) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || size > MessageQueueObject::MaxPayloadSize) {
        return static_cast<uint64_t>(-1);
    }

    auto* queue = resolveQueueHandle(current, handle, HandleRightSignal);
    if (!queue) {
        return static_cast<uint64_t>(-1);
    }

    uint8_t data[MessageQueueObject::MaxPayloadSize];
    if (size > 0) {
        if (!isValidUserPointer(dataPtr, size) || !copyFromUser(data, dataPtr, static_cast<size_t>(size))) {
            return static_cast<uint64_t>(-1);
        }
    }

    return IPCManager::get().completeRequest(requestID, data, size) ? 0 : static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_queue_request(
    uint64_t handle,
    uint64_t requestPtr,
    uint64_t responsePtr,
    uint64_t responseCapacity,
    uint64_t responseSizePtr
) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
    }

    auto* queue = resolveQueueHandle(current, handle, HandleRightWrite);
    if (!queue) {
        return static_cast<uint64_t>(-1);
    }

    IPCMessage request {};
    if (!copyIPCMessageFromUser(requestPtr, &request) || request.size > MessageQueueObject::MaxPayloadSize) {
        return static_cast<uint64_t>(-1);
    }

    uint64_t requestID = IPCManager::get().nextRequestID();
    if (!IPCManager::get().beginRequest(requestID, current)) {
        return static_cast<uint64_t>(-1);
    }

    IPCMessageHeader header {};
    header.id = requestID;
    header.senderPID = current->getPID();
    header.flags = request.flags | IPCMessageFlagRequest;
    header.reserved = 0;
    header.size = request.size;

    while (!queue->enqueue(header, request.data)) {
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
    }

    IPCManager::get().wakeQueueWaiters();

    uint8_t response[MessageQueueObject::MaxPayloadSize];
    uint64_t responseSize = 0;
    if (!IPCManager::get().waitForRequest(current, requestID, response, &responseSize)) {
        return static_cast<uint64_t>(-1);
    }

    if (responseSize > responseCapacity ||
        !isValidUserPointer(responsePtr, responseSize) ||
        !copyToUser(responsePtr, response, static_cast<size_t>(responseSize))) {
        return static_cast<uint64_t>(-1);
    }

    if (responseSizePtr && (!isValidUserPointer(responseSizePtr, sizeof(uint64_t)) ||
        !copyToUser(responseSizePtr, &responseSize, sizeof(responseSize)))) {
        return static_cast<uint64_t>(-1);
    }

    return 0;
}

uint64_t Syscall::sys_service_register(uint64_t namePtr, uint64_t queueHandle) {
    char name[IPCManager::MaxServiceName];
    if (!copyUserString(namePtr, name, sizeof(name))) {
        traceStr("[ipc:service] sys_register bad name_ptr=");
        traceHex(namePtr);
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        traceStr("[ipc:service] sys_register no current name=");
        traceStr(name);
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    traceStr("[ipc:service] sys_register name=");
    traceStr(name);
    traceProcess(current);
    traceStr(" queue_handle=");
    traceHex(queueHandle);
    traceStr("\n");

    auto* queue = reinterpret_cast<MessageQueueObject*>(
        current->getHandleObject(queueHandle, HandleType::EventQueue, HandleRightControl)
    );
    if (!queue) {
        traceStr("[ipc:service] sys_register invalid queue name=");
        traceStr(name);
        traceProcess(current);
        traceStr(" queue_handle=");
        traceHex(queueHandle);
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    const uint64_t result = IPCManager::get().registerService(current, name, queue);
    traceStr("[ipc:service] sys_register result name=");
    traceStr(name);
    traceProcess(current);
    traceStr(" result=");
    traceHex(result);
    traceStr("\n");
    if (result != static_cast<uint64_t>(-1) && strcmp(name, "input.manager") == 0) {
        traceStr("[ipc:input] input.manager registered; publishing buffered keyboard input\n");
        Keyboard::get().publishBufferedInputToInputManager();
    }
    return result;
}

uint64_t Syscall::sys_service_connect(uint64_t namePtr) {
    char name[IPCManager::MaxServiceName];
    if (!copyUserString(namePtr, name, sizeof(name))) {
        traceStr("[ipc:service] sys_connect bad name_ptr=");
        traceHex(namePtr);
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        traceStr("[ipc:service] sys_connect no current name=");
        traceStr(name);
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    ServiceObject* service = IPCManager::get().connectService(name);
    if (!service) {
        if (isInputManagerName(name)) {
            traceStr("[ipc:service] sys_connect missing name=");
            traceStr(name);
            traceProcess(current);
            traceStr("\n");
        }
        return static_cast<uint64_t>(-1);
    }

    service->retain();
    uint64_t handleValue = current->allocateHandle(
        HandleType::Service,
        kServiceHandleRights,
        service,
        retainServiceHandle,
        releaseServiceHandle
    );
    if (handleValue == static_cast<uint64_t>(-1)) {
        traceStr("[ipc:service] sys_connect handle alloc failed name=");
        traceStr(name);
        traceProcess(current);
        traceStr("\n");
        service->release();
    } else if (isInputManagerName(name)) {
        traceStr("[ipc:service] sys_connect ok name=");
        traceStr(name);
        traceProcess(current);
        traceStr(" handle=");
        traceHex(handleValue);
        traceStr(" pending=");
        traceDec(service->getQueue() ? service->getQueue()->pendingCount() : 0);
        traceStr("\n");
    }
    return handleValue;
}
