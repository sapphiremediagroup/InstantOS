#include <cpu/process/scheduler.hpp>
#include <cpu/gdt/gdt.hpp>
#include <graphics/console.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/user/session.hpp>
#include <cpu/cpuid.hpp>
#include <cpu/percpu.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/klock.hpp>
#include <cpu/smp.hpp>
#include <common/ports.hpp>
#include <time/tsc_timer.hpp>

namespace {
constexpr uint64_t kUserCodeSelector = 0x23;
constexpr uint64_t kUserDataSelector = 0x1B;
constexpr uint32_t kMsrFsBase = 0xC0000100;
constexpr uint32_t kMsrKernelGsBase = 0xC0000102;
constexpr bool kTraceContextSwitches = true;

void traceWrite(const char* str) {
    Cereal::get().write(str);
}

void traceDec(uint64_t value) {
    char buf[21];
    int pos = 0;

    if (value == 0) {
        Cereal::get().write('0');
        return;
    }

    while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        Cereal::get().write(buf[--pos]);
    }
}

void traceHex(uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    Cereal::get().write("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        Cereal::get().write(kDigits[(value >> shift) & 0xFULL]);
    }
}

uint64_t readCR3() {
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uint64_t processCR3(Process* process) {
    if (!process) {
        return 0;
    }

    if (PageTable* pageTable = process->getPageTable()) {
        return reinterpret_cast<uint64_t>(pageTable) & ADDR_MASK;
    }

    return process->getContext()->cr3 & ADDR_MASK;
}

// Switch the user FS/GS bases from oldProcess to newProcess. In the kernel the
// current process's user GS base lives in IA32_KERNEL_GS_BASE (the GS base is
// the PerCPUData pointer), so it is saved from there before loading the next.
void switchUserSegmentBases(Process* oldProcess, Process* newProcess) {
    if (oldProcess) {
        oldProcess->setUserGsBase(rdmsr(kMsrKernelGsBase));
    }
    wrmsr(kMsrFsBase, newProcess ? newProcess->getUserFsBase() : 0);
    wrmsr(kMsrKernelGsBase, newProcess ? newProcess->getUserGsBase() : 0);
}

// One line per CPU the first time it runs something other than its idle loop,
// so a serial log shows which CPUs actually took work.
void traceFirstRunOnCpu(Process* process) {
    static bool seen[SMP::kMaxCPUs] = {};
    const uint32_t cpu = currentCpuIndex();
    if (!process || process->getPriority() == ProcessPriority::Idle ||
        cpu >= SMP::kMaxCPUs || seen[cpu]) {
        return;
    }
    seen[cpu] = true;
    traceWrite("[smp] cpu ");
    traceDec(cpu);
    traceWrite(" running pid ");
    traceDec(process->getPID());
    traceWrite(" ");
    traceWrite(process->getName());
    traceWrite("\n");
}

// The IRQ exit stub swaps to the user GS when it sees the user CS set here.
void prepareInterruptReturnToUser(InterruptFrame* frame) {
    frame->cs = kUserCodeSelector;
    frame->ss = kUserDataSelector;
    frame->rflags |= 0x200;
}

const char* stateName(ProcessState state) {
    switch (state) {
        case ProcessState::Ready:
            return "ready";
        case ProcessState::Running:
            return "running";
        case ProcessState::Blocked:
            return "blocked";
        case ProcessState::Terminated:
            return "terminated";
    }
    return "unknown";
}

void traceProcess(const char* label, Process* proc) {
    traceWrite(" ");
    traceWrite(label);
    traceWrite("{pid=");
    if (proc) {
        traceDec(proc->getPID());
        traceWrite(" state=");
        traceWrite(stateName(proc->getState()));
        traceWrite(" cr3=");
        traceHex(proc->getContext()->cr3);
        traceWrite(" kstack=");
        traceHex(proc->getKernelStack());
        traceWrite(" user_rsp=");
        traceHex(proc->getSavedUserRSP());
        traceWrite(" xstate=");
        traceHex(proc->getContext()->xstate);
        traceWrite(" fpu=");
        traceHex(reinterpret_cast<uint64_t>(proc->getFPUState()));
        traceWrite(" valid_user=");
        traceWrite(proc->hasValidUserState() ? "yes" : "no");
    } else {
        traceWrite("none");
    }
    traceWrite("}");
}

void traceSwitchInvariant(const char* path, Process* oldProc, Process* newProc) {
    if (!kTraceContextSwitches) return;

    PerCPUData* cpu = getPerCPU();
    uint64_t kernelGsBase = rdmsr(kMsrKernelGsBase);

    // traceWrite("[sched:");
    // traceWrite(path);
    // traceWrite("] live{cr3=");
    // traceHex(readCR3());
    // traceWrite(" percpu=");
    // traceHex(reinterpret_cast<uint64_t>(cpu));
    // traceWrite(" gs_kernel_base=");
    // traceHex(kernelGsBase);
    // traceWrite(" gs_match=");
    // traceWrite(kernelGsBase == reinterpret_cast<uint64_t>(cpu) ? "yes" : "no");
    // traceWrite(" cpu_kstack=");
    // traceHex(cpu ? cpu->kernelStack : 0);
    // traceWrite(" cpu_user_rsp=");
    // traceHex(cpu ? cpu->userRSP : 0);
    // traceWrite("}");
    // traceProcess("old", oldProc);
    // traceProcess("new", newProc);
    // traceWrite("\n");
}
}

