// MemBench (162, root only): time one of the kernel's memory routines on its
// own (common/string.cpp's memaccel variants), so that which one memcpy() &
// co. pick, and from what size, rests on measurements. tools/membench drives
// it and prints the table.
//
//   spec   = op | variant << 8 | destination offset << 16 | source offset << 24
//   size   = bytes per call (memset32: bytes too, a multiple of 4)
//   reps   = calls per timed run
//   result = the best of kRuns timed runs, in TSC cycles, or -errno
//
// op Info returns cycles_per_us (0 when the TSC was not calibrated). Every case
// is short: the syscall runs with interrupts off and holds the big kernel lock,
// and the watchdog reports a boot CPU whose tick stops for 2 s. The result of
// each case is checked afterwards (-EIO if wrong).
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <common/ports.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace {
enum MemBenchOp : uint64_t { Memcpy = 0, Memset = 1, Memset32 = 2, Memcmp = 3, Info = 255 };
constexpr uint64_t kMaxBytes = 8ULL * 1024 * 1024;
constexpr uint64_t kBufferBytes = kMaxBytes + 4096;   // room for the offsets
constexpr int kRuns = 5;

uint8_t* gSrc = nullptr;
uint8_t* gDst = nullptr;

bool ensureBuffers() {
    if (gSrc && gDst) {
        return true;
    }
    const uint64_t frames = kBufferBytes / PMM::PAGE_SIZE;
    const uint64_t a = PMM::AllocFramesAbove(frames, PMM::KERNEL_HIGH_ALLOC_MIN);
    const uint64_t b = a ? PMM::AllocFramesAbove(frames, PMM::KERNEL_HIGH_ALLOC_MIN) : 0;
    if (!a || !b) {
        return false;   // (a stays allocated; this is a root-only test)
    }
    gSrc = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(a));
    gDst = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(b));
    return true;
}

volatile int gSink = 0;

void runOnce(uint64_t op, memaccel::Variant v, uint8_t* dst, const uint8_t* src, uint64_t size) {
    switch (op) {
        case Memcpy:   memaccel::copy(v, dst, src, size); break;
        case Memset:   memaccel::fill(v, dst, 0x5A, size); break;
        case Memset32: memaccel::fill32(v, dst, 0xA1B2C3D4U, size / 4); break;
        case Memcmp:   gSink = gSink + memaccel::compare(v, dst, src, size); break;
    }
}

bool resultOk(uint64_t op, memaccel::Variant v, uint8_t* dst, const uint8_t* src, uint64_t size) {
    switch (op) {
        case Memcpy:
            for (uint64_t i = 0; i < size; i++) if (dst[i] != src[i]) return false;
            return true;
        case Memset:
            for (uint64_t i = 0; i < size; i++) if (dst[i] != 0x5A) return false;
            return true;
        case Memset32:
            for (uint64_t i = 0; i + 4 <= size; i += 4) {
                const uint32_t w = static_cast<uint32_t>(dst[i]) | static_cast<uint32_t>(dst[i + 1]) << 8 |
                                   static_cast<uint32_t>(dst[i + 2]) << 16 | static_cast<uint32_t>(dst[i + 3]) << 24;
                if (w != 0xA1B2C3D4U) return false;
            }
            return true;
        case Memcmp: {
            // Equal buffers compare equal; a difference in the last byte is found with its sign.
            if (memaccel::compare(v, dst, src, size) != 0) return false;
            dst[size - 1] = static_cast<uint8_t>(src[size - 1] + 1);
            const bool found = memaccel::compare(v, dst, src, size) > 0;
            dst[size - 1] = src[size - 1];
            return found;
        }
    }
    return false;
}
}  // namespace

uint64_t Syscall::sys_mem_bench(uint64_t spec, uint64_t size, uint64_t reps) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrPermission);
    }
    const uint64_t op = spec & 0xFF;
    if (op == Info) {
        return cycles_per_us;
    }
    const auto v = static_cast<memaccel::Variant>((spec >> 8) & 0xFF);
    const uint64_t dstOff = (spec >> 16) & 0xFF;
    const uint64_t srcOff = (spec >> 24) & 0xFF;
    if (op > Memcmp || static_cast<uint32_t>(v) > static_cast<uint32_t>(memaccel::Variant::Avx2) ||
        size == 0 || size > kMaxBytes || dstOff >= 64 || srcOff >= 64 ||
        reps == 0 || reps > (1u << 20) || (op == Memset32 && (size % 4) != 0)) {
        return syscall_error(SysErrInvalid);
    }
    if (!memaccel::supported(v)) {
        return syscall_error(SysErrNoDevice);
    }
    if (!ensureBuffers()) {
        return syscall_error(SysErrNoMemory);
    }
    uint8_t* dst = gDst + dstOff;
    uint8_t* src = gSrc + srcOff;
    for (uint64_t i = 0; i < size; i++) {
        src[i] = static_cast<uint8_t>(i * 131 + 7);
    }
    if (op == Memcmp) {
        memcpy(dst, src, size);
    }

    uint64_t best = ~0ULL;
    for (int run = 0; run < kRuns; run++) {
        const uint64_t t0 = rdtsc();
        for (uint64_t r = 0; r < reps; r++) {
            runOnce(op, v, dst, src, size);
        }
        const uint64_t t = rdtsc() - t0;
        if (t < best) {
            best = t;
        }
    }
    if (!resultOk(op, v, dst, src, size)) {
        return syscall_error(SysErrIO);
    }
    return best;
}
