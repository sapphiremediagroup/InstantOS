#include <cpu/cereal/cereal.hpp>
#include <memory/pmm.hpp>

namespace {
struct KnownReservation {
    uint64_t start;
    uint64_t end;
    const char* label;
};

KnownReservation g_nullReservation = {0, 0, "null-page guard"};
KnownReservation g_kernelReservation = {0, 0, "kernel"};
KnownReservation g_mmapReservation = {0, 0, "memory map"};
KnownReservation g_bitmapReservation = {0, 0, "bitmap"};

uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }

    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) {
        return UINT64_MAX & ~mask;
    }
    return (value + mask) & ~mask;
}

bool ranges_overlap(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd) {
    return aStart < bEnd && bStart < aEnd;
}

bool range_contains(uint64_t outerStart, uint64_t outerEnd, uint64_t innerStart, uint64_t innerEnd) {
    return outerStart <= innerStart && outerEnd >= innerEnd;
}

void append_char(char*& cursor, char* end, char ch) {
    if (cursor < end) {
        *cursor++ = ch;
    }
}

void append_str(char*& cursor, char* end, const char* str) {
    if (!str) {
        return;
    }

    while (*str && cursor < end) {
        *cursor++ = *str++;
    }
}

void append_hex64(char*& cursor, char* end, uint64_t value) {
    static const char* digits = "0123456789ABCDEF";
    append_str(cursor, end, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        append_char(cursor, end, digits[(value >> shift) & 0xF]);
    }
}

