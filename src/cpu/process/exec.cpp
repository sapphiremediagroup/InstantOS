#include <cpu/process/exec.hpp>
#include <cpu/process/pe.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <cpu/gdt/gdt.hpp>
#include <common/string.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>

namespace {
void assignUserProcessPriority(Process* proc, const char* path) {
    if (!proc || !path) {
        return;
    }

    if (strcmp(path, "/bin/graphics-compositor.exe") == 0) {
        proc->setPriority(ProcessPriority::High);
    }
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
    
    uint64_t stack = proc->getUserStack();
    stack &= ~0xFULL;
    
    proc->getContext()->rip = entry;
    proc->getContext()->rsp = stack;
    proc->getContext()->rbp = 0;
    proc->getContext()->rflags = 0x202;
    
    return proc;
}

void ProcessExecutor::executeUserProcess(Process* proc, GDT* gdt) {
    proc->jumpToUsermode(proc->getContext()->rip, gdt);
}

constexpr uint64_t USER_CODE_BASE = 0x00007FFFFFE00000ULL;

extern "C" void processTrampoline();

Process* ProcessExecutor::createUserProcessWithCode(void* code, size_t codeSize) {
    if (codeSize >= sizeof(IMAGE_DOS_HEADER)) {
        auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(code);
        if (dosHeader->e_magic == 0x5A4D) {
            auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(
                reinterpret_cast<uint8_t*>(code) + dosHeader->e_lfanew);
            
            if (ntHeaders->Signature == 0x00004550) {
                uint32_t pid = Scheduler::get().allocatePID();
                Process* proc = new Process(pid);
                if (!proc) {
                    Console::get().drawText("[PROC] Failed to allocate process object\n");
                    return nullptr;
                }
                proc->setName("<image>");

                // Validate process creation succeeded
                if (!proc->getPageTable()) {
                    Console::get().drawText("[PROC] Failed to allocate process page table\n");
                    delete proc;
                    return nullptr;
                }
                
                uint64_t imageBase = ntHeaders->OptionalHeader.ImageBase;
                if (!imageBase) imageBase = USER_CODE_BASE;
                
                PageTable* procPT = proc->getPageTable();
                
                Console::get().drawText("[PROC] Loading PE at base: ");
                Console::get().drawHex(imageBase);
                Console::get().drawText("\n");

                uint64_t headerPages = (ntHeaders->OptionalHeader.SizeOfHeaders + 0xFFF) / 0x1000;
                if (headerPages == 0) headerPages = 1;
                uint64_t headerPhys = PMM::AllocFrames(headerPages);
                if (headerPhys) {
                    memset(reinterpret_cast<void*>(headerPhys), 0, headerPages * 0x1000);
                    size_t copyLen = codeSize > ntHeaders->OptionalHeader.SizeOfHeaders ? ntHeaders->OptionalHeader.SizeOfHeaders : codeSize;
                    memcpy(reinterpret_cast<void*>(headerPhys), code, copyLen);
                    VMM::MapRangeInto(procPT, imageBase, headerPhys, headerPages, PageFlags::Present | PageFlags::UserSuper | PageFlags::ReadWrite);
                }
                
                auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(
                    reinterpret_cast<uint8_t*>(&ntHeaders->OptionalHeader) + ntHeaders->FileHeader.SizeOfOptionalHeader);
                
                for (int i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
                    if (section[i].SizeOfRawData == 0 && section[i].Misc.VirtualSize == 0) continue;
                    
                    uint64_t virtAddr = imageBase + section[i].VirtualAddress;
                    uint64_t memSize = section[i].Misc.VirtualSize > section[i].SizeOfRawData ? section[i].Misc.VirtualSize : section[i].SizeOfRawData;
                    
                    uint64_t alignedVirt = virtAddr & ~0xFFFULL;
                    uint64_t offset = virtAddr - alignedVirt;
                    uint64_t alignedSize = (memSize + offset + 0xFFF) & ~0xFFFULL;
                    size_t pages = alignedSize / 0x1000;
                    
                    uint64_t physAddr = PMM::AllocFrames(pages);
                    if (!physAddr) {
                        Console::get().drawText("[PROC] Failed to allocate section frames: ");
                        Console::get().drawNumber(i);
                        Console::get().drawText("\n");
                        delete proc;
                        return nullptr;
                    }

                    memset(reinterpret_cast<void*>(physAddr), 0, pages * 0x1000);
                    
                    if (section[i].SizeOfRawData > 0) {
                        size_t copySize = section[i].SizeOfRawData > codeSize - section[i].PointerToRawData ? codeSize - section[i].PointerToRawData : section[i].SizeOfRawData;
                        memcpy(reinterpret_cast<void*>(physAddr + offset), reinterpret_cast<uint8_t*>(code) + section[i].PointerToRawData, copySize);
                    }
                    
                    uint64_t flags = PageFlags::Present | PageFlags::UserSuper;
                    if (section[i].Characteristics & 0x80000000) flags |= PageFlags::ReadWrite;
                    if ((section[i].Characteristics & 0x20000000) == 0) flags |= PageFlags::NoExecute;
                    
                    VMM::MapRangeInto(procPT, alignedVirt, physAddr, pages, flags);
                }

                uint64_t entry = imageBase + ntHeaders->OptionalHeader.AddressOfEntryPoint;
                Console::get().drawText("[PROC] Entry point: ");
                Console::get().drawHex(entry);
                Console::get().drawText("\n");
                
                uint64_t trampolineAddr = reinterpret_cast<uint64_t>(&processTrampoline);

                uint64_t userStack = proc->getUserStack();
                userStack &= ~0xFULL;

                uint64_t kernelStack = proc->getKernelStack();
                kernelStack -= 8;
                *reinterpret_cast<uint64_t*>(kernelStack) = userStack;
                kernelStack -= 8;
                *reinterpret_cast<uint64_t*>(kernelStack) = entry;

                proc->getContext()->rip = trampolineAddr;
                proc->getContext()->rsp = kernelStack;



                return proc;
            }
        }
    }

    uint32_t pid = Scheduler::get().allocatePID();
    
    Process* proc = new Process(pid);

    if (!proc) {
        Console::get().drawText("[PROC] Failed to allocate raw process object\n");
        return nullptr;
    }
    proc->setName("<raw>");
    
    // Validate process creation succeeded
    if (!proc->getPageTable()) {
        Console::get().drawText("[PROC] Failed to allocate raw process page table\n");
        delete proc;
        return nullptr;
    }
    
    size_t pages = (codeSize + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t codePhys = PMM::AllocFrames(pages);
    
    if (!codePhys) {
        Console::get().drawText("[PROC] Failed to allocate raw code pages\n");
        delete proc;
        return nullptr;
    }

    uint64_t codeVirt = codePhys;
    
    memset(reinterpret_cast<void*>(codeVirt), 0, pages * PAGE_SIZE);
    memcpy(reinterpret_cast<void*>(codeVirt), code, codeSize);
    
    VMM::MapRangeInto(proc->getPageTable(), USER_CODE_BASE, codePhys, pages, PageFlags::Present | PageFlags::ReadWrite | PageFlags::UserSuper);
    
    uint64_t userStack = proc->getUserStack();
    userStack &= ~0xFULL;

    uint64_t entry = USER_CODE_BASE;
    uint64_t trampolineAddr = reinterpret_cast<uint64_t>(&processTrampoline);
    
    uint64_t kernelStack = proc->getKernelStack();
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = userStack;
    kernelStack -= 8;
    *reinterpret_cast<uint64_t*>(kernelStack) = entry;
    
    proc->getContext()->rip = trampolineAddr;
    proc->getContext()->rsp = kernelStack;
    proc->getContext()->rbp = 0;
    proc->getContext()->rflags = 0x202;
    
    return proc;
}

Process* ProcessExecutor::loadUserBinary(const char* path) {
    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(path, 0, &fd);
    
    if (result != 0 || !fd) {
        return nullptr;
    }
    
    FileStats stats;
    if (fd->getNode()->ops->stat(fd->getNode(), &stats) != 0) {
        VFS::get().close(fd);
        return nullptr;
    }
    
    size_t size = stats.size;
    
    void* buffer = new uint8_t[size];
    if (VFS::get().read(fd, buffer, size) != static_cast<int64_t>(size)) {
        delete[] static_cast<uint8_t*>(buffer);
        VFS::get().close(fd);
        return nullptr;
    }
    
    VFS::get().close(fd);
    
    Process* proc = createUserProcessWithCode(buffer, size);
    if (proc) {
        proc->setName(path);
        assignUserProcessPriority(proc, path);
    }
    
    delete[] static_cast<uint8_t*>(buffer);
    
    return proc;
}

void ProcessExecutor::setupArguments(Process* proc, int argc, const char** argv) {
    if (!proc || argc < 0) return;
    
    uint64_t userStack = proc->getUserStack();
    userStack &= ~0xFULL;
    
    size_t totalStringSize = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            totalStringSize += strlen(argv[i]) + 1;
        }
    }
    totalStringSize = (totalStringSize + 7) & ~7;
    
