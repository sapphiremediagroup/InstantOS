// Signals: dispositions (sigaction), masks, sending, and delivery.
//
// A signal is acted on whenever its thread goes back to user mode: on the way
// out of a syscall (Process::handlePendingSignals from syscallHandler or
// restoreSyscallState) and on the way out of an interrupt or exception taken
// in user mode (deliverSignalsToFrame, isr.cpp), so a thread that never makes
// a syscall is reached at its next timer tick, or at once when the sender
// kicks its CPU. A handler gets Linux's x86-64 frame: siginfo_t and ucontext_t
// (every general register, the mask, the vector state as XSAVE stores it) on
// its stack, the restorer as its return address; sigreturn puts all of it
// back and the syscall returns by iretq, so rcx and r11 survive too.
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/cpuid.hpp>
#include <cpu/percpu.hpp>
#include <cpu/smp.hpp>
#include <memory/vmm.hpp>
#include <common/string.hpp>
#include <time/tsc_timer.hpp>

namespace {
constexpr uint64_t kSigBlock = 0;
constexpr uint64_t kSigUnblock = 1;
constexpr uint64_t kSigSetMask = 2;
constexpr uint64_t kMinSignalStackSize = 2048;
constexpr uint64_t kUserTop = 0x0000800000000000ULL;
constexpr uint64_t kRedZone = 128;
constexpr uint64_t kSigIgn = 1;
// Flags a program may set in rflags: CF PF AF ZF SF TF DF OF AC ID (not IF,
// IOPL, NT, RF, VM).
constexpr uint64_t kUserFlags = 0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800 | 0x40000 | 0x200000;

// siginfo_t (Linux, x86-64).
struct UserSigInfo {
    int32_t signo, errnum, code, pad0;
    union {
        struct { int32_t pid; uint32_t uid; int32_t status; int32_t pad; int64_t utime, stime; } proc;
        struct { uint64_t addr; } fault;
        uint8_t raw[112];
    };
};
static_assert(sizeof(UserSigInfo) == 128, "siginfo_t is 128 bytes");

enum {
    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
    REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2, kGregs
};

// ucontext_t (Linux, x86-64).
struct UserContext {
    uint64_t flags;
    uint64_t link;
    uint64_t stackSp;
    int32_t stackFlags;
    int32_t pad0;
    uint64_t stackSize;
    uint64_t gregs[kGregs];
    uint64_t fpregs;          // -> the XSAVE image above the frame
    uint64_t reserved1[8];
    uint64_t sigmask[16];     // sigset_t, 1024 bits
    uint8_t fpregsMem[512];
    uint64_t ssp[4];
};
static_assert(sizeof(UserContext) == 968, "ucontext_t is 968 bytes");
static_assert(__builtin_offsetof(UserContext, gregs) == 40, "uc_mcontext");
static_assert(__builtin_offsetof(UserContext, sigmask) == 296, "uc_sigmask");

// What the handler's rsp points at (Linux's rt_sigframe).
struct SignalFrame {
    uint64_t restorer;        // the handler's return address
    UserContext uc;
    UserSigInfo info;
};
static_assert(sizeof(SignalFrame) == 1104, "rt_sigframe");

struct FaultRegs {
    uint64_t trapno;
    uint64_t err;
    uint64_t cr2;
};

uint64_t validSignalMask(uint64_t mask) {
    mask &= ~1ULL;                 // no signal 0
    mask &= ~(1ULL << SIGKILL);    // never blocked
    return mask;
}

bool restartable(uint64_t number) {
    switch (static_cast<SyscallNumber>(number)) {
    using enum SyscallNumber;
    case Read: case Write: case Wait: case ThreadJoin:
    case Accept: case Connect: case Send: case Recv: case SendTo: case RecvFrom:
    case SocketSendMsg: case SocketRecvMsg: case QueueReceive: case Ioctl:
        return true;
    default:
        return false;   // sleeps, poll, futexes, sigsuspend...: EINTR, as on Linux
    }
}

void fillUserSigInfo(UserSigInfo& out, int sig, const SignalInfo& in) {
    memset(&out, 0, sizeof(out));
    out.signo = sig;
    out.code = in.code;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP) {
        if (in.code > 0 && in.code != SI_KERNEL) {
            out.fault.addr = in.addr;
            return;
        }
    }
    out.proc.pid = in.pid;
    out.proc.uid = in.uid;
    out.proc.status = in.status;
}

