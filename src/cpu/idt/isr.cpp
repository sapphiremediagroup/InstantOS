#include <cpu/watchdog.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/idt/isr.hpp>
#include <debug/diag.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/console.hpp>
#include <graphics/intel_gen9.hpp>
#include <cpu/cpuid.hpp>
#include <memory/vmm.hpp>
#include <memory/pmm.hpp>
#include <memory/heap.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/klock.hpp>
#include <cpu/tlb.hpp>
#include <cpu/smp.hpp>
#include <cpu/percpu.hpp>
#include <cpu/apic/irqs.hpp>
#include <time/tsc_timer.hpp>
#include <interrupts/timer.hpp>

Interrupt *interruptHandlers[256] = {nullptr};

extern unsigned long long runtimeBase;
static void serialExceptionDump(InterruptFrame* frame, uint64_t runtimeBase);

static void printPageWalk(uint64_t addr) {
    auto* pml4 = VMM::GetAddressSpace();
    if (!pml4) {
        Console::get().drawText("\n\t- PML4: unavailable");
        return;
    }

    uint64_t pml4i = (addr >> 39) & 0x1FF;
    uint64_t pdpti = (addr >> 30) & 0x1FF;
    uint64_t pdi   = (addr >> 21) & 0x1FF;
    uint64_t pti   = (addr >> 12) & 0x1FF;

    uint64_t pml4e = pml4->entries[pml4i];
    Console::get().drawText("\n\t- PML4E: ");
    Console::get().drawHex(pml4e);
    if (!(pml4e & Present)) return;

    auto* pdpt = reinterpret_cast<PageTable*>(pml4e & ADDR_MASK);
    uint64_t pdpte = pdpt->entries[pdpti];
    Console::get().drawText("\n\t- PDPTE: ");
    Console::get().drawHex(pdpte);
    if (!(pdpte & Present) || (pdpte & LargePage)) return;

    auto* pd = reinterpret_cast<PageTable*>(pdpte & ADDR_MASK);
    uint64_t pde = pd->entries[pdi];
    Console::get().drawText("\n\t- PDE: ");
    Console::get().drawHex(pde);
    if (!(pde & Present) || (pde & LargePage)) return;

    auto* pt = reinterpret_cast<PageTable*>(pde & ADDR_MASK);
    uint64_t pte = pt->entries[pti];
    Console::get().drawText("\n\t- PTE: ");
    Console::get().drawHex(pte);
}
void printStackTrace(uint64_t rbp, uint64_t rip) {
    Console::get().drawText("\nStack trace:\n");
    Console::get().drawText("  #0 RIP=");
    Console::get().drawHex(rip);
    Debug::printAddressSymbol(rip);
    Console::get().drawText("\n");

    int depth = 1;

    while (rbp && depth < 32) {
        if ((rbp & 0x7) || !VMM::IsMapped(rbp) || !VMM::IsMapped(rbp + sizeof(uint64_t))) {
            Console::get().drawText("  <unmapped or unaligned RBP=");
            Console::get().drawHex(rbp);
            Console::get().drawText(">\n");
            break;
        }

        uint64_t* frame = (uint64_t*)rbp;

        uint64_t return_rip = frame[1];
        uint64_t next_rbp   = frame[0];

        Console::get().drawText("  #");
        Console::get().drawNumber(depth);
        Console::get().drawText(" RIP=");
        Console::get().drawHex(return_rip);
        Debug::printAddressSymbol(return_rip);
        Console::get().drawText("\n");

        // sanity check (VERY important)
        if (next_rbp <= rbp) break;

        rbp = next_rbp;
        depth++;
    }
}

// On the way back to user mode from an interrupt or exception: pending
// signals are acted on now (a handler frame, or the default action), so they
// reach a program that makes no syscalls. Returns the vector state to load:
// that of the process the frame now returns to (the scheduler may have
// switched to another one by rewriting the frame), whose user state is in
// its userFpuState (saved when it entered the kernel).
static FPUState* deliverOnReturnToUser(InterruptFrame* frame, FPUState* fpuSave) {
    if (frame->cs != kUserCodeSelector) {
        return fpuSave;
    }
    for (int guard = 0; guard < 4; ++guard) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current || current->getPriority() == ProcessPriority::Idle) {
            return fpuSave;
        }
        if (current->deliverSignalsToFrame(frame)) {
            return current->userFpuState;
        }
        // It died: run something else (the frame becomes that process's, or
        // this CPU leaves through a context switch and never comes back).
        Scheduler::get().schedule(frame);
        if (frame->cs != kUserCodeSelector) {
            return fpuSave;
        }
    }
    Process* current = Scheduler::get().getCurrentProcess();
    return current ? current->userFpuState : fpuSave;
}

