#include <cpu/watchdog.hpp>

#include <common/ports.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/ioapic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/apic/lapic.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/klock.hpp>
#include <cpu/percpu.hpp>
#include <cpu/process/process.hpp>
#include <cpu/smp.hpp>
#include <debug/diag.hpp>
#include <fs/ahci/ahci.hpp>
#include <graphics/console.hpp>
#include <graphics/intel_gen9.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace {

constexpr uint32_t kNmiDelivery = 4;
constexpr uint64_t kLiveIntervalMs = 500;

volatile uint64_t s_lastTicks = 0;
volatile uint64_t s_lastChangeMs = 0;
volatile uint32_t s_fired = 0;         // a stall is being reported
volatile uint32_t s_nmiWanted = 0;     // the next NMI on the boot CPU is ours
volatile uint32_t s_nmiTaken = 0;
uint64_t s_nmiRip = 0;
uint64_t s_nmiRsp = 0;
uint64_t s_nmiCs = 0;
uint64_t s_nmiRflags = 0;
uint32_t s_stallOwner = 0;
uint32_t s_stallOwnerSite = 0;
uint32_t s_reports = 0;
uint32_t s_reporterCpu = 0;      // the AP reporting the stall in progress
uint64_t s_nextLiveMs = 0;

// Interrupts the boot CPU took, per vector.
volatile uint32_t s_bspVectors[256];

// The boot CPU's local APIC as its NMI handler saw it (only it can read it).
struct LapicSnapshot {
    uint64_t apicBase;       // IA32_APIC_BASE
    uint64_t cr3;
    uint64_t mappedTo;       // what the LAPIC's address translates to in that cr3
    uint32_t id, tpr, ppr, svr, esr, lvtTimer, initCount, curCount;
    uint32_t isr[8];
    uint32_t irr[8];
};
LapicSnapshot s_lapic = {};
uintptr_t s_lapicAddress = 0;

// Where the report goes: during a stall straight onto the screen, no lock
// taken (the CPU that hung may hold the console's or the log's); once the
// tick is back, into the kernel log (log-saver keeps it).
bool s_toLog = false;

void screen(const char* text) {
    if (s_toLog) {
        Cereal::get().write(text);
    } else {
        Console::get().drawRawUnlocked(text);
    }
}

// A line break; on screen it first clears the rest of the line (the live
// lines are redrawn over older ones).
void nl() {
    screen(s_toLog ? "\n" : "\033[K\n");
}

void hex(uint64_t value) {
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i) {
        const uint32_t digit = static_cast<uint32_t>(value >> (60 - 4 * i)) & 0xF;
        buf[2 + i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buf[18] = '\0';
    screen(buf);
}

void hex32(uint32_t value) {
    char buf[9];
    for (int i = 0; i < 8; ++i) {
        const uint32_t digit = (value >> (28 - 4 * i)) & 0xF;
        buf[i] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    buf[8] = '\0';
    screen(buf);
}

void dec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos > 0);
    screen(&buf[pos]);
}

// The kernel is linked for kLinkBase (kernel.map's addresses) but UEFI loads
// it wherever there is room; __ImageBase is where it actually is.
constexpr uint64_t kLinkBase = 0x140000000;
extern "C" char __ImageBase;

// A kernel address as kernel.map has it, and the function there.
void symbol(uint64_t address) {
    const uint64_t base = reinterpret_cast<uint64_t>(&__ImageBase);
    if (address < base) {
        return;   // not in the kernel image (user mode)
    }
    address = address - base + kLinkBase;
    screen(" map=");
    hex(address);
    uint64_t start = 0;
    const char* name = Debug::lookupSymbol(address, &start);
    if (name) {
        screen(" <");
        screen(name);
        screen("+");
        hex(address - start);
        screen(">");
    }
}

// What the disk is doing: commands issued (rising while stalled means a loop of
// commands, not one stuck command) and the one in flight with its registers.
void disk(uint64_t nowMs) {
    screen("  [watchdog] disk: commands=");
    dec(g_ahciDebug.commands);
    screen(" timeouts=");
    dec(g_ahciDebug.timeouts);
    const uint64_t start = g_ahciDebug.startMs;
    HBAPort* regs = g_ahciDebug.regs;
    if (start == 0 || !regs) {
        screen(", none in flight");
        nl();
        nl();
        return;
    }
    screen(", in flight on port ");
    dec(static_cast<uint64_t>(g_ahciDebug.portNum));
    screen(": cmd ");
    hex32(g_ahciDebug.cmd);
    screen(" lba ");
    dec(g_ahciDebug.lba);
    screen(" count ");
    dec(g_ahciDebug.count);
    nl();
    screen("  [watchdog] port regs: ci=");
    hex32(regs->ci);
    screen(" tfd=");
    hex32(regs->tfd);
    screen(" is=");
    hex32(regs->is);
    screen(" serr=");
    hex32(regs->serr);
    screen(" cmd=");
    hex32(regs->cmd);
    nl();
}

void interrupts();