// extern Console* console;

extern "C" void idleLoop();

Scheduler schedulerInstance;

Scheduler& Scheduler::get() {
    return schedulerInstance;
}

void Scheduler::initialize() {
    if (initialized) return;
    
    current() = nullptr;
    allProcessesHead = nullptr;
    for (int i = 0; i < 4; i++) {
        readyQueues[i] = nullptr;
    }
    
    nextPID = 1;

    // The boot CPU's idle process. Idle processes are not in allProcessesHead
    // or the ready queues; getNextProcess() falls back to the executing CPU's.
    idle() = createIdleProcess();

    initialized = true;
}

namespace {
// Idle is stateless: whenever a CPU switches to it, it restarts at idleLoop on
// an empty stack. Nothing is ever resumed on an idle stack, so an IRQ frame
// left there (by a frame-rewrite return to user) can never be resumed stale.
void resetIdleContext(Process* idleProcess) {
    ProcessContext* ctx = idleProcess->getContext();
    ctx->cr3 = reinterpret_cast<uint64_t>(VMM::GetKernelAddressSpace()) & ADDR_MASK;
    ctx->rip = reinterpret_cast<uint64_t>(idleLoop);
    ctx->rsp = idleProcess->getKernelStack() - 8;
    ctx->rflags = 0x202; // IF enabled
}
}

Process* Scheduler::createIdleProcess() {
    Process* idleProcess = new Process(0);
    idleProcess->setName("<idle>");
    idleProcess->setPriority(ProcessPriority::Idle);
    resetIdleContext(idleProcess);
    idleProcess->setState(ProcessState::Ready);
    return idleProcess;
}

void Scheduler::runIdleOnThisCpu() {
    Process* idleProcess = idle();
    current() = idleProcess;
    idleProcess->setState(ProcessState::Running);
    resetIdleContext(idleProcess);
    Syscall::get().setKernelStack(idleProcess->getKernelStack());
    switchContext(nullptr, idleProcess->getContext());
}

void Scheduler::onKernelEntry() {
    PerCPUData* cpu = getPerCPU();
    Process* parked = cpu->deferredProcess;
    if (!parked) return;
    cpu->deferredProcess = nullptr;

    if (parked->getState() == ProcessState::Ready) {
        addToReadyQueue(parked);
    } else if (parked->getState() == ProcessState::Terminated) {
        // A waiting parent skipped it while it was parked; let it look again.
        wakeParentOf(parked);
    }
}