// A page the kernel is about to write for the program: present, the
// program's and writable (a copy-on-write page is copied, a demand page made),
// so the write cannot fault in the kernel.
bool userWritable(Process* process, uint64_t address, uint64_t size) {
    if (!Syscall::isValidUserPointer(address, size)) return false;
    PageTable* space = process->getPageTable();
    for (uint64_t page = address & ~0xFFFULL; page < address + size; page += PAGE_SIZE) {
        uint64_t entry = VMM::LeafEntryIn(space, page);
        if (!(entry & Present)) {
            // As the page-fault handler does it: on the kernel's address space,
            // whose identity map no low user image shadows.
            uint64_t savedCR3;
            asm volatile("mov %%cr3, %0" : "=r"(savedCR3));
            PageTable* kernelSpace = VMM::GetKernelAddressSpace();
            if (kernelSpace) VMM::SetAddressSpace(kernelSpace);
            const bool made = process->handleDemandFault(page);
            asm volatile("mov %0, %%cr3" :: "r"(savedCR3) : "memory");
            if (!made) return false;
            entry = VMM::LeafEntryIn(space, page);
        }
        if (!(entry & Present) || !(entry & UserSuper)) return false;
        if (!(entry & ReadWrite)) {
            if (!(entry & kCowPage) || !VMM::HandleCowFault(space, page)) return false;
        }
    }
    return true;
}

bool onAltStack(const SignalHandler& sh, uint64_t sp) {
    return sh.altStackSize != 0 && sp > sh.altStackSp && sp <= sh.altStackSp + sh.altStackSize;
}

// The thread of `target`'s process that should take a process-directed
// signal: the main thread unless it blocks it and another does not.
Process* receiverFor(Process* target, int sig) {
    const uint64_t bit = 1ULL << sig;
    if (!(target->getSignalHandler()->blocked & bit) || target->isThread()) return target;
    for (Process* p = Scheduler::get().getAllProcessesHead(); p; p = p->allNext) {
        if (p != target && p->getGroupPID() == target->getPID() &&
            p->getState() != ProcessState::Terminated && !(p->getSignalHandler()->blocked & bit)) {
            return p;
        }
    }
    return target;
}
} // namespace

// --- sending -----------------------------------------------------------------

void Process::sendSignal(int sig, const SignalInfo* info) {
    if (sig <= 0 || sig >= NSIG) return;
    const uint64_t bit = 1ULL << sig;
    if (!(signalHandler.pending & bit)) {
        signalHandler.info[sig] = info ? *info : SignalInfo{SI_KERNEL, 0, 0, 0, 0};
    }
    __atomic_or_fetch(&signalHandler.pending, bit, __ATOMIC_SEQ_CST);
    if (state == ProcessState::Blocked) {
        Scheduler::get().wakeProcess(this);
        return;
    }
    // Running user code on another CPU: interrupt it so it looks now rather
    // than at its next tick.
    if (state == ProcessState::Running && SMP::schedulingActive()) {
        Scheduler::get().kickCpuRunning(this);
    }
}

void Process::dieOfSignal(int sig) {
    // A fatal signal kills the whole process, not just this thread.
    if (isThread()) {
        Scheduler::get().exitThreadGroup(this, 128 + sig, sig);
    }
    closeFilesOnExit();
    state = ProcessState::Terminated;
    exitCode = 128 + sig;
    termSignal = sig;
}

