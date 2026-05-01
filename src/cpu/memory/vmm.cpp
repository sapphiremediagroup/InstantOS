#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>

PageTable* VMM::s_pml4        = nullptr;
bool       VMM::s_initialized = false;

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
    PageTable* table = nullptr;
    if (heap_is_initialized()) {
        table = reinterpret_cast<PageTable*>(kmalloc_aligned(sizeof(PageTable), PAGE_SIZE));
    } else {
        uint64_t frame = PMM::AllocFrame();
        if (frame == 0) return nullptr;
        table = reinterpret_cast<PageTable*>(frame);
    }
    if (!table) return nullptr;
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

static PageTable* clone_table_if_needed(uint64_t& parentEntry) {
    if (!(parentEntry & Present) || (parentEntry & LargePage)) {
        return reinterpret_cast<PageTable*>(parentEntry & ADDR_MASK);
    }

    auto* table = reinterpret_cast<PageTable*>(parentEntry & ADDR_MASK);
    if (!heap_is_initialized() || is_heap_table(table)) {
        return table;
    }

    PageTable* clone = alloc_zeroed_table();
    if (!clone) return nullptr;

    for (uint64_t i = 0; i < 512; i++) {
        clone->entries[i] = table->entries[i];
    }

    const uint64_t flags = (parentEntry & ~ADDR_MASK) | ReadWrite;
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

    entry = reinterpret_cast<uint64_t>(pd) | flags | Present | ReadWrite | UserSuper;
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

    entry = reinterpret_cast<uint64_t>(pt) | flags | Present | ReadWrite | UserSuper;
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
        pml4->entries[pml4i] = reinterpret_cast<uint64_t>(pdpt) | Present | ReadWrite | UserSuper;
    }
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    if (!(pdpt->entries[pdpti] & Present)) {
        PageTable* pd = AllocTable();
        if (!pd) return;
        pdpt->entries[pdpti] = reinterpret_cast<uint64_t>(pd) | Present | ReadWrite | UserSuper;
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

    if (!(pml4->entries[pml4i] & Present)) {
        PageTable* pdpt = AllocTable();
        if (!pdpt) return;
        pml4->entries[pml4i] = reinterpret_cast<uint64_t>(pdpt) | Present | ReadWrite | UserSuper;
    }
    auto* pdpt = clone_table_if_needed(pml4->entries[pml4i]);
    if (!pdpt) return;

    if (!(pdpt->entries[pdpti] & Present)) {
        PageTable* pd = AllocTable();
        if (!pd) return;
        pdpt->entries[pdpti] = reinterpret_cast<uint64_t>(pd) | Present | ReadWrite | UserSuper;
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
    }
    if (!(pd->entries[pdi] & LargePage)) {
        if (!clone_table_if_needed(pd->entries[pdi])) return;
    }
    auto* pt = splitLargePdEntry(pd, pdi);
    if (!pt) return;

    pt->entries[pti] = physAddr | (flags & ~ADDR_MASK) | Present;
}

void VMM::MapRangeInto(PageTable* pml4, uint64_t virtualBase, uint64_t physBase,
                       uint64_t pageCount, uint64_t flags) {
    for (uint64_t i = 0; i < pageCount; i++)
        MapPageInto(pml4, virtualBase + i * PAGE_SIZE, physBase + i * PAGE_SIZE, flags);
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
}

void VMM::UnmapRangeFrom(PageTable* pml4, uint64_t virtualBase, uint64_t pageCount) {
    for (uint64_t i = 0; i < pageCount; i++) {
        UnmapPageFrom(pml4, virtualBase + i * PAGE_SIZE);
    }
}

void VMM::FreeAddressSpace(PageTable* pml4) {
    if (!pml4) return;

    for (int i = 0; i < 256; i++) {
        if (pml4->entries[i] & Present) {
            auto* pdpt = reinterpret_cast<PageTable*>(pml4->entries[i] & ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                if (pdpt->entries[j] & Present) {
                    if (pdpt->entries[j] & LargePage) {
                        // 1GB large page
                        PMM::FreeFrames(pdpt->entries[j] & ADDR_MASK, 512 * 512);
                    } else {
                        auto* pd = reinterpret_cast<PageTable*>(pdpt->entries[j] & ADDR_MASK);
                        for (int k = 0; k < 512; k++) {
                            if (pd->entries[k] & Present) {
                                if (pd->entries[k] & LargePage) {
                                    // 2MB large page
                                    PMM::FreeFrames(pd->entries[k] & ADDR_MASK, 512);
                                } else {
                                    auto* pt = reinterpret_cast<PageTable*>(pd->entries[k] & ADDR_MASK);
                                    for (int l = 0; l < 512; l++) {
                                        if (pt->entries[l] & Present) {
                                            PMM::FreeFrame(pt->entries[l] & ADDR_MASK);
                                        }
                                    }
                                    PMM::FreeFrame(reinterpret_cast<uint64_t>(pt));
                                }
                            }
                        }
                        PMM::FreeFrame(reinterpret_cast<uint64_t>(pd));
                    }
                }
            }
            PMM::FreeFrame(reinterpret_cast<uint64_t>(pdpt));
        }
    }
    PMM::FreeFrame(reinterpret_cast<uint64_t>(pml4));
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
