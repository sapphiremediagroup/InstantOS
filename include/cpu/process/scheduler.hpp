#pragma once

#include "process.hpp"
#include <cpu/idt/interrupt.hpp>
#include <stdint.h>

class Scheduler {
public:
    Scheduler() : currentProcess(nullptr), allProcessesHead(nullptr), idleProcess(nullptr), nextPID(1), initialized(false) {
        for (int i = 0; i < 4; i++) readyQueues[i] = nullptr;
    }
    
    static Scheduler& get();
    
    void initialize();
    void addProcess(Process* proc);
    void removeProcess(uint32_t pid);
    
    Process* getCurrentProcess() { return currentProcess; }
    void setCurrentProcess(Process* proc) { currentProcess = proc; }
    Process* getNextProcess();
    Process* getProcessByPID(uint32_t pid);
    
    void schedule();
    void schedule(struct InterruptFrame* frame);
    void scheduleFromSyscall();
    void yield();
    void wakeProcess(Process* process);
    void wakeAllBlockedProcesses();
    
    uint32_t allocatePID();
    
private:
    Process* currentProcess;
    Process* readyQueues[4]; // High, Normal, Low, Idle
    Process* allProcessesHead;
    Process* idleProcess;
    uint32_t nextPID;
    bool initialized;

    void addToReadyQueue(Process* proc);
    void addToReadyQueueFront(Process* proc);
    void removeFromReadyQueue(Process* proc);
    void reapTerminatedThreads();
};

extern "C" void switchContext(ProcessContext* oldCtx, ProcessContext* newCtx);
