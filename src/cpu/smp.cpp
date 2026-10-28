#include <cpu/smp.hpp>
#include <cpu/apic/apic.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/apic/irq_balance.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/idt/isr.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/cpuid.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/idt/idt.hpp>
#include <cpu/syscall/syscall.hpp>
#include <common/ports.hpp>
#include <common/string.hpp>
#include <graphics/console.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

extern "C" uint8_t smpTrampolineStart[];
extern "C" uint8_t smpTrampolineEnd[];
extern "C" uint8_t smpTrampolineData[];
extern "C" void smpApEntry();
extern "C" void smpApMain(PerCPUData* cpu);

namespace {

constexpr uint32_t MSR_GS_BASE = 0xC0000101;
constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr uint64_t EFER_LME = 1ULL << 8;
constexpr uint64_t EFER_NXE = 1ULL << 11;

// Low-memory block: trampoline page, then the temporary page tables it uses
// to reach long mode (PML4, PDPT, 4 PDs identity-mapping 0..4 GiB with 2 MiB
// pages). Startup IPIs can only target a page below 1 MiB; stay below the
// legacy VGA/BIOS hole.
constexpr uint64_t kLowPages = 7;
constexpr uint64_t kLowLimit = 0xA0000;
constexpr uint64_t kIdentityLimit = 4ULL << 30;

constexpr uint64_t kApStackSize = 16 * 1024;
constexpr uint64_t kApDoubleFaultStackSize = 16 * 1024;

// Mirrors smpTrampolineData in smp_trampoline.asm.
struct TrampolineData {
    uint64_t pml4;
    uint64_t eferBits;
    uint64_t kernelCr3;
    uint64_t stackTop;
    uint64_t entry;
    uint64_t cpu;
};

uint64_t s_lowBase = 0;
PerCPUData* s_cpus[SMP::kMaxCPUs] = {};
uint64_t s_doubleFaultStackTops[SMP::kMaxCPUs] = {};
uint32_t s_onlineCount = 1;   // the boot CPU

// Set by an AP at the end of smpApMain; the boot CPU waits for it before
// reusing the trampoline for the next AP.
volatile uint32_t s_apReady = 0;

// Released by startScheduling(); parked APs spin on it.
volatile uint32_t s_go = 0;
bool s_schedulingActive = false;

// AP scheduler tick. The boot CPU keeps its own timer (PIT or TSC deadline).
constexpr uint64_t kApTimerHz = 250;
constexpr uint32_t kLapicDivideBy16 = 0x3;
constexpr uint32_t kLapicTimerMasked = 0x10000;
// LAPIC timer counts per AP tick, measured once on the boot CPU when its own
// LAPIC timer is free (PIT mode); 0 means each AP measures it itself.
uint32_t s_lapicTicksPerTick = 0;

// Handles both the AP timer and the reschedule IPI: preempt user mode, or pick
// up work if this CPU is idle. Same policy as the boot CPU's Timer::Run.
class ApSchedulerInterrupt : public Interrupt {
public:
    static ApSchedulerInterrupt& get() {
        static ApSchedulerInterrupt instance;
        return instance;
    }

    void initialize() override {}

    void Run(InterruptFrame* frame) override {
        sendEOI();
        Process* current = Scheduler::get().getCurrentProcess();
        const bool interruptedUser = frame && frame->cs == 0x23;
        const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;
        if (frame && (interruptedUser || runningIdle)) {
            Scheduler::get().schedule(frame);
        }
    }
};

void writeDec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && pos > 0);
    Cereal::get().write(&buf[pos]);
}

