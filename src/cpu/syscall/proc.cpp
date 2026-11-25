#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/process/exec.hpp>
#include <cpu/percpu.hpp>
#include <cpu/apic/lapic.hpp>
#include <cpu/smp.hpp>
#include <cpu/klock.hpp>
#include <interrupts/timer.hpp>
#include <time/tsc_timer.hpp>
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
constexpr uint64_t kWaitNoHang = 1;    // WNOHANG
constexpr uint64_t kWaitUntraced = 2;  // WUNTRACED  (no-op: no job-control stop state)
constexpr uint64_t kWaitContinued = 8; // WCONTINUED (no-op: no job-control continue state)
// Options we accept from userspace. WUNTRACED/WCONTINUED are tolerated so that
// libc wrappers passing them don't fail; we simply never report stopped or
// continued children because the kernel has no such states yet.
constexpr uint64_t kWaitOptionMask = kWaitNoHang | kWaitUntraced | kWaitContinued;
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

    // Count limit: ProcessExecutor::kMaxArgEnv. A longer vector fails the call
    // instead of being cut short (ar gets one argument per object file).
    constexpr int kMaxCount = ProcessExecutor::kMaxArgEnv;
    int count = 0;
    for (;;) {
        uint64_t itemPtr = 0;
        if (!Syscall::copyFromUser(&itemPtr, vectorPtr + count * sizeof(uint64_t), sizeof(itemPtr))) {
            return false;
        }
        if (itemPtr == 0) {
            break;
        }
        if (++count > kMaxCount) {
            Console::get().drawText("[PROC] exec refused: too many argv/envp entries\n");
            return false;
        }
    }

    const char** values = new const char*[count + 1];
    if (!values) {
        return false;
    }

    // Argument/environment strings can be long: gcc puts its whole command
    // line in COLLECT_GCC_OPTIONS, and make runs every recipe as one `sh -c`
    // string (binutils' recursive make lines are several KiB; a 4 KiB cap
    // failed them with EINVAL). Each string is read through one scratch buffer
    // and kept at its own length.
    constexpr uint64_t kMaxArgStringLen = ProcessExecutor::kMaxArgStringLen;
    char* scratch = count ? new char[kMaxArgStringLen] : nullptr;
    if (count && !scratch) {
        delete[] values;
        return false;
    }
    for (int i = 0; i < count; i++) {
        uint64_t itemPtr = 0;
        char* item = nullptr;
        char first = 1;
        if (Syscall::copyFromUser(&itemPtr, vectorPtr + i * sizeof(uint64_t), sizeof(itemPtr)) &&
            Syscall::copyFromUser(&first, itemPtr, 1)) {
            if (first == '\0') {
                // An empty argument is valid (ld's genscripts.sh gets several
                // ""); copyStringFromUser rejects empty strings, as paths must.
                item = new char[1];
                if (item) item[0] = '\0';
            } else if (Syscall::copyStringFromUser(itemPtr, scratch, kMaxArgStringLen)) {
                const size_t len = strlen(scratch) + 1;
                item = new char[len];
                if (item) memcpy(item, scratch, len);
            }
        }
        if (!item) {
            Console::get().drawText("[PROC] exec refused: an argv/envp string over 128 KiB or unreadable\n");
            for (int j = 0; j < i; j++) delete[] values[j];
            delete[] values;
            delete[] scratch;
            return false;
        }
        values[i] = item;
    }
    delete[] scratch;
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

    // As Linux: the signal in the low 7 bits, or the exit code in the next 8.
    if (process->termSignal) {
        return process->termSignal & 0x7F;
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

    // exit() from any thread ends the whole process (thread exit is
    // sys_thread_exit). From the main thread, onProcessTerminated does it.
    if (current->isThread()) {
        Scheduler::get().exitThreadGroup(current, (int)code);
    }
    current->setExitCode((int)code);
    current->closeFilesOnExit();
    current->setState(ProcessState::Terminated);
    Scheduler::get().onProcessTerminated(current);

    Scheduler::get().scheduleFromSyscall();
    return (uint64_t)-1;
}

uint64_t Syscall::sys_getpid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getPID() : 0;
}

