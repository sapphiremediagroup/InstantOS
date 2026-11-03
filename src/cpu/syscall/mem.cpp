#include <graphics/intel_gen9_drm.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>
#include <fs/vfs/vfs.hpp>
#include <cpu/syscall/user_mapping.hpp>

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

// Run page-table mutations with the KERNEL address space active so the VMM's
// raw physical (low-identity-map) page-table walks are never shadowed by a low
// non-PIE (ET_EXEC) user image in the current CR3. Under memory pressure
// AllocTable() falls back below KERNEL_HIGH_ALLOC_MIN, placing a page-table
// frame inside the image window; a walk/store through its identity VA in the
// process's own address space then scribbles into the process's .data/.got
// (observed: collect2's GOT PTE corrupted, only under the in-OS binutils build).
// The VMM ops here take an explicit pageTable and reach frame contents only via
// PhysToVirt, so none of them need the user CR3. Mirrors sys_fork/sys_exec.
struct KernelAsScope {
    uint64_t saved;
    KernelAsScope() {
        asm volatile("mov %%cr3, %0" : "=r"(saved));
        PageTable* k = VMM::GetKernelAddressSpace();
        if (k) VMM::SetAddressSpace(k);
    }
    ~KernelAsScope() { asm volatile("mov %0, %%cr3" :: "r"(saved) : "memory"); }
};

void unmapUserPages(PageTable* pageTable, uint64_t addr, size_t pages) {
    PageTable* kernelPt = VMM::GetKernelAddressSpace();
    for (size_t i = 0; i < pages; i++) {
        const uint64_t va = addr + i * PAGE_SIZE;
        const uint64_t pa = VMM::VirtualToPhysicalIn(pageTable, va);
        if (pa) {
            const uint64_t frame = (pa & ~0xFFFULL);
            // Is this the inherited kernel/firmware identity map (VA==kernel's PA
            // for this VA)? Those frames are shared kernel memory present in every
            // process's low half.
            const bool sharedIdentity = kernelPt &&
                (VMM::VirtualToPhysicalIn(kernelPt, va) & ~0xFFFULL) == frame;

            // Always drop the mapping from THIS address space. UnmapPageFrom
            // COW-clones the shallow-shared low-half tables before clearing, so
            // the kernel's own identity map is never disturbed. Removing the alias
            // is required so a subsequent (fixed-address) mmap can be re-backed by
            // a fresh private frame via the demand-fault path -- otherwise the
            // "already backed" check would leave the supervisor identity page in
            // place and a user access would fault (observed: ld.so loading a small
            // dependency at a low fixed base over the identity window).
            const uint64_t entry = VMM::LeafEntryIn(pageTable, va);
            VMM::UnmapPageFrom(pageTable, va);

            // Only return the frame to the PMM when it is genuinely owned by this
            // address space; never free the inherited identity map (double-free /
            // frame-reuse corruption -- root cause of the P7.1 crashes).
            // A kSharedFrame page (IPC shared memory, a device frame buffer, a
            // GPU buffer object) belongs to its manager, which frees it.
            if (!sharedIdentity && !(entry & kSharedFrame)) {
                PMM::FreeFrame(frame);
            }
        }
    }
}
}

uint64_t Syscall::sys_mmap(uint64_t addr, uint64_t length, uint64_t prot) {
    if (length == 0) return syscall_error(SysErrInvalid);
    if (addr != 0 && (addr & (PAGE_SIZE - 1)) != 0) return syscall_error(SysErrInvalid);

    // Overflow guard on the requested length.
    if (length > 0x0000800000000000ULL) return syscall_error(SysErrInvalid);
    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) return syscall_error(SysErrInvalid);
    size_t aligned_length = pages * PAGE_SIZE;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    PageTable* pageTable = current->getPageTable();
    uint64_t virt = (addr != 0) ? addr : current->reserveMmapRegion(aligned_length);
    if (virt == 0) return syscall_error(SysErrNoMemory);

    // The whole range must stay in the user (lower) half and not wrap.
    if (virt >= 0x0000800000000000ULL ||
        virt + aligned_length < virt ||
        virt + aligned_length > 0x0000800000000000ULL) {
        return syscall_error(SysErrNoMemory);
    }

    // Demand-paged: record the region and return. Physical frames are allocated
    // lazily (and zero-filled) by Process::handleDemandFault() on first access,
    // so a large mmap never needs a contiguous physical block up front.
    // For an explicit fixed address, drop any prior mapping/reservation there.
    KernelAsScope kas;  // page-table ops below run unshadowed on the kernel AS
    if (addr != 0) {
        unmapUserPages(pageTable, virt, pages);
        current->mmapRemoveRange(virt, aligned_length);
        for (size_t i = 0; i < pages; i++) {
            asm volatile("invlpg (%0)" : : "r"(virt + i * PAGE_SIZE) : "memory");
        }
    }

    if (!current->mmapAddRegion(virt, aligned_length, prot)) {
        return syscall_error(SysErrNoMemory);
    }

    return virt;
}

