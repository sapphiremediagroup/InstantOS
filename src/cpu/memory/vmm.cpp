#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <graphics/console.hpp>
#include <cpu/tlb.hpp>

PageTable* VMM::s_pml4        = nullptr;
bool       VMM::s_initialized = false;
uint64_t   VMM::s_directMapBase = 0;

// Returns true if `frame` is the kernel master page table (s_pml4) itself or
// one of its directly-referenced sub-tables (PDPT/PD/PT). Used by free_table()
// to avoid returning a still-kernel-referenced frame to the PMM (see the note
// there). Walks two levels, which covers the shallow-copied tables in practice.
static bool frameIsKernelTable(uint64_t frame) {
    PageTable* k = VMM::GetKernelAddressSpace();
    if (!k) return false;
    uint64_t f = frame & ~0xFFFULL;
    if (reinterpret_cast<uint64_t>(k) == f) return true;
    for (int i = 0; i < 512; ++i) {
        uint64_t e = k->entries[i];
        if (!(e & 1)) continue;               // Present
        if (e & 0x80) continue;               // LargePage
        uint64_t pdpt = e & 0x000FFFFFFFFFF000ULL;
        if (pdpt == f) return true;
        auto* pt = reinterpret_cast<PageTable*>(pdpt);
        for (int j = 0; j < 512; ++j) {
            uint64_t e2 = pt->entries[j];
            if (!(e2 & 1) || (e2 & 0x80)) continue;
            if ((e2 & 0x000FFFFFFFFFF000ULL) == f) return true;
        }
    }
    return false;
}

static PageTable* current_pml4() {
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return reinterpret_cast<PageTable*>(cr3 & ADDR_MASK);
}

static void memzero(void* dst, uint64_t size) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < size; i++)
        p[i] = 0;
}

static PageTable* alloc_zeroed_table() {
    // Page tables are always allocated from the PMM (one physical frame each),
    // never from the buddy heap. A page table stored in a heap block is a hazard
    // for the buddy allocator: the table's PTE bytes can be misread as a free
    // block's header/free-list pointers during coalescing, corrupting the heap
    // (observed as an infinite loop in kfree() while tearing down a forked
    // address space). PMM frames are the natural home for page tables and avoid
    // this class of bug entirely. free_table() mirrors this via PMM::FreeFrame.
    // Allocate page-table frames above the non-PIE user-image window so that
    // both zeroing them here and the kernel's later software page-table walks
    // (all via the low identity map) are never shadowed by a low ET_EXEC image
    // in the active address space. Fall back to a normal frame if no high frame
    // is available (small-memory configs / degraded operation).
    uint64_t frame = PMM::AllocFramesAbove(1, PMM::KERNEL_HIGH_ALLOC_MIN);
    if (frame == 0) frame = PMM::AllocFrame();
    if (frame == 0) return nullptr;
    PageTable* table = reinterpret_cast<PageTable*>(frame);
    if ((reinterpret_cast<uint64_t>(table) & (PAGE_SIZE - 1)) != 0) return nullptr;
    memzero(table, sizeof(PageTable));
    return table;
}

static bool is_heap_table(PageTable* table) {
    if (!table || !heap_is_initialized()) {
        return false;
    }

    // Any heap arena (initial or grown) counts; a single 4 KiB allocation never
    // straddles two arenas, so testing the base address is sufficient.
    return heap_contains(table);
}

static void free_table(PageTable* table) {
    if (!table) {
        return;
    }

    // Safety net against the shallow-copy teardown bug: initializeAddressSpace
    // makes each per-process low-half PDPT a shallow copy of the kernel's, so a
    // process address space can share PD/PT sub-table frames with the kernel
    // master page table (s_pml4). FreeAddressSpace/freePrivateTable can then try
    // to return a still-kernel-referenced frame to the PMM; once reallocated and
    // overwritten it corrupts kernel page tables (observed as a triple fault on
    // the next process creation). Never free a frame the kernel still
    // references. Logged once so the underlying ownership bug stays visible.
    if (!is_heap_table(table) && frameIsKernelTable(reinterpret_cast<uint64_t>(table))) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Console::get().drawText("\n[VMM] skipped freeing kernel-referenced frame 0x");
            Console::get().drawHex(reinterpret_cast<uint64_t>(table));
            Console::get().drawText(" (shallow-copy teardown bug; see vmm.cpp free_table)\n");
        }
        return;
    }

    if (is_heap_table(table)) {
        kfree(table);
    } else {
        PMM::FreeFrame(reinterpret_cast<uint64_t>(table));
    }
}

