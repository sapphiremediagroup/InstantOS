#pragma once

#include "process.hpp"
#include "scheduler.hpp"

class GDT;

class ProcessExecutor {
public:
    static Process* createKernelProcess(void (*entry)());
    static Process* createUserProcess(uint64_t entry);
    static Process* createUserProcessWithCode(void* code, size_t codeSize);
    static Process* createUserProcessWithArgs(void* code, size_t codeSize, int argc, const char** argv);
    static Process* loadUserBinary(const char* path);
    static Process* loadUserBinaryWithArgs(const char* path, int argc, const char** argv);
    static void executeUserProcess(Process* proc, GDT* gdt);
    
private:
    static void kernelProcessWrapper();
    static void setupArguments(Process* proc, int argc, const char** argv);
};
