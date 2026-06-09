#include <common/string.hpp>
#include <cpu/cpuid.hpp>

namespace {
constexpr size_t SIMD_THRESHOLD = 128;
bool accelerationInitialized = false;
bool enableSSE2 = false;
bool enableAVX2 = false;

void* memset_rep(void* dest, int val, size_t count) {
    void* ret = dest;
    asm volatile(
        "cld\n"
        "rep stosb"
        : "+D"(dest), "+c"(count)
        : "a"((uint8_t)val)
        : "memory"
    );
    return ret;
}

void* memset32_rep(void* dest, uint32_t val, size_t count) {
    void* d = dest;
    asm volatile(
        "cld\n"
        "rep stosl"
        : "+D"(d), "+c"(count)
        : "a"(val)
        : "memory"
    );
    return dest;
}

void* memcpy_rep(void* dest, const void* src, size_t count) {
    void* ret = dest;
    asm volatile(
        "cld\n"
        "rep movsb"
        : "+D"(dest), "+S"(src), "+c"(count)
        :
        : "memory"
    );
    return ret;
}

void* memmove_backward_rep(void* dest, const void* src, size_t count) {
    if (count == 0) {
        return dest;
    }

    void* ret = dest;
    uint8_t* d = static_cast<uint8_t*>(dest) + count - 1;
    const uint8_t* s = static_cast<const uint8_t*>(src) + count - 1;
    asm volatile(
        "std\n"
        "rep movsb\n"
        "cld"
        : "+D"(d), "+S"(s), "+c"(count)
        :
        : "memory"
    );
    return ret;
}

int memcmp_scalar(const void* b1, const void* b2, size_t count) {
    const uint8_t* a = static_cast<const uint8_t*>(b1);
    const uint8_t* b = static_cast<const uint8_t*>(b2);
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
    }
    return 0;
}

void* memset_sse2(void* dest, int val, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t byte = static_cast<uint8_t>(val);
    const uint64_t pattern64 = 0x0101010101010101ULL * byte;
    asm volatile(
        "movq %0, %%xmm0\n"
        "pshufd $0x44, %%xmm0, %%xmm0"
        :
        : "r"(pattern64)
        : "xmm0");

    while (count >= 64) {
        asm volatile(
            "movdqu %%xmm0, 0(%0)\n"
            "movdqu %%xmm0, 16(%0)\n"
            "movdqu %%xmm0, 32(%0)\n"
            "movdqu %%xmm0, 48(%0)"
            :
            : "r"(d)
            : "memory");
        d += 64;
        count -= 64;
    }
    while (count >= 16) {
        asm volatile("movdqu %%xmm0, (%0)" : : "r"(d) : "memory");
        d += 16;
        count -= 16;
    }
    memset_rep(d, val, count);
    return dest;
}

void* memset32_sse2(void* dest, uint32_t val, size_t count) {
    uint32_t* d = static_cast<uint32_t*>(dest);
    asm volatile(
        "movd %k0, %%xmm0\n"
        "pshufd $0, %%xmm0, %%xmm0"
        :
        : "r"(val)
        : "xmm0");

    while (count >= 16) {
        asm volatile(
            "movdqu %%xmm0, 0(%0)\n"
            "movdqu %%xmm0, 16(%0)\n"
            "movdqu %%xmm0, 32(%0)\n"
            "movdqu %%xmm0, 48(%0)"
            :
            : "r"(d)
            : "memory");
        d += 16;
        count -= 16;
    }
    while (count >= 4) {
        asm volatile("movdqu %%xmm0, (%0)" : : "r"(d) : "memory");
        d += 4;
        count -= 4;
    }
    memset32_rep(d, val, count);
    return dest;
}

