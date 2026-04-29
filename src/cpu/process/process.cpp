#include <cpu/process/process.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/cpuid.hpp>
#include <fs/vfs/vfs.hpp>
#include <ipc/ipc.hpp>
#include <common/string.hpp>

extern "C" void enterUsermode(uint64_t entry, uint64_t stack);

constexpr uint64_t USER_STACK_TOP = 0x00007FFFFFFFE000;
constexpr size_t USER_STACK_PAGES = 4;

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
        PMM::FreeFrame(reinterpret_cast<uint64_t>(state->pageTable));
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

    state->pageTable = reinterpret_cast<PageTable*>(PMM::AllocFrame());
    if (!state->pageTable) {
        return false;
    }

    memset(state->pageTable, 0, PAGE_SIZE);
    PageTable* kPml4 = VMM::GetKernelAddressSpace();
    if (!kPml4) {
        PMM::FreeFrame(reinterpret_cast<uint64_t>(state->pageTable));
        state->pageTable = nullptr;
        return false;
    }

    for (int i = 256; i < 512; i++) {
        state->pageTable->entries[i] = kPml4->entries[i];
    }

    for (int i = 0; i < 256; i++) {
        if (kPml4->entries[i] & Present) {
            auto* srcPdpt = reinterpret_cast<PageTable*>(kPml4->entries[i] & ADDR_MASK);
            uint64_t newFrame = PMM::AllocFrame();
            if (newFrame) {
                auto* newPdpt = reinterpret_cast<PageTable*>(newFrame);
                for (int j = 0; j < 512; j++) {
                    newPdpt->entries[j] = srcPdpt->entries[j];
                }
                state->pageTable->entries[i] = newFrame | (kPml4->entries[i] & ~ADDR_MASK);
            }
        }
    }

    return true;
}
}

Process::Process(uint32_t pid)
    : sharedState(nullptr), sessionID(0), uid(0), gid(0), pid(pid), parentPID(0), exitCode(0),
      state(ProcessState::Ready), priority(ProcessPriority::Normal), kernelStack(0), userStack(0),
      userStackBase(0), userStackSize(0), fpuState(nullptr), validUserState(false), savedUserRSP(0),
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
    }
    signalHandler.pending = 0;
    signalHandler.blocked = 0;

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

    uint64_t pageTablePhys = VMM::VirtualToPhysical(reinterpret_cast<uint64_t>(sharedState->pageTable));
    if (!pageTablePhys) {
        pageTablePhys = reinterpret_cast<uint64_t>(sharedState->pageTable);
    }
    context.cr3 = pageTablePhys;
    context.xstate = reinterpret_cast<uint64_t>(fpuState);

    if (fpuState) {
        CPU::initializeExtendedState(fpuState);
    }
}

Process::Process(uint32_t pid, Process* sharedFrom, uint64_t stackSize)
    : sharedState(nullptr), sessionID(0), uid(0), gid(0), pid(pid), parentPID(0), exitCode(0),
      state(ProcessState::Ready), priority(ProcessPriority::Normal), kernelStack(0), userStack(0),
      userStackBase(0), userStackSize(0), fpuState(nullptr), validUserState(false), savedUserRSP(0),
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
    }
    signalHandler.pending = 0;
    signalHandler.blocked = 0;

    if (!sharedFrom || !sharedFrom->sharedState || !sharedFrom->sharedState->pageTable) {
        return;
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
        if (!handler) {
            state = ProcessState::Terminated;
            exitCode = 128 + sig;
            return;
        }

        context.rsp -= 128;
        context.rsp &= ~0xFULL;

        uint64_t* stack = reinterpret_cast<uint64_t*>(context.rsp);
        stack[0] = context.rip;

        context.rip = reinterpret_cast<uint64_t>(handler);
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
