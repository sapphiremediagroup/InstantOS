// GpuTest: hardware experiments on the Intel Gen9 GPU, run on request by
// /bin/gpu-test (root only), never at boot, so a mistake costs one test run
// rather than the PC's ability to boot. Each command logs what it does and
// ends with "[gpu-test] <command>: ok" or ": FAILED"; the syscall hands the
// caller everything the kernel logged meanwhile (also in /kernel.log).

#include <graphics/intel_gen9_drm.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/intel_gen9.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>

namespace {

using intel_gen9::GpuBuffer;
using intel_gen9::IntelGen9Driver;

constexpr uint64_t kPage = intel_gen9::GTT_PAGE_SIZE;

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

void flushLines(const volatile void* address, uint64_t bytes) {
    auto* line = static_cast<const volatile uint8_t*>(address);
    for (uint64_t offset = 0; offset < bytes; offset += 64) {
        asm volatile("clflush (%0)" : : "r"(line + offset) : "memory");
    }
    asm volatile("mfence" ::: "memory");
}

uint32_t patternWord(uint64_t index, uint32_t seed) {
    return static_cast<uint32_t>(index * 2654435761u) ^ seed;
}

// Writes through one path and reads back through the other: the aperture
// (the CPU through the GGTT) and the pages themselves (the direct map). Both
// directions agreeing means the GGTT entries point at the right pages.
uint64_t checkBothWays(GpuBuffer* buffer) {
    auto* aperture = static_cast<volatile uint32_t*>(buffer->aperture);
    const uint64_t words = buffer->size / sizeof(uint32_t);
    const uint64_t wordsPerPage = kPage / sizeof(uint32_t);
    uint64_t mismatches = 0;

    // Aperture -> pages.
    for (uint64_t i = 0; i < words; ++i) {
        aperture[i] = patternWord(i, 0xA5A50000u);
    }
    asm volatile("sfence" ::: "memory");
    (void)aperture[0];
    for (uint32_t page = 0; page < buffer->pageCount; ++page) {
        auto* direct = reinterpret_cast<volatile uint32_t*>(VMM::PhysToVirt(buffer->pages[page]));
        flushLines(direct, kPage);
        for (uint64_t w = 0; w < wordsPerPage; ++w) {
            if (direct[w] != patternWord(page * wordsPerPage + w, 0xA5A50000u)) {
                if (mismatches < 4) {
                    log("[gpu-test] aperture->page mismatch at page ");
                    logDec(page);
                    log(" word ");
                    logDec(w);
                    log(": ");
                    logHex(direct[w]);
                    log("\n");
                }
                ++mismatches;
            }
        }
    }

    // Pages -> aperture.
    for (uint32_t page = 0; page < buffer->pageCount; ++page) {
        auto* direct = reinterpret_cast<volatile uint32_t*>(VMM::PhysToVirt(buffer->pages[page]));
        for (uint64_t w = 0; w < wordsPerPage; ++w) {
            direct[w] = patternWord(page * wordsPerPage + w, 0x5A5A0000u);
        }
        flushLines(direct, kPage);
    }
    for (uint64_t i = 0; i < words; ++i) {
        if (aperture[i] != patternWord(i, 0x5A5A0000u)) {
            if (mismatches < 8) {
                log("[gpu-test] page->aperture mismatch at word ");
                logDec(i);
                log(": ");
                logHex(aperture[i]);
                log("\n");
            }
            ++mismatches;
        }
    }
    return mismatches;
}

bool runInfo(IntelGen9Driver& gpu) {
    if (!gpu.initializeGtt()) {
        return false;
    }
    log("[gpu-test] gtt free ");
    logDec(gpu.gttFreeBytes() >> 20);
    log(" MiB\n");
    return true;
}

bool runGtt(IntelGen9Driver& gpu) {
    if (!gpu.initializeGtt()) {
        return false;
    }
    const uint64_t freeBefore = gpu.gttFreeBytes();
    bool ok = true;

    // A mappable buffer, checked both ways through the aperture.
    GpuBuffer* mappable = gpu.createBuffer(4ULL << 20, kPage, true);
    if (!mappable) {
        log("[gpu-test] could not create a 4 MiB mappable buffer\n");
        return false;
    }
    log("[gpu-test] mappable 4 MiB at gtt ");
    logHex(mappable->gttOffset);
    log("\n");
    const uint64_t mismatches = checkBothWays(mappable);
    log("[gpu-test] aperture <-> pages: ");
    logDec(mismatches);
    log(" mismatches\n");
    ok = ok && mismatches == 0;

    // A large buffer above the aperture, aligned like a scanout surface; its
    // entries are read back.
    GpuBuffer* large = gpu.createBuffer(64ULL << 20, intel_gen9::PLANE_SURF_ALIGN, false);
    if (!large) {
        log("[gpu-test] could not create a 64 MiB buffer\n");
        ok = false;
    } else {
        log("[gpu-test] 64 MiB at gtt ");
        logHex(large->gttOffset);
        log(large->gttOffset % intel_gen9::PLANE_SURF_ALIGN == 0 ? " (aligned)\n" : " (MISALIGNED)\n");
        ok = ok && large->gttOffset % intel_gen9::PLANE_SURF_ALIGN == 0;
    }

    // Many small ones, freed in a different order, must leave the space as it was.
    GpuBuffer* small[16] = {};
    for (int i = 0; i < 16; ++i) {
        small[i] = gpu.createBuffer(static_cast<uint64_t>(i + 1) * kPage, kPage, i % 2 == 0);
        if (!small[i]) {
            log("[gpu-test] could not create a small buffer\n");
            ok = false;
        }
    }
    for (int i = 0; i < 16; i += 2) {
        gpu.destroyBuffer(small[i]);
    }
    for (int i = 1; i < 16; i += 2) {
        gpu.destroyBuffer(small[i]);
    }
    gpu.destroyBuffer(large);
    gpu.destroyBuffer(mappable);

    const uint64_t freeAfter = gpu.gttFreeBytes();
    log("[gpu-test] gtt free before ");
    logDec(freeBefore >> 10);
    log(" KiB, after ");
    logDec(freeAfter >> 10);
    log(" KiB\n");
    return ok && freeAfter == freeBefore;
}

// The flipped-to buffer, kept between FlipOn and FlipOff.
GpuBuffer* s_flipBuffer = nullptr;

// Colour bars under a white frame: the frame shows the geometry (all four
// edges visible), the bars the byte order.
void drawPattern(volatile uint32_t* pixels, uint32_t width, uint32_t height, uint32_t stride) {
    static constexpr uint32_t kBars[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    };
    const uint32_t wordsPerRow = stride / 4;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t colour = kBars[(x * 8) / width];
            const bool frame = x < 16 || y < 16 || x >= width - 16 || y >= height - 16;
            if (frame) {
                colour = 0xFFFFFF;
            } else if (y > height * 3 / 4) {
                const uint32_t level = (x * 255) / width;
                colour = (level << 16) | (level << 8) | level;   // grey ramp
            }
            pixels[static_cast<uint64_t>(y) * wordsPerRow + x] = colour;
        }
    }
    asm volatile("sfence" ::: "memory");
}

