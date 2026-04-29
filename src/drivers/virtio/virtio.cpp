#include <drivers/virtio/virtio.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <common/string.hpp>

namespace {
constexpr uint16_t kInvalidDesc = 0xFFFF;

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static void queue_barrier() {
    asm volatile("" ::: "memory");
}
}

Virtqueue::Virtqueue()
    : queueSize(0),
      lastUsedIdx(0),
      numFree(0),
      desc(nullptr),
      avail(nullptr),
      used(nullptr),
      descPages(0),
      availPages(0),
      usedPages(0),
      freeList(nullptr) {
}

Virtqueue::~Virtqueue() {
    reset();
}

bool Virtqueue::init(uint16_t requestedQueueSize) {
    reset();

    if (requestedQueueSize == 0) {
        return false;
    }

    queueSize = requestedQueueSize;

    const uint64_t descBytes = align_up(sizeof(VirtqDesc) * queueSize, 16);
    const uint64_t availBytes = align_up(sizeof(uint16_t) * 2 + sizeof(uint16_t) * queueSize, 16);
    const uint64_t usedBytes = align_up(sizeof(uint16_t) * 2 + sizeof(VirtqUsedElem) * queueSize, 16);

    descPages = (descBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    availPages = (availBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;
    usedPages = (usedBytes + PMM::PAGE_SIZE - 1) / PMM::PAGE_SIZE;

    desc = reinterpret_cast<VirtqDesc*>(PMM::AllocFrames(descPages));
    avail = reinterpret_cast<VirtqAvail*>(PMM::AllocFrames(availPages));
    used = reinterpret_cast<VirtqUsed*>(PMM::AllocFrames(usedPages));
    freeList = reinterpret_cast<uint16_t*>(kmalloc(sizeof(uint16_t) * queueSize));

    if (!desc || !avail || !used || !freeList) {
        reset();
        return false;
    }

    memset(desc, 0, descBytes);
    memset(avail, 0, availBytes);
    memset(used, 0, usedBytes);

    for (uint16_t i = 0; i < queueSize; ++i) {
        freeList[i] = i;
    }

    numFree = queueSize;
    lastUsedIdx = 0;
    return true;
}

void Virtqueue::reset() {
    if (desc) {
        PMM::FreeFrames(reinterpret_cast<uint64_t>(desc), descPages);
    }
    if (avail) {
        PMM::FreeFrames(reinterpret_cast<uint64_t>(avail), availPages);
    }
    if (used) {
        PMM::FreeFrames(reinterpret_cast<uint64_t>(used), usedPages);
    }
    if (freeList) {
        kfree(freeList);
    }

    queueSize = 0;
    lastUsedIdx = 0;
    numFree = 0;
    desc = nullptr;
    avail = nullptr;
    used = nullptr;
    descPages = 0;
    availPages = 0;
    usedPages = 0;
    freeList = nullptr;
}

int Virtqueue::allocDesc() {
    if (!freeList || numFree == 0) {
        return -1;
    }

    return freeList[--numFree];
}

void Virtqueue::freeDesc(int idx) {
    if (!freeList || idx < 0 || idx >= queueSize || numFree >= queueSize) {
        return;
    }

    desc[idx].addr = 0;
    desc[idx].len = 0;
    desc[idx].flags = 0;
    desc[idx].next = kInvalidDesc;
    freeList[numFree++] = static_cast<uint16_t>(idx);
}

void Virtqueue::setDesc(int idx, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    if (!desc || idx < 0 || idx >= queueSize) {
        return;
    }

    desc[idx].addr = addr;
    desc[idx].len = len;
    desc[idx].flags = flags;
    desc[idx].next = next;
}

void Virtqueue::addAvail(uint16_t descIdx) {
    if (!avail || queueSize == 0) {
        return;
    }

    const uint16_t idx = avail->idx;
    avail->ring[idx % queueSize] = descIdx;
    queue_barrier();
    avail->idx = idx + 1;
    queue_barrier();
}

bool Virtqueue::hasUsed() {
    if (!used) {
        return false;
    }

    queue_barrier();
    return used->idx != lastUsedIdx;
}

uint32_t Virtqueue::getUsed(uint32_t* len) {
    if (!used || queueSize == 0 || !hasUsed()) {
        if (len) {
            *len = 0;
        }
        return 0xFFFFFFFFU;
    }

    const VirtqUsedElem elem = used->ring[lastUsedIdx % queueSize];
    lastUsedIdx++;
    if (len) {
        *len = elem.len;
    }
    return elem.id;
}
