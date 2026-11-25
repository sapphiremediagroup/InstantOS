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

#define SIGBUS 7
#define SIGTRAP 5
#define SA_SIGINFO 4
#define SA_RESTORER 0x04000000
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SS_ONSTACK 1
#define SS_DISABLE 2

// siginfo_t's si_code values (Linux).
#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_TKILL (-6)
#define CLD_EXITED 1
#define CLD_KILLED 2

typedef void (*sighandler_t)(int);

// What a pending signal's siginfo_t says (one per signal: a standard signal
// sent again while pending keeps the first).
struct SignalInfo {
  int32_t code;      // si_code
  int32_t pid;       // sender (SI_USER/SI_TKILL), the child (SIGCHLD)
  uint32_t uid;
  int32_t status;    // SIGCHLD: exit status or signal
  uint64_t addr;     // fault address (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP)
};

struct SignalHandler {
  sighandler_t handlers[NSIG];
  uint64_t masks[NSIG];
  uint64_t flags[NSIG];
  uint64_t restorers[NSIG];
  uint64_t pending;
  uint64_t blocked;
  uint64_t altStackSp;
  uint64_t altStackSize;
  uint32_t altStackFlags;   // SS_DISABLE or 0; on the stack or not is where rsp is
  SignalInfo info[NSIG];
  // sigsuspend: the mask to give back once a handler has run (its frame keeps
  // it), or on the way out if none ran.
  bool suspended = false;
  uint64_t suspendSavedMask = 0;
};

// Linux ignores SIGCHLD, SIGCONT, SIGURG and SIGWINCH by default. Its default
// for SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU is to stop the process, which this
// kernel cannot do; ignoring them beats killing it (bash sends itself SIGTTIN
// until it owns its terminal, and dies before its first prompt otherwise).
inline bool signalIgnoredByDefault(int sig) {
  switch (sig) {
  case SIGCHLD:
  case 18:   // SIGCONT
  case 19:   // SIGSTOP
  case 20:   // SIGTSTP
  case 21:   // SIGTTIN
  case 22:   // SIGTTOU
  case 23:   // SIGURG
  case 28:   // SIGWINCH
    return true;
  default:
    return false;
  }
}

struct InterruptFrame;

class GDT;
struct ProcessSharedState;
// Address spaces (ProcessSharedState) alive (leak accounting, [mem]).
inline int64_t g_liveSharedStates = 0;

class Process;

struct ThreadObject {
  uint32_t tid;
  uint32_t refCount;
  bool completed;
  int exitCode;
  Process *process;
  uint32_t joiners = 0;   // asleep in ThreadJoin

  void retain() { __sync_add_and_fetch(&refCount, 1); }
  void release() {
    if (__sync_sub_and_fetch(&refCount, 1) == 0) {
      delete this;
    }
  }
};

// A process's place on the futex queues (src/cpu/syscall/futex.cpp).
struct FutexWaiter {
  uint64_t space = 0;
  uint64_t address = 0;
  Process *next = nullptr;
  bool queued = false;   // FutexWait sleeps until FutexWake clears it
};
void futexForget(Process *process);

class Process {
public:
  Process(uint32_t pid);
  // A thread of sharedFrom's process. kNoUserStack: it brings its own stack
  // (mlibc's), and the kernel makes none.
  static constexpr uint64_t kNoUserStack = ~0ULL;
  Process(uint32_t pid, Process *sharedFrom, uint64_t stackSize);
  ~Process();

  uint32_t getPID() const { return pid; }
  // The process's id: its main thread's pid. Threads have pids of their own,
  // but children, wait and the parent are the process's.
  uint32_t getGroupPID() const { return groupPID; }
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
  // The signal it died of (wait reports WIFSIGNALED), 0 if it exited.
  int termSignal = 0;

  Process *next;
  Process *allNext;
  FutexWaiter futex;

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
  // `info` null: sent by the kernel (SI_KERNEL).
  void sendSignal(int sig, const SignalInfo* info = nullptr);
  // A signal is pending and not blocked (no lock needed; a hint for the
  // scheduler tick's shortcut).
  bool signalMayBeDeliverable() const {
    return (__atomic_load_n(&signalHandler.pending, __ATOMIC_RELAXED) &
            ~__atomic_load_n(&signalHandler.blocked, __ATOMIC_RELAXED)) != 0;
  }
  // Set by sigreturn: the context to go back to is a whole one (an interrupt
  // may have stopped it anywhere), so the syscall returns by iretq, not sysret
  // (which loses rcx and r11).
  bool fullUserRestore = false;
  uint64_t lastSyscall = ~0ULL;   // for SA_RESTART

