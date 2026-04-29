#include <ipc/ipc.hpp>

#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/console.hpp>
#include <memory/pmm.hpp>

namespace {
constexpr uint64_t kSharedMapFlags = Present | ReadWrite | UserSuper | NoExecute;

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
    static constexpr char digits[] = "0123456789abcdef";
    Cereal::get().write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        Cereal::get().write(digits[(value >> shift) & 0xFULL]);
    }
}

bool isInputManagerName(const char* name) {
    return name && strncmp(name, "input.manager", IPCManager::MaxServiceName) == 0;
}

void traceServiceName(const char* prefix, const char* name) {
    traceStr(prefix);
    traceStr(name ? name : "<null>");
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
}

SharedMemoryObject::SharedMemoryObject(uint64_t sizeValue, uint64_t alignedSizeValue, uint64_t physBaseValue)
    : size(sizeValue), alignedSize(alignedSizeValue), physBase(physBaseValue), refCount(1), mappingCount(0) {
    memset(mappings, 0, sizeof(mappings));
}

void SharedMemoryObject::retain() {
    refCount++;
}

void SharedMemoryObject::release() {
    if (refCount == 0) {
        return;
    }
    refCount--;
    if (canDestroy()) {
        destroy();
    }
}

int SharedMemoryObject::findMappingSlot(uint32_t pid) const {
    for (size_t i = 0; i < mappingCount; i++) {
        if (mappings[i].pid == pid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SharedMemoryObject::canDestroy() const {
    return refCount == 0 && mappingCount == 0;
}

void SharedMemoryObject::destroy() {
    IPCManager::get().forgetSharedObject(this);
    PMM::FreeFrames(physBase, alignedSize / PAGE_SIZE);
    delete this;
}

uint64_t SharedMemoryObject::mapInto(Process* process) {
    if (!process) {
        return 0;
    }

    int existing = findMappingSlot(process->getPID());
    if (existing >= 0) {
        return mappings[existing].address;
    }
    if (mappingCount >= MaxMappings) {
        return 0;
    }

    uint64_t address = process->reserveMmapRegion(alignedSize);
    VMM::MapRangeInto(process->getPageTable(), address, physBase, alignedSize / PAGE_SIZE, kSharedMapFlags);

    mappings[mappingCount].pid = process->getPID();
    mappings[mappingCount].address = address;
    mappingCount++;
    return address;
}

bool SharedMemoryObject::unmapFrom(Process* process) {
    if (!process) {
        return false;
    }

    int slot = findMappingSlot(process->getPID());
    if (slot < 0) {
        return false;
    }

    VMM::UnmapRangeFrom(process->getPageTable(), mappings[slot].address, alignedSize / PAGE_SIZE);
    mappings[slot] = mappings[mappingCount - 1];
    mappings[mappingCount - 1].pid = 0;
    mappings[mappingCount - 1].address = 0;
    mappingCount--;

    if (canDestroy()) {
        destroy();
    }
    return true;
}

void SharedMemoryObject::unmapAllFrom(Process* process) {
    while (unmapFrom(process)) {
    }
}

SurfaceObject::SurfaceObject(uint64_t idValue, uint32_t widthValue, uint32_t heightValue, uint32_t formatValue, SharedMemoryObject* backingObject)
    : id(idValue), width(widthValue), height(heightValue), format(formatValue), backing(backingObject), refCount(1),
      committed(false), dirtyX(0), dirtyY(0), dirtyWidth(widthValue), dirtyHeight(heightValue) {
    if (backing) {
        backing->retain();
    }
}

SurfaceObject::~SurfaceObject() {
    if (backing) {
        backing->release();
    }
}

void SurfaceObject::retain() {
    refCount++;
}

void SurfaceObject::release() {
    if (refCount == 0) {
        return;
    }

    refCount--;
    if (refCount == 0) {
        IPCManager::get().forgetSurface(this);
        delete this;
    }
}

uint64_t SurfaceObject::mapInto(Process* process) {
    return backing ? backing->mapInto(process) : 0;
}

void SurfaceObject::commit(uint32_t x, uint32_t y, uint32_t widthValue, uint32_t heightValue) {
    dirtyX = x < width ? x : width;
    dirtyY = y < height ? y : height;
    dirtyWidth = widthValue;
    dirtyHeight = heightValue;

    if (dirtyX + dirtyWidth > width) {
        dirtyWidth = width > dirtyX ? width - dirtyX : 0;
    }
    if (dirtyY + dirtyHeight > height) {
        dirtyHeight = height > dirtyY ? height - dirtyY : 0;
    }
    committed = true;
}

bool SurfaceObject::consumeCommit() {
    if (!committed) {
        return false;
    }
    committed = false;
    return true;
}

WindowObject::WindowObject(
    uint64_t idValue,
    uint32_t ownerPIDValue,
    uint32_t flagsValue,
    int32_t widthValue,
    int32_t heightValue,
    MessageQueueObject* eventQueue
) : id(idValue),
    ownerPID(ownerPIDValue),
    flags(flagsValue),
    refCount(1),
    state(WindowStateNone),
    zOrder(0),
    x(0),
    y(0),
    width(widthValue),
    height(heightValue),
    restoreX(0),
    restoreY(0),
    restoreWidth(widthValue),
    restoreHeight(heightValue),
    queue(eventQueue),
    surface(nullptr) {
    memset(title, 0, sizeof(title));
    if (queue) {
        queue->retain();
    }
}

WindowObject::~WindowObject() {
    if (surface) {
        surface->release();
    }
    if (queue) {
        queue->release();
    }
}

void WindowObject::retain() {
    refCount++;
}

void WindowObject::release() {
    if (refCount == 0) {
        return;
    }

    refCount--;
    if (refCount == 0) {
        IPCManager::get().forgetWindow(this);
        delete this;
    }
}

void WindowObject::setTitle(const char* value) {
    title[0] = '\0';
    if (!value) {
        return;
    }

    strncpy(title, value, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
}

void WindowObject::attachSurface(SurfaceObject* value) {
    if (value == surface) {
        return;
    }

    if (value) {
        value->retain();
    }
    if (surface) {
        surface->release();
    }
    surface = value;
}

void WindowObject::setPosition(int32_t newX, int32_t newY) {
    x = newX;
    y = newY;
}

void WindowObject::setSize(int32_t newWidth, int32_t newHeight) {
    width = newWidth > 1 ? newWidth : 1;
    height = newHeight > 1 ? newHeight : 1;
}

void WindowObject::setFocused(bool focused) {
    if (focused) {
        state |= WindowStateFocused;
    } else {
        state &= ~WindowStateFocused;
    }
}

void WindowObject::control(WindowControlAction action, int32_t maxWidth, int32_t maxHeight) {
    switch (action) {
        case WindowControlAction::Restore:
            if (state & WindowStateClosed) {
                break;
            }
            state &= ~WindowStateMinimized;
            if (state & WindowStateMaximized) {
                state &= ~WindowStateMaximized;
                x = restoreX;
                y = restoreY;
                width = restoreWidth;
                height = restoreHeight;
            }
            break;
        case WindowControlAction::Minimize:
            if ((state & WindowStateClosed) == 0) {
                state |= WindowStateMinimized;
                state &= ~WindowStateFocused;
            }
            break;
        case WindowControlAction::Maximize:
            if (state & WindowStateClosed) {
                break;
            }
            if ((state & WindowStateMaximized) == 0) {
                restoreX = x;
                restoreY = y;
                restoreWidth = width;
                restoreHeight = height;
            }
            state &= ~WindowStateMinimized;
            state |= WindowStateMaximized;
            x = 16;
            y = 16;
            width = maxWidth > 32 ? maxWidth - 32 : maxWidth;
            height = maxHeight > 32 ? maxHeight - 32 : maxHeight;
            break;
        case WindowControlAction::Close:
            state |= WindowStateClosed;
            state &= ~WindowStateFocused;
            state &= ~WindowStateMinimized;
            break;
    }
}

bool WindowObject::enqueueWindowEvent(WindowEventAction action) {
    if (!queue) {
        return false;
    }

    Event event = {};
    event.type = EventType::Window;
    event.window.action = action;
    event.window.windowId = static_cast<uint32_t>(id);
    event.window.x = x;
    event.window.y = y;
    event.window.width = width;
    event.window.height = height;

    IPCMessageHeader header = {};
    header.id = 0;
    header.senderPID = 0;
    header.flags = IPCMessageFlagEvent;
    header.size = sizeof(event);
    return queue->enqueue(header, &event);
}

void WindowObject::snapshot(WindowInfo* out) const {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->id = id;
    out->ownerPID = ownerPID;
    out->flags = flags;
    out->state = state;
    out->x = x;
    out->y = y;
    out->width = width;
    out->height = height;
    out->surfaceID = getSurfaceID();
    out->zOrder = zOrder;
    strncpy(out->title, title, sizeof(out->title) - 1);
}

MessageQueueObject::MessageQueueObject() : refCount(1), head(0), count(0) {
    memset(messages, 0, sizeof(messages));
}

void MessageQueueObject::retain() {
    refCount++;
}

void MessageQueueObject::release() {
    if (refCount == 0) {
        return;
    }
    refCount--;
    if (refCount == 0) {
        IPCManager::get().forgetQueue(this);
        delete this;
    }
}

bool MessageQueueObject::enqueue(const IPCMessageHeader& header, const void* payload) {
    if (count >= MaxMessages || header.size > MaxPayloadSize) {
        return false;
    }

    size_t slot = (head + count) % MaxMessages;
    messages[slot].header = header;
    if (header.size > 0 && payload) {
        memcpy(messages[slot].payload, payload, static_cast<size_t>(header.size));
    }
    count++;
    return true;
}

bool MessageQueueObject::dequeue(Message* outMessage) {
    if (!outMessage || count == 0) {
        return false;
    }

    *outMessage = messages[head];
    head = (head + 1) % MaxMessages;
    count--;
    return true;
}

bool MessageQueueObject::hasMessages() const {
    return count != 0;
}

ServiceObject::ServiceObject(const char* serviceName, uint32_t serviceOwnerPID, MessageQueueObject* serviceQueue)
    : ownerPID(serviceOwnerPID), refCount(1), queue(serviceQueue) {
    name[0] = '\0';
    if (serviceName) {
        strncpy(name, serviceName, IPCMaxServiceName - 1);
        name[IPCMaxServiceName - 1] = '\0';
    }

    if (queue) {
        queue->retain();
    }
}

void ServiceObject::retain() {
    refCount++;
}

void ServiceObject::release() {
    if (refCount == 0) {
        return;
    }

    refCount--;
    if (refCount == 0) {
        if (queue) {
            queue->release();
        }
        delete this;
    }
}

IPCManager& IPCManager::get() {
    static IPCManager instance;
    return instance;
}

IPCManager::IPCManager() : requestCounter(1), surfaceIDCounter(1), windowIDCounter(1), focusedWindowID(0), nextWindowZOrder(1) {
    memset(sharedObjects, 0, sizeof(sharedObjects));
    memset(surfaces, 0, sizeof(surfaces));
    memset(windows, 0, sizeof(windows));
    memset(queues, 0, sizeof(queues));
    memset(services, 0, sizeof(services));
    memset(requests, 0, sizeof(requests));
}

void IPCManager::registerSharedObject(SharedMemoryObject* object) {
    for (size_t i = 0; i < MaxSharedObjects; i++) {
        if (!sharedObjects[i]) {
            sharedObjects[i] = object;
            return;
        }
    }
}

void IPCManager::registerQueue(MessageQueueObject* queue) {
    for (size_t i = 0; i < MaxQueues; i++) {
        if (!queues[i]) {
            queues[i] = queue;
            return;
        }
    }
}

void IPCManager::registerSurface(SurfaceObject* object) {
    for (size_t i = 0; i < MaxSurfaces; i++) {
        if (!surfaces[i]) {
            surfaces[i] = object;
            return;
        }
    }
}

void IPCManager::registerWindow(WindowObject* object) {
    for (size_t i = 0; i < MaxWindows; i++) {
        if (!windows[i]) {
            windows[i] = object;
            return;
        }
    }
}

void IPCManager::forgetSharedObject(SharedMemoryObject* object) {
    for (size_t i = 0; i < MaxSharedObjects; i++) {
        if (sharedObjects[i] == object) {
            sharedObjects[i] = nullptr;
            return;
        }
    }
}

void IPCManager::forgetSurface(SurfaceObject* object) {
    for (size_t i = 0; i < MaxSurfaces; i++) {
        if (surfaces[i] == object) {
            surfaces[i] = nullptr;
            return;
        }
    }
}

WindowObject* IPCManager::topVisibleWindow() const {
    WindowObject* best = nullptr;
    for (size_t i = 0; i < MaxWindows; i++) {
        WindowObject* window = windows[i];
        if (!window) {
            continue;
        }

        const uint32_t state = window->getState();
        if ((state & WindowStateClosed) != 0 || (state & WindowStateMinimized) != 0) {
            continue;
        }

        if (!best || window->getZOrder() > best->getZOrder()) {
            best = window;
        }
    }
    return best;
}

void IPCManager::forgetWindow(WindowObject* object) {
    if (!object) {
        return;
    }

    for (size_t i = 0; i < MaxWindows; i++) {
        if (windows[i] == object) {
            windows[i] = nullptr;
            break;
        }
    }

    if (focusedWindowID == object->getID()) {
        focusedWindowID = 0;
        WindowObject* next = topVisibleWindow();
        if (next) {
            focusWindow(next->getID());
        }
    }
}

void IPCManager::forgetQueue(MessageQueueObject* queue) {
    for (size_t i = 0; i < MaxQueues; i++) {
        if (queues[i] == queue) {
            queues[i] = nullptr;
        }
    }

    for (size_t i = 0; i < MaxServices; i++) {
        if (services[i].used && services[i].service && services[i].service->getQueue() == queue) {
            services[i].service->release();
            services[i].used = false;
            services[i].service = nullptr;
        }
    }
}

SharedMemoryObject* IPCManager::createSharedMemory(uint64_t size) {
    if (size == 0) {
        return nullptr;
    }

    uint64_t alignedSize = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t phys = PMM::AllocFrames(alignedSize / PAGE_SIZE);
    if (!phys) {
        return nullptr;
    }

    memset(reinterpret_cast<void*>(phys), 0, static_cast<size_t>(alignedSize));

    SharedMemoryObject* object = new SharedMemoryObject(size, alignedSize, phys);
    if (!object) {
        PMM::FreeFrames(phys, alignedSize / PAGE_SIZE);
        return nullptr;
    }

    registerSharedObject(object);
    return object;
}

SurfaceObject* IPCManager::createSurface(uint32_t width, uint32_t height, uint32_t format) {
    if (width == 0 || height == 0) {
        return nullptr;
    }

    const uint64_t size = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
    if (size == 0 || size > (256ULL * 1024ULL * 1024ULL)) {
        return nullptr;
    }

    SharedMemoryObject* backing = createSharedMemory(size);
    if (!backing) {
        return nullptr;
    }

    SurfaceObject* object = new SurfaceObject(surfaceIDCounter++, width, height, format, backing);
    backing->release();
    if (!object) {
        return nullptr;
    }

    registerSurface(object);
    return object;
}

WindowObject* IPCManager::createWindow(Process* owner, uint32_t flags, int32_t width, int32_t height) {
    if (!owner || width <= 0 || height <= 0) {
        return nullptr;
    }

    MessageQueueObject* queue = createQueue();
    if (!queue) {
        return nullptr;
    }

    WindowObject* object = new WindowObject(windowIDCounter++, owner->getPID(), flags, width, height, queue);
    if (!object) {
        queue->release();
        return nullptr;
    }
    queue->release();

    const int32_t offset = static_cast<int32_t>(((object->getID() - 1) % 8) * 28);
    object->setPosition(48 + offset, 48 + offset);
    object->setZOrder(nextWindowZOrder++);
    registerWindow(object);
    return object;
}

MessageQueueObject* IPCManager::createQueue() {
    MessageQueueObject* queue = new MessageQueueObject();
    if (!queue) {
        return nullptr;
    }

    registerQueue(queue);
    return queue;
}

SurfaceObject* IPCManager::lookupSurface(uint64_t id) {
    for (size_t i = 0; i < MaxSurfaces; i++) {
        if (surfaces[i] && surfaces[i]->getID() == id) {
            return surfaces[i];
        }
    }
    return nullptr;
}

WindowObject* IPCManager::lookupWindow(uint64_t id) {
    for (size_t i = 0; i < MaxWindows; i++) {
        if (windows[i] && windows[i]->getID() == id) {
            return windows[i];
        }
    }
    return nullptr;
}

SurfaceObject* IPCManager::pollCommittedSurface() {
    for (size_t i = 0; i < MaxSurfaces; i++) {
        if (surfaces[i] && surfaces[i]->consumeCommit()) {
            return surfaces[i];
        }
    }
    return nullptr;
}

size_t IPCManager::listWindows(WindowInfo* entries, size_t capacity) const {
    if (!entries || capacity == 0) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < MaxWindows && count < capacity; i++) {
        if (!windows[i]) {
            continue;
        }

        windows[i]->snapshot(&entries[count++]);
    }

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (entries[j].zOrder < entries[i].zOrder) {
                WindowInfo temp = entries[i];
                entries[i] = entries[j];
                entries[j] = temp;
            }
        }
    }

    return count;
}

bool IPCManager::focusWindow(uint64_t id) {
    WindowObject* target = lookupWindow(id);
    if (!target) {
        return false;
    }

    if (target->getState() & WindowStateClosed) {
        return false;
    }

    WindowObject* oldFocused = lookupWindow(focusedWindowID);
    if (oldFocused && oldFocused != target) {
        oldFocused->setFocused(false);
        oldFocused->enqueueWindowEvent(WindowEventAction::FocusLost);
    }

    if (target->getState() & WindowStateMinimized) {
        target->control(WindowControlAction::Restore, target->getWidth(), target->getHeight());
    }

    focusedWindowID = id;
    target->setFocused(true);
    target->setZOrder(nextWindowZOrder++);
    target->enqueueWindowEvent(WindowEventAction::FocusGained);
    wakeQueueWaiters();
    return true;
}

bool IPCManager::moveWindow(uint64_t id, int32_t x, int32_t y) {
    WindowObject* window = lookupWindow(id);
    if (!window || (window->getState() & WindowStateClosed) != 0) {
        return false;
    }

    window->setPosition(x, y);
    window->enqueueWindowEvent(WindowEventAction::Moved);
    wakeQueueWaiters();
    return true;
}

bool IPCManager::resizeWindow(uint64_t id, int32_t width, int32_t height) {
    WindowObject* window = lookupWindow(id);
    if (!window || (window->getState() & WindowStateClosed) != 0) {
        return false;
    }

    window->setSize(width, height);
    window->enqueueWindowEvent(WindowEventAction::Resized);
    wakeQueueWaiters();
    return true;
}

bool IPCManager::controlWindow(uint64_t id, WindowControlAction action, int32_t maxWidth, int32_t maxHeight) {
    WindowObject* window = lookupWindow(id);
    if (!window) {
        return false;
    }

    window->control(action, maxWidth, maxHeight);
    if (action == WindowControlAction::Close) {
        window->enqueueWindowEvent(WindowEventAction::CloseRequested);
    } else if (action == WindowControlAction::Minimize || action == WindowControlAction::Restore || action == WindowControlAction::Maximize) {
        window->enqueueWindowEvent(WindowEventAction::Resized);
        if (action == WindowControlAction::Minimize && focusedWindowID == id) {
            focusedWindowID = 0;
            WindowObject* next = topVisibleWindow();
            if (next) {
                focusWindow(next->getID());
            }
        }
    }

    wakeQueueWaiters();
    return true;
}

bool IPCManager::postEventToFocusedWindow(const Event& event) {
    if (event.type != EventType::Key) {
        return false;
    }

    WindowObject* focused = lookupWindow(focusedWindowID);
    if (!focused || !focused->getEventQueue() ||
        (focused->getState() & (WindowStateClosed | WindowStateMinimized)) != 0) {
        focused = topVisibleWindow();
        if (!focused || !focused->getEventQueue()) {
            return false;
        }

        if (focusedWindowID != focused->getID()) {
            focusWindow(focused->getID());
        }
    }

    IPCMessageHeader header = {};
    header.id = 0;
    header.senderPID = 0;
    header.flags = IPCMessageFlagEvent;
    header.size = sizeof(event);
    if (!focused->getEventQueue()->enqueue(header, &event)) {
        return false;
    }

    wakeQueueWaiters();
    return true;
}

bool IPCManager::postKeyEventToFocusedWindow(const Event& event) {
    if (event.type != EventType::Key) {
        return false;
    }

    return postEventToFocusedWindow(event);
}

uint64_t IPCManager::registerService(Process* owner, const char* name, MessageQueueObject* queue) {
    if (!owner || !name || !name[0] || !queue) {
        traceServiceName("[ipc:service] register invalid name=", name);
        traceStr(" owner=");
        traceDec(owner ? owner->getPID() : 0);
        traceStr(" queue=");
        traceHex(reinterpret_cast<uint64_t>(queue));
        traceStr("\n");
        return static_cast<uint64_t>(-1);
    }

    traceServiceName("[ipc:service] register request name=", name);
    traceStr(" owner=");
    traceDec(owner->getPID());
    traceStr(" queue=");
    traceHex(reinterpret_cast<uint64_t>(queue));
    traceStr(" pending=");
    traceDec(queue->pendingCount());
    traceStr("\n");

    for (size_t i = 0; i < MaxServices; i++) {
        if (services[i].used && services[i].service &&
            strncmp(services[i].service->getName(), name, MaxServiceName) == 0) {
            traceServiceName("[ipc:service] register duplicate name=", name);
            traceStr("\n");
            return static_cast<uint64_t>(-1);
        }
    }

    for (size_t i = 0; i < MaxServices; i++) {
        if (!services[i].used) {
            ServiceObject* service = new ServiceObject(name, owner->getPID(), queue);
            if (!service) {
                traceServiceName("[ipc:service] register alloc failed name=", name);
                traceStr("\n");
                return static_cast<uint64_t>(-1);
            }
            services[i].used = true;
            services[i].service = service;
            traceServiceName("[ipc:service] registered name=", name);
            traceStr(" slot=");
            traceDec(i);
            traceStr(" owner=");
            traceDec(owner->getPID());
            traceStr("\n");
            return 0;
        }
    }
    traceServiceName("[ipc:service] register table full name=", name);
    traceStr("\n");
    return static_cast<uint64_t>(-1);
}

ServiceObject* IPCManager::connectService(const char* name) {
    if (!name || !name[0]) {
        traceServiceName("[ipc:service] connect invalid name=", name);
        traceStr("\n");
        return nullptr;
    }

    for (size_t i = 0; i < MaxServices; i++) {
        if (services[i].used && services[i].service &&
            strncmp(services[i].service->getName(), name, MaxServiceName) == 0) {
            if (isInputManagerName(name)) {
                traceServiceName("[ipc:service] connect found name=", name);
                traceStr(" slot=");
                traceDec(i);
                traceStr(" owner=");
                traceDec(services[i].service->getOwnerPID());
                traceStr(" pending=");
                traceDec(services[i].service->getQueue() ? services[i].service->getQueue()->pendingCount() : 0);
                traceStr("\n");
            }
            return services[i].service;
        }
    }
    if (isInputManagerName(name)) {
        traceServiceName("[ipc:service] connect missing name=", name);
        traceStr("\n");
    }
    return nullptr;
}

bool IPCManager::postServiceEvent(const char* name, const void* payload, uint64_t size) {
    if (!payload || size == 0 || size > MessageQueueObject::MaxPayloadSize) {
        if (isInputManagerName(name)) {
            traceServiceName("[ipc:input] post invalid payload service=", name);
            traceStr(" payload=");
            traceHex(reinterpret_cast<uint64_t>(payload));
            traceStr(" size=");
            traceDec(size);
            traceStr("\n");
        }
        return false;
    }

    ServiceObject* service = connectService(name);
    if (!service || !service->getQueue()) {
        if (isInputManagerName(name)) {
            traceServiceName("[ipc:input] post service unavailable service=", name);
            traceStr("\n");
        }
        return false;
    }

    IPCMessageHeader header {};
    header.id = 0;
    header.senderPID = 0;
    header.flags = IPCMessageFlagEvent;
    header.reserved = 0;
    header.size = size;

    const size_t beforeCount = service->getQueue()->pendingCount();
    if (!service->getQueue()->enqueue(header, payload)) {
        if (isInputManagerName(name)) {
            traceServiceName("[ipc:input] post enqueue failed service=", name);
            traceStr(" size=");
            traceDec(size);
            traceStr(" pending=");
            traceDec(beforeCount);
            if (size >= sizeof(Event)) {
                traceInputEvent(*reinterpret_cast<const Event*>(payload));
            }
            traceStr("\n");
        }
        return false;
    }

    if (isInputManagerName(name)) {
        traceServiceName("[ipc:input] post queued service=", name);
        traceStr(" size=");
        traceDec(size);
        traceStr(" pending=");
        traceDec(beforeCount);
        traceStr("->");
        traceDec(service->getQueue()->pendingCount());
        if (size >= sizeof(Event)) {
            traceInputEvent(*reinterpret_cast<const Event*>(payload));
        }
        traceStr("\n");
    }

    wakeQueueWaiters();
    return true;
}

uint64_t IPCManager::nextRequestID() {
    return requestCounter++;
}

bool IPCManager::beginRequest(uint64_t requestID, Process* owner) {
    if (!owner || requestID == 0) {
        return false;
    }

    for (size_t i = 0; i < MaxPendingRequests; i++) {
        if (!requests[i].used) {
            requests[i].used = true;
            requests[i].id = requestID;
            requests[i].ownerPID = owner->getPID();
            requests[i].completed = false;
            requests[i].responseSize = 0;
            memset(requests[i].response, 0, sizeof(requests[i].response));
            return true;
        }
    }

    return false;
}

bool IPCManager::completeRequest(uint64_t requestID, const void* data, uint64_t size) {
    if (requestID == 0 || size > MessageQueueObject::MaxPayloadSize) {
        return false;
    }

    for (size_t i = 0; i < MaxPendingRequests; i++) {
        if (requests[i].used && requests[i].id == requestID) {
            requests[i].completed = true;
            requests[i].responseSize = size;
            if (size > 0 && data) {
                memcpy(requests[i].response, data, static_cast<size_t>(size));
            }

            Process* owner = Scheduler::get().getProcessByPID(requests[i].ownerPID);
            if (owner && owner->getState() == ProcessState::Blocked) {
                owner->setState(ProcessState::Ready);
                Scheduler::get().wakeProcess(owner);
            }
            return true;
        }
    }

    return false;
}

bool IPCManager::waitForRequest(Process* owner, uint64_t requestID, void* data, uint64_t* size) {
    if (!owner || !size) {
        return false;
    }

    for (;;) {
        bool found = false;
        for (size_t i = 0; i < MaxPendingRequests; i++) {
            if (!requests[i].used || requests[i].id != requestID || requests[i].ownerPID != owner->getPID()) {
                continue;
            }

            found = true;
            if (!requests[i].completed) {
                owner->setState(ProcessState::Blocked);
                Scheduler::get().scheduleFromSyscall();
                break;
            }

            if (data && requests[i].responseSize > 0) {
                memcpy(data, requests[i].response, static_cast<size_t>(requests[i].responseSize));
            }
            *size = requests[i].responseSize;
            requests[i].used = false;
            return true;
        }

        if (!found) {
            return false;
        }
    }
}

void IPCManager::wakeQueueWaiters() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (current && current->getState() == ProcessState::Blocked) {
        current->setState(ProcessState::Ready);
        Scheduler::get().wakeProcess(current);
    }

    for (uint32_t pid = 1; pid < 4096; pid++) {
        Process* process = Scheduler::get().getProcessByPID(pid);
        if (!process || process->getState() != ProcessState::Blocked) {
            continue;
        }

        process->setState(ProcessState::Ready);
        Scheduler::get().wakeProcess(process);
    }
}

