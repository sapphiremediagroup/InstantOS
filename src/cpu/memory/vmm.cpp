#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <graphics/console.hpp>

PageTable* VMM::s_pml4        = nullptr;
bool       VMM::s_initialized = false;

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
    uint64_t frame = PMM::AllocFrame();
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

    const uintptr_t base = heap_base();
    const size_t size = heap_size();
    const uintptr_t addr = reinterpret_cast<uintptr_t>(table);
    return addr >= base && addr + sizeof(PageTable) <= base + size;
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
        clone->entries[i] = table->entries[i];
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

    pt->entries[pti] = physAddr | (flags & ~ADDR_MASK) | Present;

    InvalidatePage(virtualAddr);
}

void VMM::UnmapPage(uint64_t virtualAddr) {
    if (!s_initialized) return;

    PageTable* pml4 = current_pml4();
    if (!pml4) return;

    virtualAddr &= ~0xFFFULL;

    uint64_t pml4i = PML4Index(virtualAddr);
    if (!(pml4->entries[pml4i] & Present)) return;
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    uint64_t pdpti = PDPTIndex(virtualAddr);
    if (!(pdpt->entries[pdpti] & Present)) return;
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return;
    }
    if (pdpt->entries[pdpti] & LargePage) {
        if (!splitLargePdptEntry(pdpt, pdpti)) return;
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = PDIndex(virtualAddr);
    if (!(pd->entries[pdi] & Present)) return;
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    if (pd->entries[pdi] & LargePage) {
        if (!splitLargePdEntry(pd, pdi)) return;
    }
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = PTIndex(virtualAddr);
    pt->entries[pti] = 0;

    if (is_table_empty(pt)) {
        pd->entries[pdi] = 0;
        free_table(pt);
    }
    if (is_table_empty(pd)) {
        pdpt->entries[pdpti] = 0;
        free_table(pd);
    }
    if (is_table_empty(pdpt)) {
        pml4->entries[pml4i] = 0;
        free_table(pdpt);
    }

    InvalidatePage(virtualAddr);
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
    for (uint64_t i = 0; i < pageCount; i++)
        UnmapPage(virtualBase + i * PAGE_SIZE);
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

    pt->entries[pti] = physAddr | (flags & ~ADDR_MASK) | Present;

    // If we just edited the live address space, flush the stale translation so
    // a subsequent access (including a kernel-side memset of a fresh mapping)
    // doesn't fault against a previously cached entry.
    if (pml4 == current_pml4()) {
        InvalidatePage(virtualAddr);
    }
}

void VMM::MapRangeInto(PageTable* pml4, uint64_t virtualBase, uint64_t physBase,
                       uint64_t pageCount, uint64_t flags) {
    for (uint64_t i = 0; i < pageCount; i++)
        MapPageInto(pml4, virtualBase + i * PAGE_SIZE, physBase + i * PAGE_SIZE, flags);
}

bool VMM::ProtectPageIn(PageTable* pml4, uint64_t virtualAddr, uint64_t flags) {
    if (!pml4) return false;

    virtualAddr &= ~0xFFFULL;

    uint64_t pml4i = PML4Index(virtualAddr);
    if (!(pml4->entries[pml4i] & Present)) return false;
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return false;

    uint64_t pdpti = PDPTIndex(virtualAddr);
    if (!(pdpt->entries[pdpti] & Present)) return false;
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return false;
    }
    if (pdpt->entries[pdpti] & LargePage) {
        if (!splitLargePdptEntry(pdpt, pdpti)) return false;
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = PDIndex(virtualAddr);
    if (!(pd->entries[pdi] & Present)) return false;
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return false;
    }
    if (pd->entries[pdi] & LargePage) {
        if (!splitLargePdEntry(pd, pdi)) return false;
    }
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = PTIndex(virtualAddr);
    if (!(pt->entries[pti] & Present)) return false;

    const uint64_t phys = pt->entries[pti] & ADDR_MASK;
    pt->entries[pti] = phys | (flags & ~ADDR_MASK) | Present;
    InvalidatePage(virtualAddr);
    return true;
}

bool VMM::ProtectRangeIn(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount, uint64_t flags) {
    for (uint64_t i = 0; i < pageCount; i++) {
        if (!ProtectPageIn(pml4, virtualBase + i * PAGE_SIZE, flags)) {
            return false;
        }
    }
    return true;
}

