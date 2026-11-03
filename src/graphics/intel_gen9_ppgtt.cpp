// Per-process GPU address spaces (see intel_gen9.hpp): 48-bit, 4-level
// PPGTTs after i915's gen8_ppgtt.c (MIT), and the private PAT their entries
// index (intel_gtt.c, bdw_setup_private_ppat). Tables are pages from the
// PMM, written through the kernel's direct map and flushed from the CPU's
// caches, so the GPU's walk sees them whatever the PAT says.

#include <graphics/intel_gen9.hpp>

#include <cpu/cereal/cereal.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>

namespace intel_gen9 {

namespace {

void log(const char* text) {
    Cereal::get().write(text);
}

void logHex(uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char text[19] = {'0', 'x'};
    for (int i = 0; i < 16; ++i) {
        text[2 + i] = kDigits[(value >> ((15 - i) * 4)) & 0xF];
    }
    text[18] = '\0';
    log(text);
}

void flushRange(const void* start, uint64_t bytes) {
    const auto* p = static_cast<const volatile uint8_t*>(start);
    for (uint64_t offset = 0; offset < bytes; offset += 64) {
        asm volatile("clflush (%0)" :: "r"(p + offset) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

} // namespace

void IntelGen9Driver::programPrivatePat() {
    const uint64_t before = (static_cast<uint64_t>(readReg(GEN8_PRIVATE_PAT_HI)) << 32) | readReg(GEN8_PRIVATE_PAT_LO);
    writeReg(GEN8_PRIVATE_PAT_LO, static_cast<uint32_t>(kGen9PrivatePat));
    writeReg(GEN8_PRIVATE_PAT_HI, static_cast<uint32_t>(kGen9PrivatePat >> 32));
    const uint64_t after = (static_cast<uint64_t>(readReg(GEN8_PRIVATE_PAT_HI)) << 32) | readReg(GEN8_PRIVATE_PAT_LO);
    log("[i915] private PAT: firmware left ");
    logHex(before);
    log(", now ");
    logHex(after);
    log(after == kGen9PrivatePat ? " (i915's)\n" : " (NOT what was written)\n");
    patProgrammed = after == kGen9PrivatePat;
}

uint64_t* IntelGen9Driver::ppgttTable(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(VMM::PhysToVirt(phys));
}

// A new table whose every entry is `fill`; 0 if out of memory.
uint64_t IntelGen9Driver::ppgttAllocTable(uint64_t fill) {
    const uint64_t phys = PMM::AllocFrame();
    if (phys == 0) {
        return 0;
    }
    uint64_t* table = ppgttTable(phys);
    for (uint32_t i = 0; i < PPGTT_ENTRIES; ++i) {
        table[i] = fill;
    }
    flushRange(table, GTT_PAGE_SIZE);
    return phys;
}

bool IntelGen9Driver::ensurePpgttScratch() {
    if (scratchPdp != 0) {
        return true;
    }
    if (scratchPage == 0) {
        return false;
    }
    // i915 maps the scratch page read-write in the PT; writes to it are lost.
    scratchPt = ppgttAllocTable(ppgttPte(scratchPage, true));
    scratchPd = scratchPt ? ppgttAllocTable(ppgttPde(scratchPt)) : 0;
    scratchPdp = scratchPd ? ppgttAllocTable(ppgttPde(scratchPd)) : 0;
    return scratchPdp != 0;
}

Ppgtt* IntelGen9Driver::createPpgtt() {
    if (!gttInitialized || !ensurePpgttScratch()) {
        return nullptr;
    }
    auto* vm = static_cast<Ppgtt*>(kmalloc(sizeof(Ppgtt)));
    if (!vm) {
        return nullptr;
    }
    vm->pml4 = ppgttAllocTable(ppgttPde(scratchPdp));
    if (vm->pml4 == 0) {
        kfree(vm);
        return nullptr;
    }
    vm->tablePages = 1;
    return vm;
}

// Frees every table below `tablePhys` (a table of `level`, 3 = PML4) that is
// not one of the shared scratch ones, then the table itself.
void IntelGen9Driver::ppgttFreeLevel(uint64_t tablePhys, uint32_t level) {
    if (level > 0) {
        const uint64_t scratchBelow = level == 3 ? scratchPdp : level == 2 ? scratchPd : scratchPt;
        const uint64_t* table = ppgttTable(tablePhys);
        for (uint32_t i = 0; i < PPGTT_ENTRIES; ++i) {
            const uint64_t child = ppgttEntryAddress(table[i]);
            if (child != 0 && child != scratchBelow) {
                ppgttFreeLevel(child, level - 1);
            }
        }
    }
    PMM::FreeFrame(tablePhys);
}

void IntelGen9Driver::destroyPpgtt(Ppgtt* vm) {
    if (!vm) {
        return;
    }
    ppgttFreeLevel(vm->pml4, 3);
    kfree(vm);
}

bool IntelGen9Driver::ppgttMap(Ppgtt* vm, uint64_t va, const GpuBuffer* buffer, bool cached) {
    if (!vm || !buffer || (va % GTT_PAGE_SIZE) != 0 ||
        va + static_cast<uint64_t>(buffer->pageCount) * GTT_PAGE_SIZE > PPGTT_VA_LIMIT) {
        return false;
    }
    const uint64_t scratchFor[4] = {0, scratchPt, scratchPd, scratchPdp};
    for (uint32_t page = 0; page < buffer->pageCount; ++page) {
        const uint64_t address = va + static_cast<uint64_t>(page) * GTT_PAGE_SIZE;
        // Down from the PML4: a level whose entry still leads to the scratch
        // chain gets a table of its own, filled with the next scratch level.
        uint64_t table = vm->pml4;
        for (uint32_t level = 3; level > 0; --level) {
            uint64_t* entries = ppgttTable(table);
            const uint32_t index = ppgttIndex(address, level);
            uint64_t child = ppgttEntryAddress(entries[index]);
            if (child == scratchFor[level]) {
                const uint64_t fill = level == 1 ? ppgttPte(scratchPage, true) : ppgttPde(scratchFor[level - 1]);
                child = ppgttAllocTable(fill);
                if (child == 0) {
                    return false;
                }
                ++vm->tablePages;
                entries[index] = ppgttPde(child);
                flushRange(&entries[index], sizeof(uint64_t));
            }
            table = child;
        }
        uint64_t* pt = ppgttTable(table);
        const uint32_t index = ppgttIndex(address, 0);
        pt[index] = ppgttPte(buffer->pages[page], cached);
        flushRange(&pt[index], sizeof(uint64_t));
    }
    return true;
}

void IntelGen9Driver::ppgttUnmap(Ppgtt* vm, uint64_t va, uint64_t bytes) {
    if (!vm) {
        return;
    }
    const uint64_t scratchFor[4] = {0, scratchPt, scratchPd, scratchPdp};
    for (uint64_t offset = 0; offset < bytes; offset += GTT_PAGE_SIZE) {
        const uint64_t address = va + offset;
        uint64_t table = vm->pml4;
        bool mapped = true;
        for (uint32_t level = 3; level > 0 && mapped; --level) {
            const uint64_t child = ppgttEntryAddress(ppgttTable(table)[ppgttIndex(address, level)]);
            mapped = child != scratchFor[level];
            table = child;
        }
        if (mapped) {
            uint64_t* pt = ppgttTable(table);
            const uint32_t index = ppgttIndex(address, 0);
            pt[index] = ppgttPte(scratchPage, true);
            flushRange(&pt[index], sizeof(uint64_t));
        }
    }
}

} // namespace intel_gen9
