section .text
extern exceptionHandler
extern irqHandler

%macro pushad 0
    push rbx
    push rdx
    push rcx
    push rax
    push rdi
    push rsi
    push rbp
    push r8
    push r9
    push r10
    push r11
%endmacro

%macro popad 0
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rsi
    pop rdi
    pop rax
    pop rcx
    pop rdx      
    pop rbx      
%endmacro

; Kernel GS invariant: inside the kernel the GS base is this CPU's PerCPUData.
; On entry from user mode (saved CS has RPL 3) swap it in; on exit swap the
; user's back only if the frame being returned to is a user frame. The check on
; exit reads the frame again because the scheduler may have rewritten it to
; resume a different context.
;   entry: [rsp+0] vector, [rsp+8] error code, [rsp+16] rip, [rsp+24] cs
;   exit (after add rsp, 16): [rsp+0] rip, [rsp+8] cs
handleISR:
    test qword [rsp + 24], 3
    jz .from_kernel
    swapgs
.from_kernel:
    pushad

    mov rcx, rsp

    mov rbp, rsp
    and rsp, ~0xF
    sub rsp, 32

    cld
    call exceptionHandler

    mov rsp, rbp

    popad
    add rsp, 16
    test qword [rsp + 8], 3
    jz .to_kernel
    swapgs
.to_kernel:
    iretq

%macro isrYErr 1
global isr%+%1
isr%+%1:
    push %1
    jmp handleISR
%endmacro

%macro isrNErr 1
global isr%+%1
isr%+%1:
    push 0
    push %1
    jmp handleISR
%endmacro

isrNErr 0
isrNErr 1
isrNErr 2
isrNErr 3
isrNErr 4
isrNErr 5
isrNErr 6
isrNErr 7
isrYErr 8
isrNErr 9
isrYErr 10
isrYErr 11
isrYErr 12
isrYErr 13
isrYErr 14
isrNErr 15
isrNErr 16
isrYErr 17
isrNErr 18
isrNErr 19
isrNErr 20
isrNErr 21
isrNErr 22
isrNErr 23
isrNErr 24
isrNErr 25
isrNErr 26
isrNErr 27
isrNErr 28
isrNErr 29
isrYErr 30
isrNErr 31

handleIRQ:
    test qword [rsp + 24], 3
    jz .from_kernel
    swapgs
.from_kernel:
    pushad

    mov rcx, rsp

    mov rbp, rsp
    and rsp, ~0xF
    sub rsp, 32

    cld
    call irqHandler

    mov rsp, rbp

    popad
    add rsp, 16
    test qword [rsp + 8], 3
    jz .to_kernel
    swapgs
.to_kernel:
    iretq

%assign i 32
%rep 	223
irq%+i:
    push 0
    push i
    jmp handleIRQ
    %assign i i+1
%endrep

global isrTable
isrTable:
%assign i 0 
%rep    32 
    DQ isr%+i 
    %assign i i+1 
%endrep

global irqTable
irqTable:
%assign i 32
%rep 	223
    DQ irq%+i
    %assign i i+1
%endrep