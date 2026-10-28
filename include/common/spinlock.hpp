#pragma once

#include <stdint.h>

// Kernel spinlocks.
//
// Spinlock     plain test-and-set lock; interrupts are left as they are. Only
//              for data never touched from interrupt context, or for try-lock
//              re-entry guards (tryLock) where the loser skips the work.
// IrqSpinlock  saves RFLAGS, disables interrupts, then takes the lock; use it
//              for anything an IRQ handler (timer, scheduler, input) can reach.
//              lock() returns the saved flags, unlock() restores them.
//
// Debug checks (both kinds): the holder's CPU is recorded, re-acquiring a lock
// the same CPU already holds is reported over serial (it would otherwise hang
// silently), and a spin that runs unusually long is reported once with the
// owner. Reports go straight to Cereal, which keeps its own lock so it can be
// used from here.

class Spinlock {
public:
    constexpr explicit Spinlock(const char* name = "spinlock") : name(name) {}

    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock();
    bool tryLock();
    void unlock();
    bool isHeld() const { return __atomic_load_n(&locked, __ATOMIC_RELAXED) != 0; }
    bool heldByThisCpu() const;

    const char* getName() const { return name; }

private:
    friend class IrqSpinlock;

    volatile uint32_t locked = 0;
    // CPU index + 1 of the holder; 0 while free.
    volatile uint32_t owner = 0;
    const char* name;

    void acquired();
    void spinUntilFree(void* caller);
};

class IrqSpinlock {
public:
    constexpr explicit IrqSpinlock(const char* name = "irq-spinlock") : inner(name) {}

    IrqSpinlock(const IrqSpinlock&) = delete;
    IrqSpinlock& operator=(const IrqSpinlock&) = delete;

    uint64_t lock();
    bool tryLock(uint64_t* outFlags);
    void unlock(uint64_t flags);
    bool isHeld() const { return inner.isHeld(); }
    bool heldByThisCpu() const { return inner.heldByThisCpu(); }

    const char* getName() const { return inner.getName(); }

private:
    Spinlock inner;
};

class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& lock) : lockRef(lock) { lockRef.lock(); }
    ~SpinlockGuard() { lockRef.unlock(); }

    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;

private:
    Spinlock& lockRef;
};

class IrqSpinlockGuard {
public:
    explicit IrqSpinlockGuard(IrqSpinlock& lock) : lockRef(lock), flags(lock.lock()) {}
    ~IrqSpinlockGuard() { lockRef.unlock(flags); }

    IrqSpinlockGuard(const IrqSpinlockGuard&) = delete;
    IrqSpinlockGuard& operator=(const IrqSpinlockGuard&) = delete;

private:
    IrqSpinlock& lockRef;
    uint64_t flags;
};

// RFLAGS helpers shared by IrqSpinlock and code that only needs interrupts off.
uint64_t saveFlagsAndDisableInterrupts();
void restoreFlags(uint64_t flags);
