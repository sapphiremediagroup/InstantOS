#pragma once

#include <iboot/memory.hpp>
#include <stdint.h>

// ── Physical Memory Manager ───────────────────────────────────────────────
// Bitmap-based allocator for 4 KiB physical frames.
// Initialized from the BootInfo memory map passed by iBoot.
//
// Usage:
//   PMM::Initialize(bootInfo->memoryMap);
//   uint64_t frame = PMM::AllocFrame();           // single 4 KiB page
//   PMM::FreeFrame(frame);
//   uint64_t block = PMM::AllocFrames(16);         // 16 contiguous pages
//   PMM::FreeFrames(block, 16);

class PMM {
public:
    static constexpr uint64_t PAGE_SIZE = 4096;

    // Initialize the PMM from the bootloader-provided memory map.
    // Must be called exactly once, early in kernel startup.
    static void Initialize(const MemoryMap& map, uint64_t kernelBase, uint64_t kernelSize);

    // Allocate a single 4 KiB physical frame.
    // Returns the physical address, or 0 on failure.
    static uint64_t AllocFrame();

    // Free a previously allocated 4 KiB physical frame.
    static void FreeFrame(uint64_t physAddr);

    // Allocate `count` contiguous 4 KiB physical frames.
    // Returns the physical base address, or 0 on failure.
    static uint64_t AllocFrames(uint64_t count);

    // Free `count` contiguous 4 KiB frames starting at `physAddr`.
    static void FreeFrames(uint64_t physAddr, uint64_t count);

    // Mark a physical range as reserved so it cannot be allocated again.
    static void ReserveRange(uint64_t physAddr, uint64_t bytes);

    static void DumpReservations();

    // ── Queries ──────────────────────────────────────────────────────────
    static uint64_t TotalFrames();         // Total tracked frames
    static uint64_t UsedFrames();          // Currently allocated frames
    static uint64_t FreeFrameCount();      // Currently free frames

    static uint64_t TotalMemory();         // Total tracked bytes
    static uint64_t FreeMemory();          // Free bytes
    static uint64_t UsedMemory();          // Used bytes

    static bool IsInitialized();

    // ── Prevent instantiation ────────────────────────────────────────────
    PMM()                        = delete;
    PMM(const PMM&)              = delete;
    PMM& operator=(const PMM&)   = delete;

private:
    // Bitmap: 1 bit per 4 KiB frame.  bit = 0 → free, bit = 1 → used.
    static uint64_t* s_bitmap;
    static uint64_t  s_bitmapSize;      // Number of uint64_t entries
    static uint64_t  s_totalFrames;     // Highest frame tracked
    static uint64_t  s_usedFrames;      // Number of frames currently marked used
    static bool      s_initialized;

    // Internal helpers
    static void SetFrame(uint64_t frame);
    static void ClearFrame(uint64_t frame);
    static bool TestFrame(uint64_t frame);

    // Find the first free frame starting at `startFrame`.
    static uint64_t FindFirstFree(uint64_t startFrame = 0);

    // Find `count` contiguous free frames.
    static uint64_t FindContiguous(uint64_t count);
};
