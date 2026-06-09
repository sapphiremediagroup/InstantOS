#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/process/exec.hpp>
#include <cpu/percpu.hpp>
#include <interrupts/timer.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>
#include <common/ports.hpp>
#include <graphics/console.hpp>

extern "C" void threadTrampoline();
extern "C" void processTrampoline();

namespace {
constexpr uint64_t kDefaultThreadStackSize = 16 * PAGE_SIZE;
constexpr uint64_t kMinThreadStackSize = 4 * PAGE_SIZE;
constexpr uint64_t kMaxThreadStackSize = 256 * PAGE_SIZE;
constexpr uint64_t kWaitNoHang = 1;
constexpr uint32_t kMsrFsBase = 0xC0000100;

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void retainThreadObject(void* object) {
    reinterpret_cast<ThreadObject*>(object)->retain();
}

void releaseThreadObject(void* object) {
    reinterpret_cast<ThreadObject*>(object)->release();
}

bool copyStringVectorFromUser(uint64_t vectorPtr, int* outCount, const char*** outValues) {
    if (!outCount || !outValues) {
        return false;
    }

    *outCount = 0;
    *outValues = nullptr;

    if (vectorPtr == 0) {
        return true;
    }

    if (!Syscall::isValidUserPointer(vectorPtr, sizeof(uint64_t))) {
        return false;
    }

    int count = 0;
    while (count < 64) {
        uint64_t itemPtr = 0;
        if (!Syscall::copyFromUser(&itemPtr, vectorPtr + count * sizeof(uint64_t), sizeof(itemPtr))) {
            return false;
        }
        if (itemPtr == 0) {
            break;
        }
        count++;
    }

    const char** values = new const char*[count + 1];
    if (!values) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        uint64_t itemPtr = 0;
        if (!Syscall::copyFromUser(&itemPtr, vectorPtr + i * sizeof(uint64_t), sizeof(itemPtr))) {
            for (int j = 0; j < i; j++) delete[] values[j];
            delete[] values;
            return false;
        }

        char* item = new char[256];
        if (!Syscall::copyStringFromUser(itemPtr, item, 256)) {
            delete[] item;
            for (int j = 0; j < i; j++) delete[] values[j];
            delete[] values;
            return false;
        }
        values[i] = item;
    }
    values[count] = nullptr;

    *outCount = count;
    *outValues = values;
    return true;
}

void freeStringVector(const char** values, int count) {
    if (!values) {
        return;
    }

    for (int i = 0; i < count; i++) {
        delete[] values[i];
    }
    delete[] values;
}

int waitStatusFor(Process* process) {
    if (!process) {
        return 0;
    }

    const int code = process->getExitCode() & 0xFF;
    return code << 8;
}

uint32_t procStateValue(ProcessState state) {
    switch (state) {
        case ProcessState::Ready: return 0;
        case ProcessState::Running: return 1;
        case ProcessState::Blocked: return 2;
        case ProcessState::Terminated: return 3;
    }
    return 0;
}

void copyProcessName(char* destination, const char* source, uint64_t size) {
    if (!destination || size == 0) {
        return;
    }

    uint64_t i = 0;
    if (source) {
        while (i + 1 < size && source[i] != '\0') {
            destination[i] = source[i];
            i++;
        }
    }
    destination[i] = '\0';
}
}

uint64_t Syscall::sys_exit(uint64_t code) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return (uint64_t)-1;
    }

    current->setExitCode((int)code);
    current->setState(ProcessState::Terminated);
    Scheduler::get().wakeParentOf(current);

    Scheduler::get().scheduleFromSyscall();
    return (uint64_t)-1;
}

uint64_t Syscall::sys_getpid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getPID() : 0;
}

