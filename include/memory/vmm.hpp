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

// OS-available PTE bit (AVL) marking a copy-on-write leaf page: the page is
// mapped read-only and shared (PMM refcount > 1); a write fault makes a private
// writable copy.  Distinct from bit 9 (kPrivateTable, used for intermediate
// table cloning in vmm.cpp) to avoid collisions.
static constexpr uint64_t kCowPage = 1ULL << 10;

// OS-available PTE bit (AVL, bit 11) marking a SHARED, kernel-owned leaf: an IPC
// shared-memory frame or the device framebuffer BAR. Such frames are mapped into
// possibly-many processes and are owned/freed by their manager (or are MMIO that
// must NEVER return to the PMM). They must be shared writable across fork (never
// copy-on-write'd) and MUST NOT be freed by address-space teardown -- otherwise
// every process that maps one double-frees it, corrupting the PMM (frames handed
// out while still in use -> reused -> arbitrary memory corruption).
static constexpr uint64_t kSharedFrame = 1ULL << 11;

struct PageTable {
    uint64_t entries[512];
} __attribute__((aligned(4096)));

class VMM {
public:
    static void Initialize();

    // Build a higher-half direct map (HHDM) of physical memory using 1 GiB
    // supervisor pages, installed in the shared kernel half of the master PML4
    // (so every address space inherits it). Kernel metadata reached through this
    // map is immune to being shadowed by a low-VA user mapping (e.g. a non-PIE
    // ET_EXEC image loaded at 0x400000): the same identity address in an active
    // user address space can point at that user's image, so a raw identity
    // access from the kernel would hit the wrong page. Returns the map's base
    // VA (0 on failure). Idempotent.
    static uint64_t InitDirectMap();
    static uint64_t DirectMapBase();                 // 0 until InitDirectMap succeeds
    static uint64_t PhysToVirt(uint64_t phys);       // phys -> direct-map VA (identity if no map)

    static void MapPage(uint64_t virtualAddr, uint64_t physAddr, uint64_t flags);
    static void UnmapPage(uint64_t virtualAddr);
    static void FreeAddressSpace(PageTable* pml4);
    static PageTable* AllocTable();

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
    static bool ProtectPageIn(PageTable* pml4, uint64_t virtualAddr, uint64_t flags);
    static bool ProtectRangeIn(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount, uint64_t flags);

    // Copy-on-write page-fault service.  Called from the page-fault handler on a
    // write to a present page.  If `faultAddr` lands on a kCowPage leaf, this
    // gives the faulting address space a private writable copy (or takes the
    // frame in place when it is the last referer) and returns true.  Returns
    // false if the address is not a COW page (caller handles it as a real fault).
    static bool HandleCowFault(PageTable* pml4, uint64_t faultAddr);
    // Whether `pml4` now permits the access a page fault with error code
    // `errCode` reported at `faultAddr`. True means the fault came from a stale
    // TLB entry (another CPU widened the mapping meanwhile): flush and retry.
    static bool PermitsFaultedAccess(PageTable* pml4, uint64_t faultAddr, uint64_t errCode);
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
    static uint64_t   s_directMapBase;   // higher-half direct map base (0 = none)

    static void       InvalidatePage(uint64_t addr);

    static constexpr uint64_t PML4Index(uint64_t addr) { return (addr >> 39) & 0x1FF; }
    static constexpr uint64_t PDPTIndex(uint64_t addr) { return (addr >> 30) & 0x1FF; }
    static constexpr uint64_t PDIndex(uint64_t addr)   { return (addr >> 21) & 0x1FF; }
    static constexpr uint64_t PTIndex(uint64_t addr)   { return (addr >> 12) & 0x1FF; }
};
