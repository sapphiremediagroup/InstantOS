#include <common/spinlock.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/percpu.hpp>
#include <cpu/tlb.hpp>

namespace {

// Pause iterations before a spin is reported as suspiciously long. Under TCG a
// pause is a few ns, so this is on the order of a second of real spinning.
constexpr uint64_t kLongSpinIterations = 1ULL << 27;

void writeHex(uint64_t value) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        const uint8_t nibble = (value >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buf[18] = '\0';
    Cereal::get().write(buf);
}

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

void report(const char* what, const char* name, uint32_t owner, void* caller) {
    Cereal::get().write("[spinlock] ");
    Cereal::get().write(what);
    Cereal::get().write(" name=");
    Cereal::get().write(name ? name : "?");
    Cereal::get().write(" owner_cpu=");
    if (owner == 0) {
        Cereal::get().write("none");
    } else {
        writeDec(owner - 1);
    }
    Cereal::get().write(" cpu=");
    writeDec(currentCpuIndex());
    Cereal::get().write(" caller=");
    writeHex(reinterpret_cast<uint64_t>(caller));
    Cereal::get().write("\n");
}

} // namespace

uint64_t saveFlagsAndDisableInterrupts() {
    uint64_t flags = 0;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void restoreFlags(uint64_t flags) {
    asm volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

bool Spinlock::heldByThisCpu() const {
    return isHeld() && owner == currentCpuIndex() + 1;
}

void Spinlock::acquired() {
    owner = currentCpuIndex() + 1;
}

void Spinlock::spinUntilFree(void* caller) {
    uint64_t flags = 0;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    if ((flags & 0x200) == 0 && heldByThisCpu()) {
        // Interrupts are off, so nothing can preempt us and let the holder (on
        // this same CPU) run again to release it: a deadlock. Report it and keep
        // spinning, which is what the old hand-rolled locks did silently. With
        // interrupts on, the holder may just be a preempted process.
        report("recursive acquire (deadlock)", name, owner, caller);
    }

    uint64_t spins = 0;
    bool reported = false;
    for (;;) {
        while (isHeld()) {
            // The holder may be waiting for this CPU to flush its TLB.
            TLB::pollPending();
            asm volatile("pause" ::: "memory");
            if (!reported && ++spins >= kLongSpinIterations) {
                report("long spin", name, owner, caller);
                reported = true;
            }
        }
        if (!__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {
            return;
        }
    }
}

void Spinlock::lock() {
    if (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {
        spinUntilFree(__builtin_return_address(0));
    }
    acquired();
}

bool Spinlock::tryLock() {
    if (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {
        return false;
    }
    acquired();
    return true;
}

void Spinlock::unlock() {
    owner = 0;
    __atomic_clear(&locked, __ATOMIC_RELEASE);
}

uint64_t IrqSpinlock::lock() {
    const uint64_t flags = saveFlagsAndDisableInterrupts();
    if (__atomic_test_and_set(&inner.locked, __ATOMIC_ACQUIRE)) {
        inner.spinUntilFree(__builtin_return_address(0));
    }
    inner.acquired();
    return flags;
}

bool IrqSpinlock::tryLock(uint64_t* outFlags) {
    const uint64_t flags = saveFlagsAndDisableInterrupts();
    if (!inner.tryLock()) {
        restoreFlags(flags);
        return false;
    }
    if (outFlags) *outFlags = flags;
    return true;
}

void IrqSpinlock::unlock(uint64_t flags) {
    inner.unlock();
    restoreFlags(flags);
}
