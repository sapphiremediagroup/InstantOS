#include <time/tsc_timer.hpp>

#include <common/ports.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/apic/lapic.hpp>
#include <cpu/cereal/cereal.hpp>
#include <graphics/console.hpp>
#include <drivers/usb/ohci.hpp>
#include <interrupts/timer.hpp>
#include <interrupts/keyboard.hpp>
#include <drivers/net/net_device.hpp>
#include <graphics/intel_gen9.hpp>
#include <time/rtc.hpp>

namespace {
constexpr uint32_t kMsrTscDeadline = 0x6E0;
constexpr uint32_t kLapicTimerDivBy16 = 0x03;
constexpr uint32_t kLapicTimerMasked = 1U << 16;
constexpr uint32_t kLapicTimerDeadlineMode = 1U << 18;
constexpr uint32_t kPitBaseFrequency = 1193182;
constexpr uint16_t kPitChannel0 = 0x40;
constexpr uint16_t kPitCommand = 0x43;

enum class TimerBackend {
    TscDeadline,
    Pit
};

TimerBackend timerBackend = TimerBackend::TscDeadline;
uint64_t timer_tick_hz = 1000;
uint64_t timer_tick_divisor = 0;
uint64_t boot_tsc = 0;
uint64_t next_tsc_deadline = 0;

uint32_t cpuid_max_basic_leaf() {
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    return eax;
}

void cpuid_read(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    eax = leaf;
    ebx = 0;
    ecx = subleaf;
    edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
}

uint64_t lapic_calibrate_tsc_frequency() {
    LAPIC& lapic = LAPIC::get();
    rtc_time start_time = {};
    rtc_time end_time = {};

    rtc_read_time(&start_time);
    do {
        rtc_read_time(&end_time);
    } while (end_time.second == start_time.second &&
             end_time.minute == start_time.minute &&
             end_time.hour == start_time.hour &&
             end_time.day == start_time.day &&
             end_time.month == start_time.month &&
             end_time.year == start_time.year);

    lapic.write(LAPIC_TIMER, kLapicTimerMasked);
    lapic.setTimerDivide(kLapicTimerDivBy16);
    lapic.write(LAPIC_TIMER_INITCNT, 0xFFFFFFFFU);

    const uint64_t start_tsc = rdtsc();
    rtc_time deadline = end_time;
    do {
        rtc_read_time(&end_time);
    } while (end_time.second == deadline.second &&
             end_time.minute == deadline.minute &&
             end_time.hour == deadline.hour &&
             end_time.day == deadline.day &&
             end_time.month == deadline.month &&
             end_time.year == deadline.year);

    const uint64_t end_tsc = rdtsc();
    lapic.write(LAPIC_TIMER, kLapicTimerMasked);
    lapic.write(LAPIC_TIMER_INITCNT, 0);

    return end_tsc - start_tsc;
}

uint64_t cpuid_tsc_frequency_candidate() {
    const uint32_t max_leaf = cpuid_max_basic_leaf();
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    if (max_leaf >= 0x15) {
        cpuid_read(0x15, 0, eax, ebx, ecx, edx);
        if (eax != 0 && ebx != 0 && ecx != 0) {
            const uint64_t crystal_hz = ecx;
            const uint64_t numerator = ebx;
            const uint64_t denominator = eax;
            return (crystal_hz * numerator) / denominator;
        }
    }

    if (max_leaf >= 0x16) {
        cpuid_read(0x16, 0, eax, ebx, ecx, edx);
        if (eax != 0) {
            return static_cast<uint64_t>(eax) * 1000000ULL;
        }
    }

    return 0;
}

uint64_t choose_tsc_frequency(uint64_t cpuidFrequency, uint64_t calibratedFrequency) {
    if (cpuidFrequency == 0) {
        return calibratedFrequency;
    }
    if (calibratedFrequency == 0) {
        return cpuidFrequency;
    }

    const uint64_t larger = cpuidFrequency > calibratedFrequency ? cpuidFrequency : calibratedFrequency;
    const uint64_t smaller = cpuidFrequency > calibratedFrequency ? calibratedFrequency : cpuidFrequency;
    const uint64_t diff = larger - smaller;

    // Trust CPUID only when it is close to the measured RTC calibration.
    if (diff <= (calibratedFrequency / 20ULL)) {
        return cpuidFrequency;
    }

    return calibratedFrequency;
}
}

volatile uint64_t uptime_ticks = 0;
volatile uint64_t uptime_ms = 0;

namespace {
// The uptime comes from the TSC once boot_tsc is set: always with the
// deadline timer, and with the PIT tick too when tsc_enable_clocksource()
// finds a constant-rate TSC. Otherwise it counts PIT ticks.
bool tsc_clock_running() {
    return boot_tsc != 0 && cycles_per_ms != 0;
}

// An invariant TSC (CPUID 80000007h EDX bit 8) ticks at one rate whatever
// the power state; so does the TSC a hypervisor (CPUID 1 ECX bit 31) offers.
bool tsc_runs_at_constant_rate() {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    if ((ecx & (1U << 31)) != 0) {
        return true;
    }
    eax = 0x80000000U; ebx = ecx = edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    if (eax < 0x80000007U) {
        return false;
    }
    eax = 0x80000007U; ebx = ecx = edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    return (edx & (1U << 8)) != 0;
}
}