// Busy-wait. Uses the calibrated TSC when the TSC-deadline timer is in use,
// otherwise a PIT channel 2 one-shot (channel 0 drives the scheduler tick).
void delayUs(uint64_t us) {
    if (cycles_per_us != 0) {
        const uint64_t start = rdtsc();
        while (rdtsc() - start < us * cycles_per_us) {
            asm volatile("pause");
        }
        return;
    }

    constexpr uint64_t kPitHz = 1193182;
    constexpr uint64_t kMaxChunkUs = 50000;   // 16-bit count limit is ~54.9 ms
    const uint8_t savedGate = inb(0x61);
    while (us > 0) {
        const uint64_t chunk = us > kMaxChunkUs ? kMaxChunkUs : us;
        uint64_t count = (chunk * kPitHz) / 1000000ULL;
        if (count == 0) count = 1;

        // Gate on, speaker off; channel 2, lobyte/hibyte, mode 0 (one-shot).
        outb(0x61, static_cast<uint8_t>((inb(0x61) & ~0x02) | 0x01));
        outb(0x43, 0xB0);
        outb(0x42, static_cast<uint8_t>(count & 0xFF));
        outb(0x42, static_cast<uint8_t>((count >> 8) & 0xFF));
        while ((inb(0x61) & 0x20) == 0) {   // OUT2 goes high at terminal count
            asm volatile("pause");
        }
        us -= chunk;
    }
    outb(0x61, savedGate);
}

void buildTemporaryPageTables() {
    auto table = [](uint64_t index) {
        return reinterpret_cast<uint64_t*>(VMM::PhysToVirt(s_lowBase + index * PMM::PAGE_SIZE));
    };
    constexpr uint64_t kPresentWritable = 0x3;
    constexpr uint64_t kLargePage = 0x80;

    uint64_t* pml4 = table(1);
    uint64_t* pdpt = table(2);
    memset(pml4, 0, PMM::PAGE_SIZE);
    memset(pdpt, 0, PMM::PAGE_SIZE);
    pml4[0] = (s_lowBase + 2 * PMM::PAGE_SIZE) | kPresentWritable;

    for (uint64_t gib = 0; gib < 4; ++gib) {
        uint64_t* pd = table(3 + gib);
        pdpt[gib] = (s_lowBase + (3 + gib) * PMM::PAGE_SIZE) | kPresentWritable;
        for (uint64_t i = 0; i < 512; ++i) {
            pd[i] = ((gib << 30) + (i << 21)) | kPresentWritable | kLargePage;
        }
    }
}

// LAPIC timer counts per AP tick, measured against delayUs() over 10 ms with
// the executing CPU's LAPIC timer (masked, so it raises nothing).
uint32_t measureLapicTicksPerTick() {
    LAPIC& lapic = LAPIC::get();
    lapic.write(LAPIC_TIMER, kLapicTimerMasked | VECTOR_AP_TIMER);
    lapic.setTimerDivide(kLapicDivideBy16);
    lapic.write(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);
    delayUs(10000);
    const uint32_t remaining = lapic.read(LAPIC_TIMER_CURCNT);
    lapic.write(LAPIC_TIMER_INITCNT, 0);

    const uint64_t per10ms = 0xFFFFFFFFULL - remaining;
    return static_cast<uint32_t>((per10ms * 100) / kApTimerHz);
}

PerCPUData* createCpuData(uint32_t index, uint8_t lapicId, uint64_t* outStackTop) {
    auto* cpu = new PerCPUData{};
    auto* gdt = new GDT();
    void* stack = kmalloc_aligned(kApStackSize, 16);
    void* dfStack = kmalloc_aligned(kApDoubleFaultStackSize, 16);
    Process* idleProcess = Scheduler::get().createIdleProcess();
    if (!cpu || !gdt || !stack || !dfStack || !idleProcess) {
        return nullptr;
    }

    cpu->self = cpu;
    cpu->cpuIndex = index;
    cpu->lapicId = lapicId;
    cpu->gdt = gdt;
    cpu->idleProcess = idleProcess;
    s_doubleFaultStackTops[index] = reinterpret_cast<uint64_t>(dfStack) + kApDoubleFaultStackSize;
    *outStackTop = reinterpret_cast<uint64_t>(stack) + kApStackSize;
    return cpu;
}