static bool is_table_empty(PageTable* table) {
    if (!table) {
        return true;
    }

    for (uint64_t i = 0; i < 512; i++) {
        if (table->entries[i] & Present) {
            return false;
        }
    }
    return true;
}

static PageTable* clone_table_if_needed(uint64_t& parentEntry) {
    if (!(parentEntry & Present) || (parentEntry & LargePage)) {
        return reinterpret_cast<PageTable*>(parentEntry & ADDR_MASK);
    }

    // Bit 9 (an AVL/OS-available bit) marks a table that is already private to
    // this address space (either freshly AllocTable()'d here, or previously
    // cloned). Such tables must not be re-cloned, or repeated MapPageInto()
    // calls into the same table would discard earlier entries. Kernel-shared
    // tables (inherited identity/code maps) lack this bit and are cloned on
    // first write (copy-on-write).
    constexpr uint64_t kPrivateTable = 1ULL << 9;

    auto* table = reinterpret_cast<PageTable*>(parentEntry & ADDR_MASK);
    if (!heap_is_initialized() || (parentEntry & kPrivateTable)) {
        return table;
    }

    PageTable* clone = alloc_zeroed_table();
    if (!clone) return nullptr;

    for (uint64_t i = 0; i < 512; i++) {
        // Inherit the shared sub-entries, but CLEAR the private-table marker:
        // the clone's children still point at the parent/kernel's shared
        // sub-tables, so a later MapPageInto() write must copy-on-write them
        // rather than mutate the shared originals (which would corrupt the
        // kernel identity map and make teardown free live kernel frames).
        // Mirrors initializeAddressSpace()'s PDPT shallow-copy in process.cpp.
        clone->entries[i] = table->entries[i] & ~kPrivateTable;
    }

    const uint64_t flags = (parentEntry & ~ADDR_MASK) | ReadWrite | kPrivateTable;
    parentEntry = reinterpret_cast<uint64_t>(clone) | flags;
    return clone;
}

static PageTable* splitLargePdptEntry(PageTable* pdpt, uint64_t pdpti) {
    if (!pdpt) return nullptr;

    uint64_t& entry = pdpt->entries[pdpti];
    if ((entry & (Present | LargePage)) != (Present | LargePage)) {
        return reinterpret_cast<PageTable*>(entry & ADDR_MASK);
    }

    PageTable* pd = alloc_zeroed_table();
    if (!pd) return nullptr;

    const uint64_t base = entry & 0x000FFFFFC0000000ULL;
    const uint64_t flags = entry & ~ADDR_MASK & ~LargePage;
    for (uint64_t i = 0; i < 512; i++) {
        pd->entries[i] = (base + i * 0x200000ULL) | flags | Present | LargePage;
    }

    entry = reinterpret_cast<uint64_t>(pd) | flags | Present | ReadWrite | UserSuper | (1ULL << 9);
    return pd;
}

static PageTable* splitLargePdEntry(PageTable* pd, uint64_t pdi) {
    if (!pd) return nullptr;

    uint64_t& entry = pd->entries[pdi];
    if ((entry & (Present | LargePage)) != (Present | LargePage)) {
        return reinterpret_cast<PageTable*>(entry & ADDR_MASK);
    }

    PageTable* pt = alloc_zeroed_table();
    if (!pt) return nullptr;

    const uint64_t base = entry & 0x000FFFFFFFE00000ULL;
    const uint64_t flags = entry & ~ADDR_MASK & ~LargePage;
    for (uint64_t i = 0; i < 512; i++) {
        pt->entries[i] = (base + i * PAGE_SIZE) | flags | Present;
    }

    entry = reinterpret_cast<uint64_t>(pt) | flags | Present | ReadWrite | UserSuper | (1ULL << 9);
    return pt;
}

PageTable* VMM::AllocTable() {
    return alloc_zeroed_table();
}

namespace {
// Pages unmapped but not yet shot down, and the page tables they emptied. A
// table is detached from its parent first and freed only after the shootdown,
// so no CPU can still reach it through a cached paging-structure entry once
// its frame is reused.
struct UnmapBatch {
    static constexpr uint32_t kMaxTables = 32;

    PageTable* pml4;
    uint64_t start = 0;
    uint64_t end = 0;
    PageTable* tables[kMaxTables];
    uint32_t tableCount = 0;

    explicit UnmapBatch(PageTable* space) : pml4(space) {}

    void addPage(uint64_t va) {
        if (start == end) {
            start = va;
            end = va + PAGE_SIZE;
            return;
        }
        if (va < start) start = va;
        if (va + PAGE_SIZE > end) end = va + PAGE_SIZE;
    }