void Process::setSignalAction(int sig, uint64_t handler, uint64_t mask, uint64_t flags, uint64_t restorer) {
    auto apply = [&](Process* p) {
        SignalHandler* sh = p->getSignalHandler();
        sh->handlers[sig] = reinterpret_cast<sighandler_t>(handler);
        sh->masks[sig] = validSignalMask(mask);
        sh->flags[sig] = flags;
        sh->restorers[sig] = restorer;
        // Ignoring a signal drops it where it is pending (POSIX).
        if (handler == kSigIgn || (handler == 0 && signalIgnoredByDefault(sig))) {
            sh->pending &= ~(1ULL << sig);
        }
    };
    apply(this);
    for (Process* p = Scheduler::get().getAllProcessesHead(); p; p = p->allNext) {
        if (p != this && p->getGroupPID() == groupPID && p->getState() != ProcessState::Terminated) {
            apply(p);
        }
    }
}

// --- delivery ----------------------------------------------------------------

namespace {
// Moves `process` into the handler for `sig`: the frame on its stack, its
// context at the handler's first instruction. False if the frame cannot be
// written (the process then dies of SIGSEGV, as on Linux).
bool setUpHandler(Process* process, int sig, bool atSyscallExit, const FaultRegs* fault) {
    SignalHandler& sh = *process->getSignalHandler();
    ProcessContext& ctx = *process->getContext();
    const uint64_t flags = sh.flags[sig];
    const uint64_t bit = 1ULL << sig;
    const uint64_t entry = reinterpret_cast<uint64_t>(sh.handlers[sig]);

    // The syscall it was in starts again once the handler returns.
    if (atSyscallExit && ctx.rax == static_cast<uint64_t>(-static_cast<int64_t>(SysErrInterrupted)) &&
        (flags & SA_RESTART) && restartable(process->lastSyscall)) {
        ctx.rip -= 2;   // the syscall instruction
        ctx.rax = process->lastSyscall;
    }

    uint64_t sp = ctx.rsp;
    const bool wasOnAlt = onAltStack(sh, sp);
    bool usingAlt = false;
    if ((flags & SA_ONSTACK) && !(sh.altStackFlags & SS_DISABLE) &&
        sh.altStackSize >= kMinSignalStackSize && !wasOnAlt) {
        sp = sh.altStackSp + sh.altStackSize;
        usingAlt = true;
    } else {
        sp -= kRedZone;   // the code it stopped may use the 128 bytes under rsp
    }
    const uint64_t stateSize = CPU::extendedStateSize();
    const uint64_t fpAddress = (sp - stateSize) & ~63ULL;
    const uint64_t frameAddress = ((fpAddress - sizeof(SignalFrame)) & ~15ULL) - 8;
    if (sp < 0x10000 || frameAddress > sp) return false;
    if (usingAlt && frameAddress < sh.altStackSp) return false;
    if (!userWritable(process, frameAddress, sp - frameAddress)) return false;

    const uint64_t oldBlocked = sh.suspended ? sh.suspendSavedMask : sh.blocked;
    sh.suspended = false;

    static SignalFrame frame;   // under the big kernel lock; keeps it off the kernel stack
    memset(&frame, 0, sizeof(frame));
    frame.restorer = sh.restorers[sig] ? sh.restorers[sig] : ctx.rip;
    UserContext& uc = frame.uc;
    uc.stackSp = sh.altStackSp;
    uc.stackSize = sh.altStackSize;
    uc.stackFlags = (sh.altStackFlags & SS_DISABLE) ? SS_DISABLE : (wasOnAlt ? SS_ONSTACK : 0);
    uint64_t* g = uc.gregs;
    g[REG_R8] = ctx.r8;   g[REG_R9] = ctx.r9;   g[REG_R10] = ctx.r10; g[REG_R11] = ctx.r11;
    g[REG_R12] = ctx.r12; g[REG_R13] = ctx.r13; g[REG_R14] = ctx.r14; g[REG_R15] = ctx.r15;
    g[REG_RDI] = ctx.rdi; g[REG_RSI] = ctx.rsi; g[REG_RBP] = ctx.rbp; g[REG_RBX] = ctx.rbx;
    g[REG_RDX] = ctx.rdx; g[REG_RAX] = ctx.rax; g[REG_RCX] = ctx.rcx; g[REG_RSP] = ctx.rsp;
    g[REG_RIP] = ctx.rip; g[REG_EFL] = ctx.rflags;
    g[REG_CSGSFS] = 0x23 | (0x1BULL << 48);
    if (fault) {
        g[REG_TRAPNO] = fault->trapno;
        g[REG_ERR] = fault->err;
        g[REG_CR2] = fault->cr2;
    }
    g[REG_OLDMASK] = oldBlocked;
    uc.sigmask[0] = oldBlocked;
    uc.fpregs = process->userFpuState ? fpAddress : 0;
    fillUserSigInfo(frame.info, sig, sh.info[sig]);

    if (process->userFpuState &&
        !Syscall::copyToUser(fpAddress, process->userFpuState, stateSize)) {
        return false;
    }
    if (!Syscall::copyToUser(frameAddress, &frame, sizeof(frame))) return false;

    uint64_t blocked = sh.blocked | sh.masks[sig];
    if (!(flags & SA_NODEFER)) blocked |= bit;
    sh.blocked = validSignalMask(blocked);
    if (flags & SA_RESETHAND) {
        process->setSignalAction(sig, 0, 0, flags & ~(SA_SIGINFO | SA_RESETHAND), 0);
    }

    ctx.rip = entry;   // SA_RESETHAND reset the table, not this delivery
    ctx.rsp = frameAddress;
    ctx.rdi = static_cast<uint64_t>(sig);
    ctx.rsi = frameAddress + __builtin_offsetof(SignalFrame, info);
    ctx.rdx = frameAddress + __builtin_offsetof(SignalFrame, uc);
    ctx.rax = 0;
    ctx.rflags &= ~(0x400ULL | 0x100ULL | 0x10000ULL);   // DF, TF, RF
    if (atSyscallExit) {
        ctx.r11 = ctx.rflags;   // sysret takes the flags from r11
        ctx.rcx = ctx.rip;
    }
    return true;
}
} // namespace