uint64_t Syscall::sys_procinfo(uint64_t entriesPtr, uint64_t capacity, uint64_t totalPtr) {
    if (capacity > 0 && !isValidUserPointer(entriesPtr, capacity * sizeof(ProcInfoEntry))) {
        return syscall_error(SysErrInvalid);
    }
    if (totalPtr && !isValidUserPointer(totalPtr, sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }

    uint64_t total = 0;
    uint64_t copied = 0;
    Process* process = Scheduler::get().getAllProcessesHead();
    while (process) {
        if (copied < capacity) {
            ProcInfoEntry entry = {};
            entry.pid = process->getPID();
            entry.parentPID = process->getParentPID();
            entry.uid = process->getUID();
            entry.gid = process->getGID();
            entry.sessionID = process->getSessionID();
            entry.state = procStateValue(process->getState());
            entry.priority = static_cast<uint32_t>(process->getPriority());
            entry.flags = process->isThread() ? 1u : 0u;
            entry.exitCode = process->getExitCode();
            copyProcessName(entry.name, process->getName(), sizeof(entry.name));

            if (!copyToUser(entriesPtr + copied * sizeof(ProcInfoEntry), &entry, sizeof(entry))) {
                return syscall_error(SysErrInvalid);
            }
            copied++;
        }

        total++;
        process = process->allNext;
    }

    if (totalPtr && !copyToUser(totalPtr, &total, sizeof(total))) {
        return syscall_error(SysErrInvalid);
    }

    return copied;
}

uint64_t Syscall::sys_fork() {
    return syscall_error(SysErrNoSys);
}

uint64_t Syscall::sys_exec(uint64_t path, uint64_t argv, uint64_t envp) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return syscall_error(SysErrInvalid);
    }

    int argc = 0;
    const char** kernelArgv = nullptr;
    if (!copyStringVectorFromUser(argv, &argc, &kernelArgv)) {
        return syscall_error(SysErrInvalid);
    }

    int envc = 0;
    const char** kernelEnvp = nullptr;
    if (!copyStringVectorFromUser(envp, &envc, &kernelEnvp)) {
        freeStringVector(kernelArgv, argc);
        return syscall_error(SysErrInvalid);
    }
    
    uint64_t userCR3;
    asm volatile("mov %%cr3, %0" : "=r"(userCR3));

    PageTable* kernelPML4 = VMM::GetAddressSpace();
    VMM::SetAddressSpace(kernelPML4);

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(pathname, argc, kernelArgv, envc, kernelEnvp);

    freeStringVector(kernelArgv, argc);
    freeStringVector(kernelEnvp, envc);

    if (!newProc) {
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrNoEntry);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || current->isThread()) {
        delete newProc;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrInvalid);
    }

    const uint64_t imageStack = newProc->getContext()->rsp;
    const uint64_t entry = *reinterpret_cast<uint64_t*>(imageStack);
    if (!current->replaceImageFrom(newProc)) {
        delete newProc;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrInvalid);
    }
    current->closeOnExecHandles();
    current->setName(pathname);

    uint64_t kernelStack = current->getKernelStack() & ~0xFULL;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = current->getUserStack();
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = entry;

    current->getContext()->rax = 0;
    current->getContext()->rbx = 0;
    current->getContext()->rcx = 0;
    current->getContext()->rdx = 0;
    current->getContext()->rsi = 0;
    current->getContext()->rdi = 0;
    current->getContext()->rbp = 0;
    current->getContext()->rsp = kernelStack;
    current->getContext()->r8 = 0;
    current->getContext()->r9 = 0;
    current->getContext()->r10 = 0;
    current->getContext()->r11 = 0;
    current->getContext()->r12 = 0;
    current->getContext()->r13 = 0;
    current->getContext()->r14 = 0;
    current->getContext()->r15 = 0;
    current->getContext()->rip = reinterpret_cast<uint64_t>(&processTrampoline);
    current->getContext()->rflags = 0x202;
    current->setState(ProcessState::Running);

    delete newProc;
    Syscall::get().setKernelStack(current->getKernelStack());
    switchContext(nullptr, current->getContext());
    __builtin_unreachable();
}

uint64_t Syscall::sys_wait(uint64_t pid, uint64_t statusPtr, uint64_t options) {
    if ((options & ~kWaitNoHang) != 0) {
        return syscall_error(SysErrInvalid);
    }
    if (statusPtr && !isValidUserPointer(statusPtr, sizeof(int))) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    const int64_t requestedPid = static_cast<int64_t>(pid);
    if (requestedPid < -1) {
        return syscall_error(SysErrInvalid);
    }

    for (;;) {
        bool hasMatchingChild = false;
        Process* child = Scheduler::get().findChild(current->getPID(), requestedPid, true, &hasMatchingChild);
        if (child) {
            const uint32_t childPid = child->getPID();
            const int status = waitStatusFor(child);
            if (statusPtr && !copyToUser(statusPtr, &status, sizeof(status))) {
                return syscall_error(SysErrInvalid);
            }

            Scheduler::get().removeProcess(childPid);
            return childPid;
        }

        if (!hasMatchingChild) {
            return syscall_error(SysErrNoChild);
        }

        if ((options & kWaitNoHang) != 0) {
            return 0;
        }

        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return syscall_error(SysErrInvalid);
        }
        if (current->hasDeliverableSignal()) {
            return syscall_error(SysErrInterrupted);
        }
    }
}

