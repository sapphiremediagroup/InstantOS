#include <cpu/process/scheduler.hpp>
#include <cpu/gdt/gdt.hpp>
#include <graphics/console.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/user/session.hpp>
#include <cpu/cpuid.hpp>
#include <cpu/percpu.hpp>
#include <cpu/cereal/cereal.hpp>
#include <common/ports.hpp>

namespace {
constexpr uint64_t kUserCodeSelector = 0x23;
constexpr uint64_t kUserDataSelector = 0x1B;
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

void prepareInterruptReturnToUser(InterruptFrame* frame) {
    frame->cs = kUserCodeSelector;
    frame->ss = kUserDataSelector;
    frame->rflags |= 0x200;

    if (rdmsr(kMsrKernelGsBase) == 0) {
        asm volatile("swapgs" ::: "memory");
    }
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
    
    currentProcess = nullptr;
    allProcessesHead = nullptr;
    for (int i = 0; i < 4; i++) {
        readyQueues[i] = nullptr;
    }
    
    nextPID = 1;
    
    // Create idle process
    idleProcess = new Process(0);
    idleProcess->setName("<idle>");
    uint64_t pml4Phys = VMM::VirtualToPhysical(reinterpret_cast<uint64_t>(VMM::GetKernelAddressSpace()));
    if (!pml4Phys) pml4Phys = reinterpret_cast<uint64_t>(VMM::GetKernelAddressSpace());
    idleProcess->getContext()->cr3 = pml4Phys;
    idleProcess->setPriority(ProcessPriority::Idle);
    idleProcess->getContext()->rip = reinterpret_cast<uint64_t>(idleLoop);
    idleProcess->getContext()->rsp = idleProcess->getKernelStack();
    idleProcess->getContext()->rflags = 0x202; // IF enabled
    idleProcess->setState(ProcessState::Ready);
    
    // We don't add idle process to allProcessesHead or regular ready queues
    // It's handled specially in getNextProcess
    
    initialized = true;
}

void Scheduler::addToReadyQueue(Process* proc) {
    if (!proc || proc->getState() != ProcessState::Ready) return;
    
    int prio = static_cast<int>(proc->getPriority());
    if (prio < 0 || prio > 3) return;
    
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
}

void Scheduler::addToReadyQueueFront(Process* proc) {
    if (!proc || proc->getState() != ProcessState::Ready) return;

    int prio = static_cast<int>(proc->getPriority());
    if (prio < 0 || prio > 3) return;

    proc->next = readyQueues[prio];
    readyQueues[prio] = proc;
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
        if (currentProcess == toDelete) currentProcess = nullptr;
        
        delete toDelete;
    }
}

void Scheduler::reapTerminatedThreads() {
    Process* previous = nullptr;
    Process* process = allProcessesHead;

    while (process) {
        Process* next = process->allNext;

        if (process != currentProcess && process->isThread() && process->getState() == ProcessState::Terminated) {
            if (previous) {
                previous->allNext = next;
            } else {
                allProcessesHead = next;
            }

            removeFromReadyQueue(process);
            delete process;
        } else {
            previous = process;
        }

        process = next;
    }
}