void* memset32_avx2(void* dest, uint32_t val, size_t count) {
    uint32_t* d = static_cast<uint32_t*>(dest);
    asm volatile(
        "vmovd %k0, %%xmm0\n"
        "vpbroadcastd %%xmm0, %%ymm0"
        :
        : "r"(val)
        : "xmm0", "ymm0");

    while (count >= 32) {
        asm volatile(
            "vmovdqu %%ymm0, 0(%0)\n"
            "vmovdqu %%ymm0, 32(%0)\n"
            "vmovdqu %%ymm0, 64(%0)\n"
            "vmovdqu %%ymm0, 96(%0)"
            :
            : "r"(d)
            : "memory");
        d += 32;
        count -= 32;
    }

    asm volatile("vzeroupper" ::: "ymm0");
    memset32_sse2(d, val, count);
    return dest;
}

void* memcpy_sse2(void* dest, const void* src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    while (count >= 64) {
        asm volatile(
            "movdqu 0(%1), %%xmm0\n"
            "movdqu 16(%1), %%xmm1\n"
            "movdqu 32(%1), %%xmm2\n"
            "movdqu 48(%1), %%xmm3\n"
            "movdqu %%xmm0, 0(%0)\n"
            "movdqu %%xmm1, 16(%0)\n"
            "movdqu %%xmm2, 32(%0)\n"
            "movdqu %%xmm3, 48(%0)"
            :
            : "r"(d), "r"(s)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
        d += 64;
        s += 64;
        count -= 64;
    }
    while (count >= 16) {
        asm volatile(
            "movdqu (%1), %%xmm0\n"
            "movdqu %%xmm0, (%0)"
            :
            : "r"(d), "r"(s)
            : "xmm0", "memory");
        d += 16;
        s += 16;
        count -= 16;
    }
    memcpy_rep(d, s, count);
    return dest;
}

int memcmp_sse2(const void* b1, const void* b2, size_t count) {
    const uint8_t* a = static_cast<const uint8_t*>(b1);
    const uint8_t* b = static_cast<const uint8_t*>(b2);

    while (count >= 16) {
        uint32_t mask = 0;
        asm volatile(
            "movdqu (%1), %%xmm0\n"
            "movdqu (%2), %%xmm1\n"
            "pcmpeqb %%xmm1, %%xmm0\n"
            "pmovmskb %%xmm0, %0"
            : "=r"(mask)
            : "r"(a), "r"(b)
            : "xmm0", "xmm1", "memory");
        if (mask != 0xFFFFU) {
            break;
        }
        a += 16;
        b += 16;
        count -= 16;
    }

    return memcmp_scalar(a, b, count);
}

int memcmp_avx2(const void* b1, const void* b2, size_t count) {
    const uint8_t* a = static_cast<const uint8_t*>(b1);
    const uint8_t* b = static_cast<const uint8_t*>(b2);

    while (count >= 32) {
        uint32_t mask = 0;
        asm volatile(
            "vmovdqu (%1), %%ymm0\n"
            "vmovdqu (%2), %%ymm1\n"
            "vpcmpeqb %%ymm1, %%ymm0, %%ymm0\n"
            "vpmovmskb %%ymm0, %0"
            : "=r"(mask)
            : "r"(a), "r"(b)
            : "ymm0", "ymm1", "memory");
        if (mask != 0xFFFFFFFFU) {
            asm volatile("vzeroupper" ::: "ymm0", "ymm1");
            return memcmp_sse2(a, b, count);
        }
        a += 32;
        b += 32;
        count -= 32;
    }

    asm volatile("vzeroupper" ::: "ymm0", "ymm1");
    return memcmp_sse2(a, b, count);
}

void refreshAccelerationFlags() {
    const CPUFeatures& features = CPU::getFeatures();
    enableSSE2 = features.sse2;
    enableAVX2 = features.avx2 && features.avxEnabled;
    accelerationInitialized = true;
}
}

void* memset(void* dest, int val, size_t count) {
    if (accelerationInitialized && enableSSE2 && count >= SIMD_THRESHOLD) {
        return memset_sse2(dest, val, count);
    }
    return memset_rep(dest, val, count);
}

