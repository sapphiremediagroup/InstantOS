// FutexWait / FutexWake: a thread sleeps until another wakes the 32-bit word it
// waits on, provided the word still holds the value it expects.
//
// No wake-up can be lost: both calls run under the big kernel lock, so nothing
// comes between a waiter reading the word and it being queued and blocked, and
// whoever changes the word changes it before calling FutexWake.
//
// A waiter is queued under a key: the address space and the address, or, for a
// page shared between processes (kSharedFrame: IPC shared memory, a dma-buf),
// the physical address. Private memory cannot use the physical address: a fork
// makes it copy-on-write, so a write between a wait and its wake may move it.
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <memory/vmm.hpp>
#include <time/tsc_timer.hpp>

namespace {
constexpr uint64_t kBuckets = 64;
Process* s_waiters[kBuckets];   // each in the order they came, linked by FutexWaiter::next

struct Key {
    uint64_t space;   // the address space's page table, 0 for a shared page
    uint64_t address;
};

bool isFutexAddress(uint64_t address) {
    return (address & 3) == 0;
}

// A shared page this process has not touched yet is made present first: its
// frame is what the others wait under.
Key keyOf(Process* process, uint64_t address) {
    uint64_t entry = VMM::LeafEntryIn(process->getPageTable(), address);
    if (!entry && process->mmapTouchShared(address)) {
        entry = VMM::LeafEntryIn(process->getPageTable(), address);
    }
    if ((entry & UserSuper) && (entry & kSharedFrame)) {
        return {0, (entry & ADDR_MASK) | (address & (PAGE_SIZE - 1))};
    }
    return {reinterpret_cast<uint64_t>(process->getPageTable()), address};
}

Process*& bucketOf(const Key& key) {
    uint64_t h = (key.space >> 12) ^ (key.address >> 2);
    h *= 0x9E3779B97F4A7C15ULL;
    return s_waiters[h >> 58];
}

void enqueue(Process* process, const Key& key) {
    FutexWaiter& waiter = process->futex;
    waiter.space = key.space;
    waiter.address = key.address;
    waiter.next = nullptr;
    waiter.queued = true;
    Process** link = &bucketOf(key);
    while (*link) link = &(*link)->futex.next;
    *link = process;
}
}

// Takes a process off its queue if it is on one (it was woken by something
// else than FutexWake, or is being freed).
void futexForget(Process* process) {
    FutexWaiter& waiter = process->futex;
    if (!waiter.queued) return;
    waiter.queued = false;
    for (Process** link = &bucketOf({waiter.space, waiter.address}); *link; link = &(*link)->futex.next) {
        if (*link == process) {
            *link = waiter.next;
            break;
        }
    }
    waiter.next = nullptr;
}

uint64_t Syscall::sys_futex_wait(uint64_t address, uint64_t expected, uint64_t timeoutNs) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !isFutexAddress(address)) return syscall_error(SysErrInvalid);
    if (!isValidUserPointer(address, sizeof(uint32_t))) return syscall_error(SysErrFault);

    // One 32-bit load (copyFromUser copies bytes). It may demand-fault the page
    // in; nothing after it gives up the lock before the waiter is blocked.
    const uint32_t value = __atomic_load_n(reinterpret_cast<const uint32_t*>(address), __ATOMIC_SEQ_CST);
    if (value != static_cast<uint32_t>(expected)) return syscall_error(SysErrAgain);
    // Also keeps a process that is about to be killed off the queues: it would
    // die in the scheduler, still queued.
    if (current->hasDeliverableSignal()) return syscall_error(SysErrInterrupted);

    const bool timed = timeoutNs != kFutexNoTimeout;
    uint64_t deadlineMs = 0;
    if (timed) {
        if (timeoutNs == 0) return syscall_error(SysErrTimedOut);
        // Never early: the deadline is rounded up to the tick's milliseconds.
        const uint64_t now = time_get_uptime_ns();
        const uint64_t deadlineNs = timeoutNs > UINT64_MAX - now ? UINT64_MAX : now + timeoutNs;
        deadlineMs = deadlineNs / 1000000 + (deadlineNs % 1000000 ? 1 : 0);
    }

    enqueue(current, keyOf(current, address));
    // Others may wake this process for reasons of their own (an IPC message);
    // it stays queued and goes back to sleep.
    while (true) {
        if (timed) current->sleepUntil(deadlineMs);
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();

        // First: a wake that counted this waiter must not be reported as a
        // timeout or an interruption.
        if (!current->futex.queued) return 0;
        if (current->hasDeliverableSignal()) {
            futexForget(current);
            return syscall_error(SysErrInterrupted);
        }
        if (timed && time_get_uptime_ms() >= deadlineMs) {
            futexForget(current);
            return syscall_error(SysErrTimedOut);
        }
    }
}

// Wakes up to `count` waiters, the longest waiting first; returns how many.
uint64_t Syscall::sys_futex_wake(uint64_t address, uint64_t count) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !isFutexAddress(address)) return syscall_error(SysErrInvalid);
    // The memory may be gone by now (a mutex freed by the thread that just got
    // it), so it is only an address here.
    if (address == 0 || address >= 0x0000800000000000ULL) return syscall_error(SysErrFault);

    const Key key = keyOf(current, address);
    uint64_t woken = 0;
    Process** link = &bucketOf(key);
    while (*link && woken < count) {
        Process* process = *link;
        FutexWaiter& waiter = process->futex;
        if (waiter.space != key.space || waiter.address != key.address) {
            link = &waiter.next;
            continue;
        }
        *link = waiter.next;
        waiter.next = nullptr;
        waiter.queued = false;
        Scheduler::get().wakeProcess(process);
        woken++;
    }
    return woken;
}