bool Scheduler::tickIsNoop(InterruptFrame* frame) {
    PerCPUData* cpu = getPerCPU();
    // A parked process is only queued again by this CPU's next locked entry.
    if (cpu->deferredProcess) return false;
    for (int prio = 0; prio <= static_cast<int>(ProcessPriority::High); ++prio) {
        if (__atomic_load_n(&readyQueues[prio], __ATOMIC_RELAXED)) return false;
    }
    Process* running = cpu->currentProcess;
    if (!running || running == cpu->idleProcess || frame->cs != kUserCodeSelector) {
        return true;
    }
    // Its affinity no longer includes this CPU: the scheduler has to move it.
    if (!running->mayRunOn(cpu->cpuIndex)) return false;
    if (running->killPending()) return false;
    // Still runnable and nothing else to run: it keeps the CPU. Killed or
    // blocked from another CPU: the scheduler has to take it off.
    return running->getState() == ProcessState::Running;
}

void Scheduler::deferUntilNextEntry(Process* proc) {
    onKernelEntry();   // never more than one parked process per CPU
    removeFromReadyQueue(proc);
    getPerCPU()->deferredProcess = proc;
}

bool Scheduler::isBusyOnAnyCpu(Process* proc) {
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (cpu && (cpu->currentProcess == proc || cpu->deferredProcess == proc)) {
            return true;
        }
    }
    return current() == proc || getPerCPU()->deferredProcess == proc;
}

void Scheduler::kickIdleCpu(Process* proc) {
    if (!SMP::schedulingActive()) return;
    PerCPUData* self = getPerCPU();
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (!cpu || cpu == self || !proc->mayRunOn(i)) continue;
        if (cpu->currentProcess && cpu->currentProcess == cpu->idleProcess) {
            SMP::sendReschedule(cpu);
            return;
        }
    }
}

void Scheduler::switchTo(Process* oldProcess, Process* nextProcess) {
    if (nextProcess == idle()) {
        resetIdleContext(nextProcess);
    }
    if (!oldProcess || oldProcess == idle()) {
        // Nothing to come back to: idle restarts fresh, and the boot path
        // never resumes.
        switchContext(nullptr, nextProcess->getContext());
        return;
    }

    const uint32_t lockDepth = KernelLock::depth();
    switchContext(oldProcess->getContext(), nextProcess->getContext());
    // Resumed, possibly on another CPU, by a context that holds the BKL.
    KernelLock::setDepth(lockDepth);
}

namespace {
// A parked process (see PerCPUData::deferredProcess) is queued by its CPU's
// onKernelEntry(), never directly: some CPU may still be on its kernel stack.
bool parkedOnSomeCpu(Process* proc) {
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (cpu && cpu->deferredProcess == proc) {
            return true;
        }
    }
    return getPerCPU()->deferredProcess == proc;
}
}

void Scheduler::addToReadyQueue(Process* proc) {
    if (!proc || proc->getState() != ProcessState::Ready) return;
    if (parkedOnSomeCpu(proc)) return;

    int prio = static_cast<int>(proc->getPriority());
    if (prio < 0 || prio > 3) return;

    // Guard against double-insertion: re-adding a process already in the queue
    // would splice the list onto itself and create a cycle. Queues are short.
    for (Process* it = readyQueues[prio]; it; it = it->next) {
        if (it == proc) return;
    }

    proc->next = nullptr;

    if (!readyQueues[prio]) {
        readyQueues[prio] = proc;
    } else {
        Process* current = readyQueues[prio];
        while (current->next) {
            current = current->next;
        }
        current->next = proc;
    }
    // Re-queueing the running process (preemption) needs no wake-up: this CPU
    // is about to pick from the queue itself, unless its affinity forbids that.
    if (proc != current() || !proc->mayRunOn(currentCpuIndex())) kickIdleCpu(proc);
}

void Scheduler::addToReadyQueueFront(Process* proc) {
    if (!proc || proc->getState() != ProcessState::Ready) return;
    if (parkedOnSomeCpu(proc)) return;

    int prio = static_cast<int>(proc->getPriority());
    if (prio < 0 || prio > 3) return;

    // See addToReadyQueue(): never insert the same node twice.
    for (Process* it = readyQueues[prio]; it; it = it->next) {
        if (it == proc) return;
    }

    proc->next = readyQueues[prio];
    readyQueues[prio] = proc;
    // See addToReadyQueue().
    if (proc != current() || !proc->mayRunOn(currentCpuIndex())) kickIdleCpu(proc);
}

