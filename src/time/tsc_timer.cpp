#include <time/tsc_timer.hpp>

#include <common/ports.hpp>
#include <cpu/apic/irqs.hpp>
#include <cpu/apic/lapic.hpp>
#include <cpu/cereal/cereal.hpp>
#include <drivers/usb/ohci.hpp>
#include <interrupts/timer.hpp>
#include <interrupts/keyboard.hpp>
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
}

volatile uint64_t uptime_ticks = 0;
volatile uint64_t uptime_ms = 0;

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

uint64_t detect_tsc_frequency() {
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

    return lapic_calibrate_tsc_frequency();
}

void lapic_enable_tsc_deadline_mode() {
    wrmsr(kMsrTscDeadline, 0);
    LAPIC::get().write(LAPIC_TIMER, VECTOR_TIMER | kLapicTimerDeadlineMode);
}

void tsc_deadline_set(uint64_t tsc_target) {
    wrmsr(kMsrTscDeadline, tsc_target);
}

void timer_schedule_next_tick() {
    if (cycles_per_ms == 0) {
        return;
    }

    tsc_deadline_set(rdtsc() + cycles_per_ms);
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
    outb(kPitCommand, 0x36);
    outb(kPitChannel0, static_cast<uint8_t>(divisor & 0xFF));
    outb(kPitChannel0, static_cast<uint8_t>((divisor >> 8) & 0xFF));
}

void timer_interrupt_handler() {
    uptime_ticks = uptime_ticks + 1;
    uptime_ms = uptime_ms + 1;

    if (timerBackend == TimerBackend::TscDeadline) {
        timer_schedule_next_tick();
    }

    LAPIC::get().sendEOI();
}

uint64_t time_get_uptime_ticks() {
    return uptime_ticks;
}

uint64_t time_get_uptime_ms() {
    return uptime_ms;
}

void Timer::initialize() {
}

void Timer::Run(InterruptFrame* frame) {
    timer_interrupt_handler();

    USBInput::get().poll();
    Keyboard::get().servicePendingInput();

    Process* current = Scheduler::get().getCurrentProcess();
    const bool interruptedUser = frame && frame->cs == kUserCodeSelector;
    const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;

    if (frame && (interruptedUser || runningIdle)) {
        Scheduler::get().schedule(frame);
    }
}