    void deferFree(PageTable* table) {
        if (tableCount == kMaxTables) {
            finish();
        }
        tables[tableCount++] = table;
    }

    void finish() {
        if (end > start) {
            TLB::shootdown(pml4, start, (end - start) / PAGE_SIZE);
        }
        for (uint32_t i = 0; i < tableCount; ++i) {
            free_table(tables[i]);
        }
        start = end = 0;
        tableCount = 0;
    }
};

void unmapLeaf(UnmapBatch& batch, uint64_t virtualAddr) {
    PageTable* pml4 = batch.pml4;
    virtualAddr &= ~0xFFFULL;

    uint64_t pml4i = (virtualAddr >> 39) & 0x1FF;
    if (!(pml4->entries[pml4i] & Present)) return;
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    uint64_t pdpti = (virtualAddr >> 30) & 0x1FF;
    if (!(pdpt->entries[pdpti] & Present)) return;
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return;
    }
    if (pdpt->entries[pdpti] & LargePage) {
        if (!splitLargePdptEntry(pdpt, pdpti)) return;
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = (virtualAddr >> 21) & 0x1FF;
    if (!(pd->entries[pdi] & Present)) return;
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    if (pd->entries[pdi] & LargePage) {
        if (!splitLargePdEntry(pd, pdi)) return;
    }
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = (virtualAddr >> 12) & 0x1FF;
    pt->entries[pti] = 0;
    batch.addPage(virtualAddr);

    if (is_table_empty(pt)) {
        pd->entries[pdi] = 0;
        batch.deferFree(pt);
    }
    if (is_table_empty(pd)) {
        pdpt->entries[pdpti] = 0;
        batch.deferFree(pd);
    }
    if (is_table_empty(pdpt)) {
        pml4->entries[pml4i] = 0;
        batch.deferFree(pdpt);
    }
}

// Rewrites a present leaf's flags, keeping its frame. No TLB flush.
bool protectLeaf(PageTable* pml4, uint64_t virtualAddr, uint64_t flags) {
    virtualAddr &= ~0xFFFULL;

    uint64_t pml4i = (virtualAddr >> 39) & 0x1FF;
    if (!(pml4->entries[pml4i] & Present)) return false;
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return false;

    uint64_t pdpti = (virtualAddr >> 30) & 0x1FF;
    if (!(pdpt->entries[pdpti] & Present)) return false;
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return false;
    }
    if (pdpt->entries[pdpti] & LargePage) {
        if (!splitLargePdptEntry(pdpt, pdpti)) return false;
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = (virtualAddr >> 21) & 0x1FF;
    if (!(pd->entries[pdi] & Present)) return false;
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return false;
    }
    if (pd->entries[pdi] & LargePage) {
        if (!splitLargePdEntry(pd, pdi)) return false;
    }
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = (virtualAddr >> 12) & 0x1FF;
    if (!(pt->entries[pti] & Present)) return false;

    const uint64_t phys = pt->entries[pti] & ADDR_MASK;
    pt->entries[pti] = phys | (flags & ~ADDR_MASK) | Present;
    return true;
}

// Stores a leaf. Replacing a present translation needs a shootdown; filling an
// empty slot only needs this CPU's (possibly cached paging-structure) entries
// dropped, and only if `pml4` is live here.
void storeLeaf(PageTable* pml4, uint64_t* slot, uint64_t value, uint64_t virtualAddr) {
    const uint64_t old = *slot;
    *slot = value;
    if ((old & Present) && old != value) {
        TLB::shootdown(pml4, virtualAddr, 1);
    } else if (pml4 == current_pml4()) {
        asm volatile("invlpg (%0)" : : "r"(virtualAddr) : "memory");
    }
}
} // namespace

