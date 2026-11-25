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
// A sanity bound on the file only: exec reads the headers and the PT_LOAD
// bytes, never the rest, so debug info costs nothing. gcc's bootstrap builds
// stage2 with -gtoggle (debug on), and its cc1/cc1plus are 280-300 MiB (P7.2).
// What a non-PIE image may map is bounded separately (kExecImageEnd).
constexpr uint64_t MAX_USER_ELF_SIZE = 2ULL * 1024 * 1024 * 1024;
// A non-PIE (ET_EXEC) image is mapped where its headers say; one placed in the
// low identity map (gcc's cc1 at 0x400000) must end below the window kernel
// memory stays out of.
constexpr uint64_t kExecImageEnd = PMM::KERNEL_HIGH_ALLOC_MIN;
constexpr int MAX_ARG_ENV = ProcessExecutor::kMaxArgEnv;
// Strings and pointer tables must leave the program this much of its stack.
constexpr uint64_t kMinStackLeft = 32 * 1024;
constexpr int MAX_SHEBANG_DEPTH = 4;   // guard against #! interpreter loops
constexpr size_t SHEBANG_LINE_MAX = 255;

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

// Where an ELF image's bytes come from: a buffer in memory (a kernel pointer
// through the direct map) or an open file. Images are read from the file piece
// by piece, straight into the frames that will hold them: cc1 is 36 MB, and
// after hours of a build physical memory has no contiguous run that large
// for a whole-file buffer (exec then failed with "invalid ELF image").
struct ElfSource {
    const uint8_t* mem = nullptr;
    FileDescriptor* fd = nullptr;
    uint64_t size = 0;

    bool read(uint64_t offset, void* dst, uint64_t len) const {
        uint64_t end = 0;
        if (!checkedAdd(offset, len, &end) || end > size) {
            return false;
        }
        if (mem) {
            memcpy(dst, mem + offset, len);
            return true;
        }
        uint8_t* out = static_cast<uint8_t*>(dst);
        fd->setOffset(offset);
        while (len > 0) {
            const int64_t got = VFS::get().read(fd, out, len);
            if (got <= 0) {
                return false;
            }
            out += got;
            len -= static_cast<uint64_t>(got);
        }
        return true;
    }
};

// Opens `path` as an image source. The caller closes src->fd.
bool openElfSource(const char* path, ElfSource* src) {
    FileDescriptor* fd = nullptr;
    if (VFS::get().open(path, 0, &fd) != 0 || !fd) {
        return false;
    }
    FileStats stats;
    if (fd->getNode()->ops->stat(fd->getNode(), &stats) != 0 || stats.size == 0 ||
        stats.size > MAX_USER_ELF_SIZE) {
        VFS::get().close(fd);
        return false;
    }
    src->mem = nullptr;
    src->fd = fd;
    src->size = stats.size;
    return true;
}

void closeElfSource(ElfSource* src) {
    if (src->fd) {
        VFS::get().close(src->fd);
        src->fd = nullptr;
    }
}

