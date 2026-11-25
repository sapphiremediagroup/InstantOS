#include <memory/shared_pages.hpp>
#include <common/string.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>

namespace {
constexpr int kENOMEM = 12, kEFBIG = 27, kENOSPC = 28;

uint8_t* frameBytes(uint64_t frame) {
    return reinterpret_cast<uint8_t*>(VMM::PhysToVirt(frame));
}
}

SharedPages* SharedPages::create(uint64_t size) {
    if (size > kMaxSize) {
        return nullptr;
    }
    auto* object = new SharedPages();
    if (object) {
        object->bytes = size;
    }
    return object;
}

SharedPages::~SharedPages() {
    for (uint64_t c = 0; c < chunkCount; c++) {
        if (!chunks[c]) continue;
        for (uint64_t i = 0; i < kPerChunk; i++) {
            if (chunks[c][i]) PMM::FreeFrame(chunks[c][i]);
        }
        kfree(chunks[c]);
    }
    if (chunks) kfree(chunks);
}

void SharedPages::destroyIfUnused() {
    if (fileRefs == 0 && mappings == 0) {
        delete this;
    }
}

void SharedPages::releaseFile() {
    fileRefs--;
    destroyIfUnused();
}

void SharedPages::release() {
    mappings--;
    destroyIfUnused();
}

uint64_t* SharedPages::slot(uint64_t index, bool make) {
    const uint64_t c = index / kPerChunk;
    if (c >= chunkCount) {
        if (!make) return nullptr;
        uint64_t count = chunkCount ? chunkCount * 2 : 4;
        if (count <= c) count = c + 1;
        auto** grown = reinterpret_cast<uint64_t**>(kmalloc(count * sizeof(uint64_t*)));
        if (!grown) return nullptr;
        for (uint64_t i = 0; i < count; i++) {
            grown[i] = i < chunkCount ? chunks[i] : nullptr;
        }
        if (chunks) kfree(chunks);
        chunks = grown;
        chunkCount = count;
    }
    if (!chunks[c]) {
        if (!make) return nullptr;
        auto* chunk = reinterpret_cast<uint64_t*>(kmalloc(kPerChunk * sizeof(uint64_t)));
        if (!chunk) return nullptr;
        memset(chunk, 0, kPerChunk * sizeof(uint64_t));
        chunks[c] = chunk;
    }
    return &chunks[c][index % kPerChunk];
}

uint64_t SharedPages::faultPage(uint64_t offset) {
    if (offset >= kMaxSize) {
        return 0;
    }
    uint64_t* frame = slot(offset / kPage, true);
    if (!frame) {
        return 0;
    }
    if (!*frame) {
        const uint64_t fresh = PMM::AllocFrame();
        if (!fresh) {
            return 0;
        }
        memset(frameBytes(fresh), 0, kPage);
        *frame = fresh;
        frames++;
    }
    return *frame;
}

int64_t SharedPages::read(void* buffer, uint64_t count, uint64_t offset) {
    if (offset >= bytes) {
        return 0;
    }
    if (count > bytes - offset) {
        count = bytes - offset;
    }
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint64_t done = 0; done < count;) {
        const uint64_t within = (offset + done) % kPage;
        uint64_t n = kPage - within;
        if (n > count - done) n = count - done;
        const uint64_t* frame = slot((offset + done) / kPage, false);
        if (frame && *frame) {
            memcpy(out + done, frameBytes(*frame) + within, n);
        } else {
            memset(out + done, 0, n);
        }
        done += n;
    }
    return static_cast<int64_t>(count);
}

int64_t SharedPages::write(const void* buffer, uint64_t count, uint64_t offset) {
    if (offset >= kMaxSize || count > kMaxSize - offset) {
        return -kEFBIG;
    }
    const auto* in = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < count) {
        const uint64_t within = (offset + done) % kPage;
        uint64_t n = kPage - within;
        if (n > count - done) n = count - done;
        const uint64_t frame = faultPage(offset + done);
        if (!frame) break;
        memcpy(frameBytes(frame) + within, in + done, n);
        done += n;
    }
    if (done == 0 && count != 0) {
        return -kENOSPC;
    }
    if (offset + done > bytes) {
        bytes = offset + done;
    }
    return static_cast<int64_t>(done);
}

int SharedPages::resize(uint64_t size) {
    if (size > kMaxSize) {
        return -kEFBIG;
    }
    if (size < bytes) {
        // Growing again must find zeros.
        if (const uint64_t within = size % kPage) {
            const uint64_t* frame = slot(size / kPage, false);
            if (frame && *frame) {
                memset(frameBytes(*frame) + within, 0, kPage - within);
            }
        }
        const uint64_t first = (size + kPage - 1) / kPage;
        for (uint64_t c = first / kPerChunk; c < chunkCount; c++) {
            if (!chunks[c]) continue;
            for (uint64_t i = 0; i < kPerChunk; i++) {
                if (!chunks[c][i] || c * kPerChunk + i < first) continue;
                if (mappings != 0) {
                    memset(frameBytes(chunks[c][i]), 0, kPage);
                } else {
                    PMM::FreeFrame(chunks[c][i]);
                    chunks[c][i] = 0;
                    frames--;
                }
            }
        }
    }
    bytes = size;
    return 0;
}