bool runFlipOn(IntelGen9Driver& gpu) {
    if (!gpu.initializeGtt()) {
        return false;
    }
    if (s_flipBuffer) {
        log("[gpu-test] already flipped\n");
        return true;
    }
    const intel_gen9::DisplayMode& mode = gpu.mode();
    const uint64_t bytes = static_cast<uint64_t>(mode.stride) * mode.height;
    s_flipBuffer = gpu.createBuffer(bytes, intel_gen9::PLANE_SURF_ALIGN, true);
    if (!s_flipBuffer) {
        log("[gpu-test] could not create the scanout buffer\n");
        return false;
    }
    drawPattern(static_cast<volatile uint32_t*>(s_flipBuffer->aperture), mode.width, mode.height, mode.stride);
    log("[gpu-test] plane A -> gtt ");
    logHex(s_flipBuffer->gttOffset);
    log("\n");
    // The compositor's flips are held back while the pattern shows.
    gpu.displayTestBegin();
    if (!gpu.flipPlane(s_flipBuffer->gttOffset)) {
        log("[gpu-test] the display did not take the new surface within 100 ms\n");
        gpu.displayTestEnd();
        gpu.destroyBuffer(s_flipBuffer);
        s_flipBuffer = nullptr;
        return false;
    }
    return true;
}