void VMM::UnmapPageFrom(PageTable* pml4, uint64_t virtualAddr) {
    if (!pml4) return;

    virtualAddr &= ~0xFFFULL;

    uint64_t pml4i = PML4Index(virtualAddr);
    if (!(pml4->entries[pml4i] & Present)) return;
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    uint64_t pdpti = PDPTIndex(virtualAddr);
    if (!(pdpt->entries[pdpti] & Present)) return;
    if (!(pdpt->entries[pdpti] & LargePage)) {
        if (!clone_table_if_needed(pdpt->entries[pdpti])) return;
    }
    if (pdpt->entries[pdpti] & LargePage) {
        if (!splitLargePdptEntry(pdpt, pdpti)) return;
    }
    auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[pdpti] & ADDR_MASK);

    uint64_t pdi = PDIndex(virtualAddr);
    if (!(pd->entries[pdi] & Present)) return;
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    if (pd->entries[pdi] & LargePage) {
        if (!splitLargePdEntry(pd, pdi)) return;
    }
    auto* pt = reinterpret_cast<PageTable*>(pd->entries[pdi] & ADDR_MASK);

    uint64_t pti = PTIndex(virtualAddr);
    pt->entries[pti] = 0;

    if (is_table_empty(pt)) {
        pd->entries[pdi] = 0;
        free_table(pt);
    }
    if (is_table_empty(pd)) {
        pdpt->entries[pdpti] = 0;
        free_table(pd);
    }
    if (is_table_empty(pdpt)) {
        pml4->entries[pml4i] = 0;
        free_table(pdpt);
    }

    if (pml4 == current_pml4()) {
        InvalidatePage(virtualAddr);
    }
}

void VMM::UnmapRangeFrom(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount) {
    for (uint64_t i = 0; i < pageCount; i++) {
        UnmapPageFrom(pml4, virtualBase + i * PAGE_SIZE);
    }
}

namespace {
void freeMappedFrames(uint64_t entry, int level) {
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

    if (level == 3 && (entry & LargePage)) {
        PMM::FreeFrames(entry & 0x000FFFFFC0000000ULL, 512 * 512);
        return;
    }

    if (level == 2 && (entry & LargePage)) {
        PMM::FreeFrames(entry & 0x000FFFFFFFE00000ULL, 512);
        return;
    }

    if (level == 1) {
        PMM::FreeFrame(entry & ADDR_MASK);
    }
}

void freePrivateTable(PageTable* table, uint64_t kernelEntry, int level) {
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

        const uint64_t baseline = kernelTable ? kernelTable->entries[i] : 0;
        if (entry == baseline) {
            continue;
        }

        // Supervisor-only entries map shared kernel memory (e.g. the identity
        // map inherited into the user address space's low half). Never free the
        // frames or sub-tables they reference; they are not owned by this user
        // address space. Without this guard a shallow-copied kernel PDPT/PD makes
        // the baseline comparison miss and we would free live kernel memory.
        if (!(entry & UserSuper)) {
            continue;
        }

        if (level == 1 || (entry & LargePage)) {
            freeMappedFrames(entry, level);
            continue;
        }

        uint64_t childKernelEntry = 0;
        if ((baseline & Present) && !(baseline & LargePage)) {
            childKernelEntry = baseline;
        }

        auto* child = reinterpret_cast<PageTable*>(entry & ADDR_MASK);
        freePrivateTable(child, childKernelEntry, level - 1);
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

        const uint64_t baseline = kernelPml4 ? kernelPml4->entries[i] : 0;
        if (entry == baseline) {
            continue;
        }

        if (entry & LargePage) {
            freeMappedFrames(entry, 4);
            continue;
        }

        // Recurse to reclaim user mappings. freePrivateTable() skips any
        // supervisor-only (kernel-shared) sub-entries, so only frames owned by
        // this user address space are freed. The per-process PDPT itself (a
        // shallow copy of the kernel's low-half PDPT created in
        // initializeAddressSpace) is always private to this address space and is
        // freed below.
        auto* pdpt = reinterpret_cast<PageTable*>(entry & ADDR_MASK);
        freePrivateTable(pdpt, baseline, 3);
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
