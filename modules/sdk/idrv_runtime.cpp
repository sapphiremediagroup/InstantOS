// The C runtime every driver module carries (the compiler emits calls to
// these), since a module imports nothing from the kernel.

#include "idrv_sdk.h"

extern "C" {

// Set by the loader before init (IdrvModule.runtimeApi); until then, and for
// small sizes, the plain string instructions.
const IdrvKernelApi* idrv_runtime_api = nullptr;

void* memcpy(void* dst, const void* src, size_t n) {
    if (n >= 64 && idrv_runtime_api) {
        return idrv_runtime_api->memcpy(dst, src, n);
    }
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int value, size_t n) {
    if (n >= 64 && idrv_runtime_api) {
        return idrv_runtime_api->memset(dst, value, n);
    }
    auto* d = static_cast<uint8_t*>(dst);
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(static_cast<uint8_t>(value)) : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    if (d <= s || d >= s + n) {
        return memcpy(dst, src, n);
    }
    for (size_t i = n; i > 0; --i) {
        d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

}  // extern "C"