void* memset32(void* dest, uint32_t val, size_t count) {
    if (accelerationInitialized && enableAVX2 && count >= SIMD_THRESHOLD) {
        return memset32_avx2(dest, val, count);
    }
    if (accelerationInitialized && enableSSE2 && count >= SIMD_THRESHOLD) {
        return memset32_sse2(dest, val, count);
    }
    return memset32_rep(dest, val, count);
}

void* memcpy(void* dest, const void* src, size_t count) {
    if (accelerationInitialized && enableSSE2 && count >= SIMD_THRESHOLD) {
        return memcpy_sse2(dest, src, count);
    }
    return memcpy_rep(dest, src, count);
}

void* memmove(void* dest, const void* src, size_t count) {
    const uintptr_t d = reinterpret_cast<uintptr_t>(dest);
    const uintptr_t s = reinterpret_cast<uintptr_t>(src);
    if (d == s || count == 0) {
        return dest;
    }
    if (d < s || d >= s + count) {
        return memcpy(dest, src, count);
    }
    return memmove_backward_rep(dest, src, count);
}

int memcmp(const void* b1, const void* b2, size_t count) {
    if (accelerationInitialized && enableAVX2 && count >= SIMD_THRESHOLD) {
        return memcmp_avx2(b1, b2, count);
    }
    if (accelerationInitialized && enableSSE2 && count >= SIMD_THRESHOLD) {
        return memcmp_sse2(b1, b2, count);
    }
    return memcmp_scalar(b1, b2, count);
}

void* memchr(const void* b1, int val, size_t count) {
    if (count == 0) {
        return nullptr;
    }
    void* result;
    asm volatile(
        "cld\n"
        "repne scasb\n"
        "je 1f\n"
        "xorq %%rdi, %%rdi\n"
        "jmp 2f\n"
        "1: decq %%rdi\n"
        "2:"
        : "+D"(b1), "+c"(count), "=D"(result)
        : "a"((uint8_t)val)
        : "memory"
    );
    return result;
}

void* memrchr(const void* b1, int val, size_t count) {
    if (count == 0) {
        return nullptr;
    }
    const void* end = (const uint8_t*)b1 + count - 1;
    void* result;
    asm volatile(
        "std\n"
        "repne scasb\n"
        "je 1f\n"
        "xorq %%rdi, %%rdi\n"
        "jmp 2f\n"
        "1: incq %%rdi\n"
        "2: cld"
        : "+D"(end), "+c"(count), "=D"(result)
        : "a"((uint8_t)val)
        : "memory"
    );
    return result;
}

void memory_init_acceleration() {
    refreshAccelerationFlags();
}

bool memory_validate_acceleration() {
    alignas(32) uint8_t source[257];
    alignas(32) uint8_t dest[257];
    alignas(32) uint8_t expected[257];

    for (size_t i = 0; i < sizeof(source); ++i) {
        source[i] = static_cast<uint8_t>((i * 37U + 11U) & 0xFFU);
        dest[i] = 0;
        expected[i] = source[i];
    }

    memcpy(dest + 3, source + 5, 179);
    for (size_t i = 0; i < 179; ++i) {
        if (dest[i + 3] != source[i + 5]) {
            return false;
        }
    }

    for (size_t i = 0; i < sizeof(dest); ++i) {
        dest[i] = source[i];
        expected[i] = source[i];
    }
    memmove(dest + 9, dest, 173);
    for (size_t i = 173; i > 0; --i) {
        expected[9 + i - 1] = expected[i - 1];
    }
    if (memcmp(dest, expected, sizeof(dest)) != 0) {
        return false;
    }

    for (size_t i = 0; i < sizeof(dest); ++i) {
        dest[i] = source[i];
        expected[i] = source[i];
    }
    memmove(dest, dest + 7, 181);
    for (size_t i = 0; i < 181; ++i) {
        expected[i] = source[i + 7];
    }
    if (memcmp(dest, expected, sizeof(dest)) != 0) {
        return false;
    }

    memset(dest + 1, 0xA5, 191);
    for (size_t i = 1; i < 192; ++i) {
        if (dest[i] != 0xA5) {
            return false;
        }
    }

    alignas(32) uint32_t words[65];
    memset32(words + 1, 0xAABBCCDDU, 63);
    for (size_t i = 1; i < 64; ++i) {
        if (words[i] != 0xAABBCCDDU) {
            return false;
        }
    }

    dest[73] ^= 0x40;
    return memcmp(source, source, sizeof(source)) == 0 &&
           memcmp(source, dest, sizeof(source)) != 0;
}

