// The Gen9 global GTT and buffer objects (see intel_gen9.hpp). Runs only on
// request (the GpuTest syscall), never at boot. Follows i915's gen8 GGTT:
// intel_ggtt.c (gen8_ggtt_insert_entries, gen8_ggtt_invalidate) and
// i915_gem_stolen.c; the mappings themselves are made at boot by mapBars().

#include <graphics/intel_gen9.hpp>

#include <cpu/acpi/pci.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/tlb.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace intel_gen9 {

namespace {

// Writing GFX_FLSH_CNTL makes the GPU (and the aperture) drop cached GTT
// entries (i915: GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN).
constexpr uint32_t REG_GFX_FLSH_CNTL = 0x101008;
constexpr uint32_t GFX_FLSH_CNTL_EN = 1;

constexpr uint64_t kCacheLine = 64;

void log(const char* text) {
    Cereal::get().write(text);
}

void logHex(uint64_t value) {
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i) {
        const uint32_t digit = static_cast<uint32_t>(value >> (60 - 4 * i)) & 0xF;
        buf[2 + i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buf[18] = '\0';
    log(buf);
}

void logDec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos > 0);
    log(&buf[pos]);
}

// The display engine does not snoop the CPU's caches: a page the CPU wrote
// through the (write-back) direct map is flushed before the GPU reads it, and
// flushed again before the CPU reads what the GPU wrote.
void flushPage(uint64_t phys) {
    auto* line = reinterpret_cast<volatile uint8_t*>(VMM::PhysToVirt(phys));
    for (uint64_t offset = 0; offset < GTT_PAGE_SIZE; offset += kCacheLine) {
        asm volatile("clflush (%0)" : : "r"(line + offset) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

} // namespace

uint64_t IntelGen9Driver::readPte(uint32_t index) const {
    return gsm[index];
}

void IntelGen9Driver::writePte(uint32_t index, uint64_t pte) {
    gsm[index] = pte;
}

// After a batch of entry writes: drain the write-combining buffer, read an
// entry back so the writes have reached the GTT, then have the GPU drop what
// it cached (i915's gen8_ggtt_invalidate).
void IntelGen9Driver::invalidateGtt() {
    asm volatile("sfence" ::: "memory");
    (void)gsm[0];
    writeReg(REG_GFX_FLSH_CNTL, GFX_FLSH_CNTL_EN);
    (void)readReg(REG_GFX_FLSH_CNTL);
}

// What the firmware left in the GTT is reclaimed, as i915 does, except what
// is still in use: the plane being scanned out (the desktop's framebuffer,
// also written through the aperture) and, to be careful, the top 2 MiB. The
// HD 530 PC's firmware fills almost every entry: the first 256 MiB map stolen
// memory one to one, the rest a filler page, the top 2 MiB alternate. The
// scan only reports what it finds (one pass; reading the entries is the slow
// part).
void IntelGen9Driver::reserveFirmwareRanges() {
    const uint64_t startMs = time_get_uptime_ms();
    const uint64_t stolenEnd = stolenBase + ggc.stolenBytes;
    uint64_t valid = 0;
    uint64_t inStolen = 0;
    uint64_t elsewhere = 0;
    uint64_t elsewherePhys = 0;
    bool elsewhereOnePage = true;
    for (uint32_t i = 0; i < gttEntries; ++i) {
        const uint64_t pte = readPte(i);
        if (pteValid(pte)) {
            ++valid;
            const uint64_t phys = ptePhys(pte);
            if (phys >= stolenBase && phys < stolenEnd) {
                ++inStolen;
            } else {
                if (elsewhere == 0) {
                    elsewherePhys = phys;
                } else if (phys != elsewherePhys) {
                    elsewhereOnePage = false;
                }
                ++elsewhere;
            }
        }
        if ((i & 0xFFFF) == 0) {
            TLB::pollPending();
        }
    }

    const uint64_t planeBytes = (static_cast<uint64_t>(fbSize) + PLANE_SURF_ALIGN - 1) / PLANE_SURF_ALIGN * PLANE_SURF_ALIGN;
    allocator.reserve(static_cast<uint32_t>(display.surfaceGtt / GTT_PAGE_SIZE),
                      static_cast<uint32_t>(planeBytes / GTT_PAGE_SIZE));
    constexpr uint32_t kTopReservedPages = (2u << 20) / GTT_PAGE_SIZE;
    if (gttEntries > kTopReservedPages) {
        allocator.reserve(gttEntries - kTopReservedPages, kTopReservedPages);
    }

    log("[i915] gtt entries=");
    logDec(gttEntries);
    log(" valid=");
    logDec(valid);
    log(" in-stolen=");
    logDec(inStolen);
    log(" elsewhere=");
    logDec(elsewhere);
    if (elsewhere != 0) {
        log(elsewhereOnePage ? " (all to " : " (several pages, first ");
        logHex(elsewherePhys);
        log(")");
    }
    log(" scan=");
    logDec(time_get_uptime_ms() - startMs);
    log(" ms\n[i915] gtt kept: plane ");
    logHex(display.surfaceGtt);
    log(" + ");
    logDec(planeBytes >> 10);
    log(" KiB, top 2 MiB; free=");
    logDec(allocator.freePages() * GTT_PAGE_SIZE / (1024 * 1024));
    log(" MiB\n");
}
bool IntelGen9Driver::initializeGtt() {
    if (gttInitialized) {
        return true;
    }
    if (!initialized || !mmio) {
        log("[i915] gtt: the driver is not initialized\n");
        return false;
    }
    if (!gsm || gttEntries == 0) {
        log("[i915] gtt: GGC reports no GTT\n");
        return false;
    }
    stolenBase = decodeBdsm(readConfig32(PCI_BDSM));
    log("[i915] gtt GGC=");
    logHex(readConfig16(PCI_GGC));
    log(" gsm=");
    logDec(ggc.gsmBytes >> 20);
    log(" MiB ggtt=");
    logDec(ggc.ggttBytes >> 20);
    log(" MiB stolen=");
    logHex(stolenBase);
    log(" + ");
    logDec(ggc.stolenBytes >> 20);
    log(" MiB aperture=");
    logHex(gmadr);
    log("\n");

    // The firmware's plane must already be in the GTT, backed by stolen
    // memory; if not, this is not the layout we understand: touch nothing.
    const uint32_t planeIndex = static_cast<uint32_t>(display.surfaceGtt / GTT_PAGE_SIZE);
    const uint64_t planePte = planeIndex < gttEntries ? readPte(planeIndex) : 0;
    log("[i915] gtt plane surface ");
    logHex(display.surfaceGtt);
    log(" entry=");
    logHex(planePte);
    log("\n");
    if (!pteValid(planePte) || ptePhys(planePte) < stolenBase ||
        ptePhys(planePte) >= stolenBase + ggc.stolenBytes) {
        log("[i915] gtt: the plane's entry is not in stolen memory; not using the GTT\n");
        return false;
    }
    if (gmadr == 0) {
        log("[i915] gtt: no aperture\n");
        return false;
    }

    auto* bitmap = static_cast<uint64_t*>(kmalloc(GttPageAllocator::wordsFor(gttEntries) * sizeof(uint64_t)));
    scratchPage = bitmap ? PMM::AllocFrame() : 0;
    if (scratchPage == 0) {
        kfree(bitmap);
        log("[i915] gtt: out of memory\n");
        return false;
    }
    auto* scratch = reinterpret_cast<uint64_t*>(VMM::PhysToVirt(scratchPage));
    for (uint64_t i = 0; i < GTT_PAGE_SIZE / sizeof(uint64_t); ++i) {
        scratch[i] = 0;
    }
    flushPage(scratchPage);

    allocator.init(bitmap, gttEntries);
    reserveFirmwareRanges();
    gttInitialized = true;
    return true;
}

GpuBuffer* IntelGen9Driver::createBuffer(uint64_t bytes, uint64_t alignment, bool mappable) {
    if (!gttInitialized || bytes == 0) {
        return nullptr;
    }
    if (alignment < GTT_PAGE_SIZE) {
        alignment = GTT_PAGE_SIZE;
    }
    const uint64_t pageCount = (bytes + GTT_PAGE_SIZE - 1) / GTT_PAGE_SIZE;
    if (pageCount > gttEntries) {
        return nullptr;
    }
    uint32_t limit = 0;
    if (mappable) {
        const uint64_t aperturePages = APERTURE_USABLE_BYTES / GTT_PAGE_SIZE;
        limit = static_cast<uint32_t>(aperturePages < gttEntries ? aperturePages : gttEntries);
    }
    const uint32_t start = allocator.alloc(static_cast<uint32_t>(pageCount),
                                           static_cast<uint32_t>(alignment / GTT_PAGE_SIZE), limit);
    if (start == GttPageAllocator::kNone) {
        return nullptr;
    }

    auto* buffer = static_cast<GpuBuffer*>(kmalloc(sizeof(GpuBuffer)));
    auto* pages = static_cast<uint64_t*>(kmalloc(pageCount * sizeof(uint64_t)));
    if (!buffer || !pages) {
        kfree(buffer);
        kfree(pages);
        allocator.free(start, static_cast<uint32_t>(pageCount));
        return nullptr;
    }
    buffer->gttOffset = static_cast<uint64_t>(start) * GTT_PAGE_SIZE;
    buffer->size = pageCount * GTT_PAGE_SIZE;
    buffer->pageCount = static_cast<uint32_t>(pageCount);
    buffer->pages = pages;
    buffer->aperture = nullptr;

    for (uint64_t i = 0; i < pageCount; ++i) {
        const uint64_t phys = PMM::AllocFrame();
        if (phys == 0) {
            buffer->pageCount = static_cast<uint32_t>(i);
            destroyBuffer(buffer);
            return nullptr;
        }
        auto* words = reinterpret_cast<uint64_t*>(VMM::PhysToVirt(phys));
        for (uint64_t w = 0; w < GTT_PAGE_SIZE / sizeof(uint64_t); ++w) {
            words[w] = 0;
        }
        flushPage(phys);
        pages[i] = phys;
        writePte(start + static_cast<uint32_t>(i), encodePte(phys));
        if ((i & 0x3FF) == 0) {
            TLB::pollPending();
        }
    }
    invalidateGtt();
    if (mappable) {
        buffer->aperture = reinterpret_cast<void*>(gmadr + buffer->gttOffset);
    }
    return buffer;
}

void IntelGen9Driver::destroyBuffer(GpuBuffer* buffer) {
    if (!buffer) {
        return;
    }
    const uint32_t start = static_cast<uint32_t>(buffer->gttOffset / GTT_PAGE_SIZE);
    const uint64_t scratchPte = encodePte(scratchPage);
    for (uint32_t i = 0; i < buffer->size / GTT_PAGE_SIZE; ++i) {
        writePte(start + i, scratchPte);
    }
    invalidateGtt();
    for (uint32_t i = 0; i < buffer->pageCount; ++i) {
        PMM::FreeFrame(buffer->pages[i]);
    }
    allocator.free(start, static_cast<uint32_t>(buffer->size / GTT_PAGE_SIZE));
    kfree(buffer->pages);
    kfree(buffer);
}

uint64_t IntelGen9Driver::gttFreeBytes() const {
    return gttInitialized ? allocator.freePages() * GTT_PAGE_SIZE : 0;
}

uint64_t IntelGen9Driver::gttTotalBytes() const {
    return gttInitialized ? static_cast<uint64_t>(gttEntries) * GTT_PAGE_SIZE : 0;
}

bool IntelGen9Driver::flipPlane(uint64_t gttOffset) {
    if (!initialized || (gttOffset % PLANE_SURF_ALIGN) != 0) {
        return false;
    }
    // PLANE_SURF is double-buffered: the write arms it and the display takes
    // it at the next vblank, which PLANE_SURFLIVE shows.
    writeReg(REG_PLANE_SURF_A, static_cast<uint32_t>(gttOffset));
    const uint64_t deadline = time_get_uptime_ms() + 100;
    while (time_get_uptime_ms() < deadline) {
        if ((readReg(REG_PLANE_SURFLIVE_A) & ~0xFFFu) == static_cast<uint32_t>(gttOffset)) {
            return true;
        }
        TLB::pollPending();
    }
    return false;
}

} // namespace intel_gen9