void Process::handlePendingSignals(bool atSyscallExit) {
    // Once terminated the exit code is fixed; do not let a still-pending signal
    // re-run the termination logic and clobber it (or double-report the death).
    if (state == ProcessState::Terminated) return;
    SignalHandler& sh = signalHandler;
    if (sh.pending && !terminateIfKilled()) {
        for (int sig = 1; sig < NSIG; sig++) {
            const uint64_t bit = 1ULL << sig;
            if (!(sh.pending & bit) || (sh.blocked & bit)) continue;
            const uint64_t handler = reinterpret_cast<uint64_t>(sh.handlers[sig]);
            if (handler == kSigIgn) {
                __atomic_and_fetch(&sh.pending, ~bit, __ATOMIC_SEQ_CST);
                continue;
            }
            if (handler == 0) {
                __atomic_and_fetch(&sh.pending, ~bit, __ATOMIC_SEQ_CST);
                if (signalIgnoredByDefault(sig)) continue;
                dieOfSignal(sig);
                return;
            }
            // A handler with no restorer cannot return through sigreturn; it is
            // only run where the old kernel ran it, at a syscall's exit.
            if (!sh.restorers[sig] && !atSyscallExit) continue;
            __atomic_and_fetch(&sh.pending, ~bit, __ATOMIC_SEQ_CST);
            if (!setUpHandler(this, sig, atSyscallExit, nullptr)) {
                dieOfSignal(SIGSEGV);
                return;
            }
            break;   // one frame at a time: the next at the next return to user mode
        }
    }
    if (state != ProcessState::Terminated && sh.suspended) {
        sh.blocked = sh.suspendSavedMask;
        sh.suspended = false;
    }
}

