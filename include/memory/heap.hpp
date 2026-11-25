#pragma once

#include <stdint.h>
#include <stddef.h>

struct HeapStats {
    size_t total_allocated;
    size_t peak_usage;
    size_t free_block_count;
};

void heap_init(void* base, size_t size);
void* kmalloc(size_t size);
void* kmalloc_aligned(size_t size, size_t align);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t new_size);
HeapStats heap_stats();
bool heap_is_initialized();
uintptr_t heap_base();
size_t heap_size();
int heap_arena_count();
int heap_low_arena_count();   // grows refused: no memory above the non-PIE window
bool heap_contains(const void* ptr);
void heap_dump_callers(int top);   // live blocks by allocating caller (leak hunting)