void Scheduler::removeFromReadyQueue(Process* proc) {
    if (!proc) return;
    
    int prio = static_cast<int>(proc->getPriority());
    if (prio < 0 || prio > 3) return;
    
    if (!readyQueues[prio]) return;
    
    if (readyQueues[prio] == proc) {
        readyQueues[prio] = proc->next;
    } else {
        Process* current = readyQueues[prio];
        while (current->next) {
            if (current->next == proc) {
                current->next = proc->next;
                break;
            }
            current = current->next;
        }
    }
    proc->next = nullptr;
}

void Scheduler::addProcess(Process* proc) {
    if (!proc) return;
    
    // Add to all processes list
    proc->allNext = allProcessesHead;
    allProcessesHead = proc;
    
    // If it's ready, add to ready queue
    if (proc->getState() == ProcessState::Ready) {
        addToReadyQueue(proc);
    }
}

void Scheduler::removeProcess(uint32_t pid) {
    if (!allProcessesHead) return;

    // Never free a process some CPU still runs or sits on the stack of; wait()
    // only gets here for children findChild() reported as fully off-CPU.
    if (Process* target = getProcessByPID(pid)) {
        if (target != current() && isBusyOnAnyCpu(target)) return;
    }

    Process* toDelete = nullptr;
    
    // Remove from all processes list
    if (allProcessesHead->getPID() == pid) {
        toDelete = allProcessesHead;
        allProcessesHead = allProcessesHead->allNext;
    } else {
        Process* current = allProcessesHead;
        while (current->allNext) {
            if (current->allNext->getPID() == pid) {
                toDelete = current->allNext;
                current->allNext = current->allNext->allNext;
                break;
            }
            current = current->allNext;
        }
    }
    
    if (toDelete) {
        // Remove from ready queue if present
        removeFromReadyQueue(toDelete);
        
        // SessionManager::get().onProcessExit(pid);
        if (current() == toDelete) current() = nullptr;

        delete toDelete;
    }
}

void Scheduler::reapTerminated() {
    Process* previous = nullptr;
    Process* process = allProcessesHead;

    while (process) {
        Process* next = process->allNext;

        // Reap terminated threads (their exit status lives in a refcounted
        // ThreadObject read by join) and terminated orphan processes (no parent
        // left to call wait()). A terminated process that still has a live
        // parent stays a zombie until the parent reaps it via sys_wait(). A
        // process some CPU still runs or sits on the kernel stack of is never
        // freed out from under it.
        const bool reapable =
            !isBusyOnAnyCpu(process) &&
            process->getState() == ProcessState::Terminated &&
            (process->isThread() || process->getParentPID() == 0);

        if (reapable) {
            if (previous) {
                previous->allNext = next;
            } else {
                allProcessesHead = next;
            }

            removeFromReadyQueue(process);
            Process* leader = nullptr;
            if (process->isThread()) {
                for (Process* p = allProcessesHead; p; p = p->allNext) {
                    if (!p->isThread() && p->sharesStateWith(process)) { leader = p; break; }
                }
            }
            delete process;
            // A dead main thread waits for its threads before wait() sees it.
            if (leader && leader->getState() == ProcessState::Terminated) {
                wakeParentOf(leader);
            }
        } else {
            previous = process;
        }

        process = next;
    }
}

Process* Scheduler::getNextProcess() {
    const uint32_t cpu = currentCpuIndex();
    // Check queues in order of priority: High (2), Normal (1), Low (0)
    for (int i = 2; i >= 0; i--) {
        Process* previous = nullptr;
        for (Process* next = readyQueues[i]; next; previous = next, next = next->next) {
            if (!next->mayRunOn(cpu)) continue;   // left for another CPU
            // Round robin within the same priority level: move it to the tail.
            if (next->next) {
                if (previous) {
                    previous->next = next->next;
                } else {
                    readyQueues[i] = next->next;
                }
                Process* tail = readyQueues[i];
                while (tail->next) {
                    tail = tail->next;
                }
                tail->next = next;
                next->next = nullptr;
            }

            return next;
        }
    }
    
    // If no user processes are ready, return the idle process
    return idle();
}