uint64_t Syscall::sys_mmap_file(uint64_t argsPtr) {
    MmapFileArgs args{};
    if (!copyFromUser(&args, argsPtr, sizeof(args))) {
        return syscall_error(SysErrInvalid);
    }

    // MAP_* bit values (Linux ABI, matching mlibc abis/linux/vm-flags.h).
    constexpr uint64_t kMapShared = 0x01;
    constexpr uint64_t kMapAnonymous = 0x20;

    const uint64_t addr = args.addr;
    const uint64_t length = args.length;
    const uint64_t prot = args.prot;
    const uint64_t flags = args.flags;
    const uint64_t offset = args.offset;

    // An anonymous request carries no backing file: fall back to the plain
    // demand-paged path (keeps a single code path for AnonAllocate/MAP_ANON).
    if (flags & kMapAnonymous) {
        return sys_mmap(addr, length, prot);
    }

    if (length == 0) return syscall_error(SysErrInvalid);
    if (addr != 0 && (addr & (PAGE_SIZE - 1)) != 0) return syscall_error(SysErrInvalid);
    if (offset & (PAGE_SIZE - 1)) return syscall_error(SysErrInvalid);
    if (length > 0x0000800000000000ULL) return syscall_error(SysErrInvalid);

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) return syscall_error(SysErrInvalid);
    size_t aligned_length = pages * PAGE_SIZE;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    // Resolve the backing file handle (must be an open file in this process).
    FileDescriptor* file = current->getFD(args.fd);
    if (!file) return syscall_error(SysErrBadFile);

    // Compute how many bytes are available from `offset`; pages beyond this are
    // zero-filled on fault and are never written back (EOF semantics).
    uint64_t fileLength = 0;
    VNode* node = file->getNode();
    if (node && node->ops && node->ops->stat) {
        FileStats st{};
        if (node->ops->stat(node, &st) == 0 && st.size > offset) {
            fileLength = st.size - offset;
        }
    }

    const bool shared = (flags & kMapShared) != 0;

    PageTable* pageTable = current->getPageTable();
    uint64_t virt = (addr != 0) ? addr : current->reserveMmapRegion(aligned_length);
    if (virt == 0) return syscall_error(SysErrNoMemory);

    if (virt >= 0x0000800000000000ULL ||
        virt + aligned_length < virt ||
        virt + aligned_length > 0x0000800000000000ULL) {
        return syscall_error(SysErrNoMemory);
    }

    // Fixed address: flush any prior MAP_SHARED contents, then drop the old
    // mapping/reservation before installing the new one.
    if (addr != 0) {
        current->mmapSyncRange(virt, aligned_length);
        unmapUserPages(pageTable, virt, pages);
        current->mmapRemoveRange(virt, aligned_length);
        for (size_t i = 0; i < pages; i++) {
            asm volatile("invlpg (%0)" : : "r"(virt + i * PAGE_SIZE) : "memory");
        }
    }

    // A GPU buffer object (mmap on /dev/dri/renderD128 at a GEM_MMAP_OFFSET
    // offset): its pages are mapped now, shared, and the region keeps the
    // object alive.
    if (drm::isDrmNode(node)) {
        drm::MmapTarget target{};
        const uint64_t resolved = drm::resolveMmap(node, offset, aligned_length, &target);
        if (resolved != 0) {
            return resolved;
        }
        uint64_t pageFlags = Present | UserSuper | kSharedFrame | target.cacheFlags;
        if (prot & MemoryProtWrite) pageFlags |= ReadWrite;
        if (!(prot & MemoryProtExecute)) pageFlags |= NoExecute;
        {
            KernelAsScope kas;
            for (size_t i = 0; i < pages; i++) {
                VMM::MapPageInto(pageTable, virt + i * PAGE_SIZE, target.pages[i], pageFlags);
            }
        }
        if (!current->mmapAddObjectRegion(virt, aligned_length, prot, target.object)) {
            KernelAsScope kas;
            unmapUserPages(pageTable, virt, pages);
            return syscall_error(SysErrNoMemory);
        }
        return virt;
    }

    if (!current->mmapAddFileRegion(virt, aligned_length, prot, file, offset, fileLength, shared)) {
        return syscall_error(SysErrNoMemory);
    }

    return virt;
}

