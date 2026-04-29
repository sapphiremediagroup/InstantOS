#include <cpu/syscall/syscall.hpp>
#include <cpu/gdt/gdt.hpp>
#include <cpu/percpu.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <debug/diag.hpp>
#include <memory/vmm.hpp>
#include <common/ports.hpp>
#include <cpuid.h>
extern "C" void atexit(){
    return;
}
constexpr uint32_t MSR_STAR   = 0xC0000081;
constexpr uint32_t MSR_EFER   = 0xC0000080;
constexpr uint32_t MSR_LSTAR  = 0xC0000082;
constexpr uint32_t MSR_SFMASK = 0xC0000084;

Syscall syscallInstance;

extern "C" uint64_t userRSP = 0;
extern "C" uint64_t kernelStackTop = 0;

extern "C" void saveSyscallState(uint64_t* stack);

extern "C" __attribute__((naked)) void syscallEntry()
{
    asm volatile (
        ".intel_syntax noprefix\n\t"

        "swapgs\n\t"
        "mov gs:[8], rsp\n\t"
        "mov rsp, gs:[0]\n\t"

        "push rax\n\t"
        "push rbx\n\t"
        "push rcx\n\t"
        "push rdx\n\t"
        "push rsi\n\t"
        "push rdi\n\t"
        "push rbp\n\t"
        "push r8\n\t"
        "push r9\n\t"
        "push r10\n\t"
        "push r11\n\t"
        "push r12\n\t"
        "push r13\n\t"
        "push r14\n\t"
        "push r15\n\t"

        "mov rbp, rsp\n\t"
        "mov rcx, rbp\n\t"
        "sub rsp, 0x28\n\t"
        "call saveSyscallState\n\t"
        "add rsp, 0x28\n\t"

        "mov rcx, [rbp + 14*8]\n\t"
        "mov rdx, [rbp + 13*8]\n\t"
        "mov r8,  [rbp + 5*8]\n\t"
        "mov r9,  [rbp + 11*8]\n\t"
        "sub rsp, 0x38\n\t"
        "mov rax, [rbp + 7*8]\n\t"
        "mov [rsp + 0x20], rax\n\t"
        "mov rax, [rbp + 6*8]\n\t"
        "mov [rsp + 0x28], rax\n\t"
        "call syscallHandler\n\t"
        "add rsp, 0x38\n\t"

        "mov [rbp + 14*8], rax\n\t"
        "mov rsp, rbp\n\t"

        "pop r15\n\t"
        "pop r14\n\t"
        "pop r13\n\t"
        "pop r12\n\t"
        "pop r11\n\t"
        "pop r10\n\t"
        "pop r9\n\t"
        "pop r8\n\t"
        "pop rbp\n\t"
        "pop rdi\n\t"
        "pop rsi\n\t"
        "pop rdx\n\t"
        "pop rcx\n\t"
        "pop rbx\n\t"
        "pop rax\n\t"

        "mov rsp, gs:[8]\n\t"
        "swapgs\n\t"
        "sysretq\n\t"
        ".att_syntax prefix\n\t"
    );
}

bool hasSyscall(){
    uint32_t eax = 0x80000001, ebx = 0, ecx = 0, edx = 0;
    cpuid(&eax, &ebx, &ecx, &edx);
    return (edx >> 11) & 1;
}

Syscall& Syscall::get() {
    return syscallInstance;
}

void Syscall::initialize() {
    if (initialized) return;
    initialized = true;

    if(!hasSyscall()){
        asm volatile("cli");
        while(1);
    }

    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 0x1);

    uint64_t star = (static_cast<uint64_t>(0x08) << 32) |
                    (static_cast<uint64_t>(0x10) << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscallEntry));

    wrmsr(MSR_SFMASK, 0x200);

    initPerCPU(kernelStackTop);
}

void Syscall::setKernelStack(uint64_t stack) {
    kernelStackTop = stack;
    getPerCPU()->kernelStack = stack;
    GDT::get().setKernelStack(stack);
}