uint64_t Syscall::sys_gettgid() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->getGroupPID() : 0;
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
    Process* parent = Scheduler::get().getCurrentProcess();
    if (!parent) {
        return syscall_error(SysErrInvalid);
    }

    // The child is a process with one thread, a copy of the caller (which may be
    // any thread). The parent's full userspace register state was captured into its context
    // by saveSyscallState() on syscall entry.
    const ProcessContext parentUserCtx = *parent->getContext();
    const uint64_t parentFsBase = parent->getUserFsBase();
    // The parent is the current process, so its live user GS base is in
    // IA32_KERNEL_GS_BASE rather than its (stale) saved copy.
    const uint64_t parentGsBase = rdmsr(0xC0000102);

    // Run the copy on the kernel address space so all phys pointers are usable.
    uint64_t userCR3;
    asm volatile("mov %%cr3, %0" : "=r"(userCR3));
    PageTable* kernelPML4 = VMM::GetAddressSpace();
    VMM::SetAddressSpace(kernelPML4);

    uint32_t childPid = Scheduler::get().allocatePID();
    Process* child = new Process(childPid);
    if (!child || !child->getPageTable()) {
        delete child;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrNoMemory);
    }

    if (!child->cloneAddressSpaceFrom(parent)) {
        delete child;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrNoMemory);
    }

    // Inherit the parent's demand-paged mmap region list (lazy pages fault in
    // per-child; already-faulted pages are shared copy-on-write above).
    if (!child->mmapCloneRegionsFrom(parent)) {
        delete child;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrNoMemory);
    }

    // Inherit identity, cwd, name, fd table, and signal dispositions.
    child->setParentPID(parent->getGroupPID());
    child->setUID(parent->getUID());
    child->setGID(parent->getGID());
    child->setSessionID(parent->getSessionID());
    child->setCwd(parent->getCwd());
    child->setName(parent->getName());
    child->setUserFsBase(parentFsBase);
    child->setUserGsBase(parentGsBase);
    child->setCpuAffinity(parent->getCpuAffinity());
    child->cloneHandlesFrom(parent, false);

    SignalHandler* dst = child->getSignalHandler();
    const SignalHandler* src = parent->getSignalHandler();
    for (int i = 0; i < NSIG; i++) {
        dst->handlers[i] = src->handlers[i];
        dst->masks[i] = src->masks[i];
        dst->flags[i] = src->flags[i];
        dst->restorers[i] = src->restorers[i];
    }
    dst->blocked = src->blocked;
    dst->pending = 0;  // pending signals are not inherited
    dst->altStackSp = src->altStackSp;   // the copied memory holds it
    dst->altStackSize = src->altStackSize;
    dst->altStackFlags = src->altStackFlags;

    // Arrange for the child to resume in usermode returning 0.
    child->setupForkResume(parentUserCtx, parentFsBase);
    child->setState(ProcessState::Ready);

    Scheduler::get().addProcess(child);

    asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
    return childPid;
}