uint64_t tsc_frequency = 0;
uint64_t cycles_per_ms = 0;
uint64_t cycles_per_us = 0;

bool cpu_has_tsc_deadline() {
    uint32_t eax = 1;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    return (ecx & (1U << 24)) != 0;
}

// Haswell through Kaby Lake have an erratum (Skylake SKL142 and kin) that can
// leave the TSC-deadline timer silent until the microcode fixes it: on an
// i3-6100 with the firmware's microcode the boot CPU's tick stopped seconds
// after boot, idling in hlt. Linux's table (apic_check_deadline_errata) of the
// first good microcode revision per model.
bool tsc_deadline_has_errata(uint32_t* modelOut, uint32_t* revisionOut) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    if (ebx != 0x756E6547U) {   // "Genu"ineIntel
        return false;
    }
    eax = 1; ebx = ecx = edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    const uint32_t family = (eax >> 8) & 0xF;
    const uint32_t model = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    if (family != 6) {
        return false;
    }
    struct Fixed { uint32_t model; uint32_t revision; };
    constexpr Fixed kFixed[] = {
        {0x3C, 0x22}, {0x45, 0x20}, {0x46, 0x17},   // Haswell, -L, -G
        {0x3D, 0x25}, {0x47, 0x17},                 // Broadwell, -G
        {0x4E, 0xB2}, {0x5E, 0xB2},                 // Skylake-L, Skylake
        {0x8E, 0x52}, {0x9E, 0x52},                 // Kaby Lake-L, Kaby Lake
    };
    uint32_t needed = 0;
    for (const Fixed& entry : kFixed) {
        if (entry.model == model) {
            needed = entry.revision;
        }
    }
    if (needed == 0) {
        return false;
    }
    // The revision is in MSR 8Bh's high half after it is zeroed and CPUID 1 runs.
    wrmsr(0x8B, 0);
    eax = 1; ebx = ecx = edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    const uint32_t revision = static_cast<uint32_t>(rdmsr(0x8B) >> 32);
    if (modelOut) *modelOut = model;
    if (revisionOut) *revisionOut = revision;
    return revision < needed;
}

uint64_t detect_tsc_frequency() {
    const uint64_t cpuidFrequency = cpuid_tsc_frequency_candidate();
    const uint64_t calibratedFrequency = lapic_calibrate_tsc_frequency();
    return choose_tsc_frequency(cpuidFrequency, calibratedFrequency);
}

void lapic_enable_tsc_deadline_mode() {
    wrmsr(kMsrTscDeadline, 0);
    timerBackend = TimerBackend::TscDeadline;
    timer_tick_hz = 1000;
    timer_tick_divisor = 0;
    boot_tsc = rdtsc();
    next_tsc_deadline = 0;
    LAPIC::get().write(LAPIC_TIMER, VECTOR_TIMER | kLapicTimerDeadlineMode);
}

void tsc_deadline_set(uint64_t tsc_target) {
    wrmsr(kMsrTscDeadline, tsc_target);
}

void timer_schedule_next_tick() {
    if (cycles_per_ms == 0) {
        return;
    }

    const uint64_t now = rdtsc();
    if (next_tsc_deadline == 0) {
        next_tsc_deadline = now + cycles_per_ms;
    } else {
        next_tsc_deadline += cycles_per_ms;
        if (next_tsc_deadline <= now) {
            const uint64_t ticksBehind = ((now - next_tsc_deadline) / cycles_per_ms) + 1ULL;
            next_tsc_deadline += ticksBehind * cycles_per_ms;
        }
    }

    tsc_deadline_set(next_tsc_deadline);
}

void pit_enable_timer_mode(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = 1000;
    }

    uint32_t divisor = kPitBaseFrequency / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    } else if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    timerBackend = TimerBackend::Pit;
    timer_tick_divisor = divisor;
    timer_tick_hz = kPitBaseFrequency / divisor;
    if (timer_tick_hz == 0) {
        timer_tick_hz = 18;
    }
    boot_tsc = 0;
    next_tsc_deadline = 0;
    outb(kPitCommand, 0x36);
    outb(kPitChannel0, static_cast<uint8_t>(divisor & 0xFF));
    outb(kPitChannel0, static_cast<uint8_t>((divisor >> 8) & 0xFF));
}

void timer_account_tick() {
    uptime_ticks = uptime_ticks + 1;
    if (tsc_clock_running()) {
        uptime_ms = (rdtsc() - boot_tsc) / cycles_per_ms;
    } else if (timer_tick_hz != 0) {
        uptime_ms = (uptime_ticks * 1000ULL) / timer_tick_hz;
    } else {
        uptime_ms = uptime_ticks;
    }

    if (timerBackend == TimerBackend::TscDeadline) {
        timer_schedule_next_tick();
    }
}