    size_t totalSize = totalStringSize + (argc + 1) * sizeof(uint64_t) + sizeof(uint64_t);
    
    userStack -= totalSize;
    userStack &= ~0xFULL;
    
    uint8_t* buffer = new uint8_t[totalSize];
    if (!buffer) return;
    memset(buffer, 0, totalSize);
    
    uint64_t stringBase = userStack + sizeof(uint64_t) + (argc + 1) * sizeof(uint64_t);
    size_t stringOffset = 0;
    
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            size_t len = strlen(argv[i]) + 1;
            memcpy(buffer + sizeof(uint64_t) + (argc + 1) * sizeof(uint64_t) + stringOffset, argv[i], len);
            
            uint64_t* argvPtr = reinterpret_cast<uint64_t*>(buffer + sizeof(uint64_t) + i * sizeof(uint64_t));
            *argvPtr = stringBase + stringOffset;
            stringOffset += len;
        }
    }
    
    uint64_t* nullPtr = reinterpret_cast<uint64_t*>(buffer + sizeof(uint64_t) + argc * sizeof(uint64_t));
    *nullPtr = 0;
    
    uint64_t* argcPtr = reinterpret_cast<uint64_t*>(buffer);
    *argcPtr = argc;
    
    uint64_t physUserStack = VMM::VirtualToPhysicalIn(proc->getPageTable(), userStack);
    if (physUserStack) {
        memcpy(reinterpret_cast<void*>(physUserStack), buffer, totalSize);
    }
    
    delete[] buffer;
    
    proc->setUserStack(userStack);
   
    uint64_t* userRspOnStack = reinterpret_cast<uint64_t*>(proc->getContext()->rsp + 8);
    uint64_t oldValue = *userRspOnStack;
    *userRspOnStack = userStack;
}

