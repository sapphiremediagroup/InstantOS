#include <cpu/tlb.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/percpu.hpp>
#include <cpu/process/process.hpp>
#include <cpu/smp.hpp>
#include <common/spinlock.hpp>
#include <memory/vmm.hpp>

namespace {

// Above this many pages a full flush is cheaper than invlpg per page.
constexpr uint64_t kMaxInvlpgPages = 32;
constexpr uint64_t kKernelHalf = 0xFFFF800000000000ULL;
constexpr uint64_t CR4_PGE = 1ULL << 7;
constexpr uint32_t kFixedDelivery = 0;

// One request in flight at a time: each target's mailbox holds one.
IrqSpinlock s_lock("tlb-shootdown");

uint64_t readCr3() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void flushAll() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & CR4_PGE) {
        // Toggling PGE also drops global entries.
        asm volatile("mov %0, %%cr4" :: "r"(cr4 & ~CR4_PGE) : "memory");
        asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    } else {
        asm volatile("mov %0, %%cr3" :: "r"(readCr3()) : "memory");
    }
}

bool coversEverySpace(PageTable* pml4, uint64_t va) {
    return !pml4 || pml4 == VMM::GetKernelAddressSpace() || va >= kKernelHalf;
}

// Whether `cpu` may cache translations of `pml4`. Under the big kernel lock no
// other CPU can switch address spaces, so its current process is stable.
bool mayCache(PerCPUData* cpu, PageTable* pml4, uint64_t va) {
    if (coversEverySpace(pml4, va)) {
        return true;
    }
    Process* running = cpu->currentProcess;
    return running && running->getPageTable() == pml4;
}

} // namespace

namespace TLB {

void flushLocal(uint64_t va, uint64_t pages) {
    if (pages == 0 || pages > kMaxInvlpgPages) {
        flushAll();
        return;
    }
    va &= ~0xFFFULL;
    for (uint64_t i = 0; i < pages; ++i) {
        asm volatile("invlpg (%0)" :: "r"(va + i * 4096) : "memory");
    }
}

void shootdown(PageTable* pml4, uint64_t va, uint64_t pages) {
    // This CPU first, if it may cache the space: it is loaded, or it belongs to
    // the current process (whose table fork and demand faults briefly unload).
    PerCPUData* self = getPerCPU();
    if (mayCache(self, pml4, va) ||
        (readCr3() & 0x000FFFFFFFFFF000ULL) == reinterpret_cast<uint64_t>(pml4)) {
        flushLocal(va, pages);
    }

    if (!SMP::schedulingActive() || SMP::onlineCount() <= 1) {
        return;
    }
    // tryLock + poll rather than lock(): another sender may be waiting for this
    // CPU to serve its request.
    uint64_t flags = 0;
    while (!s_lock.tryLock(&flags)) {
        pollPending();
        asm volatile("pause" ::: "memory");
    }

    PerCPUData* targets[SMP::kMaxCPUs];
    uint32_t count = 0;
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (!cpu || cpu == self || !mayCache(cpu, pml4, va)) {
            continue;
        }
        cpu->tlbAddr = va;
        cpu->tlbPages = pages;
        __atomic_store_n(&cpu->tlbPending, 1, __ATOMIC_RELEASE);
        LAPIC::get().sendIPI(cpu->lapicId, VECTOR_TLB_SHOOTDOWN, kFixedDelivery, 1, 0);
        targets[count++] = cpu;
    }

    for (uint32_t i = 0; i < count; ++i) {
        while (__atomic_load_n(&targets[i]->tlbPending, __ATOMIC_ACQUIRE)) {
            pollPending();
            asm volatile("pause" ::: "memory");
        }
    }

    s_lock.unlock(flags);
}

void pollPending() {
    PerCPUData* cpu = getPerCPU();
    if (!__atomic_load_n(&cpu->tlbPending, __ATOMIC_ACQUIRE)) {
        return;
    }
    // Interrupts off so the IPI cannot serve (and clear) the request between
    // reading it and clearing it, letting the sender post the next one early.
    const uint64_t flags = saveFlagsAndDisableInterrupts();
    if (__atomic_load_n(&cpu->tlbPending, __ATOMIC_ACQUIRE)) {
        flushLocal(cpu->tlbAddr, cpu->tlbPages);
        __atomic_store_n(&cpu->tlbPending, 0, __ATOMIC_RELEASE);
    }
    restoreFlags(flags);
}

void handleIpi() {
    pollPending();
    LAPIC::get().sendEOI();
}

} // namespace TLB