bool Process::deliverSignalsToFrame(InterruptFrame* frame) {
    if (state == ProcessState::Terminated) return false;
    if (state != ProcessState::Running || !signalMayBeDeliverable()) return true;
    context.rax = frame->rax; context.rbx = frame->rbx; context.rcx = frame->rcx; context.rdx = frame->rdx;
    context.rsi = frame->rsi; context.rdi = frame->rdi; context.rbp = frame->rbp; context.rsp = frame->rsp;
    context.r8 = frame->r8;   context.r9 = frame->r9;   context.r10 = frame->r10; context.r11 = frame->r11;
    context.r12 = frame->r12; context.r13 = frame->r13; context.r14 = frame->r14; context.r15 = frame->r15;
    context.rip = frame->rip; context.rflags = frame->rflags;
    handlePendingSignals(false);
    if (state == ProcessState::Terminated) return false;
    frame->rax = context.rax; frame->rbx = context.rbx; frame->rcx = context.rcx; frame->rdx = context.rdx;
    frame->rsi = context.rsi; frame->rdi = context.rdi; frame->rbp = context.rbp; frame->rsp = context.rsp;
    frame->r8 = context.r8;   frame->r9 = context.r9;   frame->r10 = context.r10; frame->r11 = context.r11;
    frame->r12 = context.r12; frame->r13 = context.r13; frame->r14 = context.r14; frame->r15 = context.r15;
    frame->rip = context.rip;
    frame->rflags = (context.rflags & kUserFlags) | 0x202;
    return true;
}

bool Process::takeFaultSignal(InterruptFrame* frame, int sig, const SignalInfo& info) {
    const uint64_t bit = 1ULL << sig;
    const uint64_t handler = reinterpret_cast<uint64_t>(signalHandler.handlers[sig]);
    if (handler == 0 || handler == kSigIgn || (signalHandler.blocked & bit) || !signalHandler.restorers[sig]) {
        return false;
    }
    context.rax = frame->rax; context.rbx = frame->rbx; context.rcx = frame->rcx; context.rdx = frame->rdx;
    context.rsi = frame->rsi; context.rdi = frame->rdi; context.rbp = frame->rbp; context.rsp = frame->rsp;
    context.r8 = frame->r8;   context.r9 = frame->r9;   context.r10 = frame->r10; context.r11 = frame->r11;
    context.r12 = frame->r12; context.r13 = frame->r13; context.r14 = frame->r14; context.r15 = frame->r15;
    context.rip = frame->rip; context.rflags = frame->rflags;
    signalHandler.info[sig] = info;
    const FaultRegs regs{frame->interrupt, frame->errCode, info.addr};
    if (!setUpHandler(this, sig, false, &regs)) return false;
    frame->rax = context.rax; frame->rbx = context.rbx; frame->rcx = context.rcx; frame->rdx = context.rdx;
    frame->rsi = context.rsi; frame->rdi = context.rdi; frame->rbp = context.rbp; frame->rsp = context.rsp;
    frame->r8 = context.r8;   frame->r9 = context.r9;   frame->r10 = context.r10; frame->r11 = context.r11;
    frame->r12 = context.r12; frame->r13 = context.r13; frame->r14 = context.r14; frame->r15 = context.r15;
    frame->rip = context.rip;
    frame->rflags = (context.rflags & kUserFlags) | 0x202;
    return true;
}

bool Process::restoreSignalFrame() {
    UserContext uc;
    if (!Syscall::copyFromUser(&uc, context.rsp, sizeof(uc))) return false;
    const uint64_t* g = uc.gregs;
    if (g[REG_RIP] >= kUserTop || g[REG_RSP] >= kUserTop) return false;
    if (uc.fpregs && userFpuState) {
        const bool copied = Syscall::copyFromUser(userFpuState, uc.fpregs, CPU::extendedStateSize());
        CPU::sanitizeExtendedState(userFpuState);   // even half copied it is loaded once more
        if (!copied) return false;
    }
    context.r8 = g[REG_R8];   context.r9 = g[REG_R9];   context.r10 = g[REG_R10]; context.r11 = g[REG_R11];
    context.r12 = g[REG_R12]; context.r13 = g[REG_R13]; context.r14 = g[REG_R14]; context.r15 = g[REG_R15];
    context.rdi = g[REG_RDI]; context.rsi = g[REG_RSI]; context.rbp = g[REG_RBP]; context.rbx = g[REG_RBX];
    context.rdx = g[REG_RDX]; context.rax = g[REG_RAX]; context.rcx = g[REG_RCX]; context.rsp = g[REG_RSP];
    context.rip = g[REG_RIP];
    context.rflags = (g[REG_EFL] & kUserFlags) | 0x202;
    signalHandler.blocked = validSignalMask(uc.sigmask[0]);
    fullUserRestore = true;
    return true;
}

