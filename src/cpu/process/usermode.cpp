#include <stdint.h>

extern "C" __attribute__((naked)) void enterUsermode(uint64_t entry, uint64_t stack) {
    asm volatile (
        ".intel_syntax noprefix\n"
        "cli\n"
        "mov r11, rdx\n"
        "mov ax, 0x1B\n"
        "mov ds, ax\n"
        "mov es, ax\n"
        "mov fs, ax\n"
        "mov gs, ax\n"
        "push 0x1B\n"
        "push r11\n"
        "pushfq\n"
        "pop rax\n"
        "or rax, 0x200\n"
        "push rax\n"
        "push 0x23\n"
        "push rcx\n"
        "iretq\n"
        ".att_syntax prefix\n"
    );
}
