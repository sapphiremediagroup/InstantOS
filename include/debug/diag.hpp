#pragma once

#include <cpu/process/handles.hpp>
#include <stdint.h>

class Process;

namespace Debug {

struct SyscallTrace {
    bool active;
    uint64_t number;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
};

void initializeKernelSymbols();

void panic(const char* reason);
void panicf(const char* reason, const char* detail);
void assertFail(const char* expr, const char* file, int line, const char* func);

const char* lookupSymbol(uint64_t address, uint64_t* symbolAddress = nullptr);
void printAddressSymbol(uint64_t address);
void printCurrentProcessSummary();
void printCurrentProcessSyscall();
void printPageFaultReason(uint64_t errCode);

const char* syscallName(uint64_t number);
const char* handleTypeName(HandleType type);

void beginSyscallTrace(Process* process, uint64_t syscallNumber, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
void endSyscallTrace(Process* process);

}

#define KASSERT(expr) ((expr) ? (void)0 : Debug::assertFail(#expr, __FILE__, __LINE__, __func__))
