#pragma once

#include "process.hpp"
#include "scheduler.hpp"

class GDT;

class ProcessExecutor {
public:
    // Limits on what exec/spawn pass to a new program, Linux's: one string up
    // to MAX_ARG_STRLEN (128 KiB), all of argv + envp up to a quarter of the
    // 8 MiB stack. The entry count only bounds the kernel's arrays.
    static constexpr int kMaxArgEnv = 8192;
    static constexpr uint64_t kMaxArgStringLen = 128 * 1024;
    static constexpr uint64_t kMaxArgBytes = 2 * 1024 * 1024;

    static Process* createKernelProcess(void (*entry)());
    static Process* createUserProcess(uint64_t entry);
    static Process* createUserProcessWithCode(void* code, size_t codeSize);
    static Process* createUserProcessWithArgs(void* code, size_t codeSize, int argc, const char** argv,
                                              int envc = 0, const char** envp = nullptr,
                                              const char* execPath = nullptr);
    static Process* loadUserBinary(const char* path);
    static Process* loadUserBinaryWithArgs(const char* path, int argc, const char** argv,
                                           int envc = 0, const char** envp = nullptr);
    static void executeUserProcess(Process* proc, GDT* gdt);
    
private:
    static void kernelProcessWrapper();
    static void setupArguments(Process* proc, int argc, const char** argv);
};