void Scheduler::schedule() {
    if (!initialized) return;

    reapTerminated();
    
    if (current()) {
        // Only SIGKILL here. This runs inside a syscall, and a handler frame
        // built into the saved context now is overwritten when the syscall
        // resumes after the switch (restoreSyscallState rebuilds the context
        // from its stack): the handler never ran but its mask stayed applied,
        // so that signal was never delivered again (an interval timer went
        // quiet after two SIGALRMs). Both syscall exits deliver signals.
        current()->terminateIfKilled();

        if (current() != idle()) {
            if (current()->getState() == ProcessState::Terminated) {
                // The current process just died here (e.g. a fatal signal
                // delivered by handlePendingSignals, or a sys_exit that landed
                // us in schedule()). Notify its parent before we switch away;
                // the actual free happens later via reapTerminated()/sys_wait().
                onProcessTerminated(current());
            } else if (current()->getState() == ProcessState::Running) {
                // Still runnable: put it back in the ready queue.
                current()->setState(ProcessState::Ready);
                addToReadyQueue(current());
            }
        }
    }
    
    Process* nextProcess = getNextProcess();
    if (!nextProcess) return;

    Process* oldProcess = current();
    current() = nextProcess;
    current()->setState(ProcessState::Running);
    current()->getContext()->cr3 = processCR3(current());
    switchUserSegmentBases(oldProcess, current());
    traceFirstRunOnCpu(current());

    // If it's a regular process, it's already removed from the ready queue head by getNextProcess
    // or specifically handled by round-robin logic there.
    // Wait, my getNextProcess round-robin logic moves it to the tail. 
    // That means it's still in the queue. We should PROBABLY remove it from the ready queue while it's running.
    if (current() != idle()) {
        removeFromReadyQueue(current());
    }

    Syscall::get().setKernelStack(nextProcess->getKernelStack());
    traceSwitchInvariant("syscall", oldProcess, nextProcess);
    
    asm volatile("cli");

    if (oldProcess) {
        if (oldProcess->getFPUState()) {
            CPU::saveExtendedState(oldProcess->getFPUState());
        }
        if (nextProcess->getFPUState()) {
            CPU::restoreExtendedState(nextProcess->getFPUState());
        }
        oldProcess->getContext()->cr3 = processCR3(oldProcess);
        oldProcess->setValidUserState(false);
        oldProcess->setSavedUserRSP(getPerCPU()->userRSP);
        switchTo(oldProcess, nextProcess);
        getPerCPU()->userRSP = current()->getSavedUserRSP();
    } else {
        if (nextProcess->getFPUState()) {
            CPU::restoreExtendedState(nextProcess->getFPUState());
        }
        switchTo(nullptr, nextProcess);
    }
    // Reached when this frame is resumed via a later switchContext() back into
    // it. oldProcess is this (now-running) process, so it cannot be Terminated;
    // notification/freeing of dead tasks is handled by onProcessTerminated() and
    // reapTerminated(). Kept only as a defensive no-op wake for the parent.
    if (oldProcess && oldProcess->getState() == ProcessState::Terminated) {
        onProcessTerminated(oldProcess);
    }
}

extern "C" void processTrampoline();

