#include <cpu/process/process.hpp>
#include <memory/heap.hpp>
#include <memory/vmm.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/cpuid.hpp>
#include <fs/vfs/vfs.hpp>
#include <ipc/ipc.hpp>
#include <common/string.hpp>

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

bool defaultIgnoredSignal(int sig) {
    return sig == SIGCHLD;
}
}

struct ProcessSharedState {
    PageTable* pageTable;
    HandleTable handleTable;
    uint64_t mmapBase;
    uint32_t refCount;

    ProcessSharedState()
        : pageTable(nullptr), mmapBase(0x0000600000000000UL), refCount(1) {}
};

namespace {
constexpr uint32_t kDefaultFileRights = HandleRightRead | HandleRightWrite | HandleRightDuplicate;
constexpr uint64_t kKernelStackSize = 4 * PAGE_SIZE;

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
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
    if (state->pageTable) {
        VMM::FreeAddressSpace(state->pageTable);
    }
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

            for (int j = 0; j < 512; j++) {
                newPdpt->entries[j] = srcPdpt->entries[j];
            }
            state->pageTable->entries[i] = reinterpret_cast<uint64_t>(newPdpt) | (kPml4->entries[i] & ~ADDR_MASK);
        }
    }

    return true;
}
}

Process::Process(uint32_t pid)
    : sharedState(nullptr), sessionID(0), uid(0), gid(0), pid(pid), parentPID(0), exitCode(0),
      state(ProcessState::Ready), priority(ProcessPriority::Normal), kernelStack(0), userStack(0),
      userStackBase(0), userStackSize(0), fpuState(nullptr), userFpuState(nullptr), validUserState(false), savedUserRSP(0),
      userFsBase(0), sleepDeadlineMs(0), sleeping(false),
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
      userStackBase(0), userStackSize(0), fpuState(nullptr), validUserState(false), savedUserRSP(0),
      userFsBase(0), sleepDeadlineMs(0), sleeping(false),
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
        uint64_t ustackPhys = VMM::VirtualToPhysicalIn(getPageTable(), userStackBase);
        if (ustackPhys) {
            kfree(reinterpret_cast<void*>(ustackPhys));
        }
        VMM::UnmapRangeFrom(getPageTable(), userStackBase, userStackSize / PAGE_SIZE);
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

bool Process::replaceImageFrom(Process* image) {
    if (!image || !sharedState || !image->sharedState || !image->sharedState->pageTable) {
        return false;
    }

    PageTable* oldPageTable = sharedState->pageTable;
    const uint64_t oldMmapBase = sharedState->mmapBase;
    const uint64_t oldUserStackBase = userStackBase;
    const uint64_t oldUserStackSize = userStackSize;
    const uint64_t oldUserStack = userStack;

    sharedState->pageTable = image->sharedState->pageTable;
    sharedState->mmapBase = image->sharedState->mmapBase;
    userStackBase = image->userStackBase;
    userStackSize = image->userStackSize;
    userStack = image->userStack;

    image->sharedState->pageTable = oldPageTable;
    image->sharedState->mmapBase = oldMmapBase;
    image->userStackBase = oldUserStackBase;
    image->userStackSize = oldUserStackSize;
    image->userStack = oldUserStack;

    context.cr3 = reinterpret_cast<uint64_t>(sharedState->pageTable) & ADDR_MASK;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);
    validUserState = false;
    savedUserRSP = 0;
    userFsBase = 0;
    exitCode = 0;
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
    uint64_t deliverable = signalHandler.pending & ~signalHandler.blocked;
    deliverable |= signalHandler.pending & (1ULL << SIGKILL);
    return deliverable != 0;
}

void Process::handlePendingSignals() {
    if (!signalHandler.pending) return;

    for (int sig = 0; sig < NSIG; sig++) {
        if (!(signalHandler.pending & (1ULL << sig))) continue;
        if (signalHandler.blocked & (1ULL << sig)) continue;

        signalHandler.pending &= ~(1ULL << sig);

        if (sig == SIGKILL) {
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