bool waitForAp(uint64_t timeoutUs) {
    for (uint64_t waited = 0; waited < timeoutUs; waited += 100) {
        if (__atomic_load_n(&s_apReady, __ATOMIC_ACQUIRE)) {
            return true;
        }
        delayUs(100);
    }
    return __atomic_load_n(&s_apReady, __ATOMIC_ACQUIRE) != 0;
}

// INIT, 10 ms, Startup, 200 us, and a second Startup only if the AP has not
// shown up yet (Intel MP spec sequence).
bool startAp(uint8_t lapicId, uint8_t vector) {
    LAPIC& lapic = LAPIC::get();
    lapic.sendInitIPI(lapicId);
    delayUs(10000);
    lapic.sendStartupIPI(lapicId, vector);
    if (waitForAp(200)) {
        return true;
    }
    lapic.sendStartupIPI(lapicId, vector);
    return waitForAp(1000000);
}

} // namespace

// Reached from the trampoline in long mode on the temporary page tables with
// rcx = PerCPUData*, rdx = kernel CR3, rsp = this AP's stack. Running from
// kernel text, switch to the kernel address space before touching the stack.
extern "C" __attribute__((naked)) void smpApEntry() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "mov cr3, rdx\n"
        "and rsp, -16\n"
        "sub rsp, 32\n"          // Microsoft x64 shadow space
        "call smpApMain\n"
        "1:\n"
        "cli\n"
        "hlt\n"
        "jmp 1b\n"
        ".att_syntax prefix\n");
}

extern "C" void smpApMain(PerCPUData* cpu) {
    // GS first so everything below (spinlock owner checks, logging) sees this
    // CPU's data. GDT::initialize() preserves the GS base across its reload.
    wrmsr(MSR_GS_BASE, reinterpret_cast<uint64_t>(cpu));
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    cpu->gdt->initialize(s_doubleFaultStackTops[cpu->cpuIndex]);
    IDT::get().load();
    CPU::initializeSecondary();
    Syscall::get().initializeCpu();
    LAPIC::get().enable();
    cpu->lapicId = LAPIC::get().getId();

    Cereal::get().write("[smp] cpu ");
    writeDec(cpu->cpuIndex);
    Cereal::get().write(" online lapic=");
    writeDec(cpu->lapicId);
    Cereal::get().write("\n");

    __atomic_store_n(&s_apReady, 1, __ATOMIC_RELEASE);

    // Parked until boot is done (interrupts off, so no hlt: spin on the flag).
    while (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }

    uint32_t ticks = s_lapicTicksPerTick;
    if (ticks == 0) {
        ticks = measureLapicTicksPerTick();   // TSC-based delay: safe per CPU
    }
    if (ticks == 0) {
        ticks = 100000;
    }
    LAPIC::get().setTimerDivide(kLapicDivideBy16);
    LAPIC::get().startTimer(ticks, VECTOR_AP_TIMER, true);

    // Become this CPU's idle loop; from here on it only runs what the
    // scheduler hands it. Never returns.
    Scheduler::get().runIdleOnThisCpu();
}

