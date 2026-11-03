// Tear-free presentation on plane A (see intel_gen9.hpp): the compositor draws
// into one of two scanout buffers while the other is shown, then flips. A
// PLANE_SURF write is double-buffered by the display, which takes it at the
// start of the next vertical blank (PLANE_SURFLIVE then shows it), so a
// frame is never scanned out half old, half new. The plane keeps the
// firmware's mode, format and stride; only its surface address changes.

#include <graphics/intel_gen9.hpp>
#include <graphics/intel_gen9_drm.hpp>

#include <cpu/cereal/cereal.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

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

void logDec(uint64_t value) {
    char digits[21];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char text[21];
    for (int i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = '\0';
    log(text);
}

} // namespace

bool IntelGen9Driver::displayAcquire(Process* owner, uint64_t userBase, uint64_t userStride, uint64_t* addresses) {
    if (!initialized || !owner || !addresses || !display.valid || !display.tiledLinear || display.bpp != 32) {
        return false;
    }
    if (!initializeGtt()) {
        return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(display.stride) * display.height;
    if (bytes > userStride) {
        return false;
    }
    for (uint32_t i = 0; i < kScanoutBuffers; ++i) {
        if (!scanout[i]) {
            scanout[i] = createBuffer(bytes, PLANE_SURF_ALIGN, false);
            if (!scanout[i]) {
                log("[i915] display: no room for a scanout buffer\n");
                return false;
            }
        }
    }
    // The display reads these pages without snooping the CPU's caches, so
    // the CPU writes them write-combining (they go straight to memory). The
    // frames stay the driver's: kSharedFrame keeps the process from freeing
    // them when it unmaps them or exits.
    const uint64_t flags = Present | ReadWrite | UserSuper | NoExecute | kSharedFrame | WriteCombine;
    for (uint32_t i = 0; i < kScanoutBuffers; ++i) {
        const uint64_t base = userBase + i * userStride;
        for (uint32_t page = 0; page < scanout[i]->pageCount; ++page) {
            VMM::MapPageInto(owner->getPageTable(), base + static_cast<uint64_t>(page) * GTT_PAGE_SIZE,
                             scanout[i]->pages[page], flags);
        }
        addresses[i] = base;
    }
    displayOwner = owner->getPID();
    log("[i915] display: pid ");
    logDec(displayOwner);
    log(" presents through ");
    logDec(kScanoutBuffers);
    log(" scanout buffers at gtt ");
    logHex(scanout[0]->gttOffset);
    log(" and ");
    logHex(scanout[1]->gttOffset);
    log("\n");
    return true;
}

// The scanout buffers are made once and never freed, so the dma-buf may
// borrow their pages.
int64_t IntelGen9Driver::displayExportBuffer(uint32_t index) {
    if (index >= kScanoutBuffers || !scanout[index]) {
        return -22;   // EINVAL
    }
    return drm::exportScanoutPages(scanout[index]->pages, scanout[index]->pageCount);
}

bool IntelGen9Driver::displayFlip(uint32_t index) {
    if (index >= kScanoutBuffers || !scanout[index]) {
        return false;
    }
    presentedSurface = scanout[index]->gttOffset;
    // Held back (only recorded) while gpu-test's pattern or a watchdog
    // report is on screen.
    if (!displayTestActive && !emergencyShown) {
        // The caller's write-combined stores reach memory before the display
        // is pointed at them.
        asm volatile("sfence" ::: "memory");
        writeReg(REG_PLANE_SURF_A, static_cast<uint32_t>(presentedSurface));
    }
    return true;
}

bool IntelGen9Driver::displayWait(uint64_t ms) {
    Process* current = Scheduler::get().getCurrentProcess();
    const uint64_t deadline = time_get_uptime_ms() + ms;
    for (;;) {
        // While gpu-test's pattern or a watchdog report is shown, neither
        // buffer is on screen.
        if (displayTestActive || emergencyShown || presentedSurface == ~0ULL) {
            return true;
        }
        if ((readReg(REG_PLANE_SURFLIVE_A) & ~0xFFFu) == static_cast<uint32_t>(presentedSurface)) {
            return true;
        }
        const uint64_t now = time_get_uptime_ms();
        if (now >= deadline || !current) {
            log("[i915] display: the flip to gtt ");
            logHex(presentedSurface);
            log(" was not on screen after ");
            logDec(ms);
            log(" ms (PLANE_SURFLIVE ");
            logHex(readReg(REG_PLANE_SURFLIVE_A));
            log(")\n");
            return false;
        }
        // A frame is 16.7 ms at 60 Hz; the tick wakes us within 1 ms of it.
        current->sleepUntil(now + 1);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();
    }
}

void IntelGen9Driver::displayRelease() {
    presentedSurface = ~0ULL;
    if (!displayTestActive && initialized) {
        writeReg(REG_PLANE_SURF_A, static_cast<uint32_t>(display.surfaceGtt));
    }
    log("[i915] display: back to the firmware framebuffer\n");
}

void IntelGen9Driver::displayProcessGone(uint32_t pid) {
    if (pid == 0 || pid != displayOwner) {
        return;
    }
    displayOwner = 0;
    displayRelease();
}

void IntelGen9Driver::displayTestBegin() {
    displayTestActive = true;
}

bool IntelGen9Driver::displayTestEnd() {
    const uint64_t surface = presentedSurface == ~0ULL ? display.surfaceGtt : presentedSurface;
    const bool back = flipPlane(surface);
    displayTestActive = false;
    return back;
}

void IntelGen9Driver::showFirmwareSurfaceUnlocked() {
    if (initialized && mmio && presentedSurface != ~0ULL) {
        emergencyShown = true;
        writeReg(REG_PLANE_SURF_A, static_cast<uint32_t>(display.surfaceGtt));
    }
}

void IntelGen9Driver::restorePresentedSurfaceUnlocked() {
    if (!emergencyShown) {
        return;
    }
    emergencyShown = false;
    if (initialized && mmio && !displayTestActive && presentedSurface != ~0ULL) {
        writeReg(REG_PLANE_SURF_A, static_cast<uint32_t>(presentedSurface));
    }
}

} // namespace intel_gen9
