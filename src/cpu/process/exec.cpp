#include <cpu/process/exec.hpp>
#include <common/elf.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <memory/heap.hpp>
#include <cpu/gdt/gdt.hpp>
#include <common/string.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>

extern "C" void processTrampoline();

namespace {
constexpr uint64_t USER_ELF_BASE = 0x0000400000000000ULL;
constexpr uint64_t USER_INTERP_BASE = 0x0000500000000000ULL;
constexpr uint64_t MAX_USER_ELF_SIZE = 32 * 1024 * 1024;
constexpr int MAX_ARG_ENV = 64;

uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool checkedAdd(uint64_t a, uint64_t b, uint64_t* out) {
    if (~0ULL - a < b) {
        return false;
    }
    *out = a + b;
    return true;
}

void assignUserProcessPriority(Process* proc, const char* path) {
    if (!proc || !path) {
        return;
    }

    if (strcmp(path, "/bin/graphics-compositor") == 0) {
        proc->setPriority(ProcessPriority::High);
    }
}

void freeBinaryBuffer(void* buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    const uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    PMM::FreeFrames(reinterpret_cast<uint64_t>(buffer), pages);
}

void* allocateBinaryBuffer(size_t size) {
    if (size == 0 || size > MAX_USER_ELF_SIZE) {
        return nullptr;
    }

    const uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    const uint64_t phys = PMM::AllocFrames(pages);
    if (!phys) {
        return nullptr;
    }

    return reinterpret_cast<void*>(phys);
}

bool copyIntoProcess(PageTable* pageTable, uint64_t dest, const void* source, size_t size) {
    const uint8_t* src = static_cast<const uint8_t*>(source);
    while (size > 0) {
        const uint64_t phys = VMM::VirtualToPhysicalIn(pageTable, dest);
        if (!phys) {
            return false;
        }

        const size_t pageOffset = static_cast<size_t>(dest & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - pageOffset;
        if (chunk > size) {
            chunk = size;
        }

        memcpy(reinterpret_cast<void*>(phys), src, chunk);
        src += chunk;
        dest += chunk;
        size -= chunk;
    }
    return true;
}

uint64_t pageFlagsFromElf(uint32_t flags) {
    uint64_t pageFlags = PageFlags::Present | PageFlags::UserSuper;
    if (flags & Elf::FlagWrite) {
        pageFlags |= PageFlags::ReadWrite;
    }
    if ((flags & Elf::FlagExecute) == 0) {
        pageFlags |= PageFlags::NoExecute;
    }
    return pageFlags;
}

struct LoadedElfImage {
    uint64_t base = 0;
    uint64_t entry = 0;
    uint64_t phdr = 0;
    uint64_t phent = 0;
    uint64_t phnum = 0;
    bool hasInterpreter = false;
    char interpreter[128] = {};
};

bool validateElfHeader(const void* image, size_t imageSize, const Elf::Header64** outHeader) {
    if (!image || imageSize < sizeof(Elf::Header64)) {
        return false;
    }

    const auto* header = static_cast<const Elf::Header64*>(image);
    if (header->ident[0] != Elf::Magic0 ||
        header->ident[1] != Elf::Magic1 ||
        header->ident[2] != Elf::Magic2 ||
        header->ident[3] != Elf::Magic3 ||
        header->ident[4] != Elf::Class64 ||
        header->ident[5] != Elf::DataLittleEndian ||
        header->ident[6] != Elf::VersionCurrent) {
        return false;
    }

    if (header->machine != Elf::MachineX86_64 ||
        header->version != Elf::VersionCurrent ||
        header->headerSize < sizeof(Elf::Header64) ||
        header->programHeaderEntrySize != sizeof(Elf::ProgramHeader64) ||
        header->programHeaderCount == 0) {
        return false;
    }

    if (header->type != Elf::TypeExecutable && header->type != Elf::TypeDynamic) {
        return false;
    }

    uint64_t phSize = 0;
    if (!checkedAdd(0, static_cast<uint64_t>(header->programHeaderEntrySize) * header->programHeaderCount, &phSize)) {
        return false;
    }

    uint64_t phEnd = 0;
    if (!checkedAdd(header->programHeaderOffset, phSize, &phEnd) || phEnd > imageSize) {
        return false;
    }

    *outHeader = header;
    return true;
}

bool loadElfImage(Process* proc, const void* image, size_t imageSize, uint64_t dynamicBase,
                  bool allowInterpreter, LoadedElfImage* loaded) {
    const Elf::Header64* header = nullptr;
    if (!proc || !loaded || !validateElfHeader(image, imageSize, &header)) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(image);
    const auto* phdrs = reinterpret_cast<const Elf::ProgramHeader64*>(bytes + header->programHeaderOffset);
    const uint64_t loadBias = header->type == Elf::TypeDynamic ? dynamicBase : 0;
    PageTable* pageTable = proc->getPageTable();
    uint64_t phdrAddress = 0;

    for (uint16_t i = 0; i < header->programHeaderCount; ++i) {
        const Elf::ProgramHeader64& ph = phdrs[i];

        if (ph.type == Elf::ProgramPhdr) {
            phdrAddress = loadBias + ph.virtualAddress;
        }

        if (ph.type == Elf::ProgramInterp) {
            if (!allowInterpreter || ph.fileSize == 0 || ph.fileSize >= sizeof(loaded->interpreter)) {
                return false;
            }

            uint64_t end = 0;
            if (!checkedAdd(ph.offset, ph.fileSize, &end) || end > imageSize) {
                return false;
            }

            memcpy(loaded->interpreter, bytes + ph.offset, ph.fileSize);
            loaded->interpreter[sizeof(loaded->interpreter) - 1] = '\0';
            loaded->hasInterpreter = true;
            continue;
        }

        if (ph.type != Elf::ProgramLoad) {
            continue;
        }

        if (ph.memorySize < ph.fileSize) {
            return false;
        }

        uint64_t fileEnd = 0;
        if (!checkedAdd(ph.offset, ph.fileSize, &fileEnd) || fileEnd > imageSize) {
            return false;
        }

        uint64_t segmentStart = 0;
        if (!checkedAdd(loadBias, ph.virtualAddress, &segmentStart)) {
            return false;
        }

        const uint64_t mappedStart = alignDown(segmentStart, PAGE_SIZE);
        const uint64_t pageOffset = segmentStart - mappedStart;
        const uint64_t mappedSize = alignUp(pageOffset + ph.memorySize, PAGE_SIZE);
        const uint64_t pages = mappedSize / PAGE_SIZE;
        if (pages == 0) {
            continue;
        }

        uint64_t phys = PMM::AllocFrames(pages);
        if (!phys) {
            return false;
        }

        memset(reinterpret_cast<void*>(phys), 0, mappedSize);
        if (ph.fileSize > 0) {
            memcpy(reinterpret_cast<void*>(phys + pageOffset), bytes + ph.offset, ph.fileSize);
        }

        VMM::MapRangeInto(pageTable, mappedStart, phys, pages, pageFlagsFromElf(ph.flags));
    }

    if (phdrAddress == 0) {
        phdrAddress = loadBias + header->programHeaderOffset;
    }

    loaded->base = loadBias;
    loaded->entry = loadBias + header->entry;
    loaded->phdr = phdrAddress;
    loaded->phent = header->programHeaderEntrySize;
    loaded->phnum = header->programHeaderCount;
    return true;
}

bool readWholeFile(const char* path, void** outBuffer, size_t* outSize) {
    if (!path || !outBuffer || !outSize) {
        return false;
    }

    *outBuffer = nullptr;
    *outSize = 0;

    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(path, 0, &fd);
    if (result != 0 || !fd) {
        return false;
    }

    FileStats stats;
    if (fd->getNode()->ops->stat(fd->getNode(), &stats) != 0 || stats.size == 0) {
        VFS::get().close(fd);
        return false;
    }

    void* buffer = allocateBinaryBuffer(stats.size);
    if (!buffer) {
        VFS::get().close(fd);
        return false;
    }

    const int64_t readBytes = VFS::get().read(fd, buffer, stats.size);
    VFS::get().close(fd);

    if (readBytes != static_cast<int64_t>(stats.size)) {
        freeBinaryBuffer(buffer, stats.size);
        return false;
    }

    *outBuffer = buffer;
    *outSize = stats.size;
    return true;
}

struct InitialStackInfo {
    uint64_t entry;
    uint64_t phdr;
    uint64_t phent;
    uint64_t phnum;
    uint64_t base;
};

bool setupInitialStack(Process* proc, const char* execPath, const InitialStackInfo& info,
                       int argc, const char** argv, int envc, const char** envp) {
    if (!proc || argc < 0 || argc > MAX_ARG_ENV || envc < 0 || envc > MAX_ARG_ENV) {
        return false;
    }

    PageTable* pageTable = proc->getPageTable();
    uint64_t stack = proc->getUserStackBase() + proc->getUserStackSize();
    uint64_t argvAddrs[MAX_ARG_ENV] = {};
    uint64_t envAddrs[MAX_ARG_ENV] = {};

    const char* path = execPath ? execPath : "";
    const size_t pathLen = strlen(path) + 1;
    stack -= pathLen;
    const uint64_t execFnAddress = stack;
    if (!copyIntoProcess(pageTable, execFnAddress, path, pathLen)) {
        return false;
    }

    for (int i = envc - 1; i >= 0; --i) {
        const char* value = envp && envp[i] ? envp[i] : "";
        const size_t len = strlen(value) + 1;
        stack -= len;
        envAddrs[i] = stack;
        if (!copyIntoProcess(pageTable, stack, value, len)) {
            return false;
        }
    }

    for (int i = argc - 1; i >= 0; --i) {
        const char* value = argv && argv[i] ? argv[i] : "";
        const size_t len = strlen(value) + 1;
        stack -= len;
        argvAddrs[i] = stack;
        if (!copyIntoProcess(pageTable, stack, value, len)) {
            return false;
        }
    }

    Elf::AuxEntry64 aux[] = {
        { Elf::AuxPhdr, info.phdr },
        { Elf::AuxPhent, info.phent },
        { Elf::AuxPhnum, info.phnum },
        { Elf::AuxEntry, info.entry },
        { Elf::AuxBase, info.base },
        { Elf::AuxPagesz, PAGE_SIZE },
        { Elf::AuxUid, proc->getUID() },
        { Elf::AuxEuid, proc->getUID() },
        { Elf::AuxGid, proc->getGID() },
        { Elf::AuxEgid, proc->getGID() },
        { Elf::AuxExecFn, execFnAddress },
        { Elf::AuxNull, 0 },
    };

    const size_t wordCount =
        1 + static_cast<size_t>(argc) + 1 +
        static_cast<size_t>(envc) + 1 +
        (sizeof(aux) / sizeof(aux[0])) * 2;
    const size_t tableSize = wordCount * sizeof(uint64_t);

    stack = alignDown(stack, 16);
    stack -= tableSize;
    stack = alignDown(stack, 16);

    uint64_t* table = new uint64_t[wordCount];
    if (!table) {
        return false;
    }

    size_t pos = 0;
    table[pos++] = static_cast<uint64_t>(argc);
    for (int i = 0; i < argc; ++i) {
        table[pos++] = argvAddrs[i];
    }
    table[pos++] = 0;
    for (int i = 0; i < envc; ++i) {
        table[pos++] = envAddrs[i];
    }
    table[pos++] = 0;
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
        table[pos++] = aux[i].type;
        table[pos++] = aux[i].value;
    }

    const bool ok = copyIntoProcess(pageTable, stack, table, tableSize);
    delete[] table;
    if (!ok) {
        return false;
    }

    proc->setUserStack(stack);

    uint64_t* userRspOnKernelStack = reinterpret_cast<uint64_t*>(proc->getContext()->rsp + 8);
    *userRspOnKernelStack = stack;
    return true;
}

void initializeTrampoline(Process* proc, uint64_t entry) {
    uint64_t kernelStack = proc->getKernelStack();
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = proc->getUserStack();
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = entry;

    proc->getContext()->rip = reinterpret_cast<uint64_t>(&processTrampoline);
    proc->getContext()->rsp = kernelStack;
    proc->getContext()->rbp = 0;
    proc->getContext()->rflags = 0x202;
}
}

