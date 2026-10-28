#include <cpu/process/process.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>
#include <memory/pmm.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/cpuid.hpp>
#include <fs/vfs/vfs.hpp>
#include <ipc/ipc.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <cpu/tlb.hpp>

extern "C" void forkChildTrampoline();

extern "C" void enterUsermode(uint64_t entry, uint64_t stack);

constexpr uint64_t USER_STACK_TOP = 0x00007FFFFFFFE000;
constexpr size_t USER_STACK_PAGES = 32;

namespace {
struct UserSignalFrame {
    uint64_t rip;
    uint64_t rsp;
    uint64_t blocked;
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t rflags;
    uint64_t altStackFlags;
};

// Linux ignores SIGCHLD, SIGCONT, SIGURG and SIGWINCH by default. Its default
// for SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU is to stop the process, which this
// kernel cannot do; ignoring them beats killing it (bash sends itself SIGTTIN
// until it owns its terminal, and dies before its first prompt otherwise).
bool defaultIgnoredSignal(int sig) {
    switch (sig) {
    case SIGCHLD:
    case 18:   // SIGCONT
    case 19:   // SIGSTOP
    case 20:   // SIGTSTP
    case 21:   // SIGTTIN
    case 22:   // SIGTTOU
    case 23:   // SIGURG
    case 28:   // SIGWINCH
        return true;
    default:
        return false;
    }
}
}

// A demand-paged memory region (VMA). Held in a sorted singly-linked list per
// address space; frames are populated lazily on page fault. When `file` is null
// the region is anonymous (zero-filled). When `file` is set the region is
// demand-paged from that file, and MAP_SHARED regions are written back.
struct VmaRegion {
    uint64_t start;         // inclusive, page-aligned
    uint64_t end;           // exclusive, page-aligned
    uint64_t prot;          // MemoryProt* bits (0 == inaccessible / PROT_NONE)
    FileDescriptor* file;   // nullptr => anonymous; else backing file (retained)
    uint64_t fileOffset;    // file byte offset mapped at `start`
    uint64_t fileLength;    // bytes from fileOffset sourced from the file (EOF clamp)
    bool shared;            // MAP_SHARED (write back) vs MAP_PRIVATE (copy)
    VmaRegion* next;
};

struct ProcessSharedState {
    PageTable* pageTable;
    HandleTable handleTable;
    uint64_t mmapBase;
    uint32_t refCount;
    VmaRegion* vmaList;   // demand-paged mmap regions, sorted by start

    ProcessSharedState()
        : pageTable(nullptr), mmapBase(0x0000600000000000UL), refCount(1), vmaList(nullptr) {}
};

namespace {
constexpr uint32_t kDefaultFileRights = HandleRightRead | HandleRightWrite | HandleRightDuplicate;
constexpr uint64_t kKernelStackSize = 4 * PAGE_SIZE;

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Write back every present page of a writable MAP_SHARED file region to its
// backing file. Conservative: it flushes all present pages (the PTE dirty bit
// is not surfaced by VirtualToPhysicalIn), which is correct — a clean page just
// rewrites identical bytes. No-op for anonymous, read-only, or private regions.
// The physical frame is reached through the kernel's identity map, as in
// handleDemandFault().
void vmaWritebackRegion(PageTable* pageTable, VmaRegion* r) {
    if (!pageTable || !r || !r->file || !r->shared || !(r->prot & MemoryProtWrite)) {
        return;
    }
    VNode* node = r->file->getNode();
    if (!node || !node->ops || !node->ops->write) {
        return;
    }
    const uint64_t mappedEnd = r->fileOffset + r->fileLength;
    for (uint64_t va = r->start; va < r->end; va += PAGE_SIZE) {
        uint64_t pa = VMM::VirtualToPhysicalIn(pageTable, va);
        if (!pa) {
            continue;   // never faulted in -> nothing dirty here
        }
        pa &= ~0xFFFULL;
        const uint64_t filePos = r->fileOffset + (va - r->start);
        if (filePos >= mappedEnd) {
            continue;   // page lies past the file-backed window
        }
        uint64_t n = mappedEnd - filePos;
        if (n > PAGE_SIZE) {
            n = PAGE_SIZE;
        }
        // Read the frame through the higher-half direct map, not the low
        // identity map (which a non-PIE user image can shadow while active).
        node->ops->write(node, reinterpret_cast<void*>(VMM::PhysToVirt(pa)), n, filePos);
    }
}

void retainSharedState(ProcessSharedState* state) {
    if (state) {
        __sync_add_and_fetch(&state->refCount, 1);
    }
}

void releaseSharedState(ProcessSharedState* state) {
    if (!state) {
        return;
    }

    if (__sync_sub_and_fetch(&state->refCount, 1) != 0) {
        return;
    }

    state->handleTable.closeAll();
    // Flush dirty MAP_SHARED file mappings while the address space (and its
    // frames) are still live. The region keeps its own reference to the backing
    // file, so it survives handleTable.closeAll() above.
    if (state->pageTable) {
        for (VmaRegion* r = state->vmaList; r; r = r->next) {
            vmaWritebackRegion(state->pageTable, r);
        }
        VMM::FreeAddressSpace(state->pageTable);
    }
    // Release demand-paged region records (the frames themselves were reclaimed
    // by FreeAddressSpace above) and drop each region's backing-file reference.
    VmaRegion* v = state->vmaList;
    while (v) {
        VmaRegion* next = v->next;
        if (v->file) {
            VFS::get().close(v->file);
        }
        delete v;
        v = next;
    }
    state->vmaList = nullptr;
    delete state;
}

void retainFileHandle(void* object) {
    VFS::get().retain(reinterpret_cast<FileDescriptor*>(object));
}

void releaseFileHandle(void* object) {
    VFS::get().close(reinterpret_cast<FileDescriptor*>(object));
}

bool initializeAddressSpace(ProcessSharedState* state) {
    if (!state) {
        return false;
    }

    state->pageTable = VMM::AllocTable();
    if (!state->pageTable) {
        return false;
    }

    PageTable* kPml4 = VMM::GetKernelAddressSpace();
    if (!kPml4) {
        VMM::FreeAddressSpace(state->pageTable);
        state->pageTable = nullptr;
        return false;
    }

    for (int i = 256; i < 512; i++) {
        state->pageTable->entries[i] = kPml4->entries[i];
    }

    for (int i = 0; i < 256; i++) {
        if (kPml4->entries[i] & Present) {
            auto* srcPdpt = reinterpret_cast<PageTable*>(kPml4->entries[i] & ADDR_MASK);
            PageTable* newPdpt = VMM::AllocTable();
            if (!newPdpt) {
                VMM::FreeAddressSpace(state->pageTable);
                state->pageTable = nullptr;
                return false;
            }

            // Copy the kernel PDPT's entries, but CLEAR the "private table" bit
            // (1<<9): those entries still point at the KERNEL's shared PD/PT
            // structures, not tables private to this address space. If the bit
            // were left set, clone_table_if_needed() would skip copy-on-write and
            // let MapPageInto() mutate the shared kernel identity map (splitting a
            // huge page / adding a leaf), corrupting it for everyone -- observed
            // as garbage in freshly-loaded non-PIE (ET_EXEC) images at low VAs,
            // whose low-VA mappings force exactly such splits.
            constexpr uint64_t kPrivateTable = 1ULL << 9;
            for (int j = 0; j < 512; j++) {
                newPdpt->entries[j] = srcPdpt->entries[j] & ~kPrivateTable;
            }
            // newPdpt itself IS private to this address space; mark it so it is
            // not needlessly re-cloned on first write.
            state->pageTable->entries[i] =
                reinterpret_cast<uint64_t>(newPdpt) | (kPml4->entries[i] & ~ADDR_MASK) | kPrivateTable;
        }
    }

    return true;
}
}

