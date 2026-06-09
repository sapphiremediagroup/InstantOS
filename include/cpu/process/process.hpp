#pragma once

#include <cpu/process/handles.hpp>
#include <cpu/user/user.hpp>
#include <debug/diag.hpp>
#include <memory/vmm.hpp>
#include <stdint.h>

class FileDescriptor;

enum class ProcessState { Ready, Running, Blocked, Terminated };

enum class ProcessPriority {
  Low = 0,
  Normal = 1,
  High = 2,
  Idle = 3 // Special priority for idle process
};

struct alignas(64) FPUState {
  uint8_t data[8192];
};

struct ProcessContext {
  uint64_t rax, rbx, rcx, rdx;
  uint64_t rsi, rdi, rbp, rsp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip, rflags, cr3, xstate;
};

#define NSIG 32
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17

#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SS_ONSTACK 1
#define SS_DISABLE 2

typedef void (*sighandler_t)(int);

struct SignalHandler {
  sighandler_t handlers[NSIG];
  uint64_t masks[NSIG];
  uint64_t flags[NSIG];
  uint64_t restorers[NSIG];
  uint64_t pending;
  uint64_t blocked;
  uint64_t altStackSp;
  uint64_t altStackSize;
  uint32_t altStackFlags;
};

class GDT;
struct ProcessSharedState;

class Process;

struct ThreadObject {
  uint32_t tid;
  uint32_t refCount;
  bool completed;
  int exitCode;
  Process *process;

  void retain() { __sync_add_and_fetch(&refCount, 1); }
  void release() {
    if (__sync_sub_and_fetch(&refCount, 1) == 0) {
      delete this;
    }
  }
};

class Process {
public:
  Process(uint32_t pid);
  Process(uint32_t pid, Process *sharedFrom, uint64_t stackSize);
  ~Process();

  uint32_t getPID() const { return pid; }
  ProcessState getState() const { return state; }
  void setState(ProcessState s) { state = s; }

  ProcessContext *getContext() { return &context; }
  FPUState *getFPUState() { return fpuState; }
  PageTable *getPageTable() const;

  uint64_t getKernelStack() const { return kernelStack; }
  uint64_t getUserStack() const { return userStack; }
  uint64_t getUserStackBase() const { return userStackBase; }
  uint64_t getUserStackSize() const { return userStackSize; }

  void setKernelStack(uint64_t stack) { kernelStack = stack; }
  void setUserStack(uint64_t stack) { userStack = stack; }

  void jumpToUsermode(uint64_t entry, GDT *gdt);

  uint32_t getParentPID() const { return parentPID; }
  void setParentPID(uint32_t ppid) { parentPID = ppid; }

  ProcessPriority getPriority() const { return priority; }
  void setPriority(ProcessPriority p) { priority = p; }

  uint32_t getUID() const { return uid; }
  void setUID(uint32_t u) { uid = u; }

  uint32_t getGID() const { return gid; }
  void setGID(uint32_t g) { gid = g; }

  uint32_t getSessionID() const { return sessionID; }
  void setSessionID(uint32_t sid) { sessionID = sid; }

  bool isPrivileged() const { return uid == ROOT_UID; }

  int getExitCode() const { return exitCode; }
  void setExitCode(int code) { exitCode = code; }

  Process *next;
  Process *allNext;

  bool hasValidUserState() const { return validUserState; }
  void setValidUserState(bool valid) { validUserState = valid; }

  uint64_t getSavedUserRSP() const { return savedUserRSP; }
  void setSavedUserRSP(uint64_t rsp) { savedUserRSP = rsp; }
  uint64_t getUserFsBase() const { return userFsBase; }
  void setUserFsBase(uint64_t base) { userFsBase = base; }

