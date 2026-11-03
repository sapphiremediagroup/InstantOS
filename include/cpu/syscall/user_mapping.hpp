#pragma once

#include <stdint.h>

class VNode;

// Mappings the kernel makes in the current process on its own behalf (not an
// mmap the process asked for): a buffer surface's dma-buf in the compositor.
// The region holds the buffer object, as an mmap of the dma-buf would.

// Maps the first `length` bytes (rounded up to pages) of a dma-buf, shared,
// read+write, write-back. The address, or 0.
uint64_t mapDmaBufIntoCurrent(VNode* dmaBuf, uint64_t length);
// Takes such a mapping (or any range) out of the current process.
void unmapRangeOfCurrent(uint64_t address, uint64_t length);
