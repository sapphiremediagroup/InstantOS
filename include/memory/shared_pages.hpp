#pragma once

#include <memory/mapped_object.hpp>
#include <stdint.h>

// Memory several processes map at once: page frames by index, made and zeroed
// as they are first touched. What is written through any mapping, or through
// write() on a file that holds it, is seen by all. It backs RamFS files (so
// /tmp and /dev/shm, shm_open's), memfds, and MAP_SHARED|MAP_ANONYMOUS. Its
// pages are mapped kSharedFrame, which also makes futexes in them work between
// processes. Under the big kernel lock.
//
// Two kinds of references keep it alive: files (retainFile) and mappings
// (MappedObject's). While something maps it no frame is given back, so a
// mapping never loses a page: truncating zeroes instead, and a page past the
// end reads as zeros (Linux raises SIGBUS there).
class SharedPages final : public MappedObject {
public:
    static constexpr uint64_t kMaxSize = 64ULL << 30;

    // With one file reference; nullptr if memory is short.
    static SharedPages* create(uint64_t size = 0);

    void retainFile() { fileRefs++; }
    void releaseFile();
    void retain() override { mappings++; }
    void release() override;

    // The frame of the page at `offset`, made if there is none; 0 if memory is
    // short or the offset is out of range.
    uint64_t faultPage(uint64_t offset) override;

    uint64_t size() const { return bytes; }
    bool mapped() const { return mappings != 0; }
    uint64_t frameCount() const { return frames; }

    int64_t read(void* buffer, uint64_t count, uint64_t offset);
    // Grows it; bytes written, or -errno.
    int64_t write(const void* buffer, uint64_t count, uint64_t offset);
    int resize(uint64_t size);   // 0 or -errno

    // memfd seals (F_SEAL_*).
    uint32_t seals = 0;

private:
    static constexpr uint64_t kPage = 4096;
    static constexpr uint64_t kPerChunk = 512;   // frames in a chunk: 2 MiB

    SharedPages() = default;
    ~SharedPages();
    void destroyIfUnused();
    uint64_t* slot(uint64_t index, bool make);

    uint64_t bytes = 0;
    uint64_t frames = 0;
    uint32_t fileRefs = 1;
    uint32_t mappings = 0;
    uint64_t** chunks = nullptr;   // each kPerChunk frame addresses, or nullptr
    uint64_t chunkCount = 0;
};
