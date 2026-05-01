#include <debug/diag.hpp>

#include <common/string.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/syscall.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>
#include <memory/heap.hpp>

namespace {

constexpr size_t kMaxKernelSymbols = 4096;
constexpr size_t kMaxSymbolName = 96;

struct KernelSymbol {
    uint64_t address;
    char name[kMaxSymbolName];
};

KernelSymbol gKernelSymbols[kMaxKernelSymbols];
size_t gKernelSymbolCount = 0;
bool gKernelSymbolsLoaded = false;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

uint8_t hexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
    return 0;
}

bool parseHex64(const char* text, uint64_t* value) {
    if (!text || !value) return false;

    uint64_t result = 0;
    size_t digits = 0;
    while (isHexDigit(text[digits])) {
        result = (result << 4) | hexValue(text[digits]);
        digits++;
    }

    if (digits == 0) {
        return false;
    }

    *value = result;
    return true;
}

void copyToken(char* dest, size_t destSize, const char* src, size_t len) {
    if (!dest || destSize == 0) return;

    size_t outLen = len;
    if (outLen >= destSize) {
        outLen = destSize - 1;
    }

    for (size_t i = 0; i < outLen; ++i) {
        dest[i] = src[i];
    }
    dest[outLen] = '\0';
}

void addKernelSymbol(uint64_t address, const char* name, size_t len) {
    if (!name || len == 0 || gKernelSymbolCount >= kMaxKernelSymbols) {
        return;
    }

    KernelSymbol& symbol = gKernelSymbols[gKernelSymbolCount++];
    symbol.address = address;
    copyToken(symbol.name, sizeof(symbol.name), name, len);
}

void parseKernelMap(char* data, size_t size) {
    gKernelSymbolCount = 0;

    size_t pos = 0;
    while (pos < size) {
        char* line = data + pos;
        size_t lineLen = 0;
        while (pos + lineLen < size && data[pos + lineLen] != '\n') {
            lineLen++;
        }

        if (pos + lineLen < size) {
            data[pos + lineLen] = '\0';
        }

        size_t idx = 0;
        while (idx < lineLen && isSpace(line[idx])) idx++;

        if (idx + 16 < lineLen && isHexDigit(line[idx])) {
            while (idx < lineLen && !isSpace(line[idx])) idx++;
            while (idx < lineLen && isSpace(line[idx])) idx++;

            const size_t nameStart = idx;
            while (idx < lineLen && !isSpace(line[idx])) idx++;
            const size_t nameLen = idx - nameStart;

            while (idx < lineLen && isSpace(line[idx])) idx++;

            uint64_t address = 0;
            if (nameLen > 0 && idx + 16 <= lineLen && parseHex64(line + idx, &address)) {
                addKernelSymbol(address, line + nameStart, nameLen);
            }
        }

        pos += lineLen;
        if (pos < size && data[pos] == '\n') {
            pos++;
        }
    }
}

void loadKernelMapFromPath(const char* path) {
    FileDescriptor* fd = nullptr;
    if (VFS::get().open(path, 0, &fd) != 0 || !fd) {
        return;
    }

    FileStats stats = {};
    if (fd->getNode()->ops->stat(fd->getNode(), &stats) != 0 || stats.size == 0) {
        VFS::get().close(fd);
        return;
    }

    char* buffer = reinterpret_cast<char*>(kmalloc(stats.size + 1));
    if (!buffer) {
        VFS::get().close(fd);
        return;
    }

    const int64_t bytesRead = VFS::get().read(fd, buffer, stats.size);
    VFS::get().close(fd);

    if (bytesRead != static_cast<int64_t>(stats.size)) {
        kfree(buffer);
        return;
    }

    buffer[stats.size] = '\0';
    parseKernelMap(buffer, stats.size);
    kfree(buffer);
}

void printHexField(const char* label, uint64_t value) {
    Console::get().drawText(label);
    Console::get().drawHex(value);
    Console::get().drawText("\n");
}

}