uint64_t Syscall::sys_kill(uint64_t pid, uint64_t sig) {
    if (sig >= NSIG) {
        return syscall_error(SysErrInvalid);
    }

    Process* target = nullptr;
    if (pid == 0) {
        target = Scheduler::get().getCurrentProcess();
    } else if (pid <= UINT32_MAX) {
        target = Scheduler::get().getProcessByPID((uint32_t)pid);
    }
    if (!target) return syscall_error(SysErrNoEntry);

    if (sig != 0) {
        target->sendSignal((int)sig);
    }
    return 0;
}

uint64_t Syscall::sys_yield() {
    Scheduler::get().yield();
    return 0;
}

uint64_t Syscall::sys_sleep(uint64_t ms) {
    if (ms == 0) return 0;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    const uint64_t start = Timer::get().getMilliseconds();
    uint64_t target = start;
    if (UINT64_MAX - target < ms) {
        target = UINT64_MAX;
    } else {
        target += ms;
    }

    while (Timer::get().getMilliseconds() < target) {
        current->sleepUntil(target);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        if (current->hasDeliverableSignal()) {
            current->clearSleep();
            return syscall_error(SysErrInterrupted);
        }
    }

    current->clearSleep();
    return 0;
}

uint64_t Syscall::sys_getppid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getParentPID() : 0;
}

uint64_t Syscall::sys_spawn(uint64_t path, uint64_t argv, uint64_t envp) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        Console::get().drawText("[spawn] invalid path pointer\n");
        return syscall_error(SysErrInvalid);
    }
    Console::get().drawText("[spawn] path: ");
    Console::get().drawText(pathname);
    Console::get().drawText("\n");

    int argc = 0;
    const char** kernelArgv = nullptr;
    if (!copyStringVectorFromUser(argv, &argc, &kernelArgv)) {
        Console::get().drawText("[spawn] invalid argv pointer\n");
        return syscall_error(SysErrInvalid);
    }

    int envc = 0;
    const char** kernelEnvp = nullptr;
    if (!copyStringVectorFromUser(envp, &envc, &kernelEnvp)) {
        Console::get().drawText("[spawn] invalid envp pointer\n");
        freeStringVector(kernelArgv, argc);
        return syscall_error(SysErrInvalid);
    }

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(pathname, argc, kernelArgv, envc, kernelEnvp);

    freeStringVector(kernelArgv, argc);
    freeStringVector(kernelEnvp, envc);

    if (!newProc) {
        Console::get().drawText("[spawn] loadUserBinaryWithArgs failed\n");
        return syscall_error(SysErrNoEntry);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (current) {
        newProc->setParentPID(current->getPID());
        newProc->setUID(current->getUID());
        newProc->setGID(current->getGID());
        newProc->setSessionID(current->getSessionID());
        newProc->setCwd(current->getCwd());
    }

    Scheduler::get().addProcess(newProc);

    uint64_t pid = newProc->getPID();
    return pid;
}

uint64_t Syscall::sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stackSize) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || entry == 0 || !isValidUserPointer(entry, 1)) {
        return syscall_error(SysErrInvalid);
    }

    if (stackSize == 0) {
        stackSize = kDefaultThreadStackSize;
    }
    stackSize = alignUp(stackSize, PAGE_SIZE);
    if (stackSize < kMinThreadStackSize || stackSize > kMaxThreadStackSize) {
        return syscall_error(SysErrInvalid);
    }

    Process* thread = new Process(Scheduler::get().allocatePID(), current, stackSize);
    if (!thread || !thread->getPageTable() || !thread->getKernelStack() || !thread->getUserStack()) {
        delete thread;
        return syscall_error(SysErrNoMemory);
    }

    auto* object = new ThreadObject();
    if (!object) {
        delete thread;
        return syscall_error(SysErrNoMemory);
    }

    object->tid = thread->getPID();
    object->refCount = 2;
    object->completed = false;
    object->exitCode = 0;
    object->process = thread;

    thread->setThreadObject(object);
    thread->setParentPID(current->getPID());
    thread->setUID(current->getUID());
    thread->setGID(current->getGID());
    thread->setSessionID(current->getSessionID());
    thread->setPriority(current->getPriority());
    thread->setCwd(current->getCwd());
    thread->setName(current->getName());

    uint64_t userStack = thread->getUserStackBase() + thread->getUserStackSize() - 40;
    thread->setUserStack(userStack);

    uint64_t kernelStack = thread->getKernelStack() & ~0xFULL;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = arg;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = userStack;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = entry;

    thread->getContext()->rip = reinterpret_cast<uint64_t>(&threadTrampoline);
    thread->getContext()->rsp = kernelStack;
    thread->getContext()->rbp = 0;
    thread->getContext()->rflags = 0x202;

    uint64_t handle = current->allocateHandle(HandleType::Thread,
                                              HandleRightWait | HandleRightSignal | HandleRightDuplicate,
                                              object,
                                              retainThreadObject,
                                              releaseThreadObject);
    if (handle == static_cast<uint64_t>(-1)) {
        delete thread;
        object->release();
        return syscall_error(SysErrNoMemory);
    }

    Scheduler::get().addProcess(thread);
    return handle;
}

