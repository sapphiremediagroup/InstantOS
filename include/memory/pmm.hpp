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
    // Refcount-aware: only actually released once the last reference drops.
    static void FreeFrame(uint64_t physAddr);

    // ── Reference counting (for copy-on-write / shared frames) ─────────────
    // Increment the share count of an already-allocated frame.
    static void IncRef(uint64_t physAddr);
    // Current reference count of a frame (0 if free/out of range).
    static uint16_t RefCount(uint64_t physAddr);

    // Allocate `count` contiguous 4 KiB physical frames.
    // Returns the physical base address, or 0 on failure.
    static uint64_t AllocFrames(uint64_t count);

    // Physical floor for kernel-internal allocations (heap arenas, page-table
    // frames) that the kernel dereferences through the LOW IDENTITY MAP. A
    // non-PIE (ET_EXEC) user image loads at 0x400000 and may span up to
    // MAX_USER_ELF_SIZE; while such an image is the active address space, it
    // shadows the identity VA of any physical frame inside that window, so a
    // kernel identity access there hits the user image instead. Keeping kernel
    // metadata above the window makes identity dereferences correct regardless
    // of which user address space is live. (256 MiB matches the exec loader's
    // MAX_USER_ELF_SIZE; 0x400000 is the conventional ET_EXEC base.)
    static constexpr uint64_t KERNEL_HIGH_ALLOC_MIN = 0x400000ULL + (256ULL * 1024 * 1024);

    // Like AllocFrames(), but only returns a contiguous block whose physical
    // base is >= minPhys. Returns 0 when no such block exists, so callers fall
    // back to AllocFrames() on small-memory configurations.
    static uint64_t AllocFramesAbove(uint64_t count, uint64_t minPhys);
    // Contiguous run that ends at or below maxPhys (e.g. real-mode memory for
    // the SMP startup trampoline). Returns 0 if none.
    static uint64_t AllocFramesBelow(uint64_t count, uint64_t maxPhys);

    // Free `count` contiguous 4 KiB frames starting at `physAddr`.
    static void FreeFrames(uint64_t physAddr, uint64_t count);

    // Mark a physical range as reserved so it cannot be allocated again.
    static void ReserveRange(uint64_t physAddr, uint64_t bytes);

    static void DumpReservations();

    // Repoint the bitmap + refcount metadata pointers from their raw physical
    // (identity) addresses to aliases in the higher-half direct map at `base`.
    // After this, PMM bookkeeping is reached through the shared kernel half and
    // can no longer be shadowed by a low-VA user mapping when a user address
    // space is active (see VMM::InitDirectMap). Call once, after the direct map
    // is built and before any user process runs. No-op if base is 0.
    static void RelocateToDirectMap(uint64_t base);

    // ── Queries ──────────────────────────────────────────────────────────
    static uint64_t TotalFrames();         // Total tracked frames
    static uint64_t UsedFrames();          // Currently allocated frames
    static uint64_t FreeFrameCount();      // Currently free frames
    static uint64_t UsableFrames();        // Frames originally available for allocation
    static uint64_t UsedUsableFrames();    // Usable frames currently not free

    static uint64_t TotalMemory();         // Total tracked bytes
    static uint64_t FreeMemory();          // Free bytes
    static uint64_t UsedMemory();          // Used bytes
    static uint64_t UsableMemory();        // Usable physical RAM bytes
    static uint64_t UsedUsableMemory();    // Used usable physical RAM bytes

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
    static uint64_t  s_usableFrames;    // Frames from bootloader Free regions
    static bool      s_initialized;

    // Per-frame reference count (parallel to the bitmap, placed right after it
    // in the same reserved metadata block).  1 for a freshly allocated frame;
    // >1 when a frame is shared (copy-on-write).  Frame is released only when
    // this reaches 0.
    static uint16_t* s_refcount;

    // Next-fit allocation cursor so single-frame allocation does not rescan
    // from the front of the bitmap on every call (O(n) → amortized fast).
    static uint64_t  s_allocCursor;

    // Internal helpers
    static void SetFrame(uint64_t frame);
    static void ClearFrame(uint64_t frame);
    static bool TestFrame(uint64_t frame);

    // Find the first free frame starting at `startFrame`.
    static uint64_t FindFirstFree(uint64_t startFrame = 0);

    // Find `count` contiguous free frames.
    static uint64_t FindContiguous(uint64_t count);
};