uint64_t Syscall::sys_exec(uint64_t path, uint64_t argv, uint64_t envp) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return syscall_error(SysErrInvalid);
    }
    if (pathname[0] == '\0') {
        return syscall_error(SysErrNoEntry);
    }
    char absolutePath[256];
    if (!makeAbsolutePath(pathname, absolutePath)) {
        return syscall_error(SysErrNameTooLong);
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

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(absolutePath, argc, kernelArgv, envc, kernelEnvp);

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

    // Past the point of failure: like Linux, the other threads die before the
    // image is replaced. They may be running on other CPUs in the old address
    // space, and their teardown unmaps their stacks from it, so wait until they
    // are reaped. Sleeping, not yielding, lets go of the BKL they need to exit.
    while (Scheduler::get().killSiblingThreads(current)) {
        current->sleepUntil(Timer::get().getMilliseconds() + 1);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
    }
    current->clearSleep();
    VMM::SetAddressSpace(kernelPML4);

    const uint64_t imageStack = newProc->getContext()->rsp;
    const uint64_t entry = *reinterpret_cast<uint64_t*>(imageStack);
    if (!current->replaceImageFrom(newProc)) {
        delete newProc;
        asm volatile("mov %0, %%cr3" :: "r"(userCR3) : "memory");
        return syscall_error(SysErrInvalid);
    }
    current->closeOnExecHandles();
    current->setName(absolutePath);

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
    if ((options & ~kWaitOptionMask) != 0) {
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
        // Declare intent to block BEFORE scanning for a reapable child. Kernel
        // syscalls run non-preemptibly here (the timer only reschedules when it
        // interrupts user mode / the idle task), so nothing can run between the
        // scan and the block. Marking Blocked first is what makes the wake-up
        // level-triggered: a child that terminates after this point takes the
        // "parent is Blocked -> wake it" path in onProcessTerminated(), while a
        // child that already terminated is found by the scan below. Either way
        // the wake-up cannot be lost.
        current->setState(ProcessState::Blocked);

        bool hasMatchingChild = false;
        Process* child = Scheduler::get().findChild(current->getGroupPID(), requestedPid, true, &hasMatchingChild);
        if (child) {
            current->setState(ProcessState::Running);
            const uint32_t childPid = child->getPID();
            const int status = waitStatusFor(child);
            if (statusPtr && !copyToUser(statusPtr, &status, sizeof(status))) {
                return syscall_error(SysErrInvalid);
            }

            Scheduler::get().removeProcess(childPid);
            return childPid;
        }

        if (!hasMatchingChild) {
            current->setState(ProcessState::Running);
            return syscall_error(SysErrNoChild);
        }

        if ((options & kWaitNoHang) != 0) {
            current->setState(ProcessState::Running);
            return 0;
        }

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

uint64_t Syscall::sys_yield() {
    Scheduler::get().yield();
    return 0;
}

namespace {
uint64_t onlineCpuMask() {
    const uint32_t count = SMP::onlineCount();
    return count >= 64 ? ~0ULL : (1ULL << count) - 1;
}

// pid 0 is the calling thread; any other pid names one thread.
Process* affinityTarget(uint64_t pid) {
    if (pid == 0) return Scheduler::get().getCurrentProcess();
    if (pid > UINT32_MAX) return nullptr;
    return Scheduler::get().getProcessByPID(static_cast<uint32_t>(pid));
}
}

// Linux sched_setaffinity: `size` bytes of CPU bitmask (bit = SMP index).
// Bits past the online CPUs are ignored; a mask with no online CPU is EINVAL.
uint64_t Syscall::sys_sched_setaffinity(uint64_t pid, uint64_t size, uint64_t maskPtr) {
    if (size == 0) return syscall_error(SysErrInvalid);
    uint64_t mask = 0;
    if (!copyFromUser(&mask, maskPtr, size < sizeof(mask) ? size : sizeof(mask))) {
        return syscall_error(SysErrInvalid);
    }
    mask &= onlineCpuMask();
    if (mask == 0) return syscall_error(SysErrInvalid);

    Process* target = affinityTarget(pid);
    if (!target || target->getState() == ProcessState::Terminated) {
        return syscall_error(SysErrNoProcess);
    }
    target->setCpuAffinity(mask);

    if (target == Scheduler::get().getCurrentProcess()) {
        // Requeue: the scheduler hands it to a CPU it may run on.
        if (!target->mayRunOn(currentCpuIndex())) Scheduler::get().yield();
        return 0;
    }
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (cpu && cpu->currentProcess == target && !target->mayRunOn(i)) {
            SMP::sendReschedule(cpu);
        }
    }
    return 0;
}

// Linux sched_getaffinity: needs room for every online CPU, in whole 8-byte
// words; writes one word and returns its size.
uint64_t Syscall::sys_sched_getaffinity(uint64_t pid, uint64_t size, uint64_t maskPtr) {
    if (size * 8 < SMP::onlineCount() || (size & 7) != 0) {
        return syscall_error(SysErrInvalid);
    }
    Process* target = affinityTarget(pid);
    if (!target) return syscall_error(SysErrNoProcess);

    const uint64_t mask = target->getCpuAffinity() & onlineCpuMask();
    if (!copyToUser(maskPtr, &mask, sizeof(mask))) return syscall_error(SysErrInvalid);
    return sizeof(mask);
}

uint64_t Syscall::sys_getcpu() {
    return currentCpuIndex();
}

// Returns the number of online CPUs; with entryPtr, also fills a CpuInfoEntry
// for CPU `index`.
uint64_t Syscall::sys_cpu_info(uint64_t index, uint64_t entryPtr) {
    const uint32_t count = SMP::onlineCount();
    if (entryPtr == 0) return count;
    PerCPUData* cpu = index < count ? SMP::cpu(static_cast<uint32_t>(index)) : nullptr;
    if (!cpu) return syscall_error(SysErrInvalid);

    CpuInfoEntry entry {};
    entry.index = cpu->cpuIndex;
    entry.lapicId = cpu->lapicId;
    entry.deviceIrqs = __atomic_load_n(&cpu->deviceIrqs, __ATOMIC_RELAXED);
    Process* running = cpu->currentProcess;
    entry.currentPid = running && running != cpu->idleProcess ? running->getPID() : 0;
    if (!copyToUser(entryPtr, &entry, sizeof(entry))) return syscall_error(SysErrInvalid);
    return count;
}

namespace {
// struct itimerval: it_interval, then it_value, each a struct timeval.
struct KernelTimeval {
    int64_t seconds;
    int64_t microseconds;
};
struct KernelItimerval {
    KernelTimeval interval;
    KernelTimeval value;
};
constexpr uint64_t kItimerReal = 0;

uint64_t timevalToMs(const KernelTimeval& tv) {
    // Round up: a nonzero timer must not become "off".
    return static_cast<uint64_t>(tv.seconds) * 1000 + (static_cast<uint64_t>(tv.microseconds) + 999) / 1000;
}

KernelTimeval msToTimeval(uint64_t ms) {
    return KernelTimeval { static_cast<int64_t>(ms / 1000), static_cast<int64_t>((ms % 1000) * 1000) };
}

void currentItimer(Process* process, KernelItimerval* out) {
    const uint64_t now = time_get_uptime_ms();
    const uint64_t left = process->alarmDeadlineMs > now ? process->alarmDeadlineMs - now
                        : (process->alarmDeadlineMs != 0 ? 1 : 0);
    out->interval = msToTimeval(process->alarmIntervalMs);
    out->value = msToTimeval(left);
}
}  // namespace

// setitimer(2) for ITIMER_REAL; the virtual and profiling timers are not
// supported. Resolution is the 1 ms tick.
uint64_t Syscall::sys_setitimer(uint64_t which, uint64_t newValue, uint64_t oldValue) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || which != kItimerReal) {
        return syscall_error(SysErrInvalid);
    }
    KernelItimerval next {};
    if (newValue != 0 && !copyFromUser(&next, newValue, sizeof(next))) {
        return syscall_error(SysErrInvalid);
    }
    if (next.value.seconds < 0 || next.value.microseconds < 0 || next.value.microseconds >= 1000000 ||
        next.interval.seconds < 0 || next.interval.microseconds < 0 || next.interval.microseconds >= 1000000) {
        return syscall_error(SysErrInvalid);
    }
    if (oldValue != 0) {
        KernelItimerval old {};
        currentItimer(current, &old);
        if (!copyToUser(oldValue, &old, sizeof(old))) {
            return syscall_error(SysErrInvalid);
        }
    }
    if (newValue != 0) {
        const uint64_t ms = timevalToMs(next.value);
        current->alarmDeadlineMs = ms ? time_get_uptime_ms() + ms : 0;
        current->alarmIntervalMs = ms ? timevalToMs(next.interval) : 0;
    }
    return 0;
}