Process::Process(uint32_t pid)
    : sharedState(nullptr), sessionID(0), uid(0), gid(0), pid(pid), parentPID(0), exitCode(0),
      state(ProcessState::Ready), priority(ProcessPriority::Normal), kernelStack(0), userStack(0),
      userStackBase(0), userStackSize(0), userStackHeapBacked(false), fpuState(nullptr), userFpuState(nullptr), validUserState(false), savedUserRSP(0),
      userFsBase(0), userGsBase(0), sleepDeadlineMs(0), sleeping(false),
      threadObject(nullptr) {
    next = nullptr;
    allNext = nullptr;
    cwd[0] = '/';
    cwd[1] = '\0';
    name[0] = '\0';
    syscallTrace.active = false;
    syscallTrace.number = 0;
    syscallTrace.arg1 = 0;
    syscallTrace.arg2 = 0;
    syscallTrace.arg3 = 0;
    syscallTrace.arg4 = 0;
    syscallTrace.arg5 = 0;

    for (int i = 0; i < NSIG; i++) {
        signalHandler.handlers[i] = nullptr;
        signalHandler.masks[i] = 0;
        signalHandler.flags[i] = 0;
        signalHandler.restorers[i] = 0;
    }
    signalHandler.pending = 0;
    signalHandler.blocked = 0;
    signalHandler.altStackSp = 0;
    signalHandler.altStackSize = 0;
    signalHandler.altStackFlags = SS_DISABLE;

    sharedState = new ProcessSharedState();
    if (!sharedState || !initializeAddressSpace(sharedState)) {
        return;
    }

    void* kstackPhys = kmalloc_aligned(kKernelStackSize, PAGE_SIZE);
    if (kstackPhys) {
        kernelStack = reinterpret_cast<uint64_t>(kstackPhys) + kKernelStackSize;
    }

    userStackBase = USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE);
    userStackSize = USER_STACK_PAGES * PAGE_SIZE;
    void* ustackPhys = kmalloc_aligned(userStackSize, PAGE_SIZE);
    if (ustackPhys) {
        memset(ustackPhys, 0, userStackSize);
        VMM::MapRangeInto(sharedState->pageTable, userStackBase, reinterpret_cast<uint64_t>(ustackPhys),
                          USER_STACK_PAGES,
                          PageFlags::Present | PageFlags::ReadWrite | PageFlags::UserSuper | PageFlags::NoExecute);
        userStack = USER_STACK_TOP - 8;
        userStackHeapBacked = true;
    }

    void* fpuPhys = kmalloc_aligned(sizeof(FPUState), 64);
    if (fpuPhys) {
        fpuState = reinterpret_cast<FPUState*>(fpuPhys);
        CPU::initializeExtendedState(fpuState);
    }

    void* userFpuPhys = kmalloc_aligned(sizeof(FPUState), 64);
    if (userFpuPhys) {
        userFpuState = reinterpret_cast<FPUState*>(userFpuPhys);
        CPU::initializeExtendedState(userFpuState);
    }

    context.rax = 0;
    context.rbx = 0;
    context.rcx = 0;
    context.rdx = 0;
    context.rsi = 0;
    context.rdi = 0;
    context.rbp = 0;
    context.rsp = kernelStack;
    context.r8 = 0;
    context.r9 = 0;
    context.r10 = 0;
    context.r11 = 0;
    context.r12 = 0;
    context.r13 = 0;
    context.r14 = 0;
    context.r15 = 0;
    context.rip = 0;
    context.rflags = 0x202;

    context.cr3 = reinterpret_cast<uint64_t>(sharedState->pageTable) & ADDR_MASK;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);

    if (fpuState) {
        CPU::initializeExtendedState(fpuState);
    }
}