void Scheduler::schedule(InterruptFrame* frame) {
    if (!initialized || !frame) return;

    reapTerminated();
    
    Process* oldProcess = current();
    const bool interruptedUser = frame->cs == kUserCodeSelector;

    if (oldProcess && oldProcess->getFPUState()) {
        CPU::saveExtendedState(oldProcess->getFPUState());
    }

    // Killed while running user code (e.g. a sibling thread of an exec): it
    // would otherwise only notice at its next syscall.
    if (oldProcess && oldProcess != idle() && interruptedUser) {
        oldProcess->terminateIfKilled();
    }

    if (oldProcess && oldProcess->getState() == ProcessState::Terminated) {
        onProcessTerminated(oldProcess);
    }

    // Save current state if it's a user process
    if (oldProcess && interruptedUser) {
        oldProcess->getContext()->rip = frame->rip;
        oldProcess->getContext()->rsp = frame->rsp;
        oldProcess->getContext()->rflags = frame->rflags;
        oldProcess->getContext()->rax = frame->rax;
        oldProcess->getContext()->rbx = frame->rbx;
        oldProcess->getContext()->rcx = frame->rcx;
        oldProcess->getContext()->rdx = frame->rdx;
        oldProcess->getContext()->rsi = frame->rsi;
        oldProcess->getContext()->rdi = frame->rdi;
        oldProcess->getContext()->rbp = frame->rbp;
        oldProcess->getContext()->r8 = frame->r8;
        oldProcess->getContext()->r9 = frame->r9;
        oldProcess->getContext()->r10 = frame->r10;
        oldProcess->getContext()->r11 = frame->r11;
        
        oldProcess->setValidUserState(true);
        
        if (oldProcess->getState() == ProcessState::Running) {
            oldProcess->setState(ProcessState::Ready);
            if (oldProcess != idle()) addToReadyQueue(oldProcess);
        }
    } else if (oldProcess) {
        if (oldProcess->getState() == ProcessState::Running) {
            oldProcess->setState(ProcessState::Ready);
            if (oldProcess != idle()) addToReadyQueue(oldProcess);
        }
    }
    
    Process* nextProcess = getNextProcess();
    if (!nextProcess) return;

    // Idle and still nothing to run (the common idle tick): keep idling rather
    // than restarting the idle loop.
    if (nextProcess == oldProcess && oldProcess == idle()) {
        oldProcess->setState(ProcessState::Running);
        return;
    }

    const bool nextHasKernelResume = !nextProcess->hasValidUserState() && nextProcess->getSavedUserRSP() != 0;

    // Brand-new tasks have no IRQ frame and no blocked-syscall context to
    // resume. They must still enter through the normal kernel switch path.
    if (interruptedUser &&
        oldProcess &&
        oldProcess->getState() == ProcessState::Running &&
        !nextProcess->hasValidUserState() &&
        !nextHasKernelResume) {
        traceSwitchInvariant("irq-defer-kernel", oldProcess, nextProcess);
        if (oldProcess) {
            removeFromReadyQueue(oldProcess);
            oldProcess->setState(ProcessState::Running);
            current() = oldProcess;
        }
        return;
    }

    // Brand-new processes start at processTrampoline and are expected to be
    // entered via the normal kernel context-switch path, not from an IRQ frame.
    if (interruptedUser &&
        oldProcess &&
        oldProcess->getState() == ProcessState::Running &&
        !nextProcess->hasValidUserState() &&
        nextProcess->getSavedUserRSP() == 0) {
        traceSwitchInvariant("irq-defer-new", oldProcess, nextProcess);
        if (oldProcess) {
            removeFromReadyQueue(oldProcess);
            oldProcess->setState(ProcessState::Running);
            current() = oldProcess;
        }
        return;
    }
    
    if (nextProcess != idle()) {
        removeFromReadyQueue(nextProcess);
    }
    
    current() = nextProcess;
    current()->setState(ProcessState::Running);
    current()->getContext()->cr3 = processCR3(current());
    switchUserSegmentBases(oldProcess, current());
    traceFirstRunOnCpu(current());

    if (nextProcess->getFPUState()) {
        CPU::restoreExtendedState(nextProcess->getFPUState());
    }
    
    if (nextProcess->hasValidUserState()) {
        frame->rip = nextProcess->getContext()->rip;
        frame->rsp = nextProcess->getContext()->rsp;
        frame->rflags = nextProcess->getContext()->rflags;
        frame->rax = nextProcess->getContext()->rax;
        frame->rbx = nextProcess->getContext()->rbx;
        frame->rcx = nextProcess->getContext()->rcx;
        frame->rdx = nextProcess->getContext()->rdx;
        frame->rsi = nextProcess->getContext()->rsi;
        frame->rdi = nextProcess->getContext()->rdi;
        frame->rbp = nextProcess->getContext()->rbp;
        frame->r8 = nextProcess->getContext()->r8;
        frame->r9 = nextProcess->getContext()->r9;
        frame->r10 = nextProcess->getContext()->r10;
        frame->r11 = nextProcess->getContext()->r11;
        
        Syscall::get().setKernelStack(nextProcess->getKernelStack());
        
        uint64_t newCR3 = nextProcess->getContext()->cr3;
        traceSwitchInvariant("irq", oldProcess, nextProcess);
        asm volatile("mov %0, %%cr3" :: "r"(newCR3) : "memory");
        if (!interruptedUser) {
            prepareInterruptReturnToUser(frame);
        }

        // This CPU returns to nextProcess from the IRQ frame on oldProcess's
        // kernel stack, and only after irqHandler drops the BKL. Until then no
        // other CPU may run (or free) oldProcess on that stack.
        if (oldProcess && oldProcess != nextProcess && oldProcess != idle()) {
            deferUntilNextEntry(oldProcess);
        }
    } else {
        // Special case for newly created processes that haven't run yet
        Syscall::get().setKernelStack(nextProcess->getKernelStack());
        traceSwitchInvariant("irq-kernel", oldProcess, nextProcess);
        
        asm volatile("cli");
        
        if (oldProcess) {
            oldProcess->getContext()->cr3 = processCR3(oldProcess);
            oldProcess->setValidUserState(false);
            oldProcess->setSavedUserRSP(interruptedUser ? frame->rsp : getPerCPU()->userRSP);
            // No GS juggling: the IRQ entry stub already put this CPU's
            // PerCPUData in GS, which is where every kernel context expects it.
            switchTo(oldProcess, nextProcess);
            getPerCPU()->userRSP = current()->getSavedUserRSP();
        } else {
            switchTo(nullptr, nextProcess);
        }
    }
}