void VMM::InvalidatePage(uint64_t addr) {
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

void VMM::Initialize() {
    s_pml4 = AllocTable();
    if (!s_pml4) return;

    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    auto* oldPml4 = reinterpret_cast<PageTable*>(cr3 & ADDR_MASK);

    for (int i = 0; i < 512; i++)
        s_pml4->entries[i] = oldPml4->entries[i];

    asm volatile("mov %0, %%cr3" : : "r"(reinterpret_cast<uint64_t>(s_pml4)) : "memory");

    s_initialized = true;
}

uint64_t VMM::DirectMapBase() {
    return s_directMapBase;
}

uint64_t VMM::PhysToVirt(uint64_t phys) {
    return s_directMapBase ? (s_directMapBase + phys) : phys;
}

uint64_t VMM::InitDirectMap() {
    if (!s_pml4) return 0;
    if (s_directMapBase) return s_directMapBase;

    // Claim a free slot in the shared kernel half of the PML4 (256..510).
    // Entries here are copied verbatim into every process address space by
    // initializeAddressSpace(), so the map is inherited by all of them.
    int idx = -1;
    for (int i = 256; i < 511; ++i) {
        if (!(s_pml4->entries[i] & Present)) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return 0;

    // A single PDPT of 512 * 1 GiB pages spans physical [0, 512 GiB) — far more
    // than the PMM metadata (which always lives in low RAM) needs. Supervisor
    // (no U/S) so user code can never reach it; NX so it never executes.
    PageTable* pdpt = AllocTable();
    if (!pdpt) return 0;
    for (uint64_t i = 0; i < 512; ++i) {
        pdpt->entries[i] = (i * 0x40000000ULL) | Present | ReadWrite | LargePage | NoExecute;
    }

    // Canonical high-half base VA for this PML4 index.
    const uint64_t base = 0xFFFF000000000000ULL | (static_cast<uint64_t>(idx) << 39);
    s_pml4->entries[idx] =
        reinterpret_cast<uint64_t>(pdpt) | Present | ReadWrite | NoExecute | (1ULL << 9);
    s_directMapBase = base;
    return base;
}

void VMM::MapPage(uint64_t virtualAddr, uint64_t physAddr, uint64_t flags) {
    if (!s_initialized) return;

    PageTable* pml4 = current_pml4();
    if (!pml4) return;

    virtualAddr &= ~0xFFFULL;
    physAddr    &= ~0xFFFULL;

    uint64_t pml4i = PML4Index(virtualAddr);
    uint64_t pdpti = PDPTIndex(virtualAddr);
    uint64_t pdi   = PDIndex(virtualAddr);
    uint64_t pti   = PTIndex(virtualAddr);

    if (!(pml4->entries[pml4i] & Present)) {
        PageTable* pdpt = AllocTable();
        if (!pdpt) return;
        pml4->entries[pml4i] = reinterpret_cast<uint64_t>(pdpt) | Present | ReadWrite | UserSuper | (1ULL << 9);
    }
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    if (!(pdpt->entries[pdpti] & Present)) {
        PageTable* pd = AllocTable();
        if (!pd) return;
        pdpt->entries[pdpti] = reinterpret_cast<uint64_t>(pd) | Present | ReadWrite | UserSuper | (1ULL << 9);
    }
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return;
    }
    auto* pd = splitLargePdptEntry(pdpt, pdpti);
    if (!pd) return;

    if (!(pd->entries[pdi] & Present)) {
        PageTable* pt = AllocTable();
        if (!pt) return;
        pd->entries[pdi] = reinterpret_cast<uint64_t>(pt) | Present | ReadWrite | UserSuper | (1ULL << 9);
    }
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    auto* pt = splitLargePdEntry(pd, pdi);
    if (!pt) return;

    storeLeaf(pml4, &pt->entries[pti], physAddr | (flags & ~ADDR_MASK) | Present, virtualAddr);
}

void VMM::UnmapPage(uint64_t virtualAddr) {
    if (!s_initialized) return;
    UnmapPageFrom(current_pml4(), virtualAddr);
}

uint64_t VMM::VirtualToPhysical(uint64_t virtualAddr) {
    if (!s_initialized) return 0;

    PageTable* pml4 = current_pml4();
    if (!pml4) return 0;

    uint64_t pml4i = PML4Index(virtualAddr);
    if (!(pml4->entries[pml4i] & Present)) return 0;
    auto* pdpt = reinterpret_cast<PageTable*>(pml4->entries[pml4i] & ADDR_MASK);

    uint64_t pdpti = PDPTIndex(virtualAddr);
    if (!(pdpt->entries[pdpti] & Present)) return 0;
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = PDIndex(virtualAddr);
    if (!(pd->entries[pdi] & Present)) return 0;
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = PTIndex(virtualAddr);
    if (!(pt->entries[pti] & Present)) return 0;

    return (pt->entries[pti] & ADDR_MASK) | (virtualAddr & 0xFFF);
}

bool VMM::IsMapped(uint64_t virtualAddr) {
    return VirtualToPhysical(virtualAddr) != 0;
}

bool VMM::IsUserMapped(uint64_t virtualAddr) {
    if (!s_initialized) return false;

    PageTable* pml4 = current_pml4();
    if (!pml4) return false;

    uint64_t pml4e = pml4->entries[PML4Index(virtualAddr)];
    if ((pml4e & (Present | UserSuper)) != (Present | UserSuper)) return false;
    auto* pdpt = reinterpret_cast<PageTable*>(pml4e & ADDR_MASK);

    uint64_t pdpte = pdpt->entries[PDPTIndex(virtualAddr)];
    if ((pdpte & (Present | UserSuper)) != (Present | UserSuper)) return false;
    if (pdpte & LargePage) return true;
    auto* pd = reinterpret_cast<PageTable*>(pdpte & ADDR_MASK);

    uint64_t pde = pd->entries[PDIndex(virtualAddr)];
    if ((pde & (Present | UserSuper)) != (Present | UserSuper)) return false;
    if (pde & LargePage) return true;
    auto* pt = reinterpret_cast<PageTable*>(pde & ADDR_MASK);

    uint64_t pte = pt->entries[PTIndex(virtualAddr)];
    return (pte & (Present | UserSuper)) == (Present | UserSuper);
}

uint64_t VMM::VirtualToPhysicalIn(PageTable* pml4, uint64_t virtualAddr) {
    if (!pml4) return 0;

    uint64_t pml4e = pml4->entries[PML4Index(virtualAddr)];
    if (!(pml4e & Present)) return 0;
    auto* pdpt = reinterpret_cast<PageTable*>(pml4e & ADDR_MASK);

    uint64_t pdpte = pdpt->entries[PDPTIndex(virtualAddr)];
    if (!(pdpte & Present)) return 0;
    if (pdpte & LargePage) {
        return (pdpte & 0x000FFFFFC0000000ULL) | (virtualAddr & 0x3FFFFFFFULL);
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpte & ADDR_MASK);

    uint64_t pde = pd->entries[PDIndex(virtualAddr)];
    if (!(pde & Present)) return 0;
    if (pde & LargePage) {
        return (pde & 0x000FFFFFFFE00000ULL) | (virtualAddr & 0x1FFFFFULL);
    }
    auto* pt = reinterpret_cast<PageTable*>(pde & ADDR_MASK);

    uint64_t pte = pt->entries[PTIndex(virtualAddr)];
    if (!(pte & Present)) return 0;

    return (pte & ADDR_MASK) | (virtualAddr & 0xFFF);
}

void VMM::MapRange(uint64_t virtualBase, uint64_t physBase,
                   uint64_t pageCount, uint64_t flags) {
    for (uint64_t i = 0; i < pageCount; i++)
        MapPage(virtualBase + i * PAGE_SIZE, physBase + i * PAGE_SIZE, flags);
}

void VMM::UnmapRange(uint64_t virtualBase, uint64_t pageCount) {
    if (!s_initialized) return;
    UnmapRangeFrom(current_pml4(), virtualBase, pageCount);
}

void VMM::MapPageInto(PageTable* pml4, uint64_t virtualAddr, uint64_t physAddr, uint64_t flags) {
    if (!pml4) return;

    virtualAddr &= ~0xFFFULL;
    physAddr    &= ~0xFFFULL;

    uint64_t pml4i = PML4Index(virtualAddr);
    uint64_t pdpti = PDPTIndex(virtualAddr);
    uint64_t pdi   = PDIndex(virtualAddr);
    uint64_t pti   = PTIndex(virtualAddr);

    // Intermediate paging structures must be at least as permissive as the leaf
    // they reach. When mapping a user page into a hierarchy whose upper entries
    // were inherited from the kernel half (which lack the U/S and/or R/W bits),
    // those bits must be promoted, otherwise the CPU treats the leaf as
    // supervisor/read-only and user accesses (and isValidUserPointer) fail.
    const uint64_t upperPromote = (flags & UserSuper) | (flags & ReadWrite);

    if (!(pml4->entries[pml4i] & Present)) {
        PageTable* pdpt = AllocTable();
        if (!pdpt) return;
        pml4->entries[pml4i] = reinterpret_cast<uint64_t>(pdpt) | Present | ReadWrite | UserSuper;
    } else {
        pml4->entries[pml4i] |= upperPromote;
    }
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    if (!(pdpt->entries[pdpti] & Present)) {
        PageTable* pd = AllocTable();
        if (!pd) return;
        pdpt->entries[pdpti] = reinterpret_cast<uint64_t>(pd) | Present | ReadWrite | UserSuper;
    } else {
        pdpt->entries[pdpti] |= upperPromote;
    }
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return;
    }
    auto* pd = splitLargePdptEntry(pdpt, pdpti);
    if (!pd) return;

    if (!(pd->entries[pdi] & Present)) {
        PageTable* pt = AllocTable();
        if (!pt) return;
        pd->entries[pdi] = reinterpret_cast<uint64_t>(pt) | Present | ReadWrite | UserSuper;
    } else {
        pd->entries[pdi] |= upperPromote;
    }
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    auto* pt = splitLargePdEntry(pd, pdi);
    if (!pt) return;

    storeLeaf(pml4, &pt->entries[pti], physAddr | (flags & ~ADDR_MASK) | Present, virtualAddr);
}

