#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/process/exec.hpp>
#include <cpu/percpu.hpp>
#include <interrupts/timer.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>
#include <graphics/console.hpp>

extern "C" void threadTrampoline();

namespace {
constexpr uint64_t kDefaultThreadStackSize = 16 * PAGE_SIZE;
constexpr uint64_t kMinThreadStackSize = 4 * PAGE_SIZE;
constexpr uint64_t kMaxThreadStackSize = 256 * PAGE_SIZE;

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void retainThreadObject(void* object) {
    reinterpret_cast<ThreadObject*>(object)->retain();
}

void releaseThreadObject(void* object) {
    reinterpret_cast<ThreadObject*>(object)->release();
}
}

uint64_t Syscall::sys_exit(uint64_t code) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return (uint64_t)-1;
    }

    current->setExitCode((int)code);
    current->setState(ProcessState::Terminated);

    Scheduler::get().scheduleFromSyscall();
    return (uint64_t)-1;
}

uint64_t Syscall::sys_getpid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getPID() : 0;
}

uint64_t Syscall::sys_fork() {
    return (uint64_t)-1;
}

uint64_t Syscall::sys_exec(uint64_t path, uint64_t argv, uint64_t envp __attribute__((unused))) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return -1;
    }
    
    if (argv != 0 && !isValidUserPointer(argv, sizeof(char*))) {
        return -1;
    }
    
    int argc = 0;
    
    if (argv != 0) {
        while (argc < 64) {
            uint64_t argPtr = 0;
            if (!copyFromUser(&argPtr, argv + argc * sizeof(uint64_t), sizeof(argPtr))) {
                return -1;
            }
            if (argPtr == 0) {
                break;
            }
            argc++;
        }
    }
    
    const char** kernelArgv = new const char*[argc + 1];
    for (int i = 0; i < argc; i++) {
        uint64_t argPtr = 0;
        if (!copyFromUser(&argPtr, argv + i * sizeof(uint64_t), sizeof(argPtr))) {
            for (int j = 0; j < i; j++) delete[] kernelArgv[j];
            delete[] kernelArgv;
            return -1;
        }

        char* kernelArg = new char[256];
        if (!copyStringFromUser(argPtr, kernelArg, 256)) {
            delete[] kernelArg;
            for (int j = 0; j < i; j++) delete[] kernelArgv[j];
            delete[] kernelArgv;
            return -1;
        }
        kernelArgv[i] = kernelArg;
    }
    kernelArgv[argc] = nullptr;
    
    uint64_t userCR3;
    asm volatile("mov %%cr3, %0" : "=r"(userCR3));

    PageTable* kernelPML4 = VMM::GetAddressSpace();
    VMM::SetAddressSpace(kernelPML4);

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(pathname, argc, kernelArgv);

    for (int i = 0; i < argc; i++) {
        delete[] kernelArgv[i];
    }
    delete[] kernelArgv;

    if (!newProc) {
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return -1;
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (current) {
        newProc->setParentPID(current->getPID());
    }

    Scheduler::get().addProcess(newProc);
    Scheduler::get().scheduleFromSyscall();

    asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
    return 0;
}

uint64_t Syscall::sys_wait(uint64_t pid, uint64_t statusPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return (uint64_t)-1;
    }
        
    Process* child = Scheduler::get().getProcessByPID((uint32_t)pid);
    if (!child) {
        return (uint64_t)-1;
    }
    
    if (child->getParentPID() != current->getPID()) {
        return (uint64_t)-1;
    }
    
    if (statusPtr) {
        int status = 0;
        if (!copyToUser(statusPtr, &status, sizeof(status))) {
            return (uint64_t)-1;
        }
    }
    
    return 0;
}

uint64_t Syscall::sys_kill(uint64_t pid, uint64_t sig) {
    Process* target = Scheduler::get().getProcessByPID((uint32_t)pid);
    if (!target) return -1;
    
    target->sendSignal((int)sig);
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
        return static_cast<uint64_t>(-1);
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
    }

    current->clearSleep();
    return 0;
}

