#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>

uint64_t Syscall::sys_mmap(uint64_t addr, uint64_t length, uint64_t prot __attribute__((unused))) {
    if (length == 0) return (uint64_t)-1;

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_length = pages * PAGE_SIZE;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    uint64_t virt = (addr != 0) ? addr : current->reserveMmapRegion(aligned_length);

    uint64_t phys = PMM::AllocFrames(pages);
    if (!phys) {
        return (uint64_t)-1;
    }

    memset(reinterpret_cast<void*>(phys), 0, aligned_length);

    VMM::MapRange(virt, phys, pages, Present | ReadWrite | UserSuper | NoExecute);

    for (size_t i = 0; i < pages; i++) {
        asm volatile("invlpg (%0)" : : "r"(virt + i * PAGE_SIZE) : "memory");
    }

    return virt;
}

uint64_t Syscall::sys_munmap(uint64_t addr, uint64_t length) {
    if (!addr || length == 0) return (uint64_t)-1;

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uint64_t va = addr + i * PAGE_SIZE;
        uint64_t pa = VMM::VirtualToPhysical(va);
        if (pa) {
            VMM::UnmapPage(va);
            PMM::FreeFrame(pa & ~0xFFFULL);
        }
    }

    return 0;
}