bool runFlipOff(IntelGen9Driver& gpu) {
    log("[gpu-test] plane A -> back to the desktop\n");
    const bool back = gpu.displayTestEnd();
    if (!back) {
        log("[gpu-test] the display did not take the desktop back within 100 ms\n");
    }
    if (s_flipBuffer && back) {
        gpu.destroyBuffer(s_flipBuffer);
        s_flipBuffer = nullptr;
    }
    return back;
}

} // namespace

uint64_t Syscall::sys_gpu_test(uint64_t command, uint64_t buffer, uint64_t length) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrPermission);
    }
    if (static_cast<GpuTestCommand>(command) == GpuTestCommand::DrmWithoutGpu) {
        drm::allowWithoutGpu();
        log("[gpu-test] /dev/dri/renderD128 now opens without an Intel GPU\n");
        return 0;
    }
    IntelGen9Driver& gpu = IntelGen9Driver::get();
    if (!gpu.isInitialized()) {
        return syscall_error(SysErrNoDevice);
    }
    if (buffer == 0 || length == 0) {
        return syscall_error(SysErrInvalid);
    }
    if (length > 64 * 1024) {
        length = 64 * 1024;
    }

    // The engine experiments reset and drive the engines: not while a
    // program's batch (EXECBUFFER2) runs on them.
    const GpuTestCommand which = static_cast<GpuTestCommand>(command);
    const bool usesEngines = which == GpuTestCommand::Engine || which == GpuTestCommand::Render ||
                             which == GpuTestCommand::Irq || which == GpuTestCommand::Ppgtt ||
                             which == GpuTestCommand::Hang;
    if (usesEngines && !gpu.claimEngines(0)) {
        return syscall_error(static_cast<SyscallErrno>(16));   // EBUSY
    }

    const uint64_t start = Cereal::get().logPosition();
    const char* name = "unknown";
    bool ok = false;
    switch (static_cast<GpuTestCommand>(command)) {
        case GpuTestCommand::Info:
            name = "info";
            ok = runInfo(gpu);
            break;
        case GpuTestCommand::Gtt:
            name = "gtt";
            ok = runGtt(gpu);
            break;
        case GpuTestCommand::FlipOn:
            name = "flip-on";
            ok = runFlipOn(gpu);
            break;
        case GpuTestCommand::FlipOff:
            name = "flip-off";
            ok = runFlipOff(gpu);
            break;
        case GpuTestCommand::Engine:
            name = "engine";
            ok = gpu.engineStoreTest(intel_gen9::EngineId::Blitter);
            break;
        case GpuTestCommand::Render:
            name = "render";
            ok = gpu.engineStoreTest(intel_gen9::EngineId::Render);
            break;
        case GpuTestCommand::Irq:
            name = "irq";
            ok = gpu.interruptTest();
            break;
        case GpuTestCommand::Ppgtt:
            name = "ppgtt";
            ok = gpu.ppgttTest();
            break;
        case GpuTestCommand::Hang:
            name = "hang";
            ok = gpu.engineHangTest();
            break;
        case GpuTestCommand::DrmState:
            name = "state";
            gpu.logRequestState();
            ok = true;
            break;
        default:
            if (usesEngines) gpu.releaseEngines();
            return syscall_error(SysErrInvalid);
    }
    if (usesEngines) {
        gpu.releaseEngines();
    }
    log("[gpu-test] ");
    log(name);
    log(ok ? ": ok\n" : ": FAILED\n");

    char* report = static_cast<char*>(kmalloc(length));
    if (!report) {
        return syscall_error(SysErrNoMemory);
    }
    const size_t count = Cereal::get().copyLogSince(start, report, length);
    const bool copied = copyToUser(buffer, report, count);
    kfree(report);
    return copied ? count : syscall_error(SysErrInvalid);
}
