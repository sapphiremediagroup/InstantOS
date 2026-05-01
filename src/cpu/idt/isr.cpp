#include <cpu/idt/interrupt.hpp>
#include <cpu/idt/isr.hpp>
#include <debug/diag.hpp>
#include <cpu/process/scheduler.hpp>
#include <graphics/console.hpp>
#include <cpu/cpuid.hpp>
#include <memory/vmm.hpp>

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

extern "C" void exceptionHandler(InterruptFrame* frame) {
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
        }

        if (current) {
            current->setExitCode(128 + ((frame->interrupt == 0x0E) ? SIGSEGV : SIGTERM));
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

Interrupt::~Interrupt() = default;

void ISR::registerIRQ(uint8_t vector, Interrupt* handler) {
    interruptHandlers[vector] = handler;
    handler->initialize();
}

extern "C" void irqHandler(InterruptFrame* frame) {
    if (frame == nullptr) {
        LAPIC::get().sendEOI();
        return;
    }

    Process* current = (frame->cs == 0x23) ? Scheduler::get().getCurrentProcess() : nullptr;
    if (current && current->userFpuState) {
        CPU::saveExtendedState(current->userFpuState);
    }

    Interrupt* handler = interruptHandlers[frame->interrupt];
    if (handler != nullptr) {
        handler->Run(frame);
    } else {
        LAPIC::get().sendEOI();
    }

    if (current && current->userFpuState) {
        CPU::restoreExtendedState(current->userFpuState);
    }
}