void Scheduler::yield() {
    schedule();
}

void Scheduler::scheduleFromSyscall() {
    schedule();
}

void Scheduler::wakeProcess(Process* process) {
    if (!process || process == idle()) {
        return;
    }

    if (process->getState() == ProcessState::Ready) {
        addToReadyQueueFront(process);
    } else if (process->getState() == ProcessState::Blocked) {
        process->clearSleep();
        process->setState(ProcessState::Ready);
        addToReadyQueueFront(process);
    }
}

void Scheduler::wakeParentOf(Process* process) {
    if (!process || process->getParentPID() == 0) {
        return;
    }

    Process* parent = getProcessByPID(process->getParentPID());
    if (parent && parent->getState() == ProcessState::Blocked) {
        wakeProcess(parent);
    }
}

void Scheduler::onProcessTerminated(Process* process) {
    if (!process) {
        return;
    }

    // Run the death bookkeeping exactly once even if several exit paths observe
    // the same termination (sys_exit + scheduler sweep, repeated schedule()s,
    // etc.). Threads are joined/reaped separately and have no children or parent
    // wait() semantics, so skip the process-level bookkeeping for them.
    if (process->isThread() || process->wasTerminationHandled()) {
        return;
    }
    process->markTerminationHandled();

    // However the main thread died, its threads go with it. wait() does not
    // report it until they are all reaped (findChild).
    killSiblingThreads(process);

    const uint32_t deadPid = process->getPID();

    // Reparent any children (live or already-zombie) to the orphan parent (0).
    // This stops them from trying to wake a parent that is going away, and lets
    // reapTerminated() collect orphaned zombies instead of leaking them.
    for (Process* child = allProcessesHead; child; child = child->allNext) {
        if (child != process && !child->isThread() && child->getParentPID() == deadPid) {
            child->setParentPID(0);
        }
    }

    // Notify the parent: raise SIGCHLD and wake it if it is blocked (e.g. parked
    // in sys_wait() or sleeping). SIGCHLD is default-ignored, so it will not
    // spuriously EINTR a waiter (see Process::hasDeliverableSignal), but the
    // wake itself is what lets a blocked wait() re-check for the new zombie.
    const uint32_t parentPid = process->getParentPID();
    if (parentPid != 0) {
        Process* parent = getProcessByPID(parentPid);
        if (parent && parent->getState() != ProcessState::Terminated) {
            parent->sendSignal(SIGCHLD);
            if (parent->getState() == ProcessState::Blocked) {
                wakeProcess(parent);
            }
        }
    }
}

