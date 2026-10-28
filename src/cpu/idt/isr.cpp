#include <cpu/idt/interrupt.hpp>
#include <cpu/idt/isr.hpp>
#include <debug/diag.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/console.hpp>
#include <cpu/cpuid.hpp>
#include <memory/vmm.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/klock.hpp>
#include <cpu/tlb.hpp>
#include <cpu/smp.hpp>
#include <cpu/percpu.hpp>
#include <cpu/apic/irqs.hpp>
#include <time/tsc_timer.hpp>

Interrupt *interruptHandlers[256] = {nullptr};

extern unsigned long long runtimeBase;

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

        if (current) {
            if (current->userFpuState) {
                CPU::saveExtendedState(current->userFpuState);
            }
            Console::get().drawText("User process crash.\n");
            Debug::printCurrentProcessSummary();
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
            const int code = 128 + ((frame->interrupt == 0x0E) ? SIGSEGV : SIGTERM);
            // A crash in any thread takes the whole process down.
            if (current->isThread()) {
                Scheduler::get().exitThreadGroup(current, code);
            }
            current->setExitCode(code);
            current->closeFilesOnExit();
            current->setState(ProcessState::Terminated);
            current->setValidUserState(false);
            Scheduler::get().schedule(frame);
        }
        return;
    }
    // Console::get().drawText("\033[2J");
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

// Exceptions take the big kernel lock like any kernel entry. A fault inside the
// kernel (e.g. demand paging a user buffer during a syscall) nests on the lock
// this CPU already holds.
extern "C" void exceptionHandler(InterruptFrame* frame) {
    KernelLock::enter(KernelLock::kSiteException + (frame->interrupt & 0xFF));
    handleException(frame);
    KernelLock::leave();
}

Interrupt::~Interrupt() = default;

void ISR::registerIRQ(uint8_t vector, Interrupt* handler) {
    interruptHandlers[vector] = handler;
    handler->initialize();
}

// Extended (FPU/SSE/AVX) state must be preserved around IRQ handlers because
// handlers freely use vector registers (e.g. the SIMD memcpy/memset in
// common/string.cpp). For a *user-mode* interruption we save into the
// interrupted process's userFpuState. For a *kernel-mode* interruption there is
// no process area to borrow, so we save into a small nesting-indexed scratch
// pool. Without this, an IRQ landing inside kernel code that has live XMM/YMM
// (e.g. a kernel thread doing floating-point pixel math) is silently corrupted.
// IRQ gates run with IF=0 so nesting is normally absent; the depth index keeps
// this correct even if a handler re-enables interrupts. The scratch pool is
// shared by all CPUs: handleIrq runs only under the big kernel lock.
namespace {
constexpr int kMaxKernelIrqFpuDepth = 4;
alignas(64) FPUState kernelIrqFpuScratch[kMaxKernelIrqFpuDepth];
int kernelIrqFpuDepth = 0;
}  // namespace

static void handleIrq(InterruptFrame* frame);

extern "C" void irqHandler(InterruptFrame* frame) {
    if (frame == nullptr) {
        LAPIC::get().sendEOI();
        return;
    }
    // Served without the lock: the sender holds it while waiting for us.
    if (frame->interrupt == VECTOR_TLB_SHOOTDOWN) {
        TLB::handleIpi();
        return;
    }
    const uint32_t site = KernelLock::kSiteIrq + (frame->interrupt & 0xFF);
    // Scheduler ticks are frequent and usually change nothing; taking the lock
    // for each would stall every CPU behind whichever one holds it.
    if ((frame->interrupt == VECTOR_AP_TIMER || frame->interrupt == VECTOR_RESCHEDULE) &&
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
        skippedTicks = 0;
    } else {
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

    if (fpuSave) {
        CPU::restoreExtendedState(fpuSave);
    }
    if (!interruptedUser) {
        --kernelIrqFpuDepth;
    }
}