void VMM::MapRangeInto(PageTable* pml4, uint64_t virtualBase, uint64_t physBase,
                       uint64_t pageCount, uint64_t flags) {
    for (uint64_t i = 0; i < pageCount; i++)
        MapPageInto(pml4, virtualBase + i * PAGE_SIZE, physBase + i * PAGE_SIZE, flags);
}

bool VMM::ProtectPageIn(PageTable* pml4, uint64_t virtualAddr, uint64_t flags) {
    if (!pml4) return false;
    if (!protectLeaf(pml4, virtualAddr, flags)) return false;
    TLB::shootdown(pml4, virtualAddr & ~0xFFFULL, 1);
    return true;
}

bool VMM::ProtectRangeIn(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount, uint64_t flags) {
    if (!pml4) return false;
    uint64_t done = 0;
    bool ok = true;
    for (; done < pageCount; done++) {
        if (!protectLeaf(pml4, virtualBase + done * PAGE_SIZE, flags)) {
            ok = false;
            break;
        }
    }
    if (done) {
        TLB::shootdown(pml4, virtualBase & ~0xFFFULL, done);
    }
    return ok;
}

bool VMM::HandleCowFault(PageTable* pml4, uint64_t faultAddr) {
    if (!pml4) return false;

    const uint64_t va = faultAddr & ~0xFFFULL;

    // Walk to the leaf PTE THROUGH THE HIGHER-HALF DIRECT MAP, never the low
    // identity map. This runs in the faulting process's address space, which may
    // be a non-PIE (ET_EXEC) image mapped at a low VA (e.g. 0x400000..). Any
    // page-table frame that happens to live in that image window (possible once
    // AllocTable() falls back below KERNEL_HIGH_ALLOC_MIN under memory pressure)
    // has its identity VA shadowed by the user image; a raw identity walk would
    // then read/write the user's image instead of the page tables. The PTE
    // *store* below (`pt->entries[pti] = ...`) is the dangerous case: it would
    // scribble a PTE-shaped word into the process's own .data/.got, e.g. zeroing
    // a GOT slot -> a later NULL write crash (observed: collect2's GOT[environ]
    // zeroed only under the heavy load of the in-OS binutils build). PhysToVirt
    // maps every physical frame at a high-half VA present in every address space.
    auto asTable = [](uint64_t phys) -> PageTable* {
        return reinterpret_cast<PageTable*>(PhysToVirt(phys & ADDR_MASK));
    };

    PageTable* pml4v = asTable(reinterpret_cast<uint64_t>(pml4));
    uint64_t pml4e = pml4v->entries[PML4Index(va)];
    if (!(pml4e & Present)) return false;
    auto* pdpt = asTable(pml4e);

    uint64_t pdpte = pdpt->entries[PDPTIndex(va)];
    if (!(pdpte & Present) || (pdpte & LargePage)) return false;
    auto* pd = asTable(pdpte);

    uint64_t pde = pd->entries[PDIndex(va)];
    if (!(pde & Present) || (pde & LargePage)) return false;
    auto* pt = asTable(pde);

    const uint64_t pti = PTIndex(va);
    const uint64_t pte = pt->entries[pti];
    if (!(pte & Present) || !(pte & kCowPage)) return false;   // not a COW page

    const uint64_t oldPhys  = pte & ADDR_MASK;
    // Restore write permission and drop the COW marker; preserve all other bits.
    const uint64_t newFlags = ((pte & ~ADDR_MASK) | ReadWrite) & ~kCowPage;

    if (PMM::RefCount(oldPhys) <= 1) {
        // We are the only referer left: reclaim the frame in place, no copy.
        pt->entries[pti] = oldPhys | newFlags;
        InvalidatePage(va);
        return true;
    }

    uint64_t newPhys = PMM::AllocFrame();
    if (!newPhys) return false;   // OOM: fall through to the fatal fault path

    // Copy the shared page. Reach both frames through the higher-half direct
    // map, NOT the low identity map: while a non-PIE (ET_EXEC) user image mapped
    // at a low VA (e.g. 0x400000) is the active address space, the identity VA
    // of a low frame is shadowed by that image, so a raw identity copy would
    // read/write the user's image instead of the intended frame.
    const uint64_t* srcW = reinterpret_cast<const uint64_t*>(PhysToVirt(oldPhys));
    uint64_t*       dstW = reinterpret_cast<uint64_t*>(PhysToVirt(newPhys));
    for (uint64_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        dstW[i] = srcW[i];
    }

    pt->entries[pti] = (newPhys & ADDR_MASK) | newFlags;
    // Other threads of this space may still read the old frame through a stale
    // entry; drop it everywhere before giving up our reference.
    TLB::shootdown(pml4, va, 1);
    PMM::FreeFrame(oldPhys);       // release this space's reference to the shared frame
    return true;
}