Process::Process(uint32_t pid, Process* sharedFrom, uint64_t stackSize)
    : sharedState(nullptr), sessionID(0), uid(0), gid(0), pid(pid), parentPID(0), exitCode(0),
      state(ProcessState::Ready), priority(ProcessPriority::Normal), kernelStack(0), userStack(0),
      userStackBase(0), userStackSize(0), userStackHeapBacked(false), fpuState(nullptr), validUserState(false), savedUserRSP(0),
      userFsBase(0), userGsBase(0), sleepDeadlineMs(0), sleeping(false),
      threadObject(nullptr) {
    next = nullptr;
    allNext = nullptr;
    cwd[0] = '/';
    cwd[1] = '\0';
    name[0] = '\0';
    syscallTrace.active = false;
    syscallTrace.number = 0;
    syscallTrace.arg1 = 0;
    syscallTrace.arg2 = 0;
    syscallTrace.arg3 = 0;
    syscallTrace.arg4 = 0;
    syscallTrace.arg5 = 0;

    for (int i = 0; i < NSIG; i++) {
        signalHandler.handlers[i] = nullptr;
        signalHandler.masks[i] = 0;
        signalHandler.flags[i] = 0;
        signalHandler.restorers[i] = 0;
    }
    signalHandler.pending = 0;
    signalHandler.blocked = 0;
    signalHandler.altStackSp = 0;
    signalHandler.altStackSize = 0;
    signalHandler.altStackFlags = SS_DISABLE;

    if (!sharedFrom || !sharedFrom->sharedState || !sharedFrom->sharedState->pageTable) {
        return;
    }

    const SignalHandler* parentSignals = sharedFrom->getSignalHandler();
    if (parentSignals) {
        for (int i = 0; i < NSIG; i++) {
            signalHandler.handlers[i] = parentSignals->handlers[i];
            signalHandler.masks[i] = parentSignals->masks[i];
            signalHandler.flags[i] = parentSignals->flags[i];
            signalHandler.restorers[i] = parentSignals->restorers[i];
        }
        signalHandler.blocked = parentSignals->blocked;
    }

    sharedState = sharedFrom->sharedState;
    retainSharedState(sharedState);

    void* kstackPhys = kmalloc_aligned(kKernelStackSize, PAGE_SIZE);
    if (kstackPhys) {
        kernelStack = reinterpret_cast<uint64_t>(kstackPhys) + kKernelStackSize;
    }

    userStackSize = alignUp(stackSize ? stackSize : (16 * PAGE_SIZE), PAGE_SIZE);
    userStackBase = reserveMmapRegion(userStackSize);
    void* ustackPhys = kmalloc_aligned(userStackSize, PAGE_SIZE);
    if (ustackPhys) {
        memset(ustackPhys, 0, userStackSize);
        VMM::MapRangeInto(sharedState->pageTable, userStackBase, reinterpret_cast<uint64_t>(ustackPhys),
                          userStackSize / PAGE_SIZE,
                          PageFlags::Present | PageFlags::ReadWrite | PageFlags::UserSuper | PageFlags::NoExecute);
        userStack = userStackBase + userStackSize - 8;
        userStackHeapBacked = true;
    }

    void* fpuPhys = kmalloc_aligned(sizeof(FPUState), 64);
    if (fpuPhys) {
        fpuState = reinterpret_cast<FPUState*>(fpuPhys);
    }

    context.rax = 0;
    context.rbx = 0;
    context.rcx = 0;
    context.rdx = 0;
    context.rsi = 0;
    context.rdi = 0;
    context.rbp = 0;
    context.rsp = kernelStack;
    context.r8 = 0;
    context.r9 = 0;
    context.r10 = 0;
    context.r11 = 0;
    context.r12 = 0;
    context.r13 = 0;
    context.r14 = 0;
    context.r15 = 0;
    context.rip = 0;
    context.rflags = 0x202;
    context.cr3 = sharedFrom->getContext()->cr3;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);

    if (fpuState) {
        CPU::initializeExtendedState(fpuState);
    }
}

Process::~Process() {
    IPCManager::get().cleanupProcess(this);

    if (threadObject) {
        threadObject->completed = true;
        threadObject->exitCode = exitCode;
        threadObject->process = nullptr;
        threadObject->release();
        threadObject = nullptr;
    }

    if (kernelStack) {
        uint64_t kstackVirt = kernelStack - kKernelStackSize;
        kfree(reinterpret_cast<void*>(kstackVirt));
    }

    if (userStackBase && userStackSize && getPageTable()) {
        if (userStackHeapBacked) {
            // Unmap (and shoot down) before freeing: sibling threads may still
            // be running in this address space on other CPUs.
            uint64_t ustackPhys = VMM::VirtualToPhysicalIn(getPageTable(), userStackBase);
            VMM::UnmapRangeFrom(getPageTable(), userStackBase, userStackSize / PAGE_SIZE);
            if (ustackPhys) {
                kfree(reinterpret_cast<void*>(ustackPhys));
            }
        }
        // For non-heap-backed (forked) stacks, FreeAddressSpace() reclaims the
        // copied frame; do not kfree() it here.
    }

    if (fpuState) {
        kfree(fpuState);
    }

    if (userFpuState) {
        kfree(userFpuState);
    }

    releaseSharedState(sharedState);
    sharedState = nullptr;
}

PageTable* Process::getPageTable() const {
    return sharedState ? sharedState->pageTable : nullptr;
}

