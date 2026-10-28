; Application processor startup trampoline.
;
; SMP::startAPs() copies smpTrampolineStart..smpTrampolineEnd to a page below
; 1 MiB, fills smpTrampolineData, and sends INIT + Startup IPIs whose vector is
; that page number. The AP starts here in 16-bit real mode at CS:IP = page:0.
;
; The code is position independent: it derives its physical base from CS, keeps
; it in ebx/rbx, and patches the few absolute addresses (GDT base, far-jump
; targets) itself. Only label differences are used, so the object carries no
; relocations for this code.
;
;   real mode -> 32-bit protected mode (flat GDT below)
;             -> PAE + temporary identity page tables + EFER.LME (+NXE)
;             -> 64-bit long mode -> smpApEntry (kernel), which loads the
;                kernel CR3 and calls smpApMain(PerCPUData*).

section .text

global smpTrampolineStart
global smpTrampolineEnd
global smpTrampolineData

%define OFF(label) ((label) - smpTrampolineStart)

; Layout of smpTrampolineData (mirrored by TrampolineData in src/cpu/smp.cpp).
%define TD_PML4  0      ; phys of the temporary PML4 (must be < 4 GiB)
%define TD_EFER  8      ; bits OR-ed into IA32_EFER (LME, NXE)
%define TD_CR3   16     ; kernel CR3, loaded by smpApEntry
%define TD_STACK 24     ; kernel stack top for this AP
%define TD_ENTRY 32     ; smpApEntry
%define TD_CPU   40     ; PerCPUData* for this AP

align 16
bits 16
smpTrampolineStart:
    cli
    cld
    mov ax, cs
    mov ds, ax
    xor ebx, ebx
    mov bx, ax
    shl ebx, 4                          ; ebx = physical base of this page

    lea eax, [ebx + OFF(tramp_gdt)]
    mov [OFF(tramp_gdtr) + 2], eax
    lea eax, [ebx + OFF(pm32)]
    mov [OFF(pm32_jump)], eax
    lea eax, [ebx + OFF(lm64)]
    mov [OFF(lm64_jump)], eax

    lgdt [OFF(tramp_gdtr)]
    mov eax, cr0
    or eax, 1                           ; PE
    mov cr0, eax
    jmp dword far [OFF(pm32_jump)]

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax

    mov eax, [ebx + OFF(smpTrampolineData) + TD_PML4]
    mov cr3, eax

    mov ecx, 0xC0000080                 ; IA32_EFER
    rdmsr
    or eax, [ebx + OFF(smpTrampolineData) + TD_EFER]
    wrmsr

    mov eax, cr0
    or eax, 0x80000001                  ; PG | PE
    mov cr0, eax

    jmp far [ebx + OFF(lm64_jump)]

bits 64
lm64:
    mov ebx, ebx                        ; upper halves are undefined after the switch
    mov rcx, [rbx + OFF(smpTrampolineData) + TD_CPU]    ; arg 0 (Microsoft x64 ABI)
    mov rdx, [rbx + OFF(smpTrampolineData) + TD_CR3]
    mov rsp, [rbx + OFF(smpTrampolineData) + TD_STACK]
    mov rax, [rbx + OFF(smpTrampolineData) + TD_ENTRY]
    jmp rax

align 8
tramp_gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08: 32-bit code, flat
    dq 0x00CF92000000FFFF               ; 0x10: data, flat
    dq 0x00AF9A000000FFFF               ; 0x18: 64-bit code
tramp_gdt_end:

tramp_gdtr:
    dw tramp_gdt_end - tramp_gdt - 1
    dd 0                                ; patched: phys of tramp_gdt

pm32_jump:
    dd 0                                ; patched: phys of pm32
    dw 0x08

lm64_jump:
    dd 0                                ; patched: phys of lm64
    dw 0x18

align 8
smpTrampolineData:
    times 6 dq 0
smpTrampolineEnd:
