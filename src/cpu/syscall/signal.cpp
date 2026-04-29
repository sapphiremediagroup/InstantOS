#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>

uint64_t Syscall::sys_signal(uint64_t sig, uint64_t handler) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    
    if (sig >= NSIG) return (uint64_t)-1;
    
    SignalHandler* sh = current->getSignalHandler();
    uint64_t old = reinterpret_cast<uint64_t>(sh->handlers[sig]);
    sh->handlers[sig] = reinterpret_cast<sighandler_t>(handler);
    
    return old;
}

uint64_t Syscall::sys_sigreturn() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    
    uint64_t* stack = reinterpret_cast<uint64_t*>(current->getContext()->rsp);
    current->getContext()->rip = stack[0];
    current->getContext()->rsp += 128;
    
    return 0;
}