void report(uint64_t nowMs, uint64_t lastMs) {
    if (!s_toLog) {
        // The banner goes into the firmware's framebuffer: put it on screen.
        intel_gen9::IntelGen9Driver::get().showFirmwareSurfaceUnlocked();
        screen("\033[1;1H\033[41;97m\n\n");
    }
    screen("  [watchdog] the boot CPU's timer tick stopped at uptime ");
    dec(lastMs);
    screen(s_toLog ? " ms and came back at " : " ms (now ");
    dec(nowMs);
    screen(s_toLog ? " ms (stall " : " ms, stall ");
    dec(s_reports);
    screen(")");
    nl();
    // The boot CPU's own clock at its last tick; against the stop time above
    // (this CPU's clock) it shows how far apart the CPUs' clocks are.
    screen("  [watchdog] boot CPU's clock at its last tick: ");
    dec(uptime_ms);
    screen(" ms; other CPUs stood in for its tick ");
    dec(timer_failover_count());
    screen(" times since boot");
    nl();

    screen("  [watchdog] boot CPU: ");
    if (s_nmiTaken) {
        screen("rip=");
        hex(s_nmiRip);
        symbol(s_nmiRip);
        nl();
        screen("               rsp=");
        hex(s_nmiRsp);
        screen(" cs=");
        hex(s_nmiCs);
        screen(" rflags=");
        hex(s_nmiRflags);
        screen((s_nmiRflags & 0x200) ? " (interrupts on)" : " (interrupts off)");
        nl();
    } else {
        screen("did not answer the NMI");
        nl();
    }

    const uint32_t owner = s_stallOwner;
    screen("  [watchdog] big kernel lock: ");
    if (owner == 0) {
        screen("free");
    } else {
        screen("held by cpu ");
        dec(owner - 1);
        screen(" since entry site ");
        dec(s_stallOwnerSite);
        screen(" (<256 syscall, 256+ irq, 512+ exception, 768 idle, 769 other, 770 resume)");
    }
    nl();

    PerCPUData* boot = SMP::cpu(0);
    Process* process = boot ? boot->currentProcess : nullptr;
    screen("  [watchdog] boot CPU runs ");
    if (process) {
        screen(process->getName());
        screen(" pid ");
        dec(process->getPID());
    } else {
        screen("nothing");
    }
    if (boot && boot->currentSyscall != ~0ULL) {
        screen(", in syscall ");
        dec(boot->currentSyscall);
    }
    nl();
    screen("  [watchdog] this report: cpu ");
    dec(currentCpuIndex());
    screen(s_toLog ? "" : "; please photograph it");
    nl();
    interrupts();
    disk(nowMs);
    if (!s_toLog) {
        screen("\033[0m");
    }
}

// The boot CPU's interrupt state: its LAPIC (from the NMI), the PIT's IOAPIC
// entry, its tick count and the vectors it has taken.
void interrupts() {
    if (s_nmiTaken) {
        const LapicSnapshot& l = s_lapic;
        screen("  [watchdog] boot lapic: base=");
        hex32(static_cast<uint32_t>(l.apicBase));
        screen(" id=");
        hex32(l.id);
        screen(" tpr=");
        hex32(l.tpr);
        screen(" ppr=");
        hex32(l.ppr);
        screen(" svr=");
        hex32(l.svr);
        screen(" esr=");
        hex32(l.esr);
        nl();
        screen("  [watchdog] lvt-timer=");
        hex32(l.lvtTimer);
        screen(" cnt=");
        hex32(l.initCount);
        screen("/");
        hex32(l.curCount);
        screen(" lapic page in cr3 ");
        hex32(static_cast<uint32_t>(l.cr3));
        screen(" maps to ");
        hex32(static_cast<uint32_t>(l.mappedTo));
        nl();
        // Vectors 255..0, high word first.
        screen("  [watchdog] isr=");
        for (int i = 7; i >= 0; --i) {
            hex32(l.isr[i]);
            screen(i ? "." : "");
        }
        nl();
        screen("  [watchdog] irr=");
        for (int i = 7; i >= 0; --i) {
            hex32(l.irr[i]);
            screen(i ? "." : "");
        }
        nl();
    }
    APICManager& apic = APICManager::get();
    const uint32_t gsi = apic.resolveIRQ(IRQ_TIMER);
    IOAPIC* ioapic = apic.getIOAPICForGSI(gsi);
    screen("  [watchdog] pit: gsi ");
    dec(gsi);
    if (ioapic) {
        screen(" ioapic entry=");
        hex(ioapic->readRedirect(static_cast<uint8_t>(gsi - ioapic->getGSIBase())));
    }
    screen(" boot ticks=");
    dec(uptime_ticks);
    nl();
    screen("  [watchdog] boot CPU vectors:");
    int shown = 0;
    for (int v = 0; v < 256 && shown < 10; ++v) {
        if (s_bspVectors[v] != 0) {
            const char digits[] = "0123456789abcdef";
            const char name[] = {' ', digits[v >> 4], digits[v & 0xF], '=', '\0'};
            screen(name);
            dec(s_bspVectors[v]);
            ++shown;
        }
    }
    if (shown == 0) {
        screen(" none");
    }
    nl();
}

