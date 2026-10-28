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

// Backing store for XSAVE/XSAVEOPT (must be >= the CPU's XSAVE area and 64-byte
// aligned). 4096 covers x87+SSE+AVX+AVX-512+PKRU (largest standard-layout offset
// is well under 4K) and matches EXTENDED_STATE_BUFFER_SIZE; keeping it small
// avoids exhausting the buddy allocator's large-block pool when many
// processes/threads are created (which previously left fpuState unallocated).
struct alignas(64) FPUState {
  uint8_t data[4096];
};

struct ProcessContext {
  uint64_t rax, rbx, rcx, rdx;
  uint64_t rsi, rdi, rbp, rsp;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rip, rflags, cr3, xstate;
};

// Number of signal slots. Signals are numbered 1..NSIG-1 and stored as bit
// positions in 64-bit masks (bit `sig`), so NSIG must be <= 64. This matches
// the Linux/mlibc ABI range closely enough for userspace (SIGCANCEL=32,
// SIGTIMER=33, SIGRTMIN=35..) without overflowing the 64-bit mask words.
#define NSIG 64
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
  // Big-kernel-lock depth at which this thread's current syscall lets storage
  // operations run without the lock (0 = never). See StorageLock.
  uint32_t getStorageUnlockDepth() const { return storageUnlockDepth; }
  void setStorageUnlockDepth(uint32_t depth) { storageUnlockDepth = depth; }
  // CPUs (bit = SMP index) this thread may run on; inherited by fork, spawn
  // and new threads. See sys_sched_setaffinity.
  uint64_t getCpuAffinity() const { return cpuAffinity; }
  void setCpuAffinity(uint64_t mask) { cpuAffinity = mask; }
  bool mayRunOn(uint32_t cpu) const { return cpu < 64 && ((cpuAffinity >> cpu) & 1) != 0; }

  uint64_t getSavedUserRSP() const { return savedUserRSP; }
  void setSavedUserRSP(uint64_t rsp) { savedUserRSP = rsp; }
  uint64_t getUserFsBase() const { return userFsBase; }
  void setUserFsBase(uint64_t base) { userFsBase = base; }
  // User GS base. Live in IA32_KERNEL_GS_BASE while this process is current
  // (see percpu.hpp); saved here by the scheduler when it switches away.
  uint64_t getUserGsBase() const { return userGsBase; }
  void setUserGsBase(uint64_t base) { userGsBase = base; }

  bool isSleeping() const { return sleeping; }
  uint64_t getSleepDeadlineMs() const { return sleepDeadlineMs; }
  // wakesOnEvents: a timed wait (poll with a timeout) that I/O wake-ups
  // (Scheduler::wakeAllBlockedProcesses) may end early, unlike a plain sleep.
  void sleepUntil(uint64_t deadlineMs, bool wakesOnEvents = false) {
    sleepDeadlineMs = deadlineMs;
    sleeping = true;
    sleepWakesOnEvents = wakesOnEvents;
  }
  void clearSleep() {
    sleepDeadlineMs = 0;
    sleeping = false;
    sleepWakesOnEvents = false;
  }
  bool wakesOnEvents() const { return sleepWakesOnEvents; }
  bool sleepDeadlineReached(uint64_t nowMs) const {
    return sleeping && nowMs >= sleepDeadlineMs;
  }

  SignalHandler *getSignalHandler() { return &signalHandler; }
  void sendSignal(int sig);

  // ITIMER_REAL (setitimer/alarm): SIGALRM at alarmDeadlineMs (0 = off),
  // then every alarmIntervalMs if nonzero. Checked by the boot CPU's tick.
  uint64_t alarmDeadlineMs = 0;
  uint64_t alarmIntervalMs = 0;
  void handlePendingSignals();
  bool hasDeliverableSignal() const;
  bool killPending() const { return (signalHandler.pending & (1ULL << SIGKILL)) != 0; }
  // Acts on a pending SIGKILL only, for paths that cannot deliver a handler
  // (an IRQ taken in user mode). True if this terminated the process.
  bool terminateIfKilled();
  // Kills the process like SIGKILL but reports `code` as its exit status
  // (another thread called exit() or crashed).
  void requestExit(int code) {
    exitRequested = true;
    exitCode = code;
    sendSignal(SIGKILL);
  }

  // One-shot guard so Scheduler::onProcessTerminated() runs its parent
  // notification / child reparenting exactly once regardless of how many
  // exit paths (sys_exit, signal kill, scheduler sweep) observe the death.
  bool wasTerminationHandled() const { return terminationHandled; }
  void markTerminationHandled() { terminationHandled = true; }

  uint64_t getMmapBase() const;
  uint64_t reserveMmapRegion(uint64_t size);

  // Demand-paged anonymous mmap regions. sys_mmap records a region and returns
  // immediately; physical frames are allocated lazily by handleDemandFault() on
  // first access. prot uses MemoryProt* bits (0 == PROT_NONE / inaccessible).
  bool mmapAddRegion(uint64_t start, uint64_t length, uint64_t prot);
  // File-backed mmap: like mmapAddRegion but the region is demand-paged from
  // `file` at `fileOffset`. `fileLength` bounds how many bytes come from the
  // file (beyond it, pages are zero-filled and never written back). `shared`
  // selects MAP_SHARED writeback vs MAP_PRIVATE copy semantics. Retains `file`
  // for the region's lifetime (released when the region is dropped).
  bool mmapAddFileRegion(uint64_t start, uint64_t length, uint64_t prot,
                         FileDescriptor* file, uint64_t fileOffset,
                         uint64_t fileLength, bool shared);
  void mmapRemoveRange(uint64_t start, uint64_t length);
  bool mmapProtectRange(uint64_t start, uint64_t length, uint64_t prot);
  // Write back the present pages of any writable MAP_SHARED file region that
  // overlaps [start, start+length) to its backing file (msync / pre-munmap
  // flush). No-op for anonymous and read-only/private regions.
  void mmapSyncRange(uint64_t start, uint64_t length);
  bool mmapRegionCovers(uint64_t addr) const;   // an accessible region contains addr?
  bool handleDemandFault(uint64_t faultAddr);   // populate a lazily-reserved page
  bool mmapCloneRegionsFrom(const Process* parent);

  bool isThread() const { return threadObject != nullptr; }
  ThreadObject *getThreadObject() { return threadObject; }
  const ThreadObject *getThreadObject() const { return threadObject; }
  void setThreadObject(ThreadObject *object) { threadObject = object; }
  bool replaceImageFrom(Process *image);
  // Threads of one process share its address space, handles and mmap regions.
  bool sharesStateWith(const Process *other) const {
    return other && sharedState && sharedState == other->sharedState;
  }

  // File descriptor management
  uint64_t allocateFD(FileDescriptor *fd);
  uint64_t allocateFD(FileDescriptor *fd, uint32_t rights);
  uint64_t allocateFD(FileDescriptor *fd, uint32_t rights, bool closeOnExec);
  uint64_t allocateFDAt(int slot, FileDescriptor *fd, uint32_t rights, bool closeOnExec);
  FileDescriptor *getFD(uint64_t fileHandle);
  FileDescriptor *getFD(uint64_t fileHandle, uint32_t requiredRights);
  void closeFD(uint64_t fileHandle);
  uint64_t duplicateFD(uint64_t fileHandle);
  bool duplicateFDTo(uint64_t oldFileHandle, uint64_t newFileHandle);
  bool getHandleCloseOnExec(uint64_t handle, bool *enabled) const;
  bool setHandleCloseOnExec(uint64_t handle, bool enabled);
  void closeOnExecHandles();
  // Release this process's open file descriptors at termination (POSIX _exit
  // semantics) so a zombie no longer pins shared file descriptions such as a
  // pipe write end. Safe/no-op when the fd table is shared with threads.
  void closeFilesOnExit();

  // Copy another process's handle table into ours (fd inheritance for
  // spawn/fork). skipCloseOnExec excludes FD_CLOEXEC handles.
  void cloneHandlesFrom(Process *source, bool skipCloseOnExec);

  // P3.2 exec fd-inheritance: sys_exec keeps the kernel handle table across an
  // image replacement (dropping CLOEXEC), but the new libc reinitializes its
  // userspace fd-number -> handle map. libc stashes that map here just before
  // exec and fetches it at entry; the buffer is a plain Process member so it
  // survives replaceImageFrom(). fetch validates each entry against the live
  // handle table so CLOEXEC-closed fds are dropped.
  static constexpr int kFdStashMax = 256;
  uint64_t *getFdStash() { return fdStash; }
  int getFdStashCount() const { return fdStashCount; }
  void setFdStashCount(int count) { fdStashCount = count; }

  // fork() support: deep-copy the parent's user address space into this
  // (freshly created) process, and arrange for this process to resume in
  // usermode with the given saved register context (child returns 0).
  bool cloneAddressSpaceFrom(Process *parent);
  void setupForkResume(const ProcessContext &userContext, uint64_t fsBase);

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
  FPUState *userFpuState = nullptr;
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
  uint64_t kernelStack = 0;
  uint64_t userStack = 0;
  uint64_t userStackBase = 0;
  uint64_t userStackSize = 0;
  // True when userStackBase maps a kmalloc()'d region this Process owns (spawn
  // path). False when the stack is part of a copied address space (fork), in
  // which case FreeAddressSpace() reclaims the frame and the destructor must
  // not kfree() it.
  bool userStackHeapBacked;
  ProcessContext context;
  FPUState *fpuState = nullptr;
  bool validUserState;
  uint64_t savedUserRSP;
  uint64_t userFsBase;
  uint64_t userGsBase;
  uint64_t sleepDeadlineMs;
  bool sleeping;
  bool terminationHandled = false;
  bool exitRequested = false;   // see requestExit()
  bool sleepWakesOnEvents = false;   // see sleepUntil()
  uint32_t storageUnlockDepth = 0;
  uint64_t cpuAffinity = ~0ULL;
  SignalHandler signalHandler;
  Debug::SyscallTrace syscallTrace;
  ThreadObject *threadObject;
  // P3.2: libc fd-table snapshot preserved across exec (see getFdStash()).
  uint64_t fdStash[kFdStashMax];
  int fdStashCount = -1;
};