namespace {

volatile uint64_t s_dutiesMs = 0;        // uptime when the tick's duties last ran
volatile uint64_t s_failovers = 0;
// The current stand-in: when the boot CPU's tick went missing and how many
// times APs ran its duties since. A long one is logged when the tick is back.
volatile uint64_t s_outageSinceMs = 0;
volatile uint64_t s_outageFailovers = 0;
constexpr uint64_t kLoggedOutageMs = 250;

void run_tick_duties(uint64_t nowMs) {
    USBInput::get().poll();
    Keyboard::get().servicePendingInput();
    Scheduler::get().wakeExpiredSleepers(nowMs);
    s_dutiesMs = nowMs;
}

} // namespace

void timer_interrupt_handler() {
    const uint64_t since = s_outageSinceMs;
    if (since != 0) {
        const uint64_t now = time_get_uptime_ms();
        if (now > since && now - since >= kLoggedOutageMs) {
            Console::get().log("[timer] the boot CPU's tick was out for {} ms; other CPUs ran its duties {} times",
                               now - since, (uint64_t)s_outageFailovers);
        }
        s_outageSinceMs = 0;
        s_outageFailovers = 0;
    }
    run_tick_duties(uptime_ms);
    LAPIC::get().sendEOI();
}

namespace {

// A GPU request that hangs while nobody waits is caught here (intel_gen9).
void gpu_timer_tick() {
    intel_gen9::IntelGen9Driver& gpu = intel_gen9::IntelGen9Driver::get();
    if (gpu.isInitialized()) {
        gpu.timerTick();
    }
}

} // namespace

bool timer_duties_overdue() {
    const uint64_t last = s_dutiesMs;
    const uint64_t now = time_get_uptime_ms();
    // The CPUs' clocks may differ by a little: never overdue from the past.
    return last != 0 && now > last && now - last > kTickDutiesOverdueMs;
}

void timer_run_overdue_duties() {
    // Checked again under the lock: another AP may just have run them.
    if (!timer_duties_overdue()) {
        return;
    }
    if (s_outageSinceMs == 0) {
        s_outageSinceMs = s_dutiesMs;
    }
    s_outageFailovers = s_outageFailovers + 1;
    s_failovers = s_failovers + 1;
    run_tick_duties(time_get_uptime_ms());
    netTimerTick();
    gpu_timer_tick();
}

uint64_t timer_failover_count() {
    return s_failovers;
}

// With the PIT driving the tick, counting its interrupts loses every one that
// lands while interrupts are off; under emulation that is a lot of them (a
// TCG guest's clock ran ~30% slow). So keep time with the TSC when it can.
bool tsc_enable_clocksource() {
    if (tsc_clock_running() || !tsc_runs_at_constant_rate()) {
        return tsc_clock_running();
    }
    if (tsc_frequency == 0) {
        tsc_frequency = detect_tsc_frequency();
    }
    if (tsc_frequency < 1000000ULL) {
        return false;
    }
    cycles_per_ms = tsc_frequency / 1000ULL;
    cycles_per_us = tsc_frequency / 1000000ULL;
    // Carry on from the ticks counted so far.
    const uint64_t elapsedMs = timer_tick_hz ? (uptime_ticks * 1000ULL) / timer_tick_hz : uptime_ms;
    boot_tsc = rdtsc() - elapsedMs * cycles_per_ms;
    return true;
}

bool time_tick_is_tsc_deadline() {
    return timerBackend == TimerBackend::TscDeadline;
}

uint64_t time_get_uptime_ticks() {
    return uptime_ticks;
}

// Nanoseconds since boot from the TSC when it keeps the time, split so it
// never disagrees with time_get_uptime_ms(); else ms resolution.
uint64_t time_get_uptime_ns() {
    if (tsc_clock_running()) {
        const uint64_t cycles = rdtsc() - boot_tsc;
        return (cycles / cycles_per_ms) * 1000000ULL + (cycles % cycles_per_ms) * 1000000ULL / cycles_per_ms;
    }
    return time_get_uptime_ms() * 1000000ULL;
}

uint64_t time_get_uptime_ms() {
    if (tsc_clock_running()) {
        return (rdtsc() - boot_tsc) / cycles_per_ms;
    }

    if (timer_tick_hz != 0) {
        return (uptime_ticks * 1000ULL) / timer_tick_hz;
    }

    return uptime_ms;
}

void Timer::initialize() {
}

void Timer::Run(InterruptFrame* frame) {
    timer_interrupt_handler();


    Process* current = Scheduler::get().getCurrentProcess();
    const bool interruptedUser = frame && frame->cs == kUserCodeSelector;
    const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;

    if (frame && (interruptedUser || runningIdle)) {
        // Before scheduling, so a process woken by a packet can run now.
        netTimerTick();
        gpu_timer_tick();
        Scheduler::get().schedule(frame);
    }
}