// --- syscalls ----------------------------------------------------------------

uint64_t Syscall::sys_signal(uint64_t sig, uint64_t handler) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (sig == 0 || sig >= NSIG || sig == SIGKILL) return syscall_error(SysErrInvalid);

    SignalHandler* sh = current->getSignalHandler();
    const uint64_t old = reinterpret_cast<uint64_t>(sh->handlers[sig]);
    current->setSignalAction(static_cast<int>(sig), handler, 0, 0, 0);
    return old;
}

uint64_t Syscall::sys_sigreturn() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    if (!current->restoreSignalFrame()) {
        current->dieOfSignal(SIGSEGV);   // a frame it cannot have been given
        return 0;
    }
    // Whatever the syscall returns goes into rax: the restored value.
    return current->getContext()->rax;
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
    current->setSignalAction(static_cast<int>(sig), next.handler, next.mask, next.flags, next.restorer);
    return 0;
}

uint64_t Syscall::sys_sigaltstack(uint64_t stackPtr, uint64_t oldStackPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    SignalHandler* sh = current->getSignalHandler();
    const bool onStack = onAltStack(*sh, current->getContext()->rsp);
    if (oldStackPtr != 0) {
        if (!isValidUserPointer(oldStackPtr, sizeof(SignalStackInfo))) {
            return syscall_error(SysErrInvalid);
        }
        SignalStackInfo old = {};
        old.sp = sh->altStackSp;
        old.size = sh->altStackSize;
        old.flags = (sh->altStackFlags & SS_DISABLE) ? SS_DISABLE : (onStack ? SS_ONSTACK : 0);
        if (!copyToUser(oldStackPtr, &old, sizeof(old))) {
            return syscall_error(SysErrInvalid);
        }
    }

    if (stackPtr == 0) {
        return 0;
    }
    if (onStack) {
        return syscall_error(SysErrPermission);   // EPERM: it is running on it
    }
    if (!isValidUserPointer(stackPtr, sizeof(SignalStackInfo))) {
        return syscall_error(SysErrInvalid);
    }

    SignalStackInfo next = {};
    if (!copyFromUser(&next, stackPtr, sizeof(next))) {
        return syscall_error(SysErrInvalid);
    }
    if ((next.flags & ~(SS_ONSTACK | SS_DISABLE)) != 0) {
        return syscall_error(SysErrInvalid);
    }

    if ((next.flags & SS_DISABLE) != 0) {
        sh->altStackSp = 0;
        sh->altStackSize = 0;
        sh->altStackFlags = SS_DISABLE;
        return 0;
    }

    if (next.size < kMinSignalStackSize) {
        return syscall_error(SysErrNoMemory);   // ENOMEM: too small
    }
    if (next.sp == 0 || !isValidUserPointer(next.sp, next.size)) {
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
    const SignalInfo info{SI_TKILL, static_cast<int32_t>(current->getGroupPID()), current->getUID(), 0, 0};
    object->process->sendSignal(static_cast<int>(sig), &info);
    return 0;
}

uint64_t Syscall::sys_kill(uint64_t pid, uint64_t sig) {
    if (sig >= NSIG) {
        return syscall_error(SysErrInvalid);
    }
    Process* current = Scheduler::get().getCurrentProcess();
    Process* target = nullptr;
    if (pid == 0) {
        target = current;
    } else if (pid <= UINT32_MAX) {
        target = Scheduler::get().getProcessByPID(static_cast<uint32_t>(pid));
    }
    if (!target || target->getState() == ProcessState::Terminated) return syscall_error(SysErrNoProcess);

    if (sig != 0) {
        const SignalInfo info{SI_USER, current ? static_cast<int32_t>(current->getGroupPID()) : 0,
                              current ? current->getUID() : 0, 0, 0};
        receiverFor(target, static_cast<int>(sig))->sendSignal(static_cast<int>(sig), &info);
    }
    return 0;
}

uint64_t Syscall::sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig) {
    if (sig >= NSIG || tid == 0 || tid > UINT32_MAX) return syscall_error(SysErrInvalid);
    Process* current = Scheduler::get().getCurrentProcess();
    Process* target = Scheduler::get().getProcessByPID(static_cast<uint32_t>(tid));
    if (!target || target->getState() == ProcessState::Terminated ||
        (tgid != 0 && target->getGroupPID() != tgid)) {
        return syscall_error(SysErrNoProcess);
    }
    if (sig != 0) {
        const SignalInfo info{SI_TKILL, current ? static_cast<int32_t>(current->getGroupPID()) : 0,
                              current ? current->getUID() : 0, 0, 0};
        target->sendSignal(static_cast<int>(sig), &info);
    }
    return 0;
}