  // ITIMER_REAL (setitimer/alarm): SIGALRM at alarmDeadlineMs (0 = off),
  // then every alarmIntervalMs if nonzero. Checked by the boot CPU's tick.
  uint64_t alarmDeadlineMs = 0;
  uint64_t alarmIntervalMs = 0;
  // Acts on the pending signals before the return to user mode: runs a
  // handler (a frame on the user stack, the context moved to the handler) or
  // the default action. `atSyscallExit`: the context is that of a syscall
  // returning (SA_RESTART applies; handlers without a restorer may run).
  void handlePendingSignals(bool atSyscallExit = true);
  // The same on the way out of an interrupt or exception taken in user mode.
  // False if the process was terminated (the caller must not return to it).
  bool deliverSignalsToFrame(InterruptFrame* frame);
  // A fault in user mode (`signal` with `info`): true if a handler takes it
  // (the frame now enters it); false if it must die (no handler, ignored or
  // blocked: Linux's force_sig).
  bool takeFaultSignal(InterruptFrame* frame, int signal, const SignalInfo& info);
  // sigaction for the whole process: every thread shares the dispositions.
  void setSignalAction(int sig, uint64_t handler, uint64_t mask, uint64_t flags, uint64_t restorer);
  // sigreturn: the frame at the user's rsp back into the context.
  bool restoreSignalFrame();
  // Dies of `sig` as its default action would (the whole thread group).
  void dieOfSignal(int sig);
  bool hasDeliverableSignal() const;
  bool killPending() const { return (signalHandler.pending & (1ULL << SIGKILL)) != 0; }
  // Acts on a pending SIGKILL only, for paths that cannot deliver a handler
  // (an IRQ taken in user mode). True if this terminated the process.
  bool terminateIfKilled();
  // Kills the process like SIGKILL but reports `code` as its exit status
  // (another thread called exit() or crashed).
  void requestExit(int code, int signal = 0) {
    exitRequested = true;
    exitCode = code;
    termSignal = signal;
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
  // Pages already mapped (kSharedFrame) that `object` owns: the region holds
  // a reference to it until munmap, exit or exec drops the region.
  // `offset` is where in the object the region starts.
  bool mmapAddObjectRegion(uint64_t start, uint64_t length, uint64_t prot, struct MappedObject* object,
                           uint64_t offset = 0);
  // True where the address is in an object's region (shared between processes),
  // after making sure its page is there.
  bool mmapTouchShared(uint64_t addr);
  void mmapRemoveRange(uint64_t start, uint64_t length);
  bool mmapProtectRange(uint64_t start, uint64_t length, uint64_t prot);
  // Write back the present pages of any writable MAP_SHARED file region that
  // overlaps [start, start+length) to its backing file (msync / pre-munmap
  // flush). No-op for anonymous and read-only/private regions.
  void mmapSyncRange(uint64_t start, uint64_t length);
  bool mmapRegionCovers(uint64_t addr) const;   // an accessible region contains addr?
  uint64_t mmapRegionCount() const;             // leak hunting (exec.cpp [mem] dump)
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
  // spawn/fork). skipCloseOnExec excludes FD_CLOEXEC handles; filesOnly
  // keeps just files, pipes and sockets.
  void cloneHandlesFrom(Process *source, bool skipCloseOnExec, bool filesOnly = false);

  // P3.2 exec fd-inheritance: sys_exec keeps the kernel handle table across an
  // image replacement (dropping CLOEXEC), but the new libc reinitializes its
  // userspace fd-number -> handle map. libc stashes that map here just before
  // exec and fetches it at entry; the buffer is a plain Process member so it
  // survives replaceImageFrom(). fetch validates each entry against the live
  // handle table so CLOEXEC-closed fds are dropped.
  // Up to RLIMIT_NOFILE's ceiling; the buffer is made to measure at stash time.
  static constexpr int kFdStashMax = 65536;
  uint64_t *getFdStash() { return fdStash; }
  int getFdStashCount() const { return fdStashCount; }
  void setFdStashCount(int count) { fdStashCount = count; }
  // A stash buffer of `count` entries (the old one goes); nullptr if out of memory.
  uint64_t *makeFdStash(int count);
  void dropFdStash();

  // RLIMIT_NOFILE, kept with the handle table (shared by threads, copied to
  // fork and spawn children, kept across exec).
  void getNofileLimit(uint64_t *soft, uint64_t *hard) const;
  void setNofileLimit(uint64_t soft, uint64_t hard);
  int openHandleCount() const;

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
  uint32_t groupPID;
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
  uint64_t *fdStash = nullptr;
  int fdStashCapacity = 0;
  int fdStashCount = -1;
};