bool Process::cloneAddressSpaceFrom(Process* parent) {
    if (!sharedState || !sharedState->pageTable || !parent || !parent->sharedState ||
        !parent->sharedState->pageTable) {
        return false;
    }

    // This Process was built with the spawn constructor, which kmalloc()'d a
    // user stack and mapped it at USER_STACK_TOP. The copy below replaces every
    // user PTE with freshly-allocated PMM frames (including the stack), so the
    // constructor's heap-backed stack must be released now to avoid leaking it
    // and to ensure the destructor frees the copied frame via FreeAddressSpace()
    // (not kfree()).
    if (userStackHeapBacked && userStackBase && userStackSize) {
        uint64_t ustackPhys = VMM::VirtualToPhysicalIn(sharedState->pageTable, userStackBase);
        VMM::UnmapRangeFrom(sharedState->pageTable, userStackBase, userStackSize / PAGE_SIZE);
        if (ustackPhys) {
            kfree(reinterpret_cast<void*>(ustackPhys));
        }
        userStackHeapBacked = false;
    }

    PageTable* parentPml4 = parent->sharedState->pageTable;
    PageTable* childPml4 = sharedState->pageTable;
    PageTable* kernelPml4 = VMM::GetKernelAddressSpace();

    // Parent pages turned read-only below must not stay writable in any CPU's
    // TLB (the parent's other threads), or their writes would leak into the
    // child's copy. One shootdown once the walk is done, on every exit path.
    bool parentDowngraded = false;
    struct ParentFlush {
        PageTable* pml4;
        bool* needed;
        ~ParentFlush() {
            if (*needed) TLB::shootdown(pml4, 0, 0);
        }
    } parentFlush{parentPml4, &parentDowngraded};

    // Walk only the user half (PML4 entries 0..255). The child already has the
    // kernel half copied from its construction.
    for (int i = 0; i < 256; i++) {
        uint64_t pml4e = parentPml4->entries[i];
        if (!(pml4e & Present)) {
            continue;
        }
        // Skip PML4 sub-trees that are shared verbatim with the kernel address
        // space (identity map, kernel code/data mapped into the low half). These
        // must NOT be copied: doing so clobbers the child's kernel mappings with
        // private frames and makes FreeAddressSpace() later free live kernel
        // memory. Only genuine user pages (U/S set at the leaf) are cloned below.
        if (kernelPml4 && pml4e == kernelPml4->entries[i]) {
            continue;
        }
        auto* parentPdpt = reinterpret_cast<PageTable*>(pml4e & ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            uint64_t pdpte = parentPdpt->entries[j];
            if (!(pdpte & Present) || (pdpte & LargePage)) {
                continue;
            }
            auto* parentPd = reinterpret_cast<PageTable*>(pdpte & ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                uint64_t pde = parentPd->entries[k];
                if (!(pde & Present) || (pde & LargePage)) {
                    continue;
                }
                auto* parentPt = reinterpret_cast<PageTable*>(pde & ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = parentPt->entries[l];
                    if (!(pte & Present)) {
                        continue;
                    }
                    // Only copy genuine userspace pages. Kernel pages reachable
                    // through the low half lack the U/S bit and must be left
                    // pointing at the shared kernel frames.
                    if (!(pte & UserSuper)) {
                        continue;
                    }

                    const uint64_t vaddr =
                        (static_cast<uint64_t>(i) << 39) |
                        (static_cast<uint64_t>(j) << 30) |
                        (static_cast<uint64_t>(k) << 21) |
                        (static_cast<uint64_t>(l) << 12);
                    const uint64_t srcPhys = pte & ADDR_MASK;
                    const uint64_t flags = pte & ~ADDR_MASK;

                    // The user stack is eagerly copied: it is small and is
                    // heap-backed (kmalloc) for every exec'd/spawn'd process
                    // (createUserProcessWithArgs kmalloc_aligned()s it), which
                    // must never enter the PMM copy-on-write refcount scheme -- a
                    // COW break or teardown FreeFrame() would hand a live kmalloc
                    // arena frame back to the PMM, which then re-allocates it (e.g.
                    // for tmpfs during an in-OS untar) while the heap still owns
                    // it, double-mapping the frame and corrupting both users
                    // (observed: binutils source text overwriting collect2's
                    // .got/stack -> GOT[environ]=0 NULL-write crash).
                    // NOTE: test against the PARENT's stack region -- `this` is the
                    // freshly bare-constructed fork child whose userStack* are 0,
                    // so using them here disabled the eager copy entirely.
                    const bool inStack = parent->userStackSize &&
                        vaddr >= parent->userStackBase &&
                        vaddr < parent->userStackBase + parent->userStackSize;

                    // A page participates in COW if it is writable now, or is
                    // already a shared COW page from an earlier fork generation.
                    const bool cowShare = (flags & ReadWrite) || (flags & kCowPage);

                    if (flags & kSharedFrame) {
                        // IPC shared memory or the framebuffer BAR: shared writable
                        // across fork, owned by its manager (or MMIO). Never COW or
                        // refcount it here -- teardown must not free it either.
                        VMM::MapPageInto(childPml4, vaddr, srcPhys, flags);
                    } else if (inStack) {
                        // Private eager copy (legacy behavior for the stack).
                        uint64_t newPhys = PMM::AllocFrames(1);
                        if (!newPhys) {
                            return false;
                        }
                        // Copy through the higher-half direct map, not the low
                        // identity map: the parent may be a non-PIE (ET_EXEC)
                        // image mapped at a low VA, whose image shadows the
                        // identity VA of these frames in the active address
                        // space, so a raw identity copy would hit the user image
                        // instead of the stack frame (observed: write fault on a
                        // read-only user page during fork).
                        memcpy(reinterpret_cast<void*>(VMM::PhysToVirt(newPhys)),
                               reinterpret_cast<const void*>(VMM::PhysToVirt(srcPhys)), PAGE_SIZE);
                        VMM::MapPageInto(childPml4, vaddr, newPhys, flags);
                    } else if (cowShare) {
                        // Copy-on-write share. Downgrade the parent to read-only
                        // COW only if it is currently writable (an already-COW
                        // parent stays as-is). Map the child with writable upper
                        // tables (so a later COW write is not blocked by a
                        // read-only intermediate entry) but a read-only COW leaf.
                        if (flags & ReadWrite) {
                            parentPt->entries[l] = (pte & ~ReadWrite) | kCowPage;
                            parentDowngraded = true;
                        }
                        VMM::MapPageInto(childPml4, vaddr, srcPhys, flags | ReadWrite);
                        if (!VMM::ProtectPageIn(childPml4, vaddr,
                                                (flags & ~ReadWrite) | kCowPage)) {
                            return false;
                        }
                        PMM::IncRef(srcPhys);
                    } else {
                        // Genuine read-only page (e.g. code/rodata): share the
                        // frame directly. A write faults to SIGSEGV as expected.
                        VMM::MapPageInto(childPml4, vaddr, srcPhys, flags);
                        PMM::IncRef(srcPhys);
                    }
                }
            }
        }
    }

    return true;
}