Process* ProcessExecutor::createKernelProcess(void (*entry)()) {
    uint32_t pid = Scheduler::get().allocatePID();
    Process* proc = new Process(pid);
    proc->setName("<kernel>");

    uint64_t stack = proc->getKernelStack();
    stack &= ~0xFULL;
    stack -= 8;

    proc->getContext()->rip = reinterpret_cast<uint64_t>(entry);
    proc->getContext()->rsp = stack;
    proc->getContext()->rbp = 0;
    proc->getContext()->rflags = 0x202;

    return proc;
}

Process* ProcessExecutor::createUserProcess(uint64_t entry) {
    uint32_t pid = Scheduler::get().allocatePID();
    Process* proc = new Process(pid);
    proc->setName("<user>");

    uint64_t stack = proc->getUserStackBase() + proc->getUserStackSize();
    stack = alignDown(stack, 16);

    proc->getContext()->rip = entry;
    proc->getContext()->rsp = stack;
    proc->getContext()->rbp = 0;
    proc->getContext()->rflags = 0x202;

    return proc;
}

void ProcessExecutor::executeUserProcess(Process* proc, GDT* gdt) {
    proc->jumpToUsermode(proc->getContext()->rip, gdt);
}

Process* ProcessExecutor::createUserProcessWithCode(void* code, size_t codeSize) {
    const char* argv[] = { "<memory>", nullptr };
    return createUserProcessWithArgs(code, codeSize, 1, argv, 0, nullptr, "<memory>");
}