Process* ProcessExecutor::createUserProcessWithArgs(void* code, size_t codeSize, int argc, const char** argv) {
    Process* proc = createUserProcessWithCode(code, codeSize);
    
    if (proc && argc > 0) {
        setupArguments(proc, argc, argv);
    }
    return proc;
}

Process* ProcessExecutor::loadUserBinaryWithArgs(const char* path, int argc, const char** argv) {
    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(path, 0, &fd);
    
    if (result != 0 || !fd) {
        return nullptr;
    }
    
    FileStats stats;
    if (fd->getNode()->ops->stat(fd->getNode(), &stats) != 0) {
        VFS::get().close(fd);
        return nullptr;
    }
    
    size_t size = stats.size;
    
    void* buffer = new uint8_t[size];
    if (VFS::get().read(fd, buffer, size) != static_cast<int64_t>(size)) {
        delete[] static_cast<uint8_t*>(buffer);
        VFS::get().close(fd);
        return nullptr;
    }
    
    VFS::get().close(fd);
    
    Process* proc = createUserProcessWithArgs(buffer, size, argc, argv);
    if (proc) {
        proc->setName(path);
        assignUserProcessPriority(proc, path);
    }
    
    delete[] static_cast<uint8_t*>(buffer);
    
    return proc;
}