uint64_t Syscall::sys_msync(uint64_t addr, uint64_t length, uint64_t /*flags*/) {
    if (!addr || length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return syscall_error(SysErrInvalid);
    }
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    const uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    current->mmapSyncRange(addr, pages * PAGE_SIZE);
    return 0;
}

uint64_t Syscall::sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
    if (!addr || length == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    PageTable* pageTable = current->getPageTable();
    const uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    // PROT_NONE is treated as read/write (legacy reserve-and-write semantics
    // relied on by userspace); explicit PROT_READ/PROT_EXEC are honored.
    const uint64_t leafFlags = pageFlagsForProtection(prot, true);

    // Re-protect only pages that are currently present; not-yet-faulted lazy
    // pages will materialize with the new protection from the updated region
    // record below. This keeps mprotect working on demand-paged mappings.
    KernelAsScope kas;  // page-table walks below run unshadowed on the kernel AS
    for (uint64_t i = 0; i < pages; i++) {
        const uint64_t va = addr + i * PAGE_SIZE;
        if (VMM::VirtualToPhysicalIn(pageTable, va) == 0) {
            continue;
        }
        // Break any copy-on-write share first so raising write permission never
        // makes a shared frame writable in place.
        VMM::HandleCowFault(pageTable, va);
        VMM::ProtectPageIn(pageTable, va, leafFlags);
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    }

    // Record the new protection for the region so lazy faults honor it.
    current->mmapProtectRange(addr, pages * PAGE_SIZE, prot);

    return 0;
}

uint64_t Syscall::sys_munmap(uint64_t addr, uint64_t length) {
    if (!addr || length == 0 || (addr & (PAGE_SIZE - 1)) != 0) return syscall_error(SysErrInvalid);

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    // Flush dirty MAP_SHARED file pages before the frames disappear, then free
    // any present frames and drop the region reservation.
    {
        KernelAsScope kas;  // page-table ops below run unshadowed on the kernel AS
        current->mmapSyncRange(addr, pages * PAGE_SIZE);
        unmapUserPages(current->getPageTable(), addr, pages);
    }
    current->mmapRemoveRange(addr, pages * PAGE_SIZE);

    return 0;
}

uint64_t mapDmaBufIntoCurrent(VNode* dmaBuf, uint64_t length) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !dmaBuf || length == 0) {
        return 0;
    }
    const size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    const uint64_t alignedLength = pages * PAGE_SIZE;
    drm::MmapTarget target {};
    if (drm::resolveMmap(dmaBuf, 0, alignedLength, &target) != 0) {
        return 0;
    }
    const uint64_t virt = current->reserveMmapRegion(alignedLength);
    if (virt == 0) {
        return 0;
    }
    const uint64_t pageFlags = Present | UserSuper | ReadWrite | NoExecute | kSharedFrame | target.cacheFlags;
    PageTable* pageTable = current->getPageTable();
    {
        KernelAsScope kas;
        for (size_t i = 0; i < pages; i++) {
            VMM::MapPageInto(pageTable, virt + i * PAGE_SIZE, target.pages[i], pageFlags);
        }
    }
    if (!current->mmapAddObjectRegion(virt, alignedLength, MemoryProtRead | MemoryProtWrite, target.object)) {
        KernelAsScope kas;
        unmapUserPages(pageTable, virt, pages);
        return 0;
    }
    return virt;
}

void unmapRangeOfCurrent(uint64_t address, uint64_t length) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || address == 0 || length == 0) {
        return;
    }
    const size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    {
        KernelAsScope kas;
        unmapUserPages(current->getPageTable(), address, pages);
    }
    current->mmapRemoveRange(address, pages * PAGE_SIZE);
}
