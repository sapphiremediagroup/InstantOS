#include <cpu/process/handles.hpp>

HandleTable::HandleTable() {
    for (int i = 0; i < MaxHandles; i++) {
        clearEntry(i);
    }
}

HandleTable::~HandleTable() {
    closeAll();
}

uint64_t HandleTable::allocate(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec) {
    if (type == HandleType::None || !object) {
        return static_cast<uint64_t>(-1);
    }

    for (int slot = FirstAllocHandle; slot < MaxHandles; slot++) {
        if (entries[slot].type == HandleType::None) {
            return allocateAt(encodeHandle(type, slot), type, rights, object, retain, release, closeOnExec);
        }
    }

    return static_cast<uint64_t>(-1);
}

uint64_t HandleTable::allocateAt(uint64_t handle, HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec) {
    HandleType handleType = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &handleType, &slot) || handleType != type || type == HandleType::None || !object) {
        return static_cast<uint64_t>(-1);
    }

    if (entries[slot].type != HandleType::None) {
        close(handle);
    }

    entries[slot].type = type;
    entries[slot].rights = rights;
    entries[slot].object = object;
    entries[slot].retain = retain;
    entries[slot].release = release;
    entries[slot].closeOnExec = closeOnExec;

    return handle;
}

bool HandleTable::close(uint64_t handle) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot) || entries[slot].type == HandleType::None || entries[slot].type != type) {
        return false;
    }

    releaseEntry(entries[slot]);
    clearEntry(slot);
    return true;
}

bool HandleTable::close(uint64_t handle, HandleType expectedType) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot) || type != expectedType || entries[slot].type != expectedType) {
        return false;
    }

    return close(handle);
}

void HandleTable::closeAll() {
    for (int slot = 0; slot < MaxHandles; slot++) {
        if (entries[slot].type != HandleType::None) {
            close(encodeHandle(entries[slot].type, slot));
        }
    }
}

HandleEntry* HandleTable::get(uint64_t handle) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot) || entries[slot].type == HandleType::None || entries[slot].type != type) {
        return nullptr;
    }
    return &entries[slot];
}

const HandleEntry* HandleTable::get(uint64_t handle) const {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot) || entries[slot].type == HandleType::None || entries[slot].type != type) {
        return nullptr;
    }
    return &entries[slot];
}

void* HandleTable::getObject(uint64_t handle, HandleType expectedType, uint32_t requiredRights) {
    HandleEntry* entry = get(handle);
    if (!entry || entry->type != expectedType) {
        return nullptr;
    }

    if ((entry->rights & requiredRights) != requiredRights) {
        return nullptr;
    }

    return entry->object;
}

uint64_t HandleTable::duplicate(uint64_t handle) {
    HandleEntry* entry = get(handle);
    if (!entry || !(entry->rights & HandleRightDuplicate)) {
        return static_cast<uint64_t>(-1);
    }

    if (entry->retain) {
        entry->retain(entry->object);
    }

    uint64_t newHandle = allocate(entry->type, entry->rights, entry->object, entry->retain, entry->release);
    if (newHandle == static_cast<uint64_t>(-1) && entry->release) {
        entry->release(entry->object);
    }

    return newHandle;
}

uint64_t HandleTable::duplicate(uint64_t handle, HandleType expectedType) {
    HandleEntry* entry = get(handle);
    if (!entry || entry->type != expectedType) {
        return static_cast<uint64_t>(-1);
    }

    return duplicate(handle);
}

bool HandleTable::duplicateTo(uint64_t oldHandle, uint64_t newHandle) {
    HandleEntry* entry = get(oldHandle);
    if (!entry || !(entry->rights & HandleRightDuplicate)) {
        return false;
    }

    if (oldHandle == newHandle) {
        return true;
    }

    HandleType newType = HandleType::None;
    int newSlot = -1;
    if (!decodeHandle(newHandle, &newType, &newSlot) || newType != entry->type) {
        return false;
    }

    if (entry->retain) {
        entry->retain(entry->object);
    }

    if (entries[newSlot].type != HandleType::None) {
        close(newHandle);
    }

    entries[newSlot].type = entry->type;
    entries[newSlot].rights = entry->rights;
    entries[newSlot].object = entry->object;
    entries[newSlot].retain = entry->retain;
    entries[newSlot].release = entry->release;
    entries[newSlot].closeOnExec = false;
    return true;
}