uint64_t Syscall::handle(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    switch ((SyscallNumber)syscall_num) {
        using enum SyscallNumber;
        case OSInfo:
            return sys_osinfo(arg1);
        case ProcInfo:
            return 0;
        case Exit:
            return sys_exit(arg1);
        case Write:
            return sys_write(arg1, arg2, arg3);
        case Read:
            return sys_read(arg1, arg2, arg3);
        case Open:
            return sys_open(arg1, arg2, arg3);
        case Close:
            return sys_close(arg1);
        case GetPID:
            return sys_getpid();
        case Fork:
            return sys_fork();
        case Exec:
            return sys_exec(arg1, arg2, arg3);
        case Wait:
            return sys_wait(arg1, arg2);
        case Kill:
            return sys_kill(arg1, arg2);
        case Mmap:
            return sys_mmap(arg1, arg2, arg3);
        case Munmap:
            return sys_munmap(arg1, arg2);
        case Yield:
            return sys_yield();
        case Sleep:
            return sys_sleep(arg1);
        case GetTime:
            return sys_gettime();
        case Clear:
            return sys_clear();
        case FBInfo:
            return sys_fb_info(arg1);
        case FBMap:
            return sys_fb_map();
        case Signal:
            return sys_signal(arg1, arg2);
        case SigReturn:
            return sys_sigreturn();
        case Login:
            return sys_login(arg1);
        case Logout:
            return sys_logout(arg1);
        case GetUID:
            return sys_getuid();
        case GetGID:
            return sys_getgid();
        case SetUID:
            return sys_setuid(arg1);
        case SetGID:
            return sys_setgid(arg1);
        case GetSessionID:
            return sys_getsessionid();
        case GetSessionInfo:
            return sys_getsessioninfo(arg1, arg2);
        case Chdir:
            return sys_chdir(arg1);
        case Getcwd:
            return sys_getcwd(arg1, arg2);
        case Mkdir:
            return sys_mkdir(arg1, arg2);
        case Rmdir:
            return sys_rmdir(arg1);
        case Unlink:
            return sys_unlink(arg1);
        case Stat:
            return sys_stat(arg1, arg2);
        case Dup:
            return sys_dup(arg1);
        case Dup2:
            return sys_dup2(arg1, arg2);
        case Pipe:
            return sys_pipe(arg1);
        case Getppid:
            return sys_getppid();
        case Spawn:
            return sys_spawn(arg1, arg2, arg3);
        case GetUserInfo:
            return sys_getuserinfo(arg1, arg2);
        case Readdir:
            return sys_readdir(arg1, arg2, arg3);
        case FBFlush:
            return sys_fb_flush(arg1, arg2, arg3, arg4);
        case SharedAlloc:
            return sys_shared_alloc(arg1);
        case SharedMap:
            return sys_shared_map(arg1);
        case SharedFree:
            return sys_shared_free(arg1);
        case SurfaceCreate:
            return sys_surface_create(arg1, arg2, arg3);
        case SurfaceMap:
            return sys_surface_map(arg1);
        case SurfaceCommit:
            return sys_surface_commit(arg1, arg2, arg3, arg4);
        case SurfacePoll:
            return sys_surface_poll(arg1);
        case CompositorCreateWindow:
            return sys_compositor_create_window(arg1, arg2, arg3, arg4);
        case WindowEventQueue:
            return sys_window_event_queue(arg1);
        case WindowSetTitle:
            return sys_window_set_title(arg1, arg2);
        case WindowAttachSurface:
            return sys_window_attach_surface(arg1, arg2);
        case WindowList:
            return sys_window_list(arg1, arg2);
        case WindowFocus:
            return sys_window_focus(arg1);
        case WindowMove:
            return sys_window_move(arg1, arg2, arg3);
        case WindowResize:
            return sys_window_resize(arg1, arg2, arg3);
        case WindowControl:
            return sys_window_control(arg1, arg2);
        case QueueCreate:
            return sys_queue_create();
        case QueueSend:
            return sys_queue_send(arg1, arg2, arg3);
        case QueueReceive:
            return sys_queue_receive(arg1, arg2, arg3);
        case QueueReply:
            return sys_queue_reply(arg1, arg2, arg3, arg4);
        case QueueRequest:
            return sys_queue_request(arg1, arg2, arg3, arg4, arg5);
        case ServiceRegister:
            return sys_service_register(arg1, arg2);
        case ServiceConnect:
            return sys_service_connect(arg1);
        case ThreadCreate:
            return sys_thread_create(arg1, arg2, arg3);
        case ThreadExit:
            return sys_thread_exit(arg1);
        case ThreadJoin:
            return sys_thread_join(arg1, arg2);
        default:
            return (uint64_t)-1;
    }
}

bool Syscall::isValidUserPointer(uint64_t ptr, size_t size) {
    if (size == 0) {
        return ptr < 0x0000800000000000ULL;
    }
    if (ptr >= 0xFFFF800000000000) {
        return false;
    }
    if (ptr == 0) {
        return false;
    }

    if (ptr + size < ptr) {
        return false;
    }
    if (ptr + size >= 0x0000800000000000) {
        return false;
    }

    uint64_t firstPage = ptr & ~0xFFFULL;
    uint64_t lastPage = (ptr + size - 1) & ~0xFFFULL;
    for (uint64_t page = firstPage;; page += PAGE_SIZE) {
        if (!VMM::IsUserMapped(page)) {
            return false;
        }
        if (page == lastPage) {
            break;
        }
    }

    return true;
}

bool Syscall::copyFromUser(void* dest, uint64_t src, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!dest || !isValidUserPointer(src, size)) {
        return false;
    }

    auto* out = reinterpret_cast<uint8_t*>(dest);
    const auto* in = reinterpret_cast<const uint8_t*>(src);
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }
    return true;
}

bool Syscall::copyToUser(uint64_t dest, const void* src, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!src || !isValidUserPointer(dest, size)) {
        return false;
    }

    auto* out = reinterpret_cast<uint8_t*>(dest);
    const auto* in = reinterpret_cast<const uint8_t*>(src);
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }
    return true;
}

bool Syscall::copyStringFromUser(uint64_t ptr, char* dest, size_t destSize) {
    if (!dest || destSize == 0) {
        return false;
    }

    for (size_t i = 0; i < destSize; i++) {
        char c = 0;
        if (!copyFromUser(&c, ptr + i, 1)) {
            dest[0] = '\0';
            return false;
        }

        dest[i] = c;
        if (c == '\0') {
            return i > 0;
        }
    }

    dest[destSize - 1] = '\0';
    return false;
}

bool Syscall::copyUserString(uint64_t ptr, char* dest, size_t destSize) {
    return copyStringFromUser(ptr, dest, destSize);
}

extern "C" uint64_t syscallHandler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    Process* current = Scheduler::get().getCurrentProcess();
    Debug::beginSyscallTrace(current, syscall_num, arg1, arg2, arg3, arg4, arg5);
    uint64_t result = Syscall::get().handle(syscall_num, arg1, arg2, arg3, arg4, arg5);
    Debug::endSyscallTrace(current);
    return result;
}