void Process::setupForkResume(const ProcessContext& userContext, uint64_t fsBase) {
    // Build a frame on the child's kernel stack consumed by forkChildTrampoline.
    uint64_t sp = kernelStack & ~0xFULL;

    // iretq frame (top): SS, RSP, RFLAGS, CS, RIP -- pushed high to low.
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = 0x1B;                  // SS
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rsp;       // user RSP
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rflags | 0x200; // RFLAGS (IF)
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = 0x23;                  // CS
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rip;       // user RIP

    // GP register frame (must match forkChildTrampoline offsets).
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = fsBase;                // [+120]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = 0;                     // [+112] rax = 0
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rbx;       // [+104]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rcx;       // [+96]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rdx;       // [+88]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rsi;       // [+80]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rdi;       // [+72]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.rbp;       // [+64]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r8;        // [+56]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r9;        // [+48]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r10;       // [+40]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r11;       // [+32]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r12;       // [+24]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r13;       // [+16]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r14;       // [+8]
    sp -= 8; *reinterpret_cast<uint64_t*>(sp) = userContext.r15;       // [+0]

    // Context the scheduler restores when switching to the child: it `ret`s
    // into forkChildTrampoline with rsp pointing at the frame above.
    context.rip = reinterpret_cast<uint64_t>(&forkChildTrampoline);
    context.rsp = sp;
    context.rbp = 0;
    context.cr3 = reinterpret_cast<uint64_t>(sharedState->pageTable) & ADDR_MASK;
    context.rflags = 0x202;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);

    // The child has NOT yet returned to user mode: it must first run
    // forkChildTrampoline (at CPL0) which performs the iretq back to user. So it
    // must be treated like a freshly-created process. If we marked the user
    // state valid here, the timer-preemption scheduler path
    // (Scheduler::schedule(InterruptFrame*)) would take its "resume user
    // directly" branch and iretq with context.rip == forkChildTrampoline as if
    // it were a *user* RIP -- faulting on an instruction fetch of supervisor
    // kernel code from CPL3. Leaving this false routes the child through
    // switchContext() -> forkChildTrampoline -> iretq, which is correct.
    setValidUserState(false);
}

uint64_t Process::getMmapBase() const {
    return sharedState ? sharedState->mmapBase : 0;
}

uint64_t Process::reserveMmapRegion(uint64_t size) {
    if (!sharedState) {
        return 0;
    }

    size = alignUp(size, PAGE_SIZE);
    uint64_t base = sharedState->mmapBase;
    sharedState->mmapBase += size;
    return base;
}

// ── Demand-paged mmap region bookkeeping ───────────────────────────────────
namespace {
// Convert MemoryProt* bits to leaf page flags for a demand-filled user page.
// A prot of 0 (PROT_NONE) is treated as read/write: userspace (mlibc's
// allocator) reserves address space with PROT_NONE and writes into it without
// an explicit commit, relying on the historical mmap behavior where reserved
// pages were always accessible. Explicit PROT_READ/PROT_EXEC are honored.
uint64_t vmaPageFlags(uint64_t prot) {
    if (prot == 0) {
        prot = MemoryProtRead | MemoryProtWrite;
    }
    uint64_t flags = Present | UserSuper;
    if (prot & MemoryProtWrite) {
        flags |= ReadWrite;
    }
    if ((prot & MemoryProtExecute) == 0) {
        flags |= NoExecute;
    }
    return flags;
}

// Ensure no region straddles `addr`: if one does, split it in two at `addr`.
bool vmaSplitAt(VmaRegion*& list, uint64_t addr) {
    for (VmaRegion* r = list; r; r = r->next) {
        if (addr > r->start && addr < r->end) {
            const uint64_t delta = addr - r->start;
            VmaRegion* tail = new VmaRegion{};
            if (!tail) return false;
            tail->start = addr;
            tail->end = r->end;
            tail->prot = r->prot;
            tail->file = r->file;
            tail->shared = r->shared;
            // The file window splits at `delta`: the tail starts that far in.
            tail->fileOffset = r->fileOffset + delta;
            tail->fileLength = (r->fileLength > delta) ? (r->fileLength - delta) : 0;
            tail->next = r->next;
            // Both halves now reference the same backing file: take a second ref.
            if (r->file) {
                VFS::get().retain(r->file);
                if (r->fileLength > delta) {
                    r->fileLength = delta;
                }
            }
            r->end = addr;
            r->next = tail;
            return true;
        }
    }
    return true;
}
}

bool Process::mmapAddRegion(uint64_t start, uint64_t length, uint64_t prot) {
    if (!sharedState || length == 0) {
        return false;
    }

    const uint64_t end = start + length;
    // Drop any existing coverage first (MAP_FIXED / re-reservation semantics).
    mmapRemoveRange(start, length);

    VmaRegion* node = new VmaRegion{};
    if (!node) {
        return false;
    }
    node->start = start;
    node->end = end;
    node->prot = prot;
    node->file = nullptr;
    node->fileOffset = 0;
    node->fileLength = 0;
    node->shared = false;

    // Insert keeping the list sorted by start address.
    VmaRegion** link = &sharedState->vmaList;
    while (*link && (*link)->start < start) {
        link = &(*link)->next;
    }
    node->next = *link;
    *link = node;
    return true;
}

bool Process::mmapAddFileRegion(uint64_t start, uint64_t length, uint64_t prot,
                                FileDescriptor* file, uint64_t fileOffset,
                                uint64_t fileLength, bool shared) {
    if (!sharedState || length == 0) {
        return false;
    }

    const uint64_t end = start + length;
    // Drop any existing coverage first (MAP_FIXED / re-reservation semantics).
    // mmapRemoveRange writes back and releases any shared file mapping it evicts.
    mmapRemoveRange(start, length);

    VmaRegion* node = new VmaRegion{};
    if (!node) {
        return false;
    }
    node->start = start;
    node->end = end;
    node->prot = prot;
    node->file = file;
    node->fileOffset = fileOffset;
    node->fileLength = fileLength;
    node->shared = shared;
    if (file) {
        VFS::get().retain(file);   // the region owns a reference until dropped
    }

    // Insert keeping the list sorted by start address.
    VmaRegion** link = &sharedState->vmaList;
    while (*link && (*link)->start < start) {
        link = &(*link)->next;
    }
    node->next = *link;
    *link = node;
    return true;
}