void Scheduler::wakeExpiredSleepers(uint64_t nowMs) {
    Process* process = allProcessesHead;
    while (process) {
        if (process->alarmDeadlineMs != 0 && nowMs >= process->alarmDeadlineMs) {
            process->alarmDeadlineMs = process->alarmIntervalMs ? nowMs + process->alarmIntervalMs : 0;
            process->sendSignal(SIGALRM);
        }
        if (process->getState() == ProcessState::Blocked && process->sleepDeadlineReached(nowMs)) {
            process->clearSleep();
            process->setState(ProcessState::Ready);
            addToReadyQueueFront(process);
        }
        process = process->allNext;
    }
}

void Scheduler::wakeAllBlockedProcesses() {
    const uint64_t nowMs = time_get_uptime_ms();
    Process* process = allProcessesHead;
    while (process) {
        if (process->getState() == ProcessState::Blocked) {
            if (process->isSleeping() && !process->wakesOnEvents() &&
                !process->sleepDeadlineReached(nowMs)) {
                process = process->allNext;
                continue;
            }

            process->clearSleep();
            process->setState(ProcessState::Ready);
            addToReadyQueue(process);
        }
        process = process->allNext;
    }
}

bool Scheduler::killSiblingThreads(Process* proc) {
    bool remaining = false;
    for (Process* p = allProcessesHead; p; p = p->allNext) {
        if (p == proc || !p->sharesStateWith(proc)) continue;
        remaining = true;
        if (p->getState() == ProcessState::Terminated) continue;
        // Blocked: woken, it dies at its next schedule() or syscall exit.
        // Running in user mode elsewhere: the IPI makes that CPU's
        // schedule(frame) act on the SIGKILL.
        p->sendSignal(SIGKILL);
        kickCpuRunning(p);
    }
    return remaining;
}

void Scheduler::exitThreadGroup(Process* caller, int code) {
    if (caller->isThread()) {
        for (Process* p = allProcessesHead; p; p = p->allNext) {
            if (!p->isThread() && p->sharesStateWith(caller) &&
                p->getState() != ProcessState::Terminated) {
                p->requestExit(code);
                kickCpuRunning(p);
            }
        }
    }
    killSiblingThreads(caller);
}

bool Scheduler::hasOtherThreads(Process* proc) {
    for (Process* p = allProcessesHead; p; p = p->allNext) {
        if (p != proc && p->sharesStateWith(proc)) return true;
    }
    return false;
}

void Scheduler::kickCpuRunning(Process* proc) {
    for (uint32_t i = 0; i < SMP::kMaxCPUs; ++i) {
        PerCPUData* cpu = SMP::cpu(i);
        if (cpu && cpu != getPerCPU() && cpu->currentProcess == proc) {
            SMP::sendReschedule(cpu);
        }
    }
}

uint32_t Scheduler::allocatePID() {
    return nextPID++;
}

Process* Scheduler::getProcessByPID(uint32_t pid) {
    if (idle() && idle()->getPID() == pid) return idle();
    
    Process* current = allProcessesHead;
    while (current) {
        if (current->getPID() == pid) {
            return current;
        }
        current = current->allNext;
    }
    return nullptr;
}

Process* Scheduler::findChild(uint32_t parentPID, int64_t pid, bool terminatedOnly, bool* hasMatchingChild) {
    if (hasMatchingChild) {
        *hasMatchingChild = false;
    }

    Process* current = allProcessesHead;
    while (current) {
        if (!current->isThread() && current->getParentPID() == parentPID) {
            bool pidMatches = false;
            if (pid == -1) {
                pidMatches = true;
            } else if (pid == 0) {
                pidMatches = true;
            } else if (pid > 0 && current->getPID() == static_cast<uint32_t>(pid)) {
                pidMatches = true;
            }

            if (pidMatches) {
                if (hasMatchingChild) {
                    *hasMatchingChild = true;
                }
                // A terminated child still parked on some CPU is not reapable
                // yet; that CPU wakes the parent again once it lets go of it.
                // Nor is one whose threads are still dying: reapTerminated()
                // wakes the parent as they go.
                if (!terminatedOnly ||
                    (current->getState() == ProcessState::Terminated && !isBusyOnAnyCpu(current) &&
                     !hasOtherThreads(current))) {
                    return current;
                }
            }
        }
        current = current->allNext;
    }

    return nullptr;
}