namespace Debug {

void initializeKernelSymbols() {
    if (gKernelSymbolsLoaded) {
        return;
    }

    gKernelSymbolsLoaded = true;
    loadKernelMapFromPath("/bin/kernel.map");
}

void panic(const char* reason) {
    Console::get().drawText("\033[2J");
    Console::get().drawText("KERNEL PANIC\n");
    if (reason) {
        Console::get().drawText(reason);
        Console::get().drawText("\n");
    }
    printCurrentProcessSummary();
    printCurrentProcessSyscall();
    asm volatile("cli");
    while (true) {
        asm volatile("hlt");
    }
}

void panicf(const char* reason, const char* detail) {
    Console::get().drawText("\033[2J");
    Console::get().drawText("KERNEL PANIC\n");
    if (reason) {
        Console::get().drawText(reason);
        Console::get().drawText("\n");
    }
    if (detail) {
        Console::get().drawText(detail);
        Console::get().drawText("\n");
    }
    printCurrentProcessSummary();
    printCurrentProcessSyscall();
    asm volatile("cli");
    while (true) {
        asm volatile("hlt");
    }
}

void assertFail(const char* expr, const char* file, int line, const char* func) {
    Console::get().drawText("\033[2J");
    Console::get().drawText("KERNEL ASSERTION FAILED\n");
    Console::get().drawText("expr: ");
    Console::get().drawText(expr ? expr : "<null>");
    Console::get().drawText("\nfile: ");
    Console::get().drawText(file ? file : "<null>");
    Console::get().drawText("\nline: ");
    Console::get().drawNumber(line);
    Console::get().drawText("\nfunc: ");
    Console::get().drawText(func ? func : "<null>");
    Console::get().drawText("\n");
    printCurrentProcessSummary();
    printCurrentProcessSyscall();
    asm volatile("cli");
    while (true) {
        asm volatile("hlt");
    }
}

const char* lookupSymbol(uint64_t address, uint64_t* symbolAddress) {
    if (!gKernelSymbolsLoaded) {
        initializeKernelSymbols();
    }

    const KernelSymbol* best = nullptr;
    for (size_t i = 0; i < gKernelSymbolCount; ++i) {
        const KernelSymbol& symbol = gKernelSymbols[i];
        if (symbol.address <= address && (!best || symbol.address > best->address)) {
            best = &symbol;
        }
    }

    if (best && symbolAddress) {
        *symbolAddress = best->address;
    }

    return best ? best->name : nullptr;
}

void printAddressSymbol(uint64_t address) {
    uint64_t symbolAddress = 0;
    const char* symbol = lookupSymbol(address, &symbolAddress);
    if (!symbol) {
        return;
    }

    Console::get().drawText(" <");
    Console::get().drawText(symbol);
    Console::get().drawText("+");
    Console::get().drawHex(address - symbolAddress);
    Console::get().drawText(">");
}

void printCurrentProcessSummary() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        Console::get().drawText("process: <none>\n");
        return;
    }

    Console::get().drawText("process: pid=");
    Console::get().drawNumber(current->getPID());
    Console::get().drawText(" name=");
    Console::get().drawText(current->getName());
    Console::get().drawText("\n");
}

