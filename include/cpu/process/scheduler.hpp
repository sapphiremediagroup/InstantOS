#pragma once

#include "process.hpp"
#include <cpu/idt/interrupt.hpp>
#include <cpu/percpu.hpp>
#include <stdint.h>

class Scheduler {
public:
    Scheduler() : allProcessesHead(nullptr), nextPID(1), initialized(false) {
        for (int i = 0; i < 4; i++) readyQueues[i] = nullptr;
    }

    static Scheduler& get();

    void initialize();
    void addProcess(Process* proc);
    void removeProcess(uint32_t pid);

    // The process running on the executing CPU.
    Process* getCurrentProcess() { return current(); }
    void setCurrentProcess(Process* proc) { current() = proc; }
    Process* getAllProcessesHead() { return allProcessesHead; }
    Process* getNextProcess();
    Process* getProcessByPID(uint32_t pid);
    Process* findChild(uint32_t parentPID, int64_t pid, bool terminatedOnly, bool* hasMatchingChild = nullptr);
    
    void schedule();
    void schedule(struct InterruptFrame* frame);
    void scheduleFromSyscall();
    void yield();
    void wakeProcess(Process* process);
    void wakeParentOf(Process* process);
    // Centralized end-of-life bookkeeping for a terminated process: reparents
    // its children, and notifies/wakes its parent (SIGCHLD + unblock a waiter).
    // Idempotent and safe to call on any termination path. Does NOT free the
    // process (that is deferred to reapTerminated()/sys_wait()).
    void onProcessTerminated(Process* process);
    void wakeExpiredSleepers(uint64_t nowMs);
    void wakeAllBlockedProcesses();
    // exec: sends SIGKILL to every other thread sharing proc's address space
    // and kicks the CPUs running them. True while any such thread has not
    // been reaped yet (its teardown still unmaps from the shared space).
    bool killSiblingThreads(Process* proc);
    // exit() or a fatal fault in any thread ends the whole process: the main
    // thread (if `caller` is not it) dies with `code` as the process's exit
    // status, every other thread by SIGKILL. Does not wait.
    void exitThreadGroup(Process* caller, int code);

    uint32_t allocatePID();

    // SMP. Each CPU has its own idle process (created by the boot CPU, see
    // SMP::startAPs). runIdleOnThisCpu() is how an application processor
    // enters the scheduler: it becomes that CPU's idle loop and never returns.
    Process* createIdleProcess();
    void runIdleOnThisCpu();
    // Called by KernelLock on each CPU's outermost kernel entry: releases the
    // process this CPU parked in PerCPUData::deferredProcess.
    void onKernelEntry();
    // Called without the big kernel lock from irqHandler for the AP timer and
    // the reschedule IPI: true when schedule(frame) would leave this CPU as it
    // is, so the tick can skip the lock. A racy read; a stale answer only
    // delays the switch to the next tick (queuing work also kicks idle CPUs).
    bool tickIsNoop(InterruptFrame* frame);
    
private:
    Process* readyQueues[4]; // High, Normal, Low, Idle
    Process* allProcessesHead;
    uint32_t nextPID;

    // Each CPU has its own running and idle process, kept in its PerCPUData.
    Process*& current() { return getPerCPU()->currentProcess; }
    Process*& idle() { return getPerCPU()->idleProcess; }
    bool initialized;

    void addToReadyQueue(Process* proc);
    void addToReadyQueueFront(Process* proc);
    void removeFromReadyQueue(Process* proc);

    // True while some CPU runs `proc` or still sits on its kernel stack; such a
    // process must not be freed or queued.
    bool isBusyOnAnyCpu(Process* proc);
    // Another Process (thread) still shares proc's address space.
    bool hasOtherThreads(Process* proc);
    // Reschedule IPI to whichever other CPU is running proc.
    void kickCpuRunning(Process* proc);
    // Wakes one idle CPU that may run `proc` (reschedule IPI) after it was queued.
    void kickIdleCpu(Process* proc);
    // Parks the process whose kernel stack this CPU is about to iretq from.
    void deferUntilNextEntry(Process* proc);
    // Context switch that keeps the BKL nesting depth per context and treats
    // the idle process as stateless (never saved, always restarted fresh).
    void switchTo(Process* oldProcess, Process* nextProcess);
    // Frees terminated threads and terminated orphan processes (parentPID == 0).
    // Processes with a live parent are left as zombies for the parent's wait().
    // Never frees the currently running process.
    void reapTerminated();
};

extern "C" void switchContext(ProcessContext* oldCtx, ProcessContext* newCtx);
