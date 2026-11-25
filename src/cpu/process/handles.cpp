#include <cpu/process/handles.hpp>
#include <memory/heap.hpp>
#include <common/string.hpp>

HandleTable::HandleTable() {
}

HandleTable::~HandleTable() {
    closeAll();
    for (int c = 0; c < MaxChunks; c++) {
        if (chunks[c]) {
            kfree(chunks[c]);
            chunks[c] = nullptr;
        }
    }
}

HandleEntry* HandleTable::slotEntry(int slot) const {
    if (!isValidSlot(slot)) {
        return nullptr;
    }
    HandleEntry* chunk = chunks[slot / ChunkSlots];
    return chunk ? &chunk[slot % ChunkSlots] : nullptr;
}

HandleEntry* HandleTable::makeSlot(int slot) {
    if (!isValidSlot(slot)) {
        return nullptr;
    }
    HandleEntry*& chunk = chunks[slot / ChunkSlots];
    if (!chunk) {
        auto* fresh = static_cast<HandleEntry*>(kmalloc(sizeof(HandleEntry) * ChunkSlots));
        if (!fresh) {
            return nullptr;
        }
        memset(fresh, 0, sizeof(HandleEntry) * ChunkSlots);   // HandleType::None, no object
        chunk = fresh;
    }
    return &chunk[slot % ChunkSlots];
}

uint64_t HandleTable::allocate(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec) {
    if (type == HandleType::None || !object) {
        return static_cast<uint64_t>(-1);
    }

    // The lowest free slot, from the first that may be free.
    int slot = searchFrom < FirstAllocHandle ? FirstAllocHandle : searchFrom;
    while (slot < MaxHandles) {
        HandleEntry* chunk = chunks[slot / ChunkSlots];
        if (!chunk) {
            break;   // a chunk not made yet: its first slot is free
        }
        if (chunk[slot % ChunkSlots].type == HandleType::None) {
            break;
        }
        slot++;
    }
    if (slot >= MaxHandles || !makeSlot(slot)) {
        return static_cast<uint64_t>(-1);
    }
    searchFrom = slot + 1;
    return allocateAt(encodeHandle(type, slot), type, rights, object, retain, release, closeOnExec);
}

uint64_t HandleTable::allocateAt(uint64_t handle, HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec) {
    HandleType handleType = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &handleType, &slot) || handleType != type || type == HandleType::None || !object) {
        return static_cast<uint64_t>(-1);
    }
    HandleEntry* entry = makeSlot(slot);
    if (!entry) {
        return static_cast<uint64_t>(-1);
    }

    if (entry->type != HandleType::None) {
        close(encodeHandle(entry->type, slot));
    }

    entry->type = type;
    entry->rights = rights;
    entry->object = object;
    entry->retain = retain;
    entry->release = release;
    entry->closeOnExec = closeOnExec;
    open++;

    return handle;
}

bool HandleTable::close(uint64_t handle) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot)) {
        return false;
    }
    HandleEntry* entry = slotEntry(slot);
    if (!entry || entry->type == HandleType::None || entry->type != type) {
        return false;
    }

    // Cleared before the release runs: it may open or close other handles.
    HandleEntry old = *entry;
    clearEntry(slot);
    releaseEntry(old);
    return true;
}

bool HandleTable::close(uint64_t handle, HandleType expectedType) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot) || type != expectedType) {
        return false;
    }
    HandleEntry* entry = slotEntry(slot);
    if (!entry || entry->type != expectedType) {
        return false;
    }

    return close(handle);
}

void HandleTable::closeAll() {
    for (int c = 0; c < MaxChunks; c++) {
        if (!chunks[c]) continue;
        for (int i = 0; i < ChunkSlots; i++) {
            const HandleEntry& entry = chunks[c][i];
            if (entry.type != HandleType::None) {
                close(encodeHandle(entry.type, c * ChunkSlots + i));
            }
        }
    }
}

void HandleTable::closeAllFiles() {
    for (int c = 0; c < MaxChunks; c++) {
        if (!chunks[c]) continue;
        for (int i = 0; i < ChunkSlots; i++) {
            if (chunks[c][i].type == HandleType::File) {
                close(encodeHandle(HandleType::File, c * ChunkSlots + i));
            }
        }
    }
}

