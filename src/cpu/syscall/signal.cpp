#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>

namespace {
constexpr uint64_t kSigBlock = 0;
constexpr uint64_t kSigUnblock = 1;
constexpr uint64_t kSigSetMask = 2;
constexpr uint32_t kSignalStackOnStack = 1;
constexpr uint32_t kSignalStackDisabled = 2;
constexpr uint64_t kMinSignalStackSize = 2048;

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

uint64_t validSignalMask(uint64_t mask) {
    // NSIG signal slots map to bits 0..NSIG-1. Build the bitmask without
    // shifting by 64 (which is undefined behaviour).
    const uint64_t allSignals = (NSIG >= 64) ? ~0ULL : ((1ULL << NSIG) - 1ULL);
    mask &= allSignals;
    mask &= ~1ULL;
    mask &= ~(1ULL << SIGKILL);
    return mask;
}
}

uint64_t Syscall::sys_signal(uint64_t sig, uint64_t handler) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    if (sig == 0 || sig >= NSIG || sig == SIGKILL) return syscall_error(SysErrInvalid);
    
    SignalHandler* sh = current->getSignalHandler();
    uint64_t old = reinterpret_cast<uint64_t>(sh->handlers[sig]);
    sh->handlers[sig] = reinterpret_cast<sighandler_t>(handler);
    sh->masks[sig] = 0;
    sh->flags[sig] = 0;
    sh->restorers[sig] = 0;
    
    return old;
}

uint64_t Syscall::sys_sigreturn() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    uint64_t* stack = reinterpret_cast<uint64_t*>(current->getContext()->rsp);
    auto* frame = reinterpret_cast<UserSignalFrame*>(stack);
    ProcessContext* context = current->getContext();
    context->rip = frame->rip;
    context->rsp = frame->rsp;
    context->rax = frame->rax;
    context->rdi = frame->rdi;
    context->rsi = frame->rsi;
    context->rdx = frame->rdx;
    context->rcx = frame->rcx;
    context->r8 = frame->r8;
    context->r9 = frame->r9;
    context->r10 = frame->r10;
    context->r11 = frame->r11;
    context->rflags = frame->rflags;
    SignalHandler* sh = current->getSignalHandler();
    sh->blocked = validSignalMask(frame->blocked);
    sh->altStackFlags = static_cast<uint32_t>(frame->altStackFlags);
    
    return 0;
}

uint64_t Syscall::sys_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    SignalHandler* sh = current->getSignalHandler();
    if (oldset != 0) {
        if (!isValidUserPointer(oldset, sizeof(uint64_t))) {
            return syscall_error(SysErrInvalid);
        }
        uint64_t old = sh->blocked;
        if (!copyToUser(oldset, &old, sizeof(old))) {
            return syscall_error(SysErrInvalid);
        }
    }

    if (set == 0) {
        return 0;
    }
    if (!isValidUserPointer(set, sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }

    uint64_t requested = 0;
    if (!copyFromUser(&requested, set, sizeof(requested))) {
        return syscall_error(SysErrInvalid);
    }
    requested = validSignalMask(requested);

    switch (how) {
        case kSigBlock:
            sh->blocked = validSignalMask(sh->blocked | requested);
            return 0;
        case kSigUnblock:
            sh->blocked = validSignalMask(sh->blocked & ~requested);
            return 0;
        case kSigSetMask:
            sh->blocked = requested;
            return 0;
        default:
            return syscall_error(SysErrInvalid);
    }
}

uint64_t Syscall::sys_sigaction(uint64_t sig, uint64_t act, uint64_t oldact) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (sig == 0 || sig >= NSIG || (act == 0 && oldact == 0)) {
        return syscall_error(SysErrInvalid);
    }

    SignalHandler* sh = current->getSignalHandler();
    if (oldact != 0) {
        if (!isValidUserPointer(oldact, sizeof(SigActionInfo))) {
            return syscall_error(SysErrInvalid);
        }
        SigActionInfo old = {};
        old.handler = reinterpret_cast<uint64_t>(sh->handlers[sig]);
        old.mask = sh->masks[sig];
        old.flags = sh->flags[sig];
        old.restorer = sh->restorers[sig];
        if (!copyToUser(oldact, &old, sizeof(old))) {
            return syscall_error(SysErrInvalid);
        }
    }

    if (act == 0) {
        return 0;
    }
    if (sig == SIGKILL || !isValidUserPointer(act, sizeof(SigActionInfo))) {
        return syscall_error(SysErrInvalid);
    }

    SigActionInfo next = {};
    if (!copyFromUser(&next, act, sizeof(next))) {
        return syscall_error(SysErrInvalid);
    }

    sh->handlers[sig] = reinterpret_cast<sighandler_t>(next.handler);
    sh->masks[sig] = validSignalMask(next.mask);
    sh->flags[sig] = next.flags;
    sh->restorers[sig] = next.restorer;
    return 0;
}

uint64_t Syscall::sys_sigaltstack(uint64_t stackPtr, uint64_t oldStackPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    SignalHandler* sh = current->getSignalHandler();
    if (oldStackPtr != 0) {
        if (!isValidUserPointer(oldStackPtr, sizeof(SignalStackInfo))) {
            return syscall_error(SysErrInvalid);
        }
        SignalStackInfo old = {};
        old.sp = sh->altStackSp;
        old.size = sh->altStackSize;
        old.flags = sh->altStackFlags;
        if (!copyToUser(oldStackPtr, &old, sizeof(old))) {
            return syscall_error(SysErrInvalid);
        }
    }

    if (stackPtr == 0) {
        return 0;
    }
    if ((sh->altStackFlags & kSignalStackOnStack) != 0) {
        return syscall_error(SysErrInvalid);
    }
    if (!isValidUserPointer(stackPtr, sizeof(SignalStackInfo))) {
        return syscall_error(SysErrInvalid);
    }

    SignalStackInfo next = {};
    if (!copyFromUser(&next, stackPtr, sizeof(next))) {
        return syscall_error(SysErrInvalid);
    }
    if ((next.flags & ~(kSignalStackOnStack | kSignalStackDisabled)) != 0 ||
        (next.flags & kSignalStackOnStack) != 0) {
        return syscall_error(SysErrInvalid);
    }

    if ((next.flags & kSignalStackDisabled) != 0) {
        sh->altStackSp = 0;
        sh->altStackSize = 0;
        sh->altStackFlags = kSignalStackDisabled;
        return 0;
    }

    if (next.sp == 0 || next.size < kMinSignalStackSize || !isValidUserPointer(next.sp, next.size)) {
        return syscall_error(SysErrInvalid);
    }

    sh->altStackSp = next.sp;
    sh->altStackSize = next.size;
    sh->altStackFlags = 0;
    return 0;
}

uint64_t Syscall::sys_thread_signal(uint64_t handle, uint64_t sig) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || sig == 0 || sig >= NSIG) return syscall_error(SysErrInvalid);

    auto* object = reinterpret_cast<ThreadObject*>(
        current->getHandleObject(handle, HandleType::Thread, HandleRightSignal)
    );
    if (!object || !object->process) {
        return syscall_error(SysErrBadFile);
    }
    object->process->sendSignal(static_cast<int>(sig));
    return 0;
}