bool HandleTable::duplicateTo(uint64_t oldHandle, uint64_t newHandle, HandleType expectedType) {
    HandleEntry* entry = get(oldHandle);
    if (!entry || entry->type != expectedType) {
        return false;
    }

    HandleType newType = HandleType::None;
    int newSlot = -1;
    if (!decodeHandle(newHandle, &newType, &newSlot) || newType != expectedType) {
        return false;
    }

    if (entries[newSlot].type != HandleType::None && entries[newSlot].type != expectedType) {
        return false;
    }

    return duplicateTo(oldHandle, newHandle);
}

bool HandleTable::getCloseOnExec(uint64_t handle, bool* enabled) const {
    const HandleEntry* entry = get(handle);
    if (!entry || !enabled) {
        return false;
    }

    *enabled = entry->closeOnExec;
    return true;
}

bool HandleTable::setCloseOnExec(uint64_t handle, bool enabled) {
    HandleEntry* entry = get(handle);
    if (!entry) {
        return false;
    }

    entry->closeOnExec = enabled;
    return true;
}

void HandleTable::closeOnExecHandles() {
    for (int slot = 0; slot < MaxHandles; slot++) {
        if (entries[slot].type != HandleType::None && entries[slot].closeOnExec) {
            close(encodeHandle(entries[slot].type, slot));
        }
    }
}

void HandleTable::cloneFrom(const HandleTable& source, bool skipCloseOnExec) {
    for (int slot = 0; slot < MaxHandles; slot++) {
        const HandleEntry& src = source.entries[slot];
        if (src.type == HandleType::None || !src.object) {
            continue;
        }
        if (skipCloseOnExec && src.closeOnExec) {
            continue;
        }

        // Bump the underlying object's refcount so both tables share it.
        if (src.retain) {
            src.retain(src.object);
        }

        if (entries[slot].type != HandleType::None) {
            releaseEntry(entries[slot]);
            clearEntry(slot);
        }
        entries[slot].type = src.type;
        entries[slot].rights = src.rights;
        entries[slot].object = src.object;
        entries[slot].retain = src.retain;
        entries[slot].release = src.release;
        entries[slot].closeOnExec = src.closeOnExec;
    }
}

uint64_t HandleTable::encodeHandle(HandleType type, int slot) {
    if (type == HandleType::None || slot >= MaxHandles) {
        return static_cast<uint64_t>(-1);
    }
    // File handles may occupy the reserved stdio slots 0/1/2 so that POSIX
    // stdin/stdout/stderr are first-class, inheritable file handles. All other
    // handle types still start at FirstAllocHandle.
    const int minSlot = (type == HandleType::File) ? 0 : FirstAllocHandle;
    if (slot < minSlot) {
        return static_cast<uint64_t>(-1);
    }

    return (static_cast<uint64_t>(type) << TypeShift) | static_cast<uint64_t>(slot);
}

bool HandleTable::decodeHandle(uint64_t handle, HandleType* type, int* slot) {
    uint64_t rawType = handle >> TypeShift;
    uint64_t rawSlot = handle & SlotMask;
    if (rawType == static_cast<uint64_t>(HandleType::None) ||
        rawType > static_cast<uint64_t>(HandleType::Socket) ||
        rawSlot >= MaxHandles) {
        return false;
    }
    const uint64_t minSlot = (rawType == static_cast<uint64_t>(HandleType::File))
        ? 0
        : static_cast<uint64_t>(FirstAllocHandle);
    if (rawSlot < minSlot) {
        return false;
    }

    if (type) {
        *type = static_cast<HandleType>(rawType);
    }
    if (slot) {
        *slot = static_cast<int>(rawSlot);
    }
    return true;
}

bool HandleTable::isValidSlot(int slot) const {
    return slot >= 0 && slot < MaxHandles;
}

void HandleTable::clearEntry(int slot) {
    entries[slot].type = HandleType::None;
    entries[slot].rights = HandleRightNone;
    entries[slot].object = nullptr;
    entries[slot].retain = nullptr;
    entries[slot].release = nullptr;
    entries[slot].closeOnExec = false;
}

void HandleTable::releaseEntry(HandleEntry& entry) {
    if (entry.release && entry.object) {
        entry.release(entry.object);
    }
}