bool VMM::PermitsFaultedAccess(PageTable* pml4, uint64_t faultAddr, uint64_t errCode) {
    constexpr uint64_t kErrWrite = 1ULL << 1;
    constexpr uint64_t kErrUser = 1ULL << 2;
    constexpr uint64_t kErrReserved = 1ULL << 3;
    constexpr uint64_t kErrFetch = 1ULL << 4;
    constexpr uint64_t kErrProtKey = 1ULL << 5;
    if (!pml4 || (errCode & (kErrReserved | kErrProtKey))) {
        return false;   // not about the translation's present/RW/US/NX bits
    }

    // Walk through the direct map (see HandleCowFault for why), accumulating
    // the effective permissions: RW and US must be set at every level, NX at
    // any level forbids fetches.
    uint64_t entry = reinterpret_cast<uint64_t>(pml4) | Present | ReadWrite | UserSuper;
    bool writable = true;
    bool user = true;
    bool noExec = false;
    for (int level = 4; level >= 1; --level) {
        auto* table = reinterpret_cast<PageTable*>(PhysToVirt(entry & ADDR_MASK));
        entry = table->entries[(faultAddr >> (12 + 9 * (level - 1))) & 0x1FF];
        if (!(entry & Present)) {
            return false;
        }
        writable = writable && (entry & ReadWrite);
        user = user && (entry & UserSuper);
        noExec = noExec || (entry & NoExecute);
        if (level > 1 && level < 4 && (entry & LargePage)) {
            break;
        }
    }

    if ((errCode & kErrWrite) && !writable) return false;
    if ((errCode & kErrUser) && !user) return false;
    if (errCode & kErrFetch) {
        if (noExec) return false;
        if (!(errCode & kErrUser) && user) return false;   // SMEP
    }
    return true;
}