void Process::mmapRemoveRange(uint64_t start, uint64_t length) {
    if (!sharedState || length == 0) {
        return;
    }

    const uint64_t end = start + length;
    // Split so no region straddles the range boundaries, then drop whole nodes.
    vmaSplitAt(sharedState->vmaList, start);
    vmaSplitAt(sharedState->vmaList, end);

    VmaRegion** link = &sharedState->vmaList;
    while (*link) {
        VmaRegion* r = *link;
        if (r->start >= start && r->end <= end) {
            *link = r->next;
            // Drop this region's backing-file reference. Callers that need the
            // contents flushed (munmap, MAP_FIXED replace) run mmapSyncRange()
            // before the frames are unmapped, so no writeback is needed here.
            if (r->file) {
                VFS::get().close(r->file);
            }
            delete r;
        } else {
            link = &r->next;
        }
    }
}

bool Process::mmapProtectRange(uint64_t start, uint64_t length, uint64_t prot) {
    if (!sharedState || length == 0) {
        return false;
    }

    const uint64_t end = start + length;
    if (!vmaSplitAt(sharedState->vmaList, start)) return false;
    if (!vmaSplitAt(sharedState->vmaList, end)) return false;

    for (VmaRegion* r = sharedState->vmaList; r; r = r->next) {
        if (r->start >= start && r->end <= end) {
            r->prot = prot;
        }
    }
    return true;
}

void Process::mmapSyncRange(uint64_t start, uint64_t length) {
    if (!sharedState || !sharedState->pageTable || length == 0) {
        return;
    }
    const uint64_t end = start + length;
    // Flush every writable MAP_SHARED file region overlapping the range. Whole
    // overlapping regions are written back (a superset of the requested window,
    // which is always correct).
    for (VmaRegion* r = sharedState->vmaList; r; r = r->next) {
        if (r->start < end && r->end > start) {
            vmaWritebackRegion(sharedState->pageTable, r);
        }
    }
}

bool Process::mmapRegionCovers(uint64_t addr) const {
    if (!sharedState) {
        return false;
    }
    for (VmaRegion* r = sharedState->vmaList; r; r = r->next) {
        if (addr >= r->start && addr < r->end) {
            return true;   // covered by a reserved region; will fault in on access
        }
    }
    return false;
}

bool Process::handleDemandFault(uint64_t faultAddr) {
    if (!sharedState || !sharedState->pageTable) {
        return false;
    }

    const uint64_t va = faultAddr & ~0xFFFULL;
    if (va >= 0x0000800000000000ULL) {
        return false;   // not a user address
    }

    // Find the covering region.
    VmaRegion* region = nullptr;
    for (VmaRegion* r = sharedState->vmaList; r; r = r->next) {
        if (va >= r->start && va < r->end) {
            region = r;
            break;
        }
    }
    if (!region) {
        return false;   // address is in no mmap region -> genuine fault (SIGSEGV)
    }

    // Already backed (e.g. a duplicate fault) -> nothing to do.
    if (VMM::VirtualToPhysicalIn(sharedState->pageTable, va)) {
        return true;
    }

    uint64_t phys = PMM::AllocFrame();
    if (!phys) {
        return false;   // OOM -> fatal fault
    }

    // File-backed region: page in the file contents at the mapped offset and
    // zero-fill any bytes past the file window (EOF tail / short read). The read
    // uses the vnode's positioned read, so the process's fd offset is untouched.
    if (region->file) {
        // Reach the fresh frame through the higher-half direct map (see the
        // anonymous-zero path below for why the identity map is unsafe here).
        uint8_t* dst = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(phys));
        const uint64_t filePos = region->fileOffset + (va - region->start);
        const uint64_t mappedEnd = region->fileOffset + region->fileLength;
        uint64_t got = 0;
        if (filePos < mappedEnd) {
            uint64_t want = mappedEnd - filePos;
            if (want > PAGE_SIZE) {
                want = PAGE_SIZE;
            }
            VNode* node = region->file->getNode();
            if (node && node->ops && node->ops->read) {
                int64_t rd = node->ops->read(node, dst, want, filePos);
                if (rd > 0) {
                    got = static_cast<uint64_t>(rd);
                }
            }
        }
        for (uint64_t i = got; i < PAGE_SIZE; i++) {
            dst[i] = 0;
        }
        VMM::MapPageInto(sharedState->pageTable, va, phys, vmaPageFlags(region->prot));
        return true;
    }

    // Anonymous region: zero the fresh frame before it becomes visible. Reach
    // it through the higher-half direct map, NOT the low identity map: when the
    // faulting process is a non-PIE (ET_EXEC) image mapped at a low VA, that
    // image shadows the identity VA of this frame in the active address space,
    // so an identity zero would clobber the user image (or fault) instead.
    uint64_t* z = reinterpret_cast<uint64_t*>(VMM::PhysToVirt(phys));
    for (uint64_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        z[i] = 0;
    }

    VMM::MapPageInto(sharedState->pageTable, va, phys, vmaPageFlags(region->prot));
    return true;
}

bool Process::mmapCloneRegionsFrom(const Process* parent) {
    if (!sharedState || !parent || !parent->sharedState) {
        return false;
    }

    // Copy the parent's region list verbatim (physical pages are shared
    // copy-on-write by cloneAddressSpaceFrom; not-yet-faulted pages fault in
    // independently on each side). File-backed regions carry their backing file
    // across; the child takes its own reference so both survive independently.
    VmaRegion** link = &sharedState->vmaList;
    for (VmaRegion* r = parent->sharedState->vmaList; r; r = r->next) {
        VmaRegion* copy = new VmaRegion{};
        if (!copy) {
            return false;
        }
        copy->start = r->start;
        copy->end = r->end;
        copy->prot = r->prot;
        copy->file = r->file;
        copy->fileOffset = r->fileOffset;
        copy->fileLength = r->fileLength;
        copy->shared = r->shared;
        copy->next = nullptr;
        if (copy->file) {
            VFS::get().retain(copy->file);
        }
        *link = copy;
        link = &copy->next;
    }
    return true;
}

