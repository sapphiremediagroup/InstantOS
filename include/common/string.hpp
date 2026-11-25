#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {

void* memset(void* dest, int val, size_t count);
void* memset32(void* dest, uint32_t val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* b1, const void* b2, size_t count);
void* memchr(const void* b1, int val, size_t count);
void* memrchr(const void* b1, int val, size_t count);

void memory_init_acceleration();
bool memory_validate_acceleration();

// Times rep / SSE2 / AVX2 on a 4 KiB copy and fill and makes memcpy, memset and
// memset32 use the fastest (string.cpp; MemBench in tools/membench is the full
// table). out[] gets each routine's pick and the cycles of every variant (0 when
// the CPU lacks it). Call after memory_init_acceleration().
struct MemAccelChoice {
    const char* routine;
    const char* picked;
    uint64_t cycles[3];   // rep, sse2, avx2
};
void memory_calibrate_acceleration(MemAccelChoice out[3]);

// The implementations behind memcpy/memset/memset32/memcmp one by one, for the
// MemBench syscall. Auto is the dispatch the functions above use.
namespace memaccel {
enum class Variant : uint32_t { Auto = 0, Rep = 1, Sse2 = 2, Avx2 = 3 };
bool supported(Variant v);   // the CPU has it and it is enabled
void* copy(Variant v, void* dest, const void* src, size_t n);
void* fill(Variant v, void* dest, int val, size_t n);
void* fill32(Variant v, void* dest, uint32_t val, size_t n);
int compare(Variant v, const void* a, const void* b, size_t n);   // Rep: byte loop
}

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);

}
