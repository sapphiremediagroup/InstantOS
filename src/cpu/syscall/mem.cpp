#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>

namespace {
uint64_t pageFlagsForProtection(uint64_t prot, bool defaultReadWrite) {
    if (prot == 0 && defaultReadWrite) {
        prot = MemoryProtRead | MemoryProtWrite;
    }

    uint64_t flags = Present;
    if (prot != 0) {
        flags |= UserSuper;
    }
    if (prot & MemoryProtWrite) {
        flags |= ReadWrite;
    }
    if ((prot & MemoryProtExecute) == 0) {
        flags |= NoExecute;
    }
    return flags;
}

void unmapUserPages(PageTable* pageTable, uint64_t addr, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        const uint64_t va = addr + i * PAGE_SIZE;
        const uint64_t pa = VMM::VirtualToPhysicalIn(pageTable, va);
        if (pa) {
            VMM::UnmapPageFrom(pageTable, va);
            PMM::FreeFrame(pa & ~0xFFFULL);
        }
    }
}
}

uint64_t Syscall::sys_mmap(uint64_t addr, uint64_t length, uint64_t prot) {
    if (length == 0) return syscall_error(SysErrInvalid);
    if (addr != 0 && (addr & (PAGE_SIZE - 1)) != 0) return syscall_error(SysErrInvalid);

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_length = pages * PAGE_SIZE;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    PageTable* pageTable = current->getPageTable();
    uint64_t virt = (addr != 0) ? addr : current->reserveMmapRegion(aligned_length);

    uint64_t phys = PMM::AllocFrames(pages);
    if (!phys) {
        return syscall_error(SysErrNoMemory);
    }

    if (addr != 0) {
        unmapUserPages(pageTable, virt, pages);
    }

    const uint64_t finalFlags = pageFlagsForProtection(prot, true);
    const uint64_t zeroFlags = finalFlags | ReadWrite;
    VMM::MapRangeInto(pageTable, virt, phys, pages, zeroFlags);
    for (size_t i = 0; i < pages; i++) {
        if (VMM::VirtualToPhysicalIn(pageTable, virt + i * PAGE_SIZE) == 0) {
            VMM::UnmapRangeFrom(pageTable, virt, i);
            PMM::FreeFrames(phys, pages);
            return syscall_error(SysErrNoMemory);
        }
    }

    // Flush any stale TLB entries for this range *before* touching the new
    // mapping. MapRangeInto / UnmapRangeFrom do not invalidate, so a prior
    // munmap (or a stale large-page translation) can otherwise leave a bogus
    // entry cached, causing a reserved-bit / protection fault on the kernel
    // memset below.
    for (size_t i = 0; i < pages; i++) {
        asm volatile("invlpg (%0)" : : "r"(virt + i * PAGE_SIZE) : "memory");
    }

    memset(reinterpret_cast<void*>(virt), 0, aligned_length);
    if (finalFlags != zeroFlags) {
        VMM::ProtectRangeIn(pageTable, virt, pages, finalFlags);
        for (size_t i = 0; i < pages; i++) {
            asm volatile("invlpg (%0)" : : "r"(virt + i * PAGE_SIZE) : "memory");
        }
    }

    return virt;
}

uint64_t Syscall::sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
    if (!addr || length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    const uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!VMM::ProtectRangeIn(current->getPageTable(), addr, pages, pageFlagsForProtection(prot, false))) {
        return syscall_error(SysErrInvalid);
    }

    // Flush stale TLB entries for the range so the new protection (e.g. clearing
    // the NX bit for JIT/-run executable pages) takes effect immediately.
    // Without this a subsequent instruction fetch faults against the cached
    // (still-NX) translation.
    for (uint64_t i = 0; i < pages; i++) {
        asm volatile("invlpg (%0)" : : "r"(addr + i * PAGE_SIZE) : "memory");
    }

    return 0;
}

uint64_t Syscall::sys_munmap(uint64_t addr, uint64_t length) {
    if (!addr || length == 0 || (addr & (PAGE_SIZE - 1)) != 0) return syscall_error(SysErrInvalid);

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    unmapUserPages(current->getPageTable(), addr, pages);

    return 0;
}