const char* syscallName(uint64_t number) {
    switch (static_cast<SyscallNumber>(number)) {
        case SyscallNumber::OSInfo: return "OSInfo";
        case SyscallNumber::ProcInfo: return "ProcInfo";
        case SyscallNumber::Exit: return "Exit";
        case SyscallNumber::Write: return "Write";
        case SyscallNumber::Read: return "Read";
        case SyscallNumber::Open: return "Open";
        case SyscallNumber::Close: return "Close";
        case SyscallNumber::GetPID: return "GetPID";
        case SyscallNumber::Fork: return "Fork";
        case SyscallNumber::Exec: return "Exec";
        case SyscallNumber::Wait: return "Wait";
        case SyscallNumber::Kill: return "Kill";
        case SyscallNumber::Mmap: return "Mmap";
        case SyscallNumber::Munmap: return "Munmap";
        case SyscallNumber::Yield: return "Yield";
        case SyscallNumber::Sleep: return "Sleep";
        case SyscallNumber::GetTime: return "GetTime";
        case SyscallNumber::Clear: return "Clear";
        case SyscallNumber::FBInfo: return "FBInfo";
        case SyscallNumber::FBMap: return "FBMap";
        case SyscallNumber::Signal: return "Signal";
        case SyscallNumber::SigReturn: return "SigReturn";
        case SyscallNumber::Login: return "Login";
        case SyscallNumber::Logout: return "Logout";
        case SyscallNumber::GetUID: return "GetUID";
        case SyscallNumber::GetGID: return "GetGID";
        case SyscallNumber::SetUID: return "SetUID";
        case SyscallNumber::SetGID: return "SetGID";
        case SyscallNumber::GetSessionID: return "GetSessionID";
        case SyscallNumber::GetSessionInfo: return "GetSessionInfo";
        case SyscallNumber::Chdir: return "Chdir";
        case SyscallNumber::Getcwd: return "Getcwd";
        case SyscallNumber::Mkdir: return "Mkdir";
        case SyscallNumber::Rmdir: return "Rmdir";
        case SyscallNumber::Unlink: return "Unlink";
        case SyscallNumber::Stat: return "Stat";
        case SyscallNumber::Dup: return "Dup";
        case SyscallNumber::Dup2: return "Dup2";
        case SyscallNumber::Pipe: return "Pipe";
        case SyscallNumber::Getppid: return "Getppid";
        case SyscallNumber::Spawn: return "Spawn";
        case SyscallNumber::GetUserInfo: return "GetUserInfo";
        case SyscallNumber::Readdir: return "Readdir";
        case SyscallNumber::FBFlush: return "FBFlush";
        case SyscallNumber::SharedAlloc: return "SharedAlloc";
        case SyscallNumber::SharedMap: return "SharedMap";
        case SyscallNumber::SharedFree: return "SharedFree";
        case SyscallNumber::SurfaceCreate: return "SurfaceCreate";
        case SyscallNumber::SurfaceMap: return "SurfaceMap";
        case SyscallNumber::SurfaceCommit: return "SurfaceCommit";
        case SyscallNumber::SurfacePoll: return "SurfacePoll";
        case SyscallNumber::CompositorCreateWindow: return "CompositorCreateWindow";
        case SyscallNumber::WindowEventQueue: return "WindowEventQueue";
        case SyscallNumber::WindowSetTitle: return "WindowSetTitle";
        case SyscallNumber::WindowAttachSurface: return "WindowAttachSurface";
        case SyscallNumber::WindowList: return "WindowList";
        case SyscallNumber::WindowFocus: return "WindowFocus";
        case SyscallNumber::WindowMove: return "WindowMove";
        case SyscallNumber::WindowResize: return "WindowResize";
        case SyscallNumber::WindowControl: return "WindowControl";
        case SyscallNumber::QueueCreate: return "QueueCreate";
        case SyscallNumber::QueueSend: return "QueueSend";
        case SyscallNumber::QueueReceive: return "QueueReceive";
        case SyscallNumber::QueueReply: return "QueueReply";
        case SyscallNumber::QueueRequest: return "QueueRequest";
        case SyscallNumber::ServiceRegister: return "ServiceRegister";
        case SyscallNumber::ServiceConnect: return "ServiceConnect";
        case SyscallNumber::NetGetMAC: return "NetGetMAC";
        case SyscallNumber::NetSend: return "NetSend";
        case SyscallNumber::NetRecv: return "NetRecv";
        case SyscallNumber::NetLinkStatus: return "NetLinkStatus";
        case SyscallNumber::NetPing: return "NetPing";
        case SyscallNumber::NetProcessPackets: return "NetProcessPackets";
        case SyscallNumber::NetGetPingReply: return "NetGetPingReply";
        case SyscallNumber::ThreadCreate: return "ThreadCreate";
        case SyscallNumber::ThreadExit: return "ThreadExit";
        case SyscallNumber::ThreadJoin: return "ThreadJoin";
        case SyscallNumber::Seek: return "Seek";
        case SyscallNumber::GPUCapsetInfo: return "GPUCapsetInfo";
        case SyscallNumber::GPUCapset: return "GPUCapset";
        case SyscallNumber::GPUContextCreate: return "GPUContextCreate";
        case SyscallNumber::GPUContextDestroy: return "GPUContextDestroy";
        case SyscallNumber::GPUResourceCreate3D: return "GPUResourceCreate3D";
        case SyscallNumber::GPUResourceDestroy: return "GPUResourceDestroy";
        case SyscallNumber::GPUResourceAssignUUID: return "GPUResourceAssignUUID";
        case SyscallNumber::GPUSubmit3D: return "GPUSubmit3D";
        case SyscallNumber::GPUWaitFence: return "GPUWaitFence";
    }

    return "Unknown";
}