void append_u64_dec(char*& cursor, char* end, uint64_t value) {
    char tmp[32];
    int idx = 0;

    if (value == 0) {
        append_char(cursor, end, '0');
        return;
    }

    while (value != 0 && idx < 31) {
        tmp[idx++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (idx > 0) {
        append_char(cursor, end, tmp[--idx]);
    }
}

void print_reservation_line(uint64_t start, uint64_t end, const char* label) {
    char buffer[160];
    char* cursor = buffer;
    char* limit = buffer + sizeof(buffer) - 1;

    append_str(cursor, limit, "[PMM] ");
    append_hex64(cursor, limit, start);
    append_str(cursor, limit, " - ");
    append_hex64(cursor, limit, end);
    append_str(cursor, limit, "  ");
    append_str(cursor, limit, label ? label : "(reserved)");
    append_char(cursor, limit, '\n');
    *cursor = '\0';

    Cereal::get().write(buffer);
}

const char* classify_run(uint64_t start, uint64_t end) {
    if (start == g_nullReservation.start && end == g_nullReservation.end) {
        return g_nullReservation.label;
    }
    if (start == g_kernelReservation.start && end == g_kernelReservation.end) {
        return g_kernelReservation.label;
    }
    if (start == g_mmapReservation.start && end == g_mmapReservation.end) {
        return g_mmapReservation.label;
    }
    if (start == g_bitmapReservation.start && end == g_bitmapReservation.end) {
        static char label[32];
        char* cursor = label;
        char* limit = label + sizeof(label) - 1;

        append_str(cursor, limit, "bitmap (");
        append_u64_dec(cursor, limit, g_bitmapReservation.end >= g_bitmapReservation.start
            ? (g_bitmapReservation.end - g_bitmapReservation.start + 1) / 1024
            : 0);
        append_str(cursor, limit, " KiB)");
        *cursor = '\0';
        return label;
    }
    return "(reserved)";
}

const char* memory_type_name(MemoryType type) {
    switch (type) {
    case MemoryType::Unusable:        return "Unusable";
    case MemoryType::Free:            return "Free";
    case MemoryType::KernelCode:      return "KernelCode";
    case MemoryType::KernelData:      return "KernelData";
    case MemoryType::BootloaderData:  return "BootloaderData";
    case MemoryType::AcpiReclaimable: return "AcpiReclaimable";
    case MemoryType::AcpiNvs:         return "AcpiNvs";
    case MemoryType::MmioRegion:      return "MmioRegion";
    case MemoryType::Reserved:        return "Reserved";
    default:                          return "Unknown";
    }
}

void print_region_debug(const MemoryRegion& region, const char* note) {
    char buffer[224];
    char* cursor = buffer;
    char* limit = buffer + sizeof(buffer) - 1;

    append_str(cursor, limit, "[PMM] region ");
    append_hex64(cursor, limit, region.base);
    append_str(cursor, limit, " - ");
    append_hex64(cursor, limit, region.end() ? region.end() - 1 : 0);
    append_str(cursor, limit, "  ");
    append_str(cursor, limit, memory_type_name(region.type));
    append_str(cursor, limit, "  ");
    append_u64_dec(cursor, limit, region.bytes() / 1024);
    append_str(cursor, limit, " KiB");
    if (note && *note) {
        append_str(cursor, limit, "  ");
        append_str(cursor, limit, note);
    }
    append_char(cursor, limit, '\n');
    *cursor = '\0';

    Cereal::get().write(buffer);
}

[[noreturn]] void fail_bitmap_placement(uint64_t bitmapBytes) {
    char buffer[160];
    char* cursor = buffer;
    char* limit = buffer + sizeof(buffer) - 1;

    append_str(cursor, limit, "[PMM] fatal: no safe bitmap placement for ");
    append_u64_dec(cursor, limit, bitmapBytes);
    append_str(cursor, limit, " bytes\n");
    *cursor = '\0';
    Cereal::get().write(buffer);

    while (true) {
        asm volatile("hlt");
    }
}

void dump_memory_map_summary(const MemoryMap& map, uint64_t bitmapBytes) {
    char buffer[160];
    char* cursor = buffer;
    char* limit = buffer + sizeof(buffer) - 1;

    append_str(cursor, limit, "[PMM] map count=");
    append_u64_dec(cursor, limit, map.count);
    append_str(cursor, limit, " bitmap=");
    append_u64_dec(cursor, limit, bitmapBytes / 1024);
    append_str(cursor, limit, " KiB\n");
    *cursor = '\0';
    Cereal::get().write(buffer);

    for (uint64_t i = 0; i < map.count; i++) {
        print_region_debug(map.regions[i], nullptr);
    }
}
}

// ── Static storage ────────────────────────────────────────────────────────
uint64_t* PMM::s_bitmap      = nullptr;
uint64_t  PMM::s_bitmapSize  = 0;
uint64_t  PMM::s_totalFrames = 0;
uint64_t  PMM::s_usedFrames  = 0;
bool      PMM::s_initialized = false;

// ── Bit-manipulation helpers ──────────────────────────────────────────────
void PMM::SetFrame(uint64_t frame) {
    s_bitmap[frame / 64] |= (1ULL << (frame % 64));
}

void PMM::ClearFrame(uint64_t frame) {
    s_bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}

bool PMM::TestFrame(uint64_t frame) {
    return (s_bitmap[frame / 64] & (1ULL << (frame % 64))) != 0;
}

// ── Initialize ────────────────────────────────────────────────────────────
void PMM::Initialize(const MemoryMap& map, uint64_t kernelBase, uint64_t kernelSize) {
    s_bitmap = nullptr;
    s_bitmapSize = 0;
    s_totalFrames = 0;
    s_usedFrames = 0;
    s_initialized = false;

    g_nullReservation = {0, 0, "null-page guard"};
    g_kernelReservation = {0, 0, "kernel"};
    g_mmapReservation = {0, 0, "memory map"};
    g_bitmapReservation = {0, 0, "bitmap"};

    uint64_t highestAddr = 0;
    for (uint64_t i = 0; i < map.count; i++) {
        uint64_t end = map.regions[i].end();
        if (end > highestAddr) {
            highestAddr = end;
        }
    }

    s_totalFrames = align_up(highestAddr, PAGE_SIZE) / PAGE_SIZE;
    s_bitmapSize = (s_totalFrames + 63) / 64;
    uint64_t bitmapBytes = s_bitmapSize * sizeof(uint64_t);

    uint64_t kernelStart = kernelBase & ~(PAGE_SIZE - 1);
    uint64_t kernelEnd = align_up(kernelBase + kernelSize, PAGE_SIZE);
    uint64_t mmapStart = reinterpret_cast<uint64_t>(map.regions) & ~(PAGE_SIZE - 1);
    uint64_t mmapBytes = map.count * sizeof(MemoryRegion);
    uint64_t mmapEnd = align_up(reinterpret_cast<uint64_t>(map.regions) + mmapBytes, PAGE_SIZE);

    for (uint64_t i = 0; i < map.count; i++) {
        const MemoryRegion& candidate = map.regions[i];
        if (candidate.type != MemoryType::Free) {
            continue;
        }

        uint64_t regionStart = candidate.base;
        uint64_t regionEnd = candidate.end();
        if (regionEnd < regionStart) {
            continue;
        }

        uint64_t regionBytes = regionEnd - regionStart;
        if (regionBytes < bitmapBytes) {
            continue;
        }

        uint64_t placementStart = regionStart;
        if (placementStart < PAGE_SIZE) {
            placementStart = PAGE_SIZE;
        }

        if (placementStart > regionEnd || regionEnd - placementStart < bitmapBytes) {
            continue;
        }

        if (ranges_overlap(placementStart, placementStart + bitmapBytes, kernelStart, kernelEnd)) {
            if (!range_contains(regionStart, regionEnd, kernelStart, kernelEnd)) {
                continue;
            }
            placementStart = align_up(kernelEnd, PAGE_SIZE);
        }

        if (placementStart > regionEnd || regionEnd - placementStart < bitmapBytes) {
            continue;
        }

        if (ranges_overlap(placementStart, placementStart + bitmapBytes, mmapStart, mmapEnd)) {
            if (!range_contains(regionStart, regionEnd, mmapStart, mmapEnd)) {
                continue;
            }
            placementStart = align_up(mmapEnd, PAGE_SIZE);
        }

        if (placementStart > regionEnd || regionEnd - placementStart < bitmapBytes) {
            continue;
        }

        const MemoryRegion& rechecked = map.regions[i];
        if (rechecked.type != MemoryType::Free) {
            continue;
        }

        s_bitmap = reinterpret_cast<uint64_t*>(placementStart);
        break;
    }

    if (!s_bitmap) {
        dump_memory_map_summary(map, bitmapBytes);
        fail_bitmap_placement(bitmapBytes);
    }

    // Step 1: mark every tracked frame used before selectively freeing RAM.
    for (uint64_t i = 0; i < s_bitmapSize; i++) {
        s_bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
    }
    s_usedFrames = s_totalFrames;

    // Step 2: clear only descriptors the bootloader classified as Free.
    for (uint64_t i = 0; i < map.count; i++) {
        const MemoryRegion& region = map.regions[i];
        if (region.type != MemoryType::Free) {
            continue;
        }

        uint64_t startFrame = region.base / PAGE_SIZE;
        uint64_t endFrame = startFrame + region.pages;
        if (endFrame < startFrame || endFrame > s_totalFrames) {
            endFrame = s_totalFrames;
        }

        for (uint64_t frame = startFrame; frame < endFrame; frame++) {
            if (TestFrame(frame)) {
                ClearFrame(frame);
                s_usedFrames--;
            }
        }
    }

    // Step 3a: reserve the loaded kernel image so the allocator never returns it.
    uint64_t kernelStartFrame = kernelStart / PAGE_SIZE;
    uint64_t kernelEndFrame = kernelEnd / PAGE_SIZE;
    for (uint64_t frame = kernelStartFrame; frame < kernelEndFrame && frame < s_totalFrames; frame++) {
        if (!TestFrame(frame)) {
            SetFrame(frame);
            s_usedFrames++;
        }
    }
    g_kernelReservation.start = kernelStart;
    g_kernelReservation.end = kernelEnd ? kernelEnd - 1 : 0;

    // Step 3b: reserve the copied memory map array because the kernel still reads it.
    uint64_t mmapStartFrame = mmapStart / PAGE_SIZE;
    uint64_t mmapEndFrame = mmapEnd / PAGE_SIZE;
    for (uint64_t frame = mmapStartFrame; frame < mmapEndFrame && frame < s_totalFrames; frame++) {
        if (!TestFrame(frame)) {
            SetFrame(frame);
            s_usedFrames++;
        }
    }
    g_mmapReservation.start = mmapStart;
    g_mmapReservation.end = mmapEnd ? mmapEnd - 1 : 0;

    // Step 3c: reserve the PMM bitmap storage because it lives inside physical RAM.
    uint64_t bitmapStart = reinterpret_cast<uint64_t>(s_bitmap) & ~(PAGE_SIZE - 1);
    uint64_t bitmapEnd = align_up(reinterpret_cast<uint64_t>(s_bitmap) + bitmapBytes, PAGE_SIZE);
    uint64_t bitmapStartFrame = bitmapStart / PAGE_SIZE;
    uint64_t bitmapEndFrame = bitmapEnd / PAGE_SIZE;
    for (uint64_t frame = bitmapStartFrame; frame < bitmapEndFrame && frame < s_totalFrames; frame++) {
        if (!TestFrame(frame)) {
            SetFrame(frame);
            s_usedFrames++;
        }
    }
    g_bitmapReservation.start = bitmapStart;
    g_bitmapReservation.end = bitmapEnd ? bitmapEnd - 1 : 0;

    // Step 3d: reserve frame zero as a null-page guard.
    if (s_totalFrames > 0 && !TestFrame(0)) {
        SetFrame(0);
        s_usedFrames++;
    }
    g_nullReservation.start = 0;
    g_nullReservation.end = PAGE_SIZE - 1;

    s_initialized = true;
}

// ── FindFirstFree ─────────────────────────────────────────────────────────
uint64_t PMM::FindFirstFree(uint64_t startFrame) {
    uint64_t startIdx = startFrame / 64;
    for (uint64_t i = startIdx; i < s_bitmapSize; i++) {
        if (s_bitmap[i] == 0xFFFFFFFFFFFFFFFFULL)
            continue;  // Entire qword is full

        // At least one bit is clear – find it
        for (uint64_t bit = 0; bit < 64; bit++) {
            uint64_t frame = i * 64 + bit;
            if (frame < startFrame)
                continue;
            if (frame >= s_totalFrames)
                return 0;
            if (!(s_bitmap[i] & (1ULL << bit)))
                return frame;
        }
    }
    return 0;  // Nothing free (0 is also reserved, so acts as "null")
}

// ── FindContiguous ────────────────────────────────────────────────────────
uint64_t PMM::FindContiguous(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return FindFirstFree(1);

    uint64_t runStart = 0;
    uint64_t runLen   = 0;
    uint64_t frame    = 1;  // Skip frame 0

    while (frame < s_totalFrames) {
        if (!TestFrame(frame)) {
            if (runLen == 0)
                runStart = frame;
            runLen++;
            if (runLen == count)
                return runStart;
            frame++;
        } else {
            runLen = 0;
            frame++;
        }
    }
    return 0;
}

// ── AllocFrame ────────────────────────────────────────────────────────────
uint64_t PMM::AllocFrame() {
    if (!s_initialized) return 0;

    uint64_t frame = FindFirstFree(1);  // Skip frame 0
    if (frame == 0) return 0;

    SetFrame(frame);
    s_usedFrames++;
    return frame * PAGE_SIZE;
}

// ── FreeFrame ─────────────────────────────────────────────────────────────
void PMM::FreeFrame(uint64_t physAddr) {
    if (!s_initialized) return;

    uint64_t frame = physAddr / PAGE_SIZE;
    if (frame == 0 || frame >= s_totalFrames) return;

    if (TestFrame(frame)) {
        ClearFrame(frame);
        s_usedFrames--;
    }
}

// ── AllocFrames ───────────────────────────────────────────────────────────
uint64_t PMM::AllocFrames(uint64_t count) {
    if (!s_initialized || count == 0) return 0;

    uint64_t start = FindContiguous(count);
    if (start == 0) return 0;

    for (uint64_t f = start; f < start + count; f++) {
        SetFrame(f);
        s_usedFrames++;
    }
    return start * PAGE_SIZE;
}

// ── FreeFrames ────────────────────────────────────────────────────────────
void PMM::FreeFrames(uint64_t physAddr, uint64_t count) {
    if (!s_initialized || count == 0) return;

    uint64_t startFrame = physAddr / PAGE_SIZE;
    for (uint64_t f = startFrame; f < startFrame + count && f < s_totalFrames; f++) {
        if (TestFrame(f)) {
            ClearFrame(f);
            s_usedFrames--;
        }
    }
}

void PMM::ReserveRange(uint64_t physAddr, uint64_t bytes) {
    if (!s_initialized || bytes == 0) return;

    uint64_t start = physAddr & ~(PAGE_SIZE - 1);
    uint64_t end = physAddr + bytes;
    if (end < physAddr || end > UINT64_MAX - (PAGE_SIZE - 1)) {
        end = UINT64_MAX & ~(PAGE_SIZE - 1);
    } else {
        end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    uint64_t startFrame = start / PAGE_SIZE;
    uint64_t endFrame = end / PAGE_SIZE;
    if (endFrame > s_totalFrames) {
        endFrame = s_totalFrames;
    }

    for (uint64_t frame = startFrame; frame < endFrame; frame++) {
        if (!TestFrame(frame)) {
            SetFrame(frame);
            s_usedFrames++;
        }
    }
}

void PMM::DumpReservations() {
    if (!s_initialized || s_totalFrames == 0) {
        Cereal::get().write("[PMM] no reservations to dump\n");
        return;
    }

    uint64_t frame = 0;
    while (frame < s_totalFrames) {
        if (!TestFrame(frame)) {
            frame++;
            continue;
        }

        uint64_t runStart = frame;
        while (frame < s_totalFrames && TestFrame(frame)) {
            frame++;
        }

        uint64_t startPhys = runStart * PAGE_SIZE;
        uint64_t endPhys = (frame * PAGE_SIZE) - 1;
        print_reservation_line(startPhys, endPhys, classify_run(startPhys, endPhys));
    }
}

// ── Queries ───────────────────────────────────────────────────────────────
uint64_t PMM::TotalFrames()    { return s_totalFrames; }
uint64_t PMM::UsedFrames()     { return s_usedFrames; }
uint64_t PMM::FreeFrameCount() { return s_totalFrames - s_usedFrames; }

uint64_t PMM::TotalMemory()    { return s_totalFrames * PAGE_SIZE; }
uint64_t PMM::FreeMemory()     { return FreeFrameCount() * PAGE_SIZE; }
uint64_t PMM::UsedMemory()     { return s_usedFrames * PAGE_SIZE; }

bool     PMM::IsInitialized()  { return s_initialized; }