void IPCManager::cleanupProcess(Process* process) {
    if (!process) {
        return;
    }

    for (size_t i = 0; i < MaxSharedObjects; i++) {
        if (sharedObjects[i]) {
            sharedObjects[i]->unmapAllFrom(process);
        }
    }

    for (size_t i = 0; i < MaxServices; i++) {
        if (services[i].used && services[i].service &&
            services[i].service->getOwnerPID() == process->getPID()) {
            services[i].service->release();
            services[i].used = false;
            services[i].service = nullptr;
        }
    }

    for (size_t i = 0; i < MaxPendingRequests; i++) {
        if (requests[i].used && requests[i].ownerPID == process->getPID()) {
            requests[i].used = false;
        }
    }
}

void retainSharedMemoryHandle(void* object) {
    if (object) {
        reinterpret_cast<SharedMemoryObject*>(object)->retain();
    }
}

void releaseSharedMemoryHandle(void* object) {
    if (object) {
        reinterpret_cast<SharedMemoryObject*>(object)->release();
    }
}

void retainSurfaceHandle(void* object) {
    if (object) {
        reinterpret_cast<SurfaceObject*>(object)->retain();
    }
}

void releaseSurfaceHandle(void* object) {
    if (object) {
        reinterpret_cast<SurfaceObject*>(object)->release();
    }
}

void retainWindowHandle(void* object) {
    if (object) {
        reinterpret_cast<WindowObject*>(object)->retain();
    }
}

void releaseWindowHandle(void* object) {
    if (object) {
        reinterpret_cast<WindowObject*>(object)->release();
    }
}

void retainMessageQueueHandle(void* object) {
    if (object) {
        reinterpret_cast<MessageQueueObject*>(object)->retain();
    }
}

void releaseMessageQueueHandle(void* object) {
    if (object) {
        reinterpret_cast<MessageQueueObject*>(object)->release();
    }
}

void retainServiceHandle(void* object) {
    if (object) {
        reinterpret_cast<ServiceObject*>(object)->retain();
    }
}

void releaseServiceHandle(void* object) {
    if (object) {
        reinterpret_cast<ServiceObject*>(object)->release();
    }
}
