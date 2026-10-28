#pragma once

#include <stdint.h>

struct VNodeOps;
class Process;

// The storage lock: the first subsystem split out of the big kernel lock.
//
// It serializes everything behind a block-backed filesystem: ext4 and FAT32
// (their VNode ops, mount and format), the partition layer and the AHCI
// driver. That code depends only on the heap, the PMM and the console, which
// have their own locks, so a storage operation can run without the BKL. While
// it polls the disk, the other CPUs keep taking syscalls and interrupts.
//
// Lock order: BKL, then storage. A CPU that holds the storage lock never waits
// for the BKL, so taking the storage lock while holding the BKL is always safe.
//
// Dropping the BKL is opt-in. A syscall that holds no unpinned pointer to
// BKL-protected state across a VFS call declares an AllowUnlocked scope. Only
// at that syscall's own nesting depth does a Section give up the BKL. Page
// faults and other nested entries keep it.
namespace StorageLock {

// Wraps every entry of a block-backed filesystem's VNodeOps table so that each
// operation runs inside a Section. Every VNode of the filesystem shares the
// table, so they are all covered. Idempotent.
void wrapOps(VNodeOps* ops);

bool heldByThisCpu();

// Holds the storage lock for its lifetime (recursive per CPU). When the current
// syscall allows it, the outermost Section also releases the BKL until it ends.
class Section {
public:
    Section();
    ~Section();

    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

private:
    uint32_t savedBklDepth;
};

// For the current syscall: storage operations may run without the BKL.
class AllowUnlocked {
public:
    explicit AllowUnlocked(bool allow);
    ~AllowUnlocked();

    AllowUnlocked(const AllowUnlocked&) = delete;
    AllowUnlocked& operator=(const AllowUnlocked&) = delete;

private:
    Process* process;
    uint32_t savedDepth;
};

} // namespace StorageLock