Process* ProcessExecutor::createUserProcessWithArgs(void* code, size_t codeSize, int argc, const char** argv,
                                                    int envc, const char** envp, const char* execPath) {
    uint32_t pid = Scheduler::get().allocatePID();
    Process* proc = new Process(pid);
    if (!proc || !proc->getPageTable()) {
        delete proc;
        return nullptr;
    }
    proc->setName("<elf>");

    LoadedElfImage mainImage;
    if (!loadElfImage(proc, code, codeSize, USER_ELF_BASE, true, &mainImage)) {
        Console::get().drawText("[PROC] invalid ELF image\n");
        delete proc;
        return nullptr;
    }

    uint64_t startEntry = mainImage.entry;
    uint64_t interpreterBase = 0;

    if (mainImage.hasInterpreter) {
        void* interpBuffer = nullptr;
        size_t interpSize = 0;
        if (!readWholeFile(mainImage.interpreter, &interpBuffer, &interpSize)) {
            Console::get().drawText("[PROC] failed to read ELF interpreter: ");
            Console::get().drawText(mainImage.interpreter);
            Console::get().drawText("\n");
            delete proc;
            return nullptr;
        }

        LoadedElfImage interpreter;
        if (!loadElfImage(proc, interpBuffer, interpSize, USER_INTERP_BASE, false, &interpreter)) {
            Console::get().drawText("[PROC] invalid ELF interpreter\n");
            freeBinaryBuffer(interpBuffer, interpSize);
            delete proc;
            return nullptr;
        }

        startEntry = interpreter.entry;
        interpreterBase = interpreter.base;
        freeBinaryBuffer(interpBuffer, interpSize);
    }

    initializeTrampoline(proc, startEntry);

    InitialStackInfo stackInfo {
        mainImage.entry,
        mainImage.phdr,
        mainImage.phent,
        mainImage.phnum,
        interpreterBase,
    };

    if (!setupInitialStack(proc, execPath, stackInfo, argc, argv, envc, envp)) {
        Console::get().drawText("[PROC] failed to build ELF initial stack\n");
        delete proc;
        return nullptr;
    }

    return proc;
}