uint64_t Syscall::sys_sigsuspend(uint64_t maskPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    uint64_t mask = 0;
    if (!copyFromUser(&mask, maskPtr, sizeof(mask))) return syscall_error(SysErrFault);
    SignalHandler* sh = current->getSignalHandler();
    sh->suspendSavedMask = sh->blocked;
    sh->suspended = true;
    sh->blocked = validSignalMask(mask);
    // Returns once something is to be done: the way out runs the handler with
    // the saved mask in its frame (handlePendingSignals).
    while (!current->hasDeliverableSignal()) {
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
    }
    return syscall_error(SysErrInterrupted);
}

uint64_t Syscall::sys_sigpending(uint64_t setPtr) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    const SignalHandler* sh = current->getSignalHandler();
    const uint64_t set = sh->pending & sh->blocked;
    if (!copyToUser(setPtr, &set, sizeof(set))) return syscall_error(SysErrFault);
    return 0;
}

uint64_t Syscall::sys_sigtimedwait(uint64_t setPtr, uint64_t infoPtr, uint64_t timeoutNs) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    uint64_t set = 0;
    if (!copyFromUser(&set, setPtr, sizeof(set))) return syscall_error(SysErrFault);
    set = validSignalMask(set);
    if (infoPtr && !isValidUserPointer(infoPtr, sizeof(UserSigInfo))) return syscall_error(SysErrFault);

    const bool timed = timeoutNs != ~0ULL;
    uint64_t deadlineMs = 0;
    if (timed) {
        const uint64_t now = time_get_uptime_ns();
        const uint64_t end = timeoutNs > UINT64_MAX - now ? UINT64_MAX : now + timeoutNs;
        deadlineMs = end / 1000000 + (end % 1000000 ? 1 : 0);
    }
    SignalHandler* sh = current->getSignalHandler();
    while (true) {
        const uint64_t ready = sh->pending & set;
        if (ready) {
            const int sig = __builtin_ctzll(ready);
            __atomic_and_fetch(&sh->pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
            if (infoPtr) {
                UserSigInfo info;
                fillUserSigInfo(info, sig, sh->info[sig]);
                if (!copyToUser(infoPtr, &info, sizeof(info))) return syscall_error(SysErrFault);
            }
            return static_cast<uint64_t>(sig);
        }
        if (current->hasDeliverableSignal()) return syscall_error(SysErrInterrupted);
        if (timed && (timeoutNs == 0 || time_get_uptime_ms() >= deadlineMs)) return syscall_error(SysErrAgain);
        if (timed) current->sleepUntil(deadlineMs);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();
    }
}
