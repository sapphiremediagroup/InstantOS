#pragma once

#include <stdint.h>

struct PageTable;

// TLB shootdown.
//
// Each CPU caches translations for the address space it is running. When the
// kernel removes or narrows a mapping (unmap, mprotect, a copy-on-write break,
// fork marking pages copy-on-write), every other CPU that may cache it must
// drop the stale entry before the change is relied on (before a frame is
// reused, before the child of a fork runs). shootdown() flushes this CPU, then
// IPIs the other CPUs that are running `pml4` and waits until each has flushed.
//
// Waiting is deadlock-free because a target always gets to the flush: in user
// mode or hlt it takes the IPI (interrupts on), and every kernel spin loop that
// runs with interrupts off (the big kernel lock, Spinlock) calls pollPending().
// Adding or widening a mapping needs no shootdown: a CPU that faults on a stale
// narrower entry finds the page table allows the access and just retries (see
// the spurious page fault check in isr.cpp).
namespace TLB {

// Flush `pages` pages from `va` on this CPU; pages == 0 flushes everything.
void flushLocal(uint64_t va, uint64_t pages);

// Flush [va, va + pages pages) of `pml4` on every CPU that may cache it.
// pml4 == nullptr, the kernel address space, or a kernel-half `va` means every
// address space (all CPUs). pages == 0 (or a large count) flushes everything.
void shootdown(PageTable* pml4, uint64_t va, uint64_t pages);

// Serves this CPU's pending shootdown request, if any.
void pollPending();

// VECTOR_TLB_SHOOTDOWN handler (runs without the big kernel lock).
void handleIpi();

} // namespace TLB