  bool isSleeping() const { return sleeping; }
  uint64_t getSleepDeadlineMs() const { return sleepDeadlineMs; }
  void sleepUntil(uint64_t deadlineMs) {
    sleepDeadlineMs = deadlineMs;
    sleeping = true;
  }
  void clearSleep() {
    sleepDeadlineMs = 0;
    sleeping = false;
  }
  bool sleepDeadlineReached(uint64_t nowMs) const {
    return sleeping && nowMs >= sleepDeadlineMs;
  }

  SignalHandler *getSignalHandler() { return &signalHandler; }
  void sendSignal(int sig);
  void handlePendingSignals();
  bool hasDeliverableSignal() const;

  uint64_t getMmapBase() const;
  uint64_t reserveMmapRegion(uint64_t size);

  bool isThread() const { return threadObject != nullptr; }
  ThreadObject *getThreadObject() { return threadObject; }
  const ThreadObject *getThreadObject() const { return threadObject; }
  void setThreadObject(ThreadObject *object) { threadObject = object; }
  bool replaceImageFrom(Process *image);

  // File descriptor management
  uint64_t allocateFD(FileDescriptor *fd);
  uint64_t allocateFD(FileDescriptor *fd, uint32_t rights);
  uint64_t allocateFD(FileDescriptor *fd, uint32_t rights, bool closeOnExec);
  FileDescriptor *getFD(uint64_t fileHandle);
  FileDescriptor *getFD(uint64_t fileHandle, uint32_t requiredRights);
  void closeFD(uint64_t fileHandle);
  uint64_t duplicateFD(uint64_t fileHandle);
  bool duplicateFDTo(uint64_t oldFileHandle, uint64_t newFileHandle);
  bool getHandleCloseOnExec(uint64_t handle, bool *enabled) const;
  bool setHandleCloseOnExec(uint64_t handle, bool enabled);
  void closeOnExecHandles();

  // Typed handle management
  uint64_t allocateHandle(HandleType type, uint32_t rights, void *object, HandleRetainFn retain, HandleReleaseFn release);
  bool closeHandle(uint64_t handle);
  bool closeHandle(uint64_t handle, HandleType expectedType);
  uint64_t duplicateHandle(uint64_t handle);
  bool duplicateHandleTo(uint64_t oldHandle, uint64_t newHandle);
  HandleEntry *getHandle(uint64_t handle);
  void *getHandleObject(uint64_t handle, HandleType expectedType, uint32_t requiredRights = HandleRightNone);

  // Working directory
  const char *getCwd() const { return cwd; }
  void setCwd(const char *path) {
    if (path) {
      size_t len = 0;
      while (path[len] && len < sizeof(cwd) - 1)
        len++;
      for (size_t i = 0; i < len; i++)
        cwd[i] = path[i];
      cwd[len] = '\0';
    }
  }

  const char* getName() const { return name; }
  void setName(const char* value) {
    if (!value) {
      name[0] = '\0';
      return;
    }

    size_t len = 0;
    while (value[len] && len < sizeof(name) - 1) {
      name[len] = value[len];
      len++;
    }
    name[len] = '\0';
  }

  Debug::SyscallTrace& getSyscallTrace() { return syscallTrace; }
  const Debug::SyscallTrace& getSyscallTrace() const { return syscallTrace; }
  FPUState *userFpuState;
private:
  ProcessSharedState *sharedState;
  char cwd[256];
  char name[64];
  uint32_t sessionID;
  uint32_t uid;
  uint32_t gid;
  uint32_t pid;
  uint32_t parentPID;
  int exitCode;
  ProcessState state;
  ProcessPriority priority;
  uint64_t kernelStack;
  uint64_t userStack;
  uint64_t userStackBase;
  uint64_t userStackSize;
  ProcessContext context;
  FPUState *fpuState;
  bool validUserState;
  uint64_t savedUserRSP;
  uint64_t userFsBase;
  uint64_t sleepDeadlineMs;
  bool sleeping;
  SignalHandler signalHandler;
  Debug::SyscallTrace syscallTrace;
  ThreadObject *threadObject;
};