HandleEntry* HandleTable::get(uint64_t handle) {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot)) {
        return nullptr;
    }
    HandleEntry* entry = slotEntry(slot);
    if (!entry || entry->type == HandleType::None || entry->type != type) {
        return nullptr;
    }
    return entry;
}

const HandleEntry* HandleTable::get(uint64_t handle) const {
    HandleType type = HandleType::None;
    int slot = -1;
    if (!decodeHandle(handle, &type, &slot)) {
        return nullptr;
    }
    const HandleEntry* entry = slotEntry(slot);
    if (!entry || entry->type == HandleType::None || entry->type != type) {
        return nullptr;
    }
    return entry;
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
    const HandleEntry copy = *entry;

    if (copy.retain) {
        copy.retain(copy.object);
    }

    uint64_t newHandle = allocate(copy.type, copy.rights, copy.object, copy.retain, copy.release);
    if (newHandle == static_cast<uint64_t>(-1) && copy.release) {
        copy.release(copy.object);
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
    const HandleEntry copy = *entry;
    HandleEntry* target = makeSlot(newSlot);
    if (!target) {
        return false;
    }

    if (copy.retain) {
        copy.retain(copy.object);
    }

    if (target->type != HandleType::None) {
        close(encodeHandle(target->type, newSlot));
    }

    target->type = copy.type;
    target->rights = copy.rights;
    target->object = copy.object;
    target->retain = copy.retain;
    target->release = copy.release;
    target->closeOnExec = false;
    open++;
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

    const HandleEntry* target = slotEntry(newSlot);
    if (target && target->type != HandleType::None && target->type != expectedType) {
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
    for (int c = 0; c < MaxChunks; c++) {
        if (!chunks[c]) continue;
        for (int i = 0; i < ChunkSlots; i++) {
            const HandleEntry& entry = chunks[c][i];
            if (entry.type != HandleType::None && entry.closeOnExec) {
                close(encodeHandle(entry.type, c * ChunkSlots + i));
            }
        }
    }
}

void HandleTable::cloneFrom(const HandleTable& source, bool skipCloseOnExec, bool filesOnly) {
    nofileSoft = source.nofileSoft;
    nofileHard = source.nofileHard;
    for (int c = 0; c < MaxChunks; c++) {
        if (!source.chunks[c]) continue;
        for (int i = 0; i < ChunkSlots; i++) {
            const HandleEntry& src = source.chunks[c][i];
            const int slot = c * ChunkSlots + i;
            if (src.type == HandleType::None || !src.object) {
                continue;
            }
            if (skipCloseOnExec && src.closeOnExec) {
                continue;
            }
            if (filesOnly && src.type != HandleType::File && src.type != HandleType::Pipe &&
                src.type != HandleType::Socket) {
                continue;
            }
            HandleEntry* dst = makeSlot(slot);
            if (!dst) {
                continue;   // out of memory: the child goes without it
            }

            // Bump the underlying object's refcount so both tables share it.
            if (src.retain) {
                src.retain(src.object);
            }

            if (dst->type != HandleType::None) {
                close(encodeHandle(dst->type, slot));
            }
            dst->type = src.type;
            dst->rights = src.rights;
            dst->object = src.object;
            dst->retain = src.retain;
            dst->release = src.release;
            dst->closeOnExec = src.closeOnExec;
            open++;
        }
    }
}

uint64_t HandleTable::encodeHandle(HandleType type, int slot) {
    if (type == HandleType::None || slot < 0 || slot >= MaxHandles) {
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
    HandleEntry* entry = slotEntry(slot);
    if (!entry) {
        return;
    }
    if (entry->type != HandleType::None) {
        open--;
        if (slot < searchFrom && slot >= FirstAllocHandle) {
            searchFrom = slot;
        }
    }
    entry->type = HandleType::None;
    entry->rights = HandleRightNone;
    entry->object = nullptr;
    entry->retain = nullptr;
    entry->release = nullptr;
    entry->closeOnExec = false;
}

void HandleTable::releaseEntry(HandleEntry& entry) {
    if (entry.release && entry.object) {
        entry.release(entry.object);
    }
}
