#include <cpu/klock.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/percpu.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/tlb.hpp>

// Define to collect per-entry-site lock statistics, printed to serial every
// kProfileDumpEvery acquisitions by the boot CPU.
// #define KLOCK_PROFILE

namespace {

volatile uint32_t s_locked = 0;
volatile uint32_t s_owner = 0;   // owning CPU index + 1; 0 while free
uint32_t s_depth = 0;            // nesting depth; only the owner touches it

// Pause iterations before a wait is reported (same scale as Spinlock).
constexpr uint64_t kLongSpinIterations = 1ULL << 27;

uint32_t self() {
    return currentCpuIndex() + 1;
}

void writeDec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos > 0);
    Cereal::get().write(&buf[pos]);
}

void report(const char* what) {
    const uint32_t owner = __atomic_load_n(&s_owner, __ATOMIC_RELAXED);
    Cereal::get().write("[klock] ");
    Cereal::get().write(what);
    Cereal::get().write(" cpu=");
    writeDec(currentCpuIndex());
    Cereal::get().write(" owner_cpu=");
    if (owner == 0) {
        Cereal::get().write("none");
    } else {
        writeDec(owner - 1);
    }
    Cereal::get().write("\n");
}

#ifdef KLOCK_PROFILE
struct SiteStats {
    uint64_t count;
    uint64_t contended;
    uint64_t spinCycles;
    uint64_t holdCycles;
};
SiteStats s_stats[KernelLock::kSiteCount];
uint64_t s_total = 0;
uint32_t s_holdSite = 0;      // owner only
uint64_t s_holdStart = 0;     // owner only
constexpr uint64_t kProfileDumpEvery = 1ULL << 14;

uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void writeSite(uint32_t site) {
    if (site < KernelLock::kSiteIrq) {
        Cereal::get().write("sys ");
        writeDec(site);
    } else if (site < KernelLock::kSiteException) {
        Cereal::get().write("irq ");
        writeDec(site - KernelLock::kSiteIrq);
    } else if (site < KernelLock::kSiteIdlePoll) {
        Cereal::get().write("exc ");
        writeDec(site - KernelLock::kSiteException);
    } else if (site == KernelLock::kSiteIdlePoll) {
        Cereal::get().write("idle-poll");
    } else if (site == KernelLock::kSiteResume) {
        Cereal::get().write("resume");
    } else {
        Cereal::get().write("other");
    }
}

// Under the lock. Prints the sites with the most spin and hold time.
void dumpProfile() {
    uint64_t spinTotal = 0, holdTotal = 0;
    for (const SiteStats& st : s_stats) {
        spinTotal += st.spinCycles;
        holdTotal += st.holdCycles;
    }
    Cereal::get().write("[klock-prof] acquisitions=");
    writeDec(s_total);
    Cereal::get().write(" spin_mcyc=");
    writeDec(spinTotal / 1000000);
    Cereal::get().write(" hold_mcyc=");
    writeDec(holdTotal / 1000000);
    Cereal::get().write("\n");
    bool shown[KernelLock::kSiteCount] = {};
    for (int rank = 0; rank < 12; ++rank) {
        uint32_t best = KernelLock::kSiteCount;
        for (uint32_t i = 0; i < KernelLock::kSiteCount; ++i) {
            if (shown[i] || s_stats[i].count == 0) continue;
            if (best == KernelLock::kSiteCount ||
                s_stats[i].holdCycles + s_stats[i].spinCycles >
                    s_stats[best].holdCycles + s_stats[best].spinCycles) {
                best = i;
            }
        }
        if (best == KernelLock::kSiteCount) break;
        shown[best] = true;
        const SiteStats& st = s_stats[best];
        Cereal::get().write("[klock-prof]   ");
        writeSite(best);
        Cereal::get().write(": n=");
        writeDec(st.count);
        Cereal::get().write(" contended=");
        writeDec(st.contended);
        Cereal::get().write(" spin_kcyc=");
        writeDec(st.spinCycles / 1000);
        Cereal::get().write(" hold_kcyc=");
        writeDec(st.holdCycles / 1000);
        Cereal::get().write("\n");
    }
}
#endif

void unlock() {
#ifdef KLOCK_PROFILE
    if (s_holdSite < KernelLock::kSiteCount) {
        s_stats[s_holdSite].holdCycles += rdtsc() - s_holdStart;
    }
#endif
    __atomic_store_n(&s_owner, 0, __ATOMIC_RELAXED);
    s_depth = 0;
    __atomic_clear(&s_locked, __ATOMIC_RELEASE);
}

} // namespace

namespace KernelLock {

// Bookkeeping once this CPU has taken the free lock (outermost entry).
static void acquired(uint32_t me, uint32_t site, uint64_t spinStart, bool contended) {
    __atomic_store_n(&s_owner, me, __ATOMIC_RELAXED);
    s_depth = 1;

#ifdef KLOCK_PROFILE
    if (site >= kSiteCount) site = kSiteOther;
    s_holdStart = rdtsc();
    s_holdSite = site;
    SiteStats& st = s_stats[site];
    ++st.count;
    if (contended) {
        ++st.contended;
        st.spinCycles += s_holdStart - spinStart;
    }
    if ((++s_total % kProfileDumpEvery) == 0) {
        dumpProfile();
    }
#else
    (void)site;
    (void)spinStart;
    (void)contended;
#endif

    // Outermost entry on this CPU: it is now off whatever kernel stack it last
    // returned to user mode from, so a process parked there can be released.
    Scheduler::get().onKernelEntry();
}

void enter(uint32_t site) {
    const uint32_t me = self();
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == me) {
        ++s_depth;
        return;
    }

#ifdef KLOCK_PROFILE
    const uint64_t spinStart = rdtsc();
#else
    const uint64_t spinStart = 0;
#endif
    bool contended = false;
    uint64_t spins = 0;
    while (__atomic_test_and_set(&s_locked, __ATOMIC_ACQUIRE)) {
        contended = true;
        while (__atomic_load_n(&s_locked, __ATOMIC_RELAXED)) {
            // The holder may be waiting for this CPU to flush its TLB.
            TLB::pollPending();
            asm volatile("pause" ::: "memory");
            if (++spins == kLongSpinIterations) {
                report("long wait");
            }
        }
    }
    acquired(me, site, spinStart, contended);
}

bool tryEnter(uint32_t site) {
    const uint32_t me = self();
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == me) {
        ++s_depth;
        return true;
    }
    if (__atomic_load_n(&s_locked, __ATOMIC_RELAXED) ||
        __atomic_test_and_set(&s_locked, __ATOMIC_ACQUIRE)) {
        return false;
    }
    acquired(me, site, 0, false);
    return true;
}

void leave() {
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) != self()) {
        report("leave without holding the lock");
        return;
    }
    if (--s_depth == 0) {
        unlock();
    }
}

void releaseAll() {
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self()) {
        unlock();
    }
}

uint32_t depth() {
    return __atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self() ? s_depth : 0;
}

void setDepth(uint32_t depth) {
    if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) != self()) {
        report("setDepth without holding the lock");
        return;
    }
    if (depth == 0) {
        unlock();
        return;
    }
    s_depth = depth;
}

bool heldByThisCpu() {
    return __atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self();
}

uint32_t suspend() {
    const uint32_t saved = depth();
    if (saved != 0) {
        unlock();
    }
    return saved;
}

void resume(uint32_t savedDepth) {
    if (savedDepth == 0) {
        return;
    }
    enter(kSiteResume);
    s_depth = savedDepth;
}

} // namespace KernelLock

extern "C" void kernelLockExitToUser() {
    KernelLock::releaseAll();
}