static void handleException(InterruptFrame* frame) {
    // Copy-on-write fast path: a write to a *present* page (err bits P|W set)
    // that lands on a COW-marked leaf is resolved by handing the faulting
    // address space a private writable copy, then resuming the instruction.
    // Works for both user faults and kernel-side writes (e.g. copyToUser).
    if (frame->interrupt == 0x0E && (frame->errCode & 0x3) == 0x3) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        Process* faulting = Scheduler::get().getCurrentProcess();
        if (faulting && faulting->getPageTable() &&
            VMM::HandleCowFault(faulting->getPageTable(), cr2)) {
            return;   // fault serviced; re-execute the faulting instruction
        }
    }

    // Demand paging: a not-present fault (err bit 0 == 0) on a lazily-reserved
    // mmap region is populated with a fresh zero page on first touch. Handles
    // both user faults and kernel-side accesses (copyToUser, driver reads) into
    // demand-paged buffers.
    if (frame->interrupt == 0x0E && (frame->errCode & 0x1) == 0) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        Process* faulting = Scheduler::get().getCurrentProcess();
        if (faulting && faulting->getPageTable()) {
            // handleDemandFault() calls MapPageInto(), which walks/updates the
            // faulting process's page tables via the LOW IDENTITY MAP (raw
            // physical pointers). If the faulting process is a non-PIE (ET_EXEC)
            // image at a low VA and any page-table frame lives in that image
            // window (possible once AllocTable() falls back below
            // KERNEL_HIGH_ALLOC_MIN under memory pressure), an identity walk in
            // the process's own address space is shadowed by the image and the
            // PTE store scribbles into the process's .data/.got instead. Run the
            // fault-in on the kernel address space, whose full low identity map
            // is never shadowed by a user image (mirrors sys_fork/sys_exec).
            uint64_t savedCR3;
            asm volatile("mov %%cr3, %0" : "=r"(savedCR3));
            PageTable* kpml4 = VMM::GetKernelAddressSpace();
            if (kpml4) VMM::SetAddressSpace(kpml4);
            bool handled = faulting->handleDemandFault(cr2);
            asm volatile("mov %0, %%cr3" :: "r"(savedCR3) : "memory");
            if (handled) {
                return;   // page populated; re-execute the faulting instruction
            }
        }
    }

    // Stale TLB entry: another CPU widened this mapping (COW break, mprotect,
    // demand fault by a sibling thread) without a shootdown, which only
    // narrowing changes get. Flush the entry and retry. Faulting on the same
    // address over and over means the cause is not the TLB (the flush did not
    // help), so after a few retries it is handled as a real fault.
    if (frame->interrupt == 0x0E) {
        constexpr uint32_t kMaxRetries = 64;
        static uint64_t lastAddr[SMP::kMaxCPUs];
        static uint32_t retries[SMP::kMaxCPUs];
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        const uint32_t cpu = currentCpuIndex() % SMP::kMaxCPUs;
        if (VMM::PermitsFaultedAccess(VMM::GetAddressSpace(), cr2, frame->errCode)) {
            const uint64_t page = cr2 & ~0xFFFULL;
            retries[cpu] = (lastAddr[cpu] == page) ? retries[cpu] + 1 : 1;
            lastAddr[cpu] = page;
            if (retries[cpu] <= kMaxRetries) {
                TLB::flushLocal(cr2, 1);
                return;
            }
        }
    }

    const char* exception_names[] = {
        "Division By Zero", "Debug", "Non-Maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Unknown Instruction", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 Floating-Point", "Alignment Check", "Machine Check", "SIMD Floating-Point",
        "Virtualization", "Control Protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection", "VMM Communication", "Security", "Reserved"
    };

    if (frame->cs == 0x23) {
        Process* current = Scheduler::get().getCurrentProcess();

        // The signal Linux raises for this exception; a handler for it runs
        // instead of the process dying.
        int faultSignal = SIGSEGV;
        SignalInfo faultInfo{SI_KERNEL, 0, 0, 0, 0};
        switch (frame->interrupt) {
        case 0:  faultSignal = SIGFPE;  faultInfo = {1 /*FPE_INTDIV*/, 0, 0, 0, frame->rip}; break;
        case 1:  faultSignal = SIGTRAP; faultInfo = {2 /*TRAP_TRACE*/, 0, 0, 0, frame->rip}; break;
        case 3:  faultSignal = SIGTRAP; break;
        case 6:  faultSignal = SIGILL;  faultInfo = {2 /*ILL_ILLOPN*/, 0, 0, 0, frame->rip}; break;
        case 11: case 12: faultSignal = SIGBUS; break;
        case 14: {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            // ACCERR only for a page of the program's (the kernel's identity
            // map of low memory is present too, but not the program's).
            const uint64_t leaf = current ? VMM::LeafEntryIn(current->getPageTable(), cr2) : 0;
            const bool mapped = (leaf & Present) && (leaf & UserSuper);
            faultInfo = {mapped ? 2 /*SEGV_ACCERR*/ : 1 /*SEGV_MAPERR*/, 0, 0, 0, cr2};
            break;
        }
        case 16: case 19: faultSignal = SIGFPE; faultInfo = {7 /*FPE_FLTINV*/, 0, 0, 0, frame->rip}; break;
        case 17: faultSignal = SIGBUS;  faultInfo = {1 /*BUS_ADRALN*/, 0, 0, 0, frame->rip}; break;
        default: break;
        }
        if (current && current->takeFaultSignal(frame, faultSignal, faultInfo)) {
            return;
        }

        if (current) {
            // exceptionHandler saved the program's vector registers into
            // userFpuState on entry; saving them again here would take the
            // kernel's values instead.
            Console::get().drawText("User process crash.\n");
            Debug::printCurrentProcessSummary();
            Console::get().drawText("free memory: ");
            Console::get().drawNumber(static_cast<int64_t>(PMM::FreeMemory() / (1024 * 1024)));
            Console::get().drawText(" MiB, heap arenas ");
            Console::get().drawNumber(heap_arena_count());
            Console::get().drawText(" (low ");
            Console::get().drawNumber(heap_low_arena_count());
            Console::get().drawText("), low page tables ");
            Console::get().drawNumber(static_cast<int64_t>(VMM::lowPageTables));
            Console::get().drawText("\n");
            const char* exception_name = (frame->interrupt < 32) ? exception_names[frame->interrupt] : "Unknown Exception";
            Console::get().drawText("exception: ");
            Console::get().drawText(exception_name);
            Console::get().drawText("\nrip: ");
            Console::get().drawHex(frame->rip);
            Debug::printAddressSymbol(frame->rip);
            Console::get().drawText("\n");
            if (frame->interrupt == 0x0E) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                Console::get().drawText("cr2: ");
                Console::get().drawHex(cr2);
                Console::get().drawText("\nerr: ");
                Console::get().drawNumber(frame->errCode);
                Debug::printPageFaultReason(frame->errCode);
                printPageWalk(cr2);
                Console::get().drawText("\n");
            }
            Debug::printCurrentProcessSyscall();

            // Dump the user stack top + a frame walk. For an indirect/NULL call
            // the return address the CALL just pushed is at [rsp], which points
            // straight at the calling code (crucial when rip=0). Runs in the
            // faulting process's address space, so user addresses are mapped.
            Console::get().drawText("rsp: ");
            Console::get().drawHex(frame->rsp);
            for (int i = 0; i < 6; i++) {
                uint64_t addr = frame->rsp + static_cast<uint64_t>(i) * 8;
                if (!VMM::IsMapped(addr)) break;
                Console::get().drawText("\n  [rsp+");
                Console::get().drawNumber(i * 8);
                Console::get().drawText("]=");
                Console::get().drawHex(*reinterpret_cast<uint64_t*>(addr));
            }
            Console::get().drawText("\n");
            printStackTrace(frame->rbp, frame->rip);
        }

        if (current) {
            const int code = 128 + faultSignal;
            // A crash in any thread takes the whole process down.
            if (current->isThread()) {
                Scheduler::get().exitThreadGroup(current, code, faultSignal);
            }
            current->setExitCode(code);
            current->termSignal = faultSignal;
            current->closeFilesOnExit();
            current->setState(ProcessState::Terminated);
            current->setValidUserState(false);
            Scheduler::get().schedule(frame);
        }
        return;
    }
    // A fault while this CPU was already reporting one (e.g. in the console's
    // drawing code): say so on the serial port alone and stop, rather than
    // recursing until the stack overflows.
    static volatile uint32_t reporting[SMP::kMaxCPUs];
    const uint32_t reportCpu = currentCpuIndex() % SMP::kMaxCPUs;
    if (reporting[reportCpu]++ != 0) {
        Console::get().releaseLockHeldByThisCpu();
        serialExceptionDump(frame, runtimeBase);
        Cereal::get().write("\n[exception] another fault while reporting one, vector ");
        Cereal::get().write(frame->interrupt == 0x0E ? "0x0e" : (frame->interrupt == 0x0D ? "0x0d" : "?"));
        Cereal::get().write("\n");
        Console::get().drawText("\t- nested RIP: ");
        Console::get().drawHex(frame->rip);
        Console::get().drawText("\n");
        asm volatile("cli");
        while (true) asm volatile("hlt");
    }
    serialExceptionDump(frame, runtimeBase);
    // A kernel exception's report is drawn into the firmware's framebuffer,
    // unless this CPU faulted inside the console (then serial and log only).
    Debug::takeScreenForReport();
    if (Console::get().releaseLockHeldByThisCpu()) {
        Cereal::get().write("\n[exception] faulted while holding the console lock\n");
    }
    const char* exception_name = (frame->interrupt < 32) ? exception_names[frame->interrupt] : "Unknown";
    Console::get().drawText(exception_name);
    Console::get().drawText("\n\t- Interrupt: ");
    Console::get().drawNumber(frame->interrupt);
    Console::get().drawText("\n\t- Error Code: ");
    Console::get().drawNumber(frame->errCode);
    Console::get().drawText("\n\t- RIP: ");
    Console::get().drawHex(frame->rip);
    Console::get().drawText(" (offset: ");
    Console::get().drawHex(frame->rip - runtimeBase);
    Console::get().drawText(")");
    Debug::printAddressSymbol(frame->rip);
    Console::get().drawText("\n\t- RSP: ");
    Console::get().drawHex(frame->rsp);
    Console::get().drawText("\n\t- CS: ");
    Console::get().drawHex(frame->cs);
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    Console::get().drawText("\n\t- CR3: ");
    Console::get().drawHex(cr3);

    if (frame->interrupt == 0x0E) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        Console::get().drawText("\n\t- CR2: ");
        Console::get().drawHex(cr2);
        Debug::printPageFaultReason(frame->errCode);
        printPageWalk(cr2);
    }

    Console::get().drawText("\n\t- RAX: ");
    Console::get().drawHex(frame->rax);
    Console::get().drawText("\n\t- RBP: ");
    Console::get().drawHex(frame->rbp);
    Console::get().drawText("\n");
    Debug::printCurrentProcessSummary();
    Debug::printCurrentProcessSyscall();

    printStackTrace(frame->rbp, frame->rip);

    while(1);
}