const char* handleTypeName(HandleType type) {
    switch (type) {
        case HandleType::None: return "None";
        case HandleType::File: return "File";
        case HandleType::Process: return "Process";
        case HandleType::Thread: return "Thread";
        case HandleType::Window: return "Window";
        case HandleType::Surface: return "Surface";
        case HandleType::EventQueue: return "EventQueue";
        case HandleType::Service: return "Service";
        case HandleType::Timer: return "Timer";
        case HandleType::SharedMemory: return "SharedMemory";
        case HandleType::GpuContext: return "GpuContext";
        case HandleType::Font: return "Font";
        case HandleType::Pipe: return "Pipe";
    }

    return "Unknown";
}

void printCurrentProcessSyscall() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return;
    }

    const SyscallTrace& trace = current->getSyscallTrace();
    if (!trace.active) {
        return;
    }

    Console::get().drawText("syscall: ");
    Console::get().drawText(syscallName(trace.number));
    Console::get().drawText(" (#");
    Console::get().drawNumber(trace.number);
    Console::get().drawText(")\n");

    printHexField("  arg1=", trace.arg1);
    printHexField("  arg2=", trace.arg2);
    printHexField("  arg3=", trace.arg3);
    printHexField("  arg4=", trace.arg4);
    printHexField("  arg5=", trace.arg5);

    HandleType handleType = HandleType::None;
    int slot = -1;
    if (HandleTable::decodeHandle(trace.arg1, &handleType, &slot)) {
        Console::get().drawText("  handle1=");
        Console::get().drawText(handleTypeName(handleType));
        Console::get().drawText(" slot=");
        Console::get().drawNumber(slot);
        Console::get().drawText("\n");
    }
}

void printPageFaultReason(uint64_t errCode) {
    Console::get().drawText("\n\t- PF Reason:");
    if (errCode & 0x1) Console::get().drawText(" protection");
    else Console::get().drawText(" not-present");
    if (errCode & 0x2) Console::get().drawText(" write");
    else Console::get().drawText(" read");
    if (errCode & 0x4) Console::get().drawText(" user");
    else Console::get().drawText(" kernel");
    if (errCode & 0x8) Console::get().drawText(" reserved-bit");
    if (errCode & 0x10) Console::get().drawText(" instruction-fetch");
    if (errCode & 0x20) Console::get().drawText(" protection-key");
    if (errCode & 0x40) Console::get().drawText(" shadow-stack");
    if (errCode & 0x8000) Console::get().drawText(" sgx");
}

void beginSyscallTrace(Process* process, uint64_t syscallNumber, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (!process) {
        return;
    }

    SyscallTrace& trace = process->getSyscallTrace();
    trace.active = true;
    trace.number = syscallNumber;
    trace.arg1 = arg1;
    trace.arg2 = arg2;
    trace.arg3 = arg3;
    trace.arg4 = arg4;
    trace.arg5 = arg5;
}

void endSyscallTrace(Process* process) {
    if (!process) {
        return;
    }

    process->getSyscallTrace().active = false;
}

}