Process* Scheduler::getNextProcess() {
    // Check queues in order of priority: High (2), Normal (1), Low (0)
    for (int i = 2; i >= 0; i--) {
        if (readyQueues[i]) {
            Process* next = readyQueues[i];
            
            // Round robin within the same priority level
            // Move head to tail
            if (next->next) {
                readyQueues[i] = next->next;
                
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
    return idleProcess;
}

void Scheduler::schedule() {
    if (!initialized) return;

    reapTerminatedThreads();
    
    if (currentProcess) {
        currentProcess->handlePendingSignals();
        
        // If the current process is still running, put it back in the ready queue
        if (currentProcess != idleProcess && currentProcess->getState() == ProcessState::Running) {
            currentProcess->setState(ProcessState::Ready);
            addToReadyQueue(currentProcess);
        }
    }
    
    Process* nextProcess = getNextProcess();
    if (!nextProcess) return;

    Process* oldProcess = currentProcess;
    currentProcess = nextProcess;
    currentProcess->setState(ProcessState::Running);
    
    // If it's a regular process, it's already removed from the ready queue head by getNextProcess
    // or specifically handled by round-robin logic there.
    // Wait, my getNextProcess round-robin logic moves it to the tail. 
    // That means it's still in the queue. We should PROBABLY remove it from the ready queue while it's running.
    if (currentProcess != idleProcess) {
        removeFromReadyQueue(currentProcess);
    }

    Syscall::get().setKernelStack(nextProcess->getKernelStack());
    traceSwitchInvariant("syscall", oldProcess, nextProcess);
    
    asm volatile("cli");
    
    if (oldProcess) {
        oldProcess->setValidUserState(false);
        oldProcess->setSavedUserRSP(getPerCPU()->userRSP);
        switchContext(oldProcess->getContext(), nextProcess->getContext());
        getPerCPU()->userRSP = currentProcess->getSavedUserRSP();
    } else {
        switchContext(nullptr, nextProcess->getContext());
    }
    if (oldProcess && oldProcess->getState() == ProcessState::Terminated) {
        removeProcess(oldProcess->getPID());
    }
}

extern "C" void processTrampoline();

void Scheduler::schedule(InterruptFrame* frame) {
    if (!initialized || !frame) return;

    reapTerminatedThreads();
    
    Process* oldProcess = currentProcess;
    const bool interruptedUser = frame->cs == kUserCodeSelector;

    if (oldProcess && oldProcess->getFPUState()) {
        CPU::saveExtendedState(oldProcess->getFPUState());
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
            if (oldProcess != idleProcess) addToReadyQueue(oldProcess);
        }
    } else if (oldProcess) {
        if (oldProcess->getState() == ProcessState::Running) {
            oldProcess->setState(ProcessState::Ready);
            if (oldProcess != idleProcess) addToReadyQueue(oldProcess);
        }
    }
    
    Process* nextProcess = getNextProcess();
    if (!nextProcess) return;

    const bool nextHasKernelResume = !nextProcess->hasValidUserState() && nextProcess->getSavedUserRSP() != 0;

    // Brand-new tasks have no IRQ frame and no blocked-syscall context to
    // resume. They must still enter through the normal kernel switch path.
    if (interruptedUser && !nextProcess->hasValidUserState() && !nextHasKernelResume) {
        traceSwitchInvariant("irq-defer-kernel", oldProcess, nextProcess);
        if (oldProcess) {
            removeFromReadyQueue(oldProcess);
            oldProcess->setState(ProcessState::Running);
            currentProcess = oldProcess;
        }
        return;
    }

    // Brand-new processes start at processTrampoline and are expected to be
    // entered via the normal kernel context-switch path, not from an IRQ frame.
    if (!nextProcess->hasValidUserState() && nextProcess->getSavedUserRSP() == 0) {
        traceSwitchInvariant("irq-defer-new", oldProcess, nextProcess);
        if (oldProcess) {
            removeFromReadyQueue(oldProcess);
            oldProcess->setState(ProcessState::Running);
            currentProcess = oldProcess;
        }
        return;
    }
    
    if (nextProcess != idleProcess) {
        removeFromReadyQueue(nextProcess);
    }
    
    currentProcess = nextProcess;
    currentProcess->setState(ProcessState::Running);

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
    } else {
        // Special case for newly created processes that haven't run yet
        Syscall::get().setKernelStack(nextProcess->getKernelStack());
        traceSwitchInvariant("irq-kernel", oldProcess, nextProcess);
        
        asm volatile("cli");
        
        if (oldProcess) {
            oldProcess->setValidUserState(false);
            oldProcess->setSavedUserRSP(interruptedUser ? frame->rsp : getPerCPU()->userRSP);
            if (interruptedUser) {
                asm volatile("swapgs" ::: "memory");
            }
            switchContext(oldProcess->getContext(), nextProcess->getContext());
            if (interruptedUser) {
                asm volatile("swapgs" ::: "memory");
            }
            getPerCPU()->userRSP = currentProcess->getSavedUserRSP();
        } else {
            switchContext(nullptr, nextProcess->getContext());
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
    if (!process || process == idleProcess) {
        return;
    }

    if (process->getState() == ProcessState::Ready) {
        addToReadyQueueFront(process);
    }
}

void Scheduler::wakeAllBlockedProcesses() {
    Process* process = allProcessesHead;
    while (process) {
        if (process->getState() == ProcessState::Blocked) {
            process->setState(ProcessState::Ready);
            addToReadyQueue(process);
        }
        process = process->allNext;
    }
}

uint32_t Scheduler::allocatePID() {
    return nextPID++;
}

Process* Scheduler::getProcessByPID(uint32_t pid) {
    if (idleProcess && idleProcess->getPID() == pid) return idleProcess;
    
    Process* current = allProcessesHead;
    while (current) {
        if (current->getPID() == pid) {
            return current;
        }
        current = current->allNext;
    }
    return nullptr;
}