void VMM::UnmapPageFrom(PageTable* pml4, uint64_t virtualAddr) {
    UnmapRangeFrom(pml4, virtualAddr, 1);
}

void VMM::UnmapRangeFrom(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount) {
    if (!pml4) return;
    UnmapBatch batch(pml4);
    for (uint64_t i = 0; i < pageCount; i++) {
        unmapLeaf(batch, virtualBase + i * PAGE_SIZE);
    }
    batch.finish();
}

namespace {
// The low half of every user address space inherits the kernel/UEFI identity map
// (VA==PA) verbatim -- VMM::Initialize() copies the firmware CR3/PML4 and
// initializeAddressSpace() shallow-copies that low half into every process. Those
// frames are shared kernel/firmware memory (framebuffer, initrd, kernel image,
// PMM metadata) that happen to carry the U/S bit from the firmware page tables,
// so the UserSuper test alone cannot tell them apart from process-private user
// pages. The authoritative test is whether the KERNEL master table maps this VA
// to the very same physical frame; if so the frame is kernel-shared and must
// never be reclaimed by a single address space's teardown (freeing it hands a
// live kernel frame back to the PMM, which re-hands it out -> frame reuse /
// use-after-free corruption; root cause of the P7.1 collect2/daemon crashes).
static bool isKernelInheritedIdentity(uint64_t va, uint64_t pa) {
    PageTable* k = VMM::GetKernelAddressSpace();
    return k && (VMM::VirtualToPhysicalIn(k, va) & ~0xFFFULL) == (pa & ~0xFFFULL);
}

void freeMappedFrames(uint64_t entry, int level, uint64_t va) {
    if (!(entry & Present)) {
        return;
    }

    // Never free supervisor-only mappings. The low half of every user address
    // space inherits the kernel's identity/large-page maps (which lack the U/S
    // bit). Those frames are shared kernel memory and must not be reclaimed when
    // a single user address space is torn down.
    if (!(entry & UserSuper)) {
        return;
    }

    // Shared, manager-owned leaves (IPC shared memory, framebuffer BAR) are never
    // reclaimed by a single address space's teardown; freeing them would return a
    // frame still mapped elsewhere (or MMIO) to the PMM, which then re-hands it
    // out -> use-after-free corruption. (Root cause of the P7.1 frame reuse.)
    if (entry & kSharedFrame) {
        return;
    }

    uint64_t pa;
    uint64_t count;
    if (level == 3 && (entry & LargePage)) {
        pa = entry & 0x000FFFFFC0000000ULL;
        count = 512 * 512;
    } else if (level == 2 && (entry & LargePage)) {
        pa = entry & 0x000FFFFFFFE00000ULL;
        count = 512;
    } else if (level == 1) {
        pa = entry & ADDR_MASK;
        count = 1;
    } else {
        return;
    }

    // Inherited kernel/firmware identity map -> shared, do not reclaim.
    if (isKernelInheritedIdentity(va, pa)) {
        return;
    }

    if (count == 1) {
        PMM::FreeFrame(pa);
    } else {
        PMM::FreeFrames(pa, count);
    }
}

void freePrivateTable(PageTable* table, uint64_t kernelEntry, int level, uint64_t vaBase) {
    if (!table || level <= 0) {
        return;
    }

    PageTable* kernelTable = nullptr;
    if ((kernelEntry & Present) && !(kernelEntry & LargePage)) {
        kernelTable = reinterpret_cast<PageTable*>(kernelEntry & ADDR_MASK);
    }

    for (int i = 0; i < 512; ++i) {
        const uint64_t entry = table->entries[i];
        if (!(entry & Present)) {
            continue;
        }

        // Ignore the private-table marker (bit 9) in the comparison: both
        // initializeAddressSpace() and clone_table_if_needed() clear it on
        // shallow-copied entries, so an unmodified inherited entry differs from
        // the kernel baseline by exactly that bit. Masking it lets us recognise
        // still-shared kernel subtrees (same frame) and skip them, instead of
        // recursing into and freeing live kernel memory.
        constexpr uint64_t kPrivateTable = 1ULL << 9;
        const uint64_t baseline = kernelTable ? kernelTable->entries[i] : 0;
        if ((entry & ~kPrivateTable) == (baseline & ~kPrivateTable)) {
            continue;
        }

        // Supervisor-only entries map shared kernel memory (e.g. the identity
        // map inherited into the user address space's low half). Never free the
        // frames or sub-tables they reference; they are not owned by this user
        // address space. Second line of defence behind the masked comparison
        // above.
        if (!(entry & UserSuper)) {
            continue;
        }

        const uint64_t entryVa = vaBase | (static_cast<uint64_t>(i) << (12 + 9 * (level - 1)));

        if (level == 1 || (entry & LargePage)) {
            freeMappedFrames(entry, level, entryVa);
            continue;
        }

        uint64_t childKernelEntry = 0;
        if ((baseline & Present) && !(baseline & LargePage)) {
            childKernelEntry = baseline;
        }

        auto* child = reinterpret_cast<PageTable*>(entry & ADDR_MASK);
        freePrivateTable(child, childKernelEntry, level - 1, entryVa);
        free_table(child);
    }
}
}