Process* ProcessExecutor::loadUserBinary(const char* path) {
    const char* argv[] = { path, nullptr };
    return loadUserBinaryWithArgs(path, 1, argv);
}

Process* ProcessExecutor::loadUserBinaryWithArgs(const char* path, int argc, const char** argv,
                                                 int envc, const char** envp) {
    void* buffer = nullptr;
    size_t size = 0;
    if (!readWholeFile(path, &buffer, &size)) {
        Console::get().drawText("[PROC] failed to read user binary: ");
        Console::get().drawText(path ? path : "<null>");
        Console::get().drawText("\n");
        return nullptr;
    }

    const char* defaultArgv[] = { path, nullptr };
    if (argc == 0) {
        argc = 1;
        argv = defaultArgv;
    }

    Process* proc = createUserProcessWithArgs(buffer, size, argc, argv, envc, envp, path);
    if (proc) {
        proc->setName(path);
        assignUserProcessPriority(proc, path);
    } else {
        Console::get().drawText("[PROC] create ELF process failed path=");
        Console::get().drawText(path ? path : "<null>");
        Console::get().drawText(" size=");
        Console::get().drawNumber(static_cast<int64_t>(size));
        Console::get().drawText("\n");
    }

    freeBinaryBuffer(buffer, size);
    return proc;
}

void ProcessExecutor::setupArguments(Process* proc, int argc, const char** argv) {
    InitialStackInfo empty {};
    setupInitialStack(proc, proc ? proc->getName() : "", empty, argc, argv, 0, nullptr);
}