bool copyIntoProcess(PageTable* pageTable, uint64_t dest, const void* source, size_t size) {
    if (size == 0) {
        return true;
    }
    // Every destination page must already be present in the target address space.
    for (uint64_t va = dest & ~0xFFFULL; va < dest + size; va += PAGE_SIZE) {
        if (!VMM::VirtualToPhysicalIn(pageTable, va)) {
            return false;
        }
    }
    // Write through the destination's own virtual address with the target address
    // space active, NOT through the low identity map of the backing physical
    // frame: an unrelated low user mapping (e.g. another non-PIE image loaded at
    // 0x400000) can shadow that identity VA in whatever page table is currently
    // active, so an identity write would land on the wrong, read-only page.
    uint64_t savedCr3;
    asm volatile("mov %%cr3, %0" : "=r"(savedCr3));
    VMM::SetAddressSpace(pageTable);
    memcpy(reinterpret_cast<void*>(dest), source, size);
    asm volatile("mov %0, %%cr3" : : "r"(savedCr3) : "memory");
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

// Frames are allocated in runs of up to this many pages, a page at a time when
// no run that long is free, so a large segment needs no contiguous memory.
constexpr uint64_t kSegmentRunPages = 256;

bool loadElfImage(Process* proc, const ElfSource& src, uint64_t dynamicBase,
                  bool allowInterpreter, LoadedElfImage* loaded) {
    if (!proc || !loaded) {
        return false;
    }

    // Everything is written through the higher-half direct map, never a frame's
    // low identity address: this runs in the calling process's address space,
    // which may be a non-PIE (ET_EXEC) image mapped low (0x400000) that shadows
    // those identity addresses (seen as a spurious "invalid ELF interpreter"
    // when gcc exec'd cc1).
    Elf::Header64 headerCopy;
    if (!src.read(0, &headerCopy, sizeof(headerCopy))) {
        return false;
    }
    const Elf::Header64* header = nullptr;
    if (!validateElfHeader(&headerCopy, src.size, &header)) {
        return false;
    }

    const uint16_t phCount = header->programHeaderCount;
    Elf::ProgramHeader64* phdrs = new Elf::ProgramHeader64[phCount];
    if (!phdrs) {
        return false;
    }
    struct FreePhdrs {
        Elf::ProgramHeader64* p;
        ~FreePhdrs() { delete[] p; }
    } freePhdrs{phdrs};
    if (!src.read(header->programHeaderOffset, phdrs, sizeof(Elf::ProgramHeader64) * phCount)) {
        return false;
    }

    const uint64_t loadBias = header->type == Elf::TypeDynamic ? dynamicBase : 0;
    PageTable* pageTable = proc->getPageTable();
    uint64_t phdrAddress = 0;

    for (uint16_t i = 0; i < phCount; ++i) {
        const Elf::ProgramHeader64& ph = phdrs[i];

        if (ph.type == Elf::ProgramPhdr) {
            phdrAddress = loadBias + ph.virtualAddress;
        }

        if (ph.type == Elf::ProgramInterp) {
            if (!allowInterpreter || ph.fileSize == 0 || ph.fileSize >= sizeof(loaded->interpreter)) {
                return false;
            }
            if (!src.read(ph.offset, loaded->interpreter, ph.fileSize)) {
                return false;
            }
            loaded->interpreter[ph.fileSize] = '\0';
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
        if (!checkedAdd(ph.offset, ph.fileSize, &fileEnd) || fileEnd > src.size) {
            return false;
        }

        uint64_t segmentStart = 0;
        if (!checkedAdd(loadBias, ph.virtualAddress, &segmentStart)) {
            return false;
        }

        uint64_t segmentEnd = 0;
        if (!checkedAdd(segmentStart, ph.memorySize, &segmentEnd)) {
            return false;
        }
        // Only an image placed in the window can run into its end; the native
        // userland's ET_EXEC programs are linked far above it.
        if (header->type != Elf::TypeDynamic && segmentStart < kExecImageEnd &&
            segmentEnd > kExecImageEnd) {
            Console::get().drawText("[PROC] non-PIE image reaches past the low-memory window\n");
            return false;
        }

        const uint64_t mappedStart = alignDown(segmentStart, PAGE_SIZE);
        const uint64_t pageOffset = segmentStart - mappedStart;
        const uint64_t pages = alignUp(pageOffset + ph.memorySize, PAGE_SIZE) / PAGE_SIZE;
        const uint64_t elfFlags = pageFlagsFromElf(ph.flags);
        // The file's bytes land at [fileStart, fileStop) in the mapping.
        const uint64_t fileStart = pageOffset;
        const uint64_t fileStop = pageOffset + ph.fileSize;

        for (uint64_t done = 0; done < pages;) {
            uint64_t run = pages - done < kSegmentRunPages ? pages - done : kSegmentRunPages;
            uint64_t phys = run > 1 ? PMM::AllocFrames(run) : 0;
            if (!phys) {
                run = 1;
                phys = PMM::AllocFrame();
                if (!phys) {
                    return false;   // frames mapped so far go with the process
                }
            }

            const uint64_t runVa = mappedStart + done * PAGE_SIZE;
            VMM::MapRangeInto(pageTable, runVa, phys, run, elfFlags | PageFlags::ReadWrite);

            uint8_t* dst = reinterpret_cast<uint8_t*>(VMM::PhysToVirt(phys));
            const uint64_t runLo = done * PAGE_SIZE;
            const uint64_t runHi = runLo + run * PAGE_SIZE;
            memset(dst, 0, run * PAGE_SIZE);
            const uint64_t lo = fileStart > runLo ? fileStart : runLo;
            const uint64_t hi = fileStop < runHi ? fileStop : runHi;
            if (lo < hi && !src.read(ph.offset + (lo - fileStart), dst + (lo - runLo), hi - lo)) {
                return false;
            }

            // Re-apply the real protection: drop write for read-only segments.
            if (!(ph.flags & Elf::FlagWrite)) {
                VMM::MapRangeInto(pageTable, runVa, phys, run, elfFlags);
            }
            done += run;
        }
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

char* dupString(const char* s) {
    if (!s) return nullptr;
    const size_t len = strlen(s);
    char* copy = new char[len + 1];
    if (!copy) return nullptr;
    memcpy(copy, s, len + 1);
    return copy;
}

// Parse a "#!interp [arg]" first line. Returns false if `buffer` is not a
// script. On success copies the interpreter path into interp[] and the single
// optional argument into arg[] (arg[0]=='\0' when there is none). Matches the
// common convention: everything after the interpreter (up to end-of-line) is a
// single argument, with surrounding whitespace trimmed.
bool parseShebang(const void* buffer, size_t size, char* interp, size_t interpSize,
                  char* arg, size_t argSize) {
    const char* p = static_cast<const char*>(buffer);
    if (size < 2 || p[0] != '#' || p[1] != '!') {
        return false;
    }

    const size_t cap = size < SHEBANG_LINE_MAX ? size : SHEBANG_LINE_MAX;
    size_t lineLen = 0;
    while (lineLen < cap && p[lineLen] != '\n') {
        lineLen++;
    }

    size_t i = 2; // skip "#!"
    while (i < lineLen && (p[i] == ' ' || p[i] == '\t')) i++;

    size_t j = 0;
    while (i < lineLen && p[i] != ' ' && p[i] != '\t' && p[i] != '\r') {
        if (j + 1 >= interpSize) return false; // interpreter path too long
        interp[j++] = p[i++];
    }
    interp[j] = '\0';
    if (j == 0) return false; // empty interpreter

    while (i < lineLen && (p[i] == ' ' || p[i] == '\t')) i++;
    size_t a = 0;
    while (i < lineLen && p[i] != '\r' && p[i] != '\n') {
        if (a + 1 >= argSize) break; // truncate an over-long argument
        arg[a++] = p[i++];
    }
    while (a > 0 && (arg[a - 1] == ' ' || arg[a - 1] == '\t')) a--;
    arg[a] = '\0';
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

    const char* path = execPath ? execPath : "";
    const size_t pathLen = strlen(path) + 1;

    // Everything below must fit in the user stack with room to run.
    size_t needed = pathLen + 64 + static_cast<size_t>(argc + envc + 32) * sizeof(uint64_t) * 2;
    for (int i = 0; i < envc; ++i) needed += (envp && envp[i] ? strlen(envp[i]) : 0) + 1;
    for (int i = 0; i < argc; ++i) needed += (argv && argv[i] ? strlen(argv[i]) : 0) + 1;
    if (needed > ProcessExecutor::kMaxArgBytes) {
        Console::get().drawText("[PROC] exec refused: argv + envp over 2 MiB\n");
        return false;
    }
    // Beyond the mapped top of the stack the strings go into its demand-paged
    // reserve (Process ctor); copyIntoProcess needs those pages present.
    const uint64_t lowest = alignDown(stack - needed - kMinStackLeft, PAGE_SIZE);
    for (uint64_t va = lowest; va < proc->getUserStackBase(); va += PAGE_SIZE) {
        if (!proc->handleDemandFault(va)) {
            Console::get().drawText("[PROC] exec refused: no stack for argv + envp\n");
            return false;
        }
    }

    // On the heap: the kernel stack is 16 KiB.
    uint64_t* argvAddrs = new uint64_t[argc + envc + 1];
    if (!argvAddrs) {
        return false;
    }
    uint64_t* envAddrs = argvAddrs + argc;
    struct FreeAddrs {
        uint64_t* p;
        ~FreeAddrs() { delete[] p; }
    } freeAddrs{argvAddrs};

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

namespace {
Process* createProcessFromSource(const ElfSource& image, int argc, const char** argv,
                                 int envc, const char** envp, const char* execPath) {
    uint32_t pid = Scheduler::get().allocatePID();
    Process* proc = new Process(pid);
    if (!proc || !proc->getPageTable()) {
        delete proc;
        return nullptr;
    }
    proc->setName("<elf>");

    LoadedElfImage mainImage;
    if (!loadElfImage(proc, image, USER_ELF_BASE, true, &mainImage)) {
        Console::get().drawText("[PROC] invalid ELF image or out of memory (free MiB: ");
        Console::get().drawNumber(static_cast<int64_t>(PMM::FreeMemory() / (1024 * 1024)));
        Console::get().drawText(")\n");
        delete proc;
        return nullptr;
    }

    uint64_t startEntry = mainImage.entry;
    uint64_t interpreterBase = 0;

    if (mainImage.hasInterpreter) {
        ElfSource interpSource;
        if (!openElfSource(mainImage.interpreter, &interpSource)) {
            Console::get().drawText("[PROC] failed to read ELF interpreter: ");
            Console::get().drawText(mainImage.interpreter);
            Console::get().drawText("\n");
            delete proc;
            return nullptr;
        }

        LoadedElfImage interpreter;
        const bool ok = loadElfImage(proc, interpSource, USER_INTERP_BASE, false, &interpreter);
        closeElfSource(&interpSource);
        if (!ok) {
            Console::get().drawText("[PROC] invalid ELF interpreter or out of memory\n");
            delete proc;
            return nullptr;
        }

        startEntry = interpreter.entry;
        interpreterBase = interpreter.base;
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
}

// `code` is a physical (identity) address, read through the direct map.
Process* ProcessExecutor::createUserProcessWithArgs(void* code, size_t codeSize, int argc, const char** argv,
                                                    int envc, const char** envp, const char* execPath) {
    if (!code || codeSize == 0) {
        return nullptr;
    }
    ElfSource source;
    source.mem = reinterpret_cast<const uint8_t*>(VMM::PhysToVirt(reinterpret_cast<uint64_t>(code)));
    source.size = codeSize;
    return createProcessFromSource(source, argc, argv, envc, envp, execPath);
}

Process* ProcessExecutor::loadUserBinary(const char* path) {
    const char* argv[] = { path, nullptr };
    return loadUserBinaryWithArgs(path, 1, argv);
}

Process* ProcessExecutor::loadUserBinaryWithArgs(const char* path, int argc, const char** argv,
                                                 int envc, const char** envp) {
    if (!path) {
        return nullptr;
    }

    // Working argv, seeded from the caller. Rewritten in place by each level of
    // "#!" resolution: argv := [interp, optional-arg, scriptpath, oldargv[1..]].
    // Both vectors live on the heap: the kernel stack is 16 KiB.
    const char** curArgv = new const char*[2 * (MAX_ARG_ENV + 1)];
    if (!curArgv) {
        return nullptr;
    }
    const char** oldArgv = curArgv + MAX_ARG_ENV + 1;
    struct FreeArgv {
        const char** p;
        ~FreeArgv() { delete[] p; }
    } freeArgv{curArgv};
    int curArgc = 0;
    if (argc <= 0 || !argv) {
        curArgv[curArgc++] = path;
    } else {
        for (int i = 0; i < argc && curArgc < MAX_ARG_ENV; ++i) {
            curArgv[curArgc++] = argv[i];
        }
    }
    curArgv[curArgc] = nullptr;

    // Interpreter/script/arg strings synthesised during shebang resolution.
    char* owned[MAX_SHEBANG_DEPTH * 3];
    int ownedCount = 0;

    const char* curPath = path;
    ElfSource source;
    Process* proc = nullptr;
    bool haveElf = false;

    for (int depth = 0; depth <= MAX_SHEBANG_DEPTH; ++depth) {
        if (!openElfSource(curPath, &source)) {
            Console::get().drawText("[PROC] failed to read user binary: ");
            Console::get().drawText(curPath);
            Console::get().drawText("\n");
            break;
        }

        char interp[256];
        char shArg[256];
        char firstLine[SHEBANG_LINE_MAX];
        const uint64_t lineBytes = source.size < sizeof(firstLine) ? source.size : sizeof(firstLine);
        if (!source.read(0, firstLine, lineBytes)) {
            Console::get().drawText("[PROC] failed to read user binary: ");
            Console::get().drawText(curPath);
            Console::get().drawText("\n");
            closeElfSource(&source);
            break;
        }
        if (!parseShebang(firstLine, lineBytes, interp, sizeof(interp), shArg, sizeof(shArg))) {
            haveElf = true; // `source` is a real image to load
            break;
        }

        closeElfSource(&source);

        if (depth == MAX_SHEBANG_DEPTH ||
            ownedCount + 3 > static_cast<int>(sizeof(owned) / sizeof(owned[0]))) {
            Console::get().drawText("[PROC] too many nested #! interpreters\n");
            break;
        }

        char* interpCopy = dupString(interp);
        char* scriptCopy = dupString(curPath);
        char* argCopy = shArg[0] ? dupString(shArg) : nullptr;
        if (!interpCopy || !scriptCopy || (shArg[0] && !argCopy)) {
            delete[] interpCopy;
            delete[] scriptCopy;
            delete[] argCopy;
            break;
        }
        owned[ownedCount++] = interpCopy;
        owned[ownedCount++] = scriptCopy;
        if (argCopy) owned[ownedCount++] = argCopy;

        const int oldArgc = curArgc;
        for (int i = 0; i < oldArgc; ++i) oldArgv[i] = curArgv[i];

        int n = 0;
        curArgv[n++] = interpCopy;
        if (argCopy && n < MAX_ARG_ENV) curArgv[n++] = argCopy;
        if (n < MAX_ARG_ENV) curArgv[n++] = scriptCopy;
        for (int i = 1; i < oldArgc && n < MAX_ARG_ENV; ++i) curArgv[n++] = oldArgv[i];
        curArgv[n] = nullptr;
        curArgc = n;

        curPath = interpCopy; // loop and load the interpreter
    }

    if (haveElf) {
        proc = createProcessFromSource(source, curArgc, curArgv, envc, envp, path);
        if (proc) {
            proc->setName(path);
            assignUserProcessPriority(proc, path);
            // Memory over a long build (P7.1: is something leaking?).
            static uint64_t s_execs = 0;
            if ((++s_execs & 255) == 0) {
                const HeapStats hs = heap_stats();
                Console::get().drawText("[mem] execs=");
                Console::get().drawNumber(static_cast<int64_t>(s_execs));
                Console::get().drawText(" free MiB=");
                Console::get().drawNumber(static_cast<int64_t>(PMM::FreeMemory() >> 20));
                Console::get().drawText(" heap used MiB=");
                Console::get().drawNumber(static_cast<int64_t>(hs.total_allocated >> 20));
                Console::get().drawText(" arenas=");
                Console::get().drawNumber(heap_arena_count());
                Console::get().drawText(" low=");
                Console::get().drawNumber(heap_low_arena_count());
                Console::get().drawText(" spaces=");
                Console::get().drawNumber(g_liveSharedStates);
                Console::get().drawText(" tables=");
                Console::get().drawNumber(VMM::liveTables);
                Console::get().drawText("\n");
                if ((s_execs & 16383) == 0) {   // live heap blocks by caller, mmap-heavy processes
                    heap_dump_callers(12);
                    for (Process* p = Scheduler::get().getAllProcessesHead(); p; p = p->allNext) {
                        const uint64_t regions = p->mmapRegionCount();
                        if (regions >= 64) {
                            Console::get().drawText("[heap] regions=");
                            Console::get().drawNumber(static_cast<int64_t>(regions));
                            Console::get().drawText(" pid=");
                            Console::get().drawNumber(static_cast<int64_t>(p->getPID()));
                            Console::get().drawText(" ");
                            Console::get().drawText(p->getName());
                            Console::get().drawText("\n");
                        }
                    }
                }
            }
        } else {
            Console::get().drawText("[PROC] create ELF process failed path=");
            Console::get().drawText(path);
            Console::get().drawText("\n");
        }
    }

    closeElfSource(&source);
    for (int i = 0; i < ownedCount; ++i) {
        delete[] owned[i];
    }
    return proc;
}

void ProcessExecutor::setupArguments(Process* proc, int argc, const char** argv) {
    InitialStackInfo empty {};
    setupInitialStack(proc, proc ? proc->getName() : "", empty, argc, argv, 0, nullptr);
}