namespace SMP {

void reserveLowMemory() {
    s_lowBase = PMM::AllocFramesBelow(kLowPages, kLowLimit);
}

void startAPs() {
    s_cpus[0] = getPerCPU();

    // Any CPU, the boot CPU included, can be sent a reschedule IPI.
    ISR::registerIRQ(VECTOR_AP_TIMER, &ApSchedulerInterrupt::get());
    ISR::registerIRQ(VECTOR_RESCHEDULE, &ApSchedulerInterrupt::get());

    APICManager& apic = APICManager::get();
    const uint32_t listed = apic.getCPUCount();
    if (listed <= 1) {
        Console::get().drawText("[SMP] single CPU\n");
        return;
    }
    if (s_lowBase == 0) {
        Console::get().drawText("[SMP] no free real-mode memory for the AP trampoline; staying on 1 CPU\n");
        return;
    }
    if (reinterpret_cast<uint64_t>(&smpApEntry) + PMM::PAGE_SIZE > kIdentityLimit) {
        Console::get().drawText("[SMP] kernel above 4 GiB, outside the trampoline identity map; staying on 1 CPU\n");
        return;
    }

    const uint64_t trampolineSize = static_cast<uint64_t>(smpTrampolineEnd - smpTrampolineStart);
    const uint64_t dataOffset = static_cast<uint64_t>(smpTrampolineData - smpTrampolineStart);
    auto* page = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(s_lowBase));
    memcpy(page, smpTrampolineStart, trampolineSize);
    auto* data = reinterpret_cast<volatile TrampolineData*>(page + dataOffset);
    buildTemporaryPageTables();

    const uint8_t vector = static_cast<uint8_t>(s_lowBase >> 12);
    const uint64_t kernelCr3 = reinterpret_cast<uint64_t>(VMM::GetKernelAddressSpace()) & ADDR_MASK;
    const uint64_t eferBits = EFER_LME | (CPU::getFeatures().nxEnabled ? EFER_NXE : 0);
    const uint32_t bspLapicId = getPerCPU()->lapicId;

    // In PIT mode the boot CPU's LAPIC timer is unused, so measure the AP tick
    // here once. (In TSC-deadline mode it drives the boot CPU's tick; each AP
    // then measures with its own timer against the TSC.)
    if (cycles_per_us == 0) {
        s_lapicTicksPerTick = measureLapicTicksPerTick();
    }

    for (uint32_t i = 0; i < listed && s_onlineCount < kMaxCPUs; ++i) {
        const uint8_t lapicId = apic.getAPICId(static_cast<uint8_t>(i));
        if (lapicId == bspLapicId) {
            continue;
        }

        const uint32_t index = s_onlineCount;
        uint64_t stackTop = 0;
        PerCPUData* cpu = createCpuData(index, lapicId, &stackTop);
        if (!cpu) {
            Console::get().drawText("[SMP] out of memory for AP state\n");
            break;
        }

        data->pml4 = s_lowBase + PMM::PAGE_SIZE;
        data->eferBits = eferBits;
        data->kernelCr3 = kernelCr3;
        data->stackTop = stackTop;
        data->entry = reinterpret_cast<uint64_t>(&smpApEntry);
        data->cpu = reinterpret_cast<uint64_t>(cpu);
        __atomic_store_n(&s_apReady, 0, __ATOMIC_RELEASE);

        if (!startAp(lapicId, vector)) {
            Cereal::get().write("[smp] lapic ");
            writeDec(lapicId);
            Cereal::get().write(" did not come online\n");
            continue;
        }

        s_cpus[index] = cpu;
        s_onlineCount++;
    }

    Console::get().drawText("[SMP] CPUs online: ");
    Console::get().drawNumber(static_cast<int64_t>(s_onlineCount));
    Console::get().drawText("/");
    Console::get().drawNumber(static_cast<int64_t>(listed));
    Console::get().drawText("\n");
}

void startScheduling() {
    IRQBalance::distribute();
    s_schedulingActive = true;
    __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
}

bool schedulingActive() {
    return s_schedulingActive;
}

void sendReschedule(PerCPUData* cpu) {
    if (!cpu) return;
    constexpr uint32_t kFixedDelivery = 0;
    LAPIC::get().sendIPI(cpu->lapicId, VECTOR_RESCHEDULE, kFixedDelivery, 1, 0);
}

uint32_t onlineCount() {
    return s_onlineCount;
}

PerCPUData* cpu(uint32_t index) {
    return index < kMaxCPUs ? s_cpus[index] : nullptr;
}

} // namespace SMP