// The essentials of a kernel exception, straight to the serial port and the
// log before anything else: the console's drawing can fault too (a corrupted
// framebuffer object), and then the report below never gets past its first
// character.
static void serialHex(const char* label, uint64_t value) {
    char buf[20];
    for (int i = 0; i < 16; ++i) {
        const uint32_t d = static_cast<uint32_t>(value >> (60 - 4 * i)) & 0xF;
        buf[i] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[16] = 0;
    Cereal::get().write(label);
    Cereal::get().write(buf);
}

static void serialExceptionDump(InterruptFrame* frame, uint64_t runtimeBase) {
    uint64_t cr2 = 0, cr3 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    Cereal::get().write("\n[exception] kernel");
    serialHex(" vector=", frame->interrupt);
    serialHex(" err=", frame->errCode);
    serialHex(" cpu=", currentCpuIndex());
    serialHex("\n[exception] rip=", frame->rip);
    serialHex(" map=", frame->rip - runtimeBase + 0x140000000ULL);
    serialHex(" cr2=", cr2);
    serialHex(" cr3=", cr3);
    serialHex("\n[exception] rsp=", frame->rsp);
    serialHex(" rbp=", frame->rbp);
    serialHex(" rflags=", frame->rflags);
    serialHex("\n[exception] rax=", frame->rax);
    serialHex(" rbx=", frame->rbx);
    serialHex(" rcx=", frame->rcx);
    serialHex(" rdx=", frame->rdx);
    serialHex("\n[exception] rsi=", frame->rsi);
    serialHex(" rdi=", frame->rdi);
    serialHex(" r8=", frame->r8);
    serialHex(" r9=", frame->r9);
    serialHex(" r10=", frame->r10);
    serialHex(" r11=", frame->r11);
    Process* current = Scheduler::get().getCurrentProcess();
    if (current) {
        serialHex("\n[exception] pid=", current->getPID());
    }
    // Frame-pointer walk (the kernel keeps frame pointers), as map addresses.
    Cereal::get().write("\n[exception] stack:");
    uint64_t rbp = frame->rbp;
    for (int i = 0; i < 16 && rbp && (rbp & 7) == 0; ++i) {
        if (!VMM::IsMapped(rbp) || !VMM::IsMapped(rbp + 8)) break;
        const uint64_t ret = reinterpret_cast<uint64_t*>(rbp)[1];
        serialHex(" ", ret >= runtimeBase ? ret - runtimeBase + 0x140000000ULL : ret);
        const uint64_t next = reinterpret_cast<uint64_t*>(rbp)[0];
        if (next <= rbp) break;
        rbp = next;
    }
    Cereal::get().write("\n");
}

// Exceptions take the big kernel lock like any kernel entry. A fault inside the
// kernel (e.g. demand paging a user buffer during a syscall) nests on the lock
// this CPU already holds.
// Extended (FPU/SSE/AVX) state must be preserved around IRQ and exception
// handlers because kernel code freely uses vector registers (the SIMD
// memcpy/memset in common/string.cpp, and whatever the compiler emits: the
// kernel is built with SSE allowed). For a *user-mode* entry we save into the
// process's userFpuState, as the syscall path does. For a *kernel-mode* entry
// there is no process area to borrow, so we save into a small nesting-indexed
// scratch pool. Without this, an IRQ landing inside kernel code that has live
// XMM/YMM (e.g. a kernel thread doing floating-point pixel math) is silently
// corrupted, and so was a program whose page fault read a file-backed page
// (ext4's memcpy took xmm0-3; the suite's "page faults keep the program's SSE
// registers"), or a kernel memcpy whose store faulted. IRQ gates run with IF=0
// so nesting is normally absent; the depth index keeps this correct even if a
// handler re-enables interrupts or faults. The scratch pool is shared by all
// CPUs: IRQ and exception handlers run only under the big kernel lock.
namespace {
constexpr int kMaxKernelIrqFpuDepth = 4;
alignas(64) FPUState kernelIrqFpuScratch[kMaxKernelIrqFpuDepth];
int kernelIrqFpuDepth = 0;
}  // namespace

extern "C" void exceptionHandler(InterruptFrame* frame) {
    // The watchdog's NMI (cpu/watchdog.hpp): record where this CPU was and go
    // straight back, taking no lock (this CPU may be the one stuck holding it).
    if (frame->interrupt == 2 && Watchdog::onNmi(frame)) {
        return;
    }
    KernelLock::enter(KernelLock::kSiteException + (frame->interrupt & 0xFF));
    const bool fromUser = (frame->cs & 3) == 3;
    FPUState* fpuSave = nullptr;
    if (fromUser) {
        Process* current = Scheduler::get().getCurrentProcess();
        fpuSave = current ? current->userFpuState : nullptr;
    } else {
        if (kernelIrqFpuDepth < kMaxKernelIrqFpuDepth) {
            fpuSave = &kernelIrqFpuScratch[kernelIrqFpuDepth];
        }
        ++kernelIrqFpuDepth;  // increment even when over cap to balance the decrement
    }
    if (fpuSave) {
        CPU::saveExtendedState(fpuSave);
    }
    handleException(frame);
    fpuSave = deliverOnReturnToUser(frame, fpuSave);
    if (fpuSave) {
        CPU::restoreExtendedState(fpuSave);
    }
    if (!fromUser) {
        --kernelIrqFpuDepth;
    }
    KernelLock::leave();
}

Interrupt::~Interrupt() = default;

void ISR::registerIRQ(uint8_t vector, Interrupt* handler) {
    interruptHandlers[vector] = handler;
    handler->initialize();
}

static void handleIrq(InterruptFrame* frame);

extern "C" void irqHandler(InterruptFrame* frame) {
    if (frame == nullptr) {
        LAPIC::get().sendEOI();
        return;
    }
    Watchdog::noteIrq(static_cast<uint8_t>(frame->interrupt));
    if (frame->interrupt == VECTOR_AP_TIMER) {
        Watchdog::apTick();   // before any lock: it watches for a hung boot CPU
    }
    // Served without the lock: the sender holds it while waiting for us.
    if (frame->interrupt == VECTOR_TLB_SHOOTDOWN) {
        TLB::handleIpi();
        return;
    }
    // Before looking at the ready queues (see Scheduler::kickIdleCpu).
    if (frame->interrupt == VECTOR_RESCHEDULE) {
        __atomic_exchange_n(&getPerCPU()->idleKicked, 0U, __ATOMIC_SEQ_CST);
    }
    const uint32_t site = KernelLock::kSiteIrq + (frame->interrupt & 0xFF);
    // Scheduler ticks are frequent and usually change nothing; taking the lock
    // for each would stall every CPU behind whichever one holds it.
    // Not when the boot CPU's tick duties are overdue: this tick runs them.
    if ((frame->interrupt == VECTOR_AP_TIMER || frame->interrupt == VECTOR_RESCHEDULE) &&
        !(frame->interrupt == VECTOR_AP_TIMER && timer_duties_overdue()) &&
        Scheduler::get().tickIsNoop(frame)) {
        LAPIC::get().sendEOI();
        return;
    }
    if (frame->interrupt == VECTOR_TIMER) {
        // The boot CPU's tick keeps time without the lock. The rest (input,
        // sleepers, preemption) is skipped while another CPU holds the lock,
        // for at most kMaxSkippedTicks ticks in a row.
        constexpr uint32_t kMaxSkippedTicks = 4;
        static uint32_t skippedTicks = 0;   // only the boot CPU takes this IRQ
        timer_account_tick();
        if (!KernelLock::tryEnter(site)) {
            if (++skippedTicks < kMaxSkippedTicks) {
                LAPIC::get().sendEOI();
                return;
            }
            KernelLock::enter(site);
        }
        skippedTicks = 0;    } else {
        KernelLock::enter(site);
    }
    handleIrq(frame);
    KernelLock::leave();
}

static void handleIrq(InterruptFrame* frame) {

    const bool interruptedUser = (frame->cs == 0x23);
    Process* current = interruptedUser ? Scheduler::get().getCurrentProcess() : nullptr;

    FPUState* fpuSave = nullptr;
    if (interruptedUser) {
        fpuSave = current ? current->userFpuState : nullptr;
    } else {
        if (kernelIrqFpuDepth < kMaxKernelIrqFpuDepth) {
            fpuSave = &kernelIrqFpuScratch[kernelIrqFpuDepth];
        }
        ++kernelIrqFpuDepth;  // increment even when over cap to balance the decrement
    }

    if (fpuSave) {
        CPU::saveExtendedState(fpuSave);
    }

    if (frame->interrupt != VECTOR_TIMER && frame->interrupt != VECTOR_AP_TIMER &&
        frame->interrupt != VECTOR_RESCHEDULE) {
        getPerCPU()->deviceIrqs++;
    }

    Interrupt* handler = interruptHandlers[frame->interrupt];
    if (handler != nullptr) {
        handler->Run(frame);
    } else {
        LAPIC::get().sendEOI();
    }

    fpuSave = deliverOnReturnToUser(frame, fpuSave);
    if (fpuSave) {
        CPU::restoreExtendedState(fpuSave);
    }
    if (!interruptedUser) {
        --kernelIrqFpuDepth;
    }
}
