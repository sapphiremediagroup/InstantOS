#include <common/string.hpp>



void* memset(void* dest, int val, size_t count) {
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

void* memset32(void* dest, uint32_t val, size_t count) {
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

void* memcpy(void* dest, const void* src, size_t count) {
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

void* memmove(void* dest, const void* src, size_t count) {
    void* ret = dest;
    if (src >= dest) {
        asm volatile(
            "cld\n"
            "rep movsb"
            : "+D"(dest), "+S"(src), "+c"(count)
            :
            : "memory"
        );
    } else {
        asm volatile(
            "std\n"
            "rep movsb\n"
            "cld"
            : "+D"(dest), "+S"(src), "+c"(count)
            :
            : "memory"
        );
    }
    return ret;
}

int memcmp(const void* b1, const void* b2, size_t count) {
    int result;
    asm volatile(
        "cld\n"
        "repe cmpsb\n"
        "je 1f\n"
        "movzbl -1(%%rdi), %%eax\n"
        "movzbl -1(%%rsi), %%ecx\n"
        "subl %%ecx, %%eax\n"
        "jmp 2f\n"
        "1: xorl %%eax, %%eax\n"
        "2:"
        : "=a"(result), "+D"(b1), "+S"(b2), "+c"(count)
        :
        : "memory"
    );
    return result;
}

void* memchr(const void* b1, int val, size_t count) {
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