uint64_t Syscall::sys_getppid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getParentPID() : 0;
}

uint64_t Syscall::sys_spawn(uint64_t path, uint64_t argv, uint64_t envp __attribute__((unused))) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        Console::get().drawText("[spawn] invalid path pointer\n");
        return (uint64_t)-1;
    }
    Console::get().drawText("[spawn] path: ");
    Console::get().drawText(pathname);
    Console::get().drawText("\n");
    
    if (argv != 0 && !isValidUserPointer(argv, sizeof(char*))) {
        Console::get().drawText("[spawn] invalid argv pointer\n");
        return (uint64_t)-1;
    }
    
    int argc = 0;
    
    if (argv != 0) {
        while (argc < 64) {
            uint64_t argPtr = 0;
            if (!copyFromUser(&argPtr, argv + argc * sizeof(uint64_t), sizeof(argPtr))) {
                return (uint64_t)-1;
            }
            if (argPtr == 0) {
                break;
            }
            argc++;
        }
    }
    
    const char** kernelArgv = new const char*[argc + 1];
    for (int i = 0; i < argc; i++) {
        uint64_t argPtr = 0;
        if (!copyFromUser(&argPtr, argv + i * sizeof(uint64_t), sizeof(argPtr))) {
            for (int j = 0; j < i; j++) delete[] kernelArgv[j];
            delete[] kernelArgv;
            return (uint64_t)-1;
        }

        char* kernelArg = new char[256];
        if (!copyStringFromUser(argPtr, kernelArg, 256)) {
            delete[] kernelArg;
            for (int j = 0; j < i; j++) delete[] kernelArgv[j];
            delete[] kernelArgv;
            return (uint64_t)-1;
        }
        kernelArgv[i] = kernelArg;
    }
    kernelArgv[argc] = nullptr;
    
    uint64_t userCR3;
    asm volatile("mov %%cr3, %0" : "=r"(userCR3));
    
    PageTable* kernelPML4 = VMM::GetKernelAddressSpace();
    VMM::SetAddressSpace(kernelPML4);

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(pathname, argc, kernelArgv);

    for (int i = 0; i < argc; i++) {
        delete[] kernelArgv[i];
    }
    delete[] kernelArgv;

    if (!newProc) {
        Console::get().drawText("[spawn] loadUserBinaryWithArgs failed\n");
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return (uint64_t)-1;
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
    asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
    return pid;
}

uint64_t Syscall::sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stackSize) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || entry == 0 || !isValidUserPointer(entry, 1)) {
        return static_cast<uint64_t>(-1);
    }

    if (stackSize == 0) {
        stackSize = kDefaultThreadStackSize;
    }
    stackSize = alignUp(stackSize, PAGE_SIZE);
    if (stackSize < kMinThreadStackSize || stackSize > kMaxThreadStackSize) {
        return static_cast<uint64_t>(-1);
    }

    Process* thread = new Process(Scheduler::get().allocatePID(), current, stackSize);
    if (!thread || !thread->getPageTable() || !thread->getKernelStack() || !thread->getUserStack()) {
        delete thread;
        return static_cast<uint64_t>(-1);
    }

    auto* object = new ThreadObject();
    if (!object) {
        delete thread;
        return static_cast<uint64_t>(-1);
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
        return static_cast<uint64_t>(-1);
    }

    Scheduler::get().addProcess(thread);
    return handle;
}

uint64_t Syscall::sys_thread_exit(uint64_t code) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return static_cast<uint64_t>(-1);
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
        return static_cast<uint64_t>(-1);
    }

    if (statusPtr && !isValidUserPointer(statusPtr, sizeof(int))) {
        return static_cast<uint64_t>(-1);
    }

    auto* object = reinterpret_cast<ThreadObject*>(
        current->getHandleObject(handle, HandleType::Thread, HandleRightWait)
    );
    if (!object || object->tid == current->getPID()) {
        return static_cast<uint64_t>(-1);
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
        return static_cast<uint64_t>(-1);
    }

    current->closeHandle(handle, HandleType::Thread);
    object->release();
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