size_t strlen(const char* str) {
    size_t len;
    asm volatile(
        "cld\n"
        "xorl %%eax, %%eax\n"
        "movq $-1, %%rcx\n"
        "repne scasb\n"
        "notq %%rcx\n"
        "decq %%rcx"
        : "=c"(len)
        : "D"(str)
        : "rax", "memory"
    );
    return len;
}

char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    asm volatile(
        "1:\n"
        "lodsb\n"
        "stosb\n"
        "testb %%al, %%al\n"
        "jnz 1b"
        : "+D"(dest), "+S"(src)
        :
        : "rax", "memory"
    );
    return ret;
}

char* strncpy(char* dest, const char* src, size_t n) {
    char* ret = dest;
    asm volatile(
        "1:\n"
        "testq %%rcx, %%rcx\n"
        "jz 3f\n"
        "lodsb\n"
        "stosb\n"
        "decq %%rcx\n"
        "testb %%al, %%al\n"
        "jnz 1b\n"
        "2:\n"
        "testq %%rcx, %%rcx\n"
        "jz 3f\n"
        "movb $0, (%%rdi)\n"
        "incq %%rdi\n"
        "decq %%rcx\n"
        "jmp 2b\n"
        "3:"
        : "+D"(dest), "+S"(src), "+c"(n)
        :
        : "rax", "memory"
    );
    return ret;
}

int strcmp(const char* s1, const char* s2) {
    int result;
    asm volatile(
        "1:\n"
        "movb (%%rdi), %%al\n"
        "movb (%%rsi), %%cl\n"
        "cmpb %%cl, %%al\n"
        "jne 2f\n"
        "testb %%al, %%al\n"
        "jz 3f\n"
        "incq %%rdi\n"
        "incq %%rsi\n"
        "jmp 1b\n"
        "2:\n"
        "movzbl %%al, %%eax\n"
        "movzbl %%cl, %%ecx\n"
        "subl %%ecx, %%eax\n"
        "jmp 4f\n"
        "3: xorl %%eax, %%eax\n"
        "4:"
        : "=a"(result), "+D"(s1), "+S"(s2)
        :
        : "rcx", "memory"
    );
    return result;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    int result;
    asm volatile(
        "1:\n"
        "testq %%rcx, %%rcx\n"
        "jz 3f\n"
        "movb (%%rdi), %%al\n"
        "movb (%%rsi), %%r8b\n"
        "cmpb %%r8b, %%al\n"
        "jne 2f\n"
        "testb %%al, %%al\n"
        "jz 3f\n"
        "incq %%rdi\n"
        "incq %%rsi\n"
        "decq %%rcx\n"
        "jmp 1b\n"
        "2:\n"
        "movzbl %%al, %%eax\n"
        "movzbl %%r8b, %%r8d\n"
        "subl %%r8d, %%eax\n"
        "jmp 4f\n"
        "3: xorl %%eax, %%eax\n"
        "4:"
        : "=a"(result), "+D"(s1), "+S"(s2), "+c"(n)
        :
        : "r8", "memory"
    );
    return result;
}