bool Process::replaceImageFrom(Process* image) {
    if (!image || !sharedState || !image->sharedState || !image->sharedState->pageTable) {
        return false;
    }

    PageTable* oldPageTable = sharedState->pageTable;
    const uint64_t oldMmapBase = sharedState->mmapBase;
    VmaRegion* oldVmaList = sharedState->vmaList;
    const uint64_t oldUserStackBase = userStackBase;
    const uint64_t oldUserStackSize = userStackSize;
    const uint64_t oldUserStack = userStack;
    const bool oldUserStackHeapBacked = userStackHeapBacked;

    sharedState->pageTable = image->sharedState->pageTable;
    sharedState->mmapBase = image->sharedState->mmapBase;
    sharedState->vmaList = image->sharedState->vmaList;
    userStackBase = image->userStackBase;
    userStackSize = image->userStackSize;
    userStack = image->userStack;
    userStackHeapBacked = image->userStackHeapBacked;

    // The old address space (and its region list) moves to the throwaway image
    // process, which is destroyed right after exec -> releaseSharedState frees it.
    image->sharedState->pageTable = oldPageTable;
    image->sharedState->mmapBase = oldMmapBase;
    image->sharedState->vmaList = oldVmaList;
    image->userStackBase = oldUserStackBase;
    image->userStackSize = oldUserStackSize;
    image->userStack = oldUserStack;
    image->userStackHeapBacked = oldUserStackHeapBacked;

    context.cr3 = reinterpret_cast<uint64_t>(sharedState->pageTable) & ADDR_MASK;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);
    validUserState = false;
    savedUserRSP = 0;
    userFsBase = 0;
    userGsBase = 0;
    if (Scheduler::get().getCurrentProcess() == this) {
        wrmsr(0xC0000102, 0); // live user GS base of the current process
    }
    exitCode = 0;
    exitRequested = false;
    signalHandler.pending = 0;
    signalHandler.blocked = 0;
    for (int i = 0; i < NSIG; ++i) {
        signalHandler.handlers[i] = nullptr;
        signalHandler.masks[i] = 0;
        signalHandler.flags[i] = 0;
        signalHandler.restorers[i] = 0;
    }
    signalHandler.altStackSp = 0;
    signalHandler.altStackSize = 0;
    signalHandler.altStackFlags = SS_DISABLE;
    return true;
}

void Process::jumpToUsermode(uint64_t entry, GDT* gdt) {
    VMM::SetAddressSpace(getPageTable());

    if (gdt) {
        gdt->setKernelStack(kernelStack);
    }

    Syscall::get().setKernelStack(kernelStack);

    context.rip = entry;
    context.rsp = userStack;

    enterUsermode(entry, userStack);
}

void Process::sendSignal(int sig) {
    if (sig < 0 || sig >= NSIG) return;
    signalHandler.pending |= (1ULL << sig);
    if (state == ProcessState::Blocked) {
        Scheduler::get().wakeProcess(this);
    }
}

bool Process::hasDeliverableSignal() const {
    // SIGKILL can never be blocked or ignored and always interrupts a wait.
    if (signalHandler.pending & (1ULL << SIGKILL)) {
        return true;
    }

    // A pending signal only interrupts a blocking syscall (EINTR) if it would
    // actually be acted upon. Signals whose disposition is "ignore" -- either
    // SIG_IGN or SIG_DFL with a default-ignore action such as SIGCHLD -- are
    // discarded by handlePendingSignals() and must NOT wake/EINTR a waiter.
    uint64_t deliverable = signalHandler.pending & ~signalHandler.blocked;
    while (deliverable) {
        const int sig = __builtin_ctzll(deliverable);
        deliverable &= deliverable - 1;

        const sighandler_t handler = signalHandler.handlers[sig];
        if (handler == reinterpret_cast<sighandler_t>(1)) {
            continue; // SIG_IGN: explicitly ignored.
        }
        if (!handler && defaultIgnoredSignal(sig)) {
            continue; // SIG_DFL with a default-ignore disposition (SIGCHLD).
        }
        return true; // Custom handler, or a default action that terminates.
    }
    return false;
}

void Process::handlePendingSignals() {
    // Once terminated the exit code is fixed; do not let a still-pending signal
    // re-run the termination logic and clobber it (or double-report the death).
    if (state == ProcessState::Terminated) return;
    if (!signalHandler.pending) return;
    // SIGKILL wins over any lower-numbered signal that has a handler.
    if (terminateIfKilled()) return;

    for (int sig = 0; sig < NSIG; sig++) {
        if (!(signalHandler.pending & (1ULL << sig))) continue;
        if (signalHandler.blocked & (1ULL << sig)) continue;

        signalHandler.pending &= ~(1ULL << sig);

        if (sig == SIGKILL) {
            closeFilesOnExit();
            state = ProcessState::Terminated;
            exitCode = 128 + sig;
            return;
        }

        sighandler_t handler = signalHandler.handlers[sig];
        if (handler == reinterpret_cast<sighandler_t>(1)) {
            continue;
        }
        if (!handler) {
            if (defaultIgnoredSignal(sig)) {
                continue;
            }
            // A fatal signal kills the whole process, not just this thread.
            if (isThread()) {
                Scheduler::get().exitThreadGroup(this, 128 + sig);
            }
            closeFilesOnExit();
            state = ProcessState::Terminated;
            exitCode = 128 + sig;
            return;
        }

        uint64_t frameStack = context.rsp;
        const bool useAltStack =
            (signalHandler.flags[sig] & SA_ONSTACK) != 0 &&
            (signalHandler.altStackFlags & (SS_DISABLE | SS_ONSTACK)) == 0 &&
            signalHandler.altStackSp != 0 &&
            signalHandler.altStackSize >= 2048;
        const uint32_t oldAltStackFlags = signalHandler.altStackFlags;
        if (useAltStack) {
            frameStack = signalHandler.altStackSp + signalHandler.altStackSize;
            signalHandler.altStackFlags |= SS_ONSTACK;
        }

        frameStack = (frameStack - sizeof(UserSignalFrame) - 16) & ~0xFULL;
        frameStack += 8;

        const uint64_t oldBlocked = signalHandler.blocked;
        signalHandler.blocked |= signalHandler.masks[sig] | (1ULL << sig);
        if (sig == SIGKILL) {
            signalHandler.blocked &= ~(1ULL << SIGKILL);
        }

        uint64_t* stack = reinterpret_cast<uint64_t*>(frameStack);
        stack[0] = signalHandler.restorers[sig] ? signalHandler.restorers[sig] : context.rip;
        auto* frame = reinterpret_cast<UserSignalFrame*>(frameStack + sizeof(uint64_t));
        frame->rip = context.rip;
        frame->rsp = context.rsp;
        frame->blocked = oldBlocked;
        frame->rax = context.rax;
        frame->rdi = context.rdi;
        frame->rsi = context.rsi;
        frame->rdx = context.rdx;
        frame->rcx = context.rcx;
        frame->r8 = context.r8;
        frame->r9 = context.r9;
        frame->r10 = context.r10;
        frame->r11 = context.r11;
        frame->rflags = context.rflags;
        frame->altStackFlags = oldAltStackFlags;

        context.rip = reinterpret_cast<uint64_t>(handler);
        context.rsp = frameStack;
        context.rdi = sig;

        break;
    }
}