uint64_t Syscall::sys_thread_exit(uint64_t code) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    if (!current->isThread()) {
        return sys_exit(code);
    }

    if (ThreadObject* object = current->getThreadObject()) {
        object->exitCode = static_cast<int>(code);
        object->completed = true;
    }

    current->setExitCode(static_cast<int>(code));
    current->setState(ProcessState::Terminated);
    Scheduler::get().scheduleFromSyscall();
    return static_cast<uint64_t>(-1);
}

uint64_t Syscall::sys_thread_join(uint64_t handle, uint64_t statusPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    if (statusPtr && !isValidUserPointer(statusPtr, sizeof(int))) {
        return syscall_error(SysErrInvalid);
    }

    auto* object = reinterpret_cast<ThreadObject*>(
        current->getHandleObject(handle, HandleType::Thread, HandleRightWait)
    );
    if (!object || object->tid == current->getPID()) {
        return syscall_error(SysErrBadFile);
    }

    object->retain();
    while (!object->completed) {
        Process* target = object->process;
        if (!target || target->getState() == ProcessState::Terminated) {
            object->completed = true;
            if (target) {
                object->exitCode = target->getExitCode();
            }
            break;
        }

        Scheduler::get().yield();
    }

    int status = object->exitCode;
    if (statusPtr && !copyToUser(statusPtr, &status, sizeof(status))) {
        object->release();
        return syscall_error(SysErrInvalid);
    }

    current->closeHandle(handle, HandleType::Thread);
    object->release();
    return 0;
}

uint64_t Syscall::sys_set_thread_pointer(uint64_t pointer) {
    if (pointer >= 0x0000800000000000ULL) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    current->setUserFsBase(pointer);
    wrmsr(kMsrFsBase, pointer);
    return 0;
}

extern "C" void saveSyscallState(uint64_t* stack) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return;
    
    current->getContext()->r15 = stack[0];
    current->getContext()->r14 = stack[1];
    current->getContext()->r13 = stack[2];
    current->getContext()->r12 = stack[3];
    current->getContext()->r11 = stack[4];
    current->getContext()->r10 = stack[5];
    current->getContext()->r9 = stack[6];
    current->getContext()->r8 = stack[7];
    current->getContext()->rbp = stack[8];
    current->getContext()->rdi = stack[9];
    current->getContext()->rsi = stack[10];
    current->getContext()->rdx = stack[11];
    current->getContext()->rcx = stack[12];
    current->getContext()->rbx = stack[13];
    current->getContext()->rax = stack[14];
    
    current->getContext()->rip = stack[12];
    current->getContext()->rflags = stack[4];
    current->getContext()->rsp = getPerCPU()->userRSP;
    
    current->setValidUserState(true);
}

extern "C" void restoreSyscallState(uint64_t* stack, uint64_t result) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !stack) return;

    ProcessContext* context = current->getContext();
    if (!context) return;

    if (!current->hasValidUserState()) {
        context->r15 = stack[0];
        context->r14 = stack[1];
        context->r13 = stack[2];
        context->r12 = stack[3];
        context->r11 = stack[4];
        context->r10 = stack[5];
        context->r9 = stack[6];
        context->r8 = stack[7];
        context->rbp = stack[8];
        context->rdi = stack[9];
        context->rsi = stack[10];
        context->rdx = stack[11];
        context->rcx = stack[12];
        context->rbx = stack[13];
        context->rax = result;
        context->rip = stack[12];
        context->rflags = stack[4];
        context->rsp = getPerCPU()->userRSP;
        current->setValidUserState(true);
        current->handlePendingSignals();
    }

    stack[0] = context->r15;
    stack[1] = context->r14;
    stack[2] = context->r13;
    stack[3] = context->r12;
    stack[4] = context->r11;
    stack[5] = context->r10;
    stack[6] = context->r9;
    stack[7] = context->r8;
    stack[8] = context->rbp;
    stack[9] = context->rdi;
    stack[10] = context->rsi;
    stack[11] = context->rdx;
    stack[12] = context->rip;
    stack[13] = context->rbx;
    stack[14] = context->rax;

    getPerCPU()->userRSP = context->rsp;
}