// Redrawn under the report while the stall lasts: whether the disk count
// moves tells a loop of commands from one stuck command.
void live(uint64_t nowMs) {
    // Timed on this CPU only: the CPUs' clocks need not agree.
    static uint64_t lastCommands = ~0ULL;
    static uint64_t sameSinceMs = 0;
    const uint64_t commands = g_ahciDebug.commands;
    if (commands != lastCommands) {
        lastCommands = commands;
        sameSinceMs = nowMs;
    }
    screen("\033[19;1H\033[41;97m  [watchdog] live at ");
    dec(nowMs);
    screen(" ms; no new disk command for ");
    dec(nowMs - sameSinceMs);
    screen(" ms");
    nl();
    disk(nowMs);
    nl();
    screen("\033[0m");
}

} // namespace

namespace Watchdog {

void noteIrq(uint8_t vector) {
    if (currentCpuIndex() == 0) {
        s_bspVectors[vector] = s_bspVectors[vector] + 1;
    }
}

void apTick() {
    if (!SMP::schedulingActive()) {
        return;
    }
    const uint64_t now = time_get_uptime_ms();     // TSC: needs no tick
    const uint64_t ticks = time_get_uptime_ticks();
    if (ticks != s_lastTicks || s_lastChangeMs == 0) {
        // The tick is back after a reported stall: put the report in the
        // kernel log too and watch for the next one. Only the AP that claimed
        // the stall (1 -> 2) writes it.
        if (s_fired == 1 && __atomic_exchange_n(&s_fired, 2u, __ATOMIC_ACQ_REL) == 1) {
            s_toLog = true;
            report(now, s_lastChangeMs);
            s_toLog = false;
            // The banner was drawn into the firmware's framebuffer, which
            // replaced the compositor's on screen: give the screen back.
            intel_gen9::IntelGen9Driver::get().restorePresentedSurfaceUnlocked();
            __atomic_store_n(&s_nmiTaken, 0u, __ATOMIC_RELAXED);
            s_lastTicks = ticks;
            s_lastChangeMs = now;
            __atomic_store_n(&s_fired, 0u, __ATOMIC_RELEASE);
            return;
        }
        if (s_fired) {
            return;
        }
        s_lastTicks = ticks;
        s_lastChangeMs = now;
        return;
    }
    if (now - s_lastChangeMs < kStallMs) {
        return;
    }
    if (s_fired == 1 && currentCpuIndex() == s_reporterCpu) {
        if (now >= s_nextLiveMs) {
            s_nextLiveMs = now + kLiveIntervalMs;
            live(now);
        }
        return;
    }
    // One AP reports, once per stall.
    if (s_fired || __atomic_exchange_n(&s_fired, 1u, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    s_reports++;
    s_reporterCpu = currentCpuIndex();
    s_nextLiveMs = now + kLiveIntervalMs;
    s_stallOwner = KernelLock::debugOwner();
    s_stallOwnerSite = KernelLock::debugOwnerSite();
    const uint64_t lastMs = s_lastChangeMs;
    PerCPUData* boot = SMP::cpu(0);
    if (boot) {
        __atomic_store_n(&s_nmiWanted, 1u, __ATOMIC_RELEASE);
        LAPIC::get().sendIPI(boot->lapicId, 0, kNmiDelivery, 1, 0);
        const uint64_t until = time_get_uptime_ms() + 200;
        while (!__atomic_load_n(&s_nmiTaken, __ATOMIC_ACQUIRE) && time_get_uptime_ms() < until) {
            asm volatile("pause" ::: "memory");
        }
    }
    report(now, lastMs);
}

bool onNmi(InterruptFrame* frame) {
    if (!__atomic_load_n(&s_nmiWanted, __ATOMIC_ACQUIRE) || !frame) {
        return false;
    }
    s_nmiRip = frame->rip;
    s_nmiRsp = frame->rsp;
    s_nmiCs = frame->cs;
    s_nmiRflags = frame->rflags;

    LapicSnapshot& l = s_lapic;
    l.apicBase = rdmsr(0x1B);
    asm volatile("mov %%cr3, %0" : "=r"(l.cr3));
    const uint64_t lapicAddress = l.apicBase & 0xFFFFF000ULL;
    l.mappedTo = VMM::VirtualToPhysicalIn(VMM::GetAddressSpace(), lapicAddress);
    LAPIC& lapic = LAPIC::get();
    l.id = lapic.read(0x20);
    l.tpr = lapic.read(0x80);
    l.ppr = lapic.read(0xA0);
    l.svr = lapic.read(0xF0);
    lapic.write(0x280, 0);           // latch the error status
    l.esr = lapic.read(0x280);
    l.lvtTimer = lapic.read(0x320);
    l.initCount = lapic.read(0x380);
    l.curCount = lapic.read(0x390);
    for (int i = 0; i < 8; ++i) {
        l.isr[i] = lapic.read(0x100 + 0x10 * i);
        l.irr[i] = lapic.read(0x200 + 0x10 * i);
    }
    __atomic_store_n(&s_nmiWanted, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_nmiTaken, 1u, __ATOMIC_RELEASE);
    return true;
}

} // namespace Watchdog