uint64_t Syscall::sys_getitimer(uint64_t which, uint64_t value) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || which != kItimerReal) {
        return syscall_error(SysErrInvalid);
    }
    KernelItimerval now {};
    currentItimer(current, &now);
    return copyToUser(value, &now, sizeof(now)) ? 0 : syscall_error(SysErrInvalid);
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
    if (current && current->isThread()) {
        current = Scheduler::get().getProcessByPID(current->getGroupPID());
    }
    return current ? current->getParentPID() : 0;
}

uint64_t Syscall::sys_spawn(uint64_t path, uint64_t argv, uint64_t envp) {
    char pathname[256];
    if (!copyUserString(path, pathname, sizeof(pathname))) {
        return syscall_error(SysErrInvalid);
    }
    if (pathname[0] == '\0') {
        return syscall_error(SysErrNoEntry);
    }
    char absolutePath[256];
    if (!makeAbsolutePath(pathname, absolutePath)) {
        return syscall_error(SysErrNameTooLong);
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

    // Load the ELF with the kernel page table active. The loader writes freshly
    // allocated segment frames through the identity map (memset/memcpy on the
    // physical address); with a user page table active, a non-PIE image's low
    // segment mappings (0x400000+) shadow the identity map, so an identity write
    // to a colliding physical frame lands on a read-only user page and faults.
    // sys_exec already switches address spaces before loading; sys_spawn must do
    // the same and then restore the caller's address space (it returns to the
    // caller rather than replacing the image).
    uint64_t callerCR3;
    asm volatile("mov %%cr3, %0" : "=r"(callerCR3));
    VMM::SetAddressSpace(VMM::GetKernelAddressSpace());

    Process* newProc = ProcessExecutor::loadUserBinaryWithArgs(absolutePath, argc, kernelArgv, envc, kernelEnvp);

    asm volatile("mov %0, %%cr3" : : "r"(callerCR3) : "memory");

    freeStringVector(kernelArgv, argc);
    freeStringVector(kernelEnvp, envc);

    if (!newProc) {
        return syscall_error(SysErrNoEntry);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (current) {
        newProc->setParentPID(current->getGroupPID());
        newProc->setUID(current->getUID());
        newProc->setGID(current->getGID());
        newProc->setSessionID(current->getSessionID());
        newProc->setCwd(current->getCwd());
        newProc->setCpuAffinity(current->getCpuAffinity());
        // Inherit the parent's open file descriptors (stdin/stdout/stderr and
        // any others) so spawned programs share the controlling terminal.
        // Only those: a new program cannot use the parent's windows, surfaces
        // or service connections, and holding them would keep the parent's
        // window on screen after it exits, for as long as the child runs.
        newProc->cloneHandlesFrom(current, true, true);
    }

    Scheduler::get().addProcess(newProc);

    uint64_t pid = newProc->getPID();
    return pid;
}

namespace {
constexpr uint64_t kRlimitNofile = 7;            // Linux's RLIMIT_NOFILE
constexpr uint64_t kRlimInfinity = ~0ULL;
struct RlimitPair {
    uint64_t cur;
    uint64_t max;
};
}

// Resource limits. Only RLIMIT_NOFILE means anything: mlibc keeps its fd
// numbers below the soft limit (the kernel's handles are more than files and
// stop at HandleTable::MaxHandles). The others read as unlimited and setting
// them does nothing, as before.
uint64_t Syscall::sys_getrlimit(uint64_t resource, uint64_t limitPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (resource > 15) return syscall_error(SysErrInvalid);
    RlimitPair limit{kRlimInfinity, kRlimInfinity};
    if (resource == kRlimitNofile) {
        current->getNofileLimit(&limit.cur, &limit.max);
    }
    if (!copyToUser(limitPtr, &limit, sizeof(limit))) return syscall_error(SysErrFault);
    return 0;
}

uint64_t Syscall::sys_setrlimit(uint64_t resource, uint64_t limitPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (resource > 15) return syscall_error(SysErrInvalid);
    RlimitPair limit{};
    if (!copyFromUser(&limit, limitPtr, sizeof(limit))) return syscall_error(SysErrFault);
    if (limit.cur > limit.max) return syscall_error(SysErrInvalid);
    if (resource != kRlimitNofile) return 0;
    uint64_t soft = 0, hard = 0;
    current->getNofileLimit(&soft, &hard);
    // As Linux: never above the system's ceiling (nr_open), and raising the hard
    // limit is for root.
    if (limit.max > HandleTable::MaxNofile) return syscall_error(SysErrPermission);
    if (limit.max > hard && current->getUID() != 0) return syscall_error(SysErrPermission);
    current->setNofileLimit(limit.cur, limit.max);
    return 0;
}

uint64_t Syscall::sys_fdtable_stash(uint64_t ptr, uint64_t count) {
    // Snapshot libc's fd-number -> handle table into the Process so it survives
    // the image replacement in the following exec. See Process::getFdStash().
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (count > static_cast<uint64_t>(Process::kFdStashMax)) {
        count = static_cast<uint64_t>(Process::kFdStashMax);
    }
    if (count == 0) { current->dropFdStash(); current->setFdStashCount(0); return 0; }
    if (!isValidUserPointer(ptr, count * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }
    uint64_t* stash = current->makeFdStash(static_cast<int>(count));
    if (!stash) {
        return syscall_error(SysErrNoMemory);
    }
    if (!copyFromUser(stash, ptr, count * sizeof(uint64_t))) {
        current->dropFdStash();
        return syscall_error(SysErrInvalid);
    }
    current->setFdStashCount(static_cast<int>(count));
    return 0;
}

uint64_t Syscall::sys_fdtable_fetch(uint64_t ptr, uint64_t count) {
    // Restore the fd-table snapshot at process entry, validating each entry
    // against the live handle table so CLOEXEC handles closed during exec are
    // dropped. Returns 0 (no-op) for a process that never stashed (e.g. the
    // initial process, or a fork child that did not exec).
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    int stashed = current->getFdStashCount();
    if (stashed <= 0) return 0;
    // (0, 0): how many entries there are, for a caller sizing its table.
    if (ptr == 0 && count == 0) return static_cast<uint64_t>(stashed);
    if (count > static_cast<uint64_t>(stashed)) {
        count = static_cast<uint64_t>(stashed);
    }
    if (!isValidUserPointer(ptr, count * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }
    uint64_t* stash = current->getFdStash();
    // fd 0/1/2 use the stdio handle convention (values 0/1/2, not real handles);
    // always keep them. For fd>=3, drop handles that no longer exist.
    for (uint64_t i = 3; i < count; i++) {
        if (stash[i] != 0 && current->getHandle(stash[i]) == nullptr) {
            stash[i] = 0;
        }
    }
    if (!copyToUser(ptr, stash, count * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }
    current->dropFdStash();   // one-shot per exec
    return count;
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
    thread->setCpuAffinity(current->getCpuAffinity());
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

// POSIX/mlibc-style thread spawn: the caller (mlibc's Clone sysdep) supplies an
// already-prepared user stack (from PrepareStack) whose top holds the mlibc
// thread-entry arguments, and the new thread begins at `entry`
// (mlibc's __mlibc_thread_entry). Unlike sys_thread_create this does NOT hand
// back a kernel join handle: mlibc joins in userspace via a futex on the TCB's
// didExit flag, so we return the new thread's TID directly. The thread sets its
// own TLS (fs base) via SET_THREAD_POINTER from its trampoline.
uint64_t Syscall::sys_thread_clone(uint64_t entry, uint64_t userStack) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || entry == 0 || !isValidUserPointer(entry, 1)) {
        return syscall_error(SysErrInvalid);
    }
    // The prepared stack must be a valid, writable user pointer; the trampoline
    // immediately pops four words (entry/tcb/arg/nul) from it.
    if (userStack == 0 || !isValidUserPointer(userStack, 4 * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }

    // Sharing the parent's page table is what makes this a thread rather than a
    // process. mlibc runs it on `userStack`, so the kernel makes no stack.
    Process* thread = new Process(Scheduler::get().allocatePID(), current, Process::kNoUserStack);
    if (!thread || !thread->getPageTable() || !thread->getKernelStack()) {
        delete thread;
        return syscall_error(SysErrNoMemory);
    }

    auto* object = new ThreadObject();
    if (!object) {
        delete thread;
        return syscall_error(SysErrNoMemory);
    }

    // refCount=1: only the thread's own Process references it (no join handle).
    // ~Process() releases it (1->0) when the thread is reaped, and the exit
    // status is observed by mlibc through the TCB, not this object.
    object->tid = thread->getPID();
    object->refCount = 1;
    object->completed = false;
    object->exitCode = 0;
    object->process = thread;

    thread->setThreadObject(object);
    thread->setParentPID(current->getPID());
    thread->setUID(current->getUID());
    thread->setGID(current->getGID());
    thread->setSessionID(current->getSessionID());
    thread->setPriority(current->getPriority());
    thread->setCpuAffinity(current->getCpuAffinity());
    thread->setCwd(current->getCwd());
    thread->setName(current->getName());

    thread->setUserStack(userStack);

    // threadTrampoline pops entry/userStack/arg from the kernel stack, then
    // iretq's to `entry` with rsp=userStack. arg is unused by the mlibc entry
    // stub (which reads everything off userStack), so pass 0.
    uint64_t kernelStack = thread->getKernelStack() & ~0xFULL;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = 0;          // arg (unused)
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = userStack;  // initial user rsp
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = entry;      // __mlibc_thread_entry

    thread->getContext()->rip = reinterpret_cast<uint64_t>(&threadTrampoline);
    thread->getContext()->rsp = kernelStack;
    thread->getContext()->rbp = 0;
    thread->getContext()->rflags = 0x202;

    Scheduler::get().addProcess(thread);
    return static_cast<uint64_t>(thread->getPID());
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
        if (object->joiners) Scheduler::get().wakeThreadsOf(current->getGroupPID());
    }

    current->setExitCode(static_cast<int>(code));
    current->setState(ProcessState::Terminated);
    Scheduler::get().scheduleFromSyscall();
    return static_cast<uint64_t>(-1);
}

// The thread ends, then its word says so: whoever waits for that may free the
// thread's stack and TLS, since nothing runs in user mode on them any more.
uint64_t Syscall::sys_thread_exit_notify(uint64_t word) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (current && current->isThread() && (word & 3) == 0 && isValidUserPointer(word, sizeof(uint32_t))) {
        __atomic_store_n(reinterpret_cast<uint32_t*>(word), 0U, __ATOMIC_SEQ_CST);
        sys_futex_wake(word, ~0ULL);   // under the big kernel lock, as is the exit
    }
    return sys_thread_exit(0);
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

        // Woken by the thread's exit (sys_thread_exit); the deadline is for a
        // thread that ends in another way.
        object->joiners++;
        current->sleepUntil(time_get_uptime_ms() + 100, true);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();
        object->joiners--;
        if (current->killPending()) {
            object->release();
            return syscall_error(SysErrInterrupted);
        }
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

// First C code on syscall entry (interrupts are off via SFMASK): take the big
// kernel lock for the whole syscall. restoreSyscallState() drops it.
extern "C" void saveSyscallState(uint64_t* stack) {
    KernelLock::enter(stack[14] < KernelLock::kSiteIrq ? static_cast<uint32_t>(stack[14])
                                                      : KernelLock::kSiteOther);
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

static uint64_t restoreSyscallStateLocked(uint64_t* stack, uint64_t result);

// Last C code before sysret: drops the big kernel lock taken in
// saveSyscallState() (the syscall may have been resumed on another CPU; the
// lock moved with it through the context switch). Nonzero: return by iretq
// (syscallEntry), with rip and rflags in PerCPUData.
extern "C" uint64_t restoreSyscallState(uint64_t* stack, uint64_t result) {
    const uint64_t full = restoreSyscallStateLocked(stack, result);
    KernelLock::leave();
    return full;
}

static uint64_t restoreSyscallStateLocked(uint64_t* stack, uint64_t result) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !stack) return 0;

    ProcessContext* context = current->getContext();
    if (!context) return 0;

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
        if (current->getState() == ProcessState::Terminated) {
            // Same hazard as syscallHandler(): a fatal signal terminated us on
            // the way back to user mode. Do not restore/return; reschedule so
            // the parent is notified and we never run again. (Never returns.)
            Scheduler::get().scheduleFromSyscall();
        }
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
    if (current->fullUserRestore) {
        // sigreturn checked rip, rsp and rflags (restoreSignalFrame).
        current->fullUserRestore = false;
        stack[4] = context->r11;
        stack[12] = context->rcx;
        getPerCPU()->iretRip = context->rip;
        getPerCPU()->iretRflags = context->rflags;
        return 1;
    }
    return 0;
}