bool Process::terminateIfKilled() {
    if (state == ProcessState::Terminated || !killPending()) return false;
    signalHandler.pending &= ~(1ULL << SIGKILL);
    closeFilesOnExit();
    state = ProcessState::Terminated;
    if (!exitRequested) exitCode = 128 + SIGKILL;
    return true;
}

void Process::closeFilesOnExit() {
    // POSIX _exit semantics: drop this process's file descriptors at termination
    // so a lingering zombie no longer pins shared file descriptions (e.g. a pipe
    // write end). Without this, a peer blocked reading that pipe never sees EOF
    // until the parent reaps the zombie via wait() -- but the parent may itself
    // be the blocked reader (command substitution `$(cmd)`), which deadlocks.
    //
    // Only act when we exclusively own the descriptor table (refCount == 1).
    // Threads share the table (refCount > 1); closing it would break siblings,
    // so their descriptors are released when the last reference drops in
    // ~Process -> releaseSharedState. closeAllFiles() clears each slot, so the
    // later closeAll() at reap is a harmless no-op over these slots.
    if (!sharedState || sharedState->refCount != 1) {
        return;
    }
    sharedState->handleTable.closeAllFiles();
}

uint64_t Process::allocateFD(FileDescriptor* fd) {
    return allocateHandle(HandleType::File, kDefaultFileRights, fd, retainFileHandle, releaseFileHandle);
}

uint64_t Process::allocateFD(FileDescriptor* fd, uint32_t rights) {
    return allocateHandle(HandleType::File, rights, fd, retainFileHandle, releaseFileHandle);
}

uint64_t Process::allocateFD(FileDescriptor* fd, uint32_t rights, bool closeOnExec) {
    return sharedState
        ? sharedState->handleTable.allocate(HandleType::File, rights, fd, retainFileHandle, releaseFileHandle, closeOnExec)
        : static_cast<uint64_t>(-1);
}

uint64_t Process::allocateFDAt(int slot, FileDescriptor* fd, uint32_t rights, bool closeOnExec) {
    if (!sharedState) {
        return static_cast<uint64_t>(-1);
    }
    uint64_t handle = HandleTable::encodeHandle(HandleType::File, slot);
    if (handle == static_cast<uint64_t>(-1)) {
        return handle;
    }
    return sharedState->handleTable.allocateAt(handle, HandleType::File, rights, fd,
                                               retainFileHandle, releaseFileHandle, closeOnExec);
}

void Process::cloneHandlesFrom(Process* source, bool skipCloseOnExec) {
    if (!sharedState || !source || !source->sharedState) {
        return;
    }
    sharedState->handleTable.cloneFrom(source->sharedState->handleTable, skipCloseOnExec);
}

FileDescriptor* Process::getFD(uint64_t fileHandle) {
    return reinterpret_cast<FileDescriptor*>(getHandleObject(fileHandle, HandleType::File));
}

FileDescriptor* Process::getFD(uint64_t fileHandle, uint32_t requiredRights) {
    return reinterpret_cast<FileDescriptor*>(getHandleObject(fileHandle, HandleType::File, requiredRights));
}

void Process::closeFD(uint64_t fileHandle) {
    closeHandle(fileHandle, HandleType::File);
}

uint64_t Process::duplicateFD(uint64_t fileHandle) {
    return sharedState ? sharedState->handleTable.duplicate(fileHandle, HandleType::File) : static_cast<uint64_t>(-1);
}

bool Process::duplicateFDTo(uint64_t oldFileHandle, uint64_t newFileHandle) {
    return sharedState && sharedState->handleTable.duplicateTo(oldFileHandle, newFileHandle, HandleType::File);
}

bool Process::getHandleCloseOnExec(uint64_t handle, bool* enabled) const {
    return sharedState && sharedState->handleTable.getCloseOnExec(handle, enabled);
}

bool Process::setHandleCloseOnExec(uint64_t handle, bool enabled) {
    return sharedState && sharedState->handleTable.setCloseOnExec(handle, enabled);
}

void Process::closeOnExecHandles() {
    if (sharedState) {
        sharedState->handleTable.closeOnExecHandles();
    }
}

uint64_t Process::allocateHandle(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release) {
    return sharedState ? sharedState->handleTable.allocate(type, rights, object, retain, release) : static_cast<uint64_t>(-1);
}

bool Process::closeHandle(uint64_t handle) {
    return sharedState && sharedState->handleTable.close(handle);
}

bool Process::closeHandle(uint64_t handle, HandleType expectedType) {
    return sharedState && sharedState->handleTable.close(handle, expectedType);
}

uint64_t Process::duplicateHandle(uint64_t handle) {
    return sharedState ? sharedState->handleTable.duplicate(handle) : static_cast<uint64_t>(-1);
}

bool Process::duplicateHandleTo(uint64_t oldHandle, uint64_t newHandle) {
    return sharedState && sharedState->handleTable.duplicateTo(oldHandle, newHandle);
}

HandleEntry* Process::getHandle(uint64_t handle) {
    return sharedState ? sharedState->handleTable.get(handle) : nullptr;
}

void* Process::getHandleObject(uint64_t handle, HandleType expectedType, uint32_t requiredRights) {
    return sharedState ? sharedState->handleTable.getObject(handle, expectedType, requiredRights) : nullptr;
}