void VMM::FreeAddressSpace(PageTable* pml4) {
    if (!pml4) return;

    PageTable* kernelPml4 = s_pml4;

    for (int i = 0; i < 256; i++) {
        const uint64_t entry = pml4->entries[i];
        if (!(entry & Present)) {
            continue;
        }

        constexpr uint64_t kPrivateTable = 1ULL << 9;
        const uint64_t baseline = kernelPml4 ? kernelPml4->entries[i] : 0;
        if ((entry & ~kPrivateTable) == (baseline & ~kPrivateTable)) {
            continue;
        }

        const uint64_t vaBase = static_cast<uint64_t>(i) << 39;

        if (entry & LargePage) {
            freeMappedFrames(entry, 4, vaBase);
            continue;
        }

        // Recurse to reclaim user mappings. freePrivateTable() skips any
        // supervisor-only (kernel-shared) sub-entries, so only frames owned by
        // this user address space are freed. The per-process PDPT itself (a
        // shallow copy of the kernel's low-half PDPT created in
        // initializeAddressSpace) is always private to this address space and is
        // freed below.
        auto* pdpt = reinterpret_cast<PageTable*>(entry & ADDR_MASK);
        freePrivateTable(pdpt, baseline, 3, vaBase);
        free_table(pdpt);
    }

    free_table(pml4);
}

void VMM::SetAddressSpace(PageTable* pml4) {
    if (!pml4) {
        // Don't switch to null page table!
        return;
    }
    uint64_t phys = reinterpret_cast<uint64_t>(pml4) & ADDR_MASK;
    asm volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

PageTable* VMM::GetAddressSpace() {
    PageTable* pml4 = current_pml4();
    return pml4 ? pml4 : s_pml4;
}

PageTable* VMM::GetKernelAddressSpace() {
    return s_pml4;
}

bool VMM::IsInitialized() {
    return s_initialized;
}
