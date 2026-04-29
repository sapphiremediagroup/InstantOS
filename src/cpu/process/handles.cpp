#include <cpu/process/handles.hpp>

HandleTable::HandleTable() {
    for (int i = 0; i < MaxHandles; i++) {
        clearEntry(i);
    }
}

HandleTable::~HandleTable() {
    closeAll();
}

uint64_t HandleTable::allocate(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release) {
    if (type == HandleType::None || !object) {
        return static_cast<uint64_t>(-1);
    }

    for (int slot = FirstAllocHandle; slot < MaxHandles; slot++) {
        if (entries[slot].type == HandleType::None) {
            return allocateAt(encodeHandle(type, slot), type, rights, object, retain, release);
        }
    }

    return static_cast<uint64_t>(-1);
}

uint64_t HandleTable::allocateAt(uint64_t handle, HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release) {
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
    for (int slot = FirstAllocHandle; slot < MaxHandles; slot++) {
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

uint64_t HandleTable::encodeHandle(HandleType type, int slot) {
    if (type == HandleType::None || slot < FirstAllocHandle || slot >= MaxHandles) {
        return static_cast<uint64_t>(-1);
    }

    return (static_cast<uint64_t>(type) << TypeShift) | static_cast<uint64_t>(slot);
}

bool HandleTable::decodeHandle(uint64_t handle, HandleType* type, int* slot) {
    uint64_t rawType = handle >> TypeShift;
    uint64_t rawSlot = handle & SlotMask;
    if (rawType == static_cast<uint64_t>(HandleType::None) ||
        rawType > static_cast<uint64_t>(HandleType::Pipe) ||
        rawSlot < FirstAllocHandle ||
        rawSlot >= MaxHandles) {
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
}

void HandleTable::releaseEntry(HandleEntry& entry) {
    if (entry.release && entry.object) {
        entry.release(entry.object);
    }
}
