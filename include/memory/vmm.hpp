#pragma once

#include <stdint.h>

static constexpr uint64_t PAGE_SIZE = 4096;

enum PageFlags : uint64_t {
    Present    = 1ULL << 0,
    ReadWrite  = 1ULL << 1,
    UserSuper  = 1ULL << 2,
    WriteThru  = 1ULL << 3,
    CacheDisab = 1ULL << 4,
    Accessed   = 1ULL << 5,
    Dirty      = 1ULL << 6,
    LargePage  = 1ULL << 7,
    Global     = 1ULL << 8,
    NoExecute  = 1ULL << 63,
};

static constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;

struct PageTable {
    uint64_t entries[512];
} __attribute__((aligned(4096)));

class VMM {
public:
    static void Initialize();

    static void MapPage(uint64_t virtualAddr, uint64_t physAddr, uint64_t flags);
    static void UnmapPage(uint64_t virtualAddr);
    static void FreeAddressSpace(PageTable* pml4);

    static uint64_t VirtualToPhysical(uint64_t virtualAddr);
    static uint64_t VirtualToPhysicalIn(PageTable* pml4, uint64_t virtualAddr);
    static bool     IsMapped(uint64_t virtualAddr);
    static bool     IsUserMapped(uint64_t virtualAddr);

    static void MapRange(uint64_t virtualBase, uint64_t physBase,
                         uint64_t pageCount, uint64_t flags);
    static void UnmapRange(uint64_t virtualBase, uint64_t pageCount);

    static void MapPageInto(PageTable* pml4, uint64_t virtualAddr, uint64_t physAddr, uint64_t flags);
    static void MapRangeInto(PageTable* pml4, uint64_t virtualBase, uint64_t physBase,
                             uint64_t pageCount, uint64_t flags);
    static void UnmapPageFrom(PageTable* pml4, uint64_t virtualAddr);
    static void UnmapRangeFrom(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount);

    static void SetAddressSpace(PageTable* pml4);
    static PageTable* GetAddressSpace();
    static PageTable* GetKernelAddressSpace();

    static bool IsInitialized();

    VMM()                        = delete;
    VMM(const VMM&)              = delete;
    VMM& operator=(const VMM&)   = delete;

private:
    static PageTable* s_pml4;
    static bool       s_initialized;

    static PageTable* AllocTable();
    static void       InvalidatePage(uint64_t addr);

    static constexpr uint64_t PML4Index(uint64_t addr) { return (addr >> 39) & 0x1FF; }
    static constexpr uint64_t PDPTIndex(uint64_t addr) { return (addr >> 30) & 0x1FF; }
    static constexpr uint64_t PDIndex(uint64_t addr)   { return (addr >> 21) & 0x1FF; }
    static constexpr uint64_t PTIndex(uint64_t addr)   { return (addr >> 12) & 0x1FF; }
};
