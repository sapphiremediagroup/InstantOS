#pragma once

#include <stddef.h>
#include <stdint.h>

enum class HandleType : uint16_t {
  None = 0,
  File,
  Process,
  Thread,
  Window,
  Surface,
  EventQueue,
  Service,
  Timer,
  SharedMemory,
  GpuContext,
  Font,
  Pipe,
  Socket
};

enum HandleRights : uint32_t {
  HandleRightNone = 0,
  HandleRightRead = 1 << 0,
  HandleRightWrite = 1 << 1,
  HandleRightMap = 1 << 2,
  HandleRightSignal = 1 << 3,
  HandleRightWait = 1 << 4,
  HandleRightControl = 1 << 5,
  HandleRightDuplicate = 1 << 6
};

using HandleRetainFn = void (*)(void* object);
using HandleReleaseFn = void (*)(void* object);

struct HandleEntry {
  HandleType type;
  uint32_t rights;
  void* object;
  HandleRetainFn retain;
  HandleReleaseFn release;
  bool closeOnExec;
};

// A process's handles (shared by its threads). Slots live in chunks of
// ChunkSlots made as they are needed and never moved or freed before the table
// is, so a HandleEntry* stays valid while its slot is open.
class HandleTable {
public:
  static constexpr int MaxHandles = 131072;
  static constexpr int ChunkSlots = 256;
  static constexpr int MaxChunks = MaxHandles / ChunkSlots;
  static constexpr int FirstAllocHandle = 3;
  static constexpr uint64_t TypeShift = 48;
  static constexpr uint64_t SlotMask = 0xFFFFFFFFULL;

  // RLIMIT_NOFILE (GetRlimit/SetRlimit). The kernel does not count handles
  // against it (they are more than files); mlibc keeps its fd numbers under it.
  static constexpr uint64_t DefaultNofileSoft = 1024;
  static constexpr uint64_t MaxNofile = 65536;
  uint64_t nofileSoft = DefaultNofileSoft;
  uint64_t nofileHard = MaxNofile;
  int openCount() const { return open; }

  HandleTable();
  ~HandleTable();

  uint64_t allocate(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec = false);
  uint64_t allocateAt(uint64_t handle, HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release, bool closeOnExec = false);
  bool close(uint64_t handle);
  bool close(uint64_t handle, HandleType expectedType);
  void closeAll();
  // Close only File-type handles (regular files, pipes, sockets, ptys). Used at
  // process termination to release file descriptions (e.g. pipe write ends)
  // without disturbing non-file handles (IPC/shmem) that have separate cleanup.
  void closeAllFiles();

  HandleEntry* get(uint64_t handle);
  const HandleEntry* get(uint64_t handle) const;
  void* getObject(uint64_t handle, HandleType expectedType, uint32_t requiredRights = HandleRightNone);

  uint64_t duplicate(uint64_t handle);
  uint64_t duplicate(uint64_t handle, HandleType expectedType);
  bool duplicateTo(uint64_t oldHandle, uint64_t newHandle);
  bool duplicateTo(uint64_t oldHandle, uint64_t newHandle, HandleType expectedType);
  bool getCloseOnExec(uint64_t handle, bool* enabled) const;
  bool setCloseOnExec(uint64_t handle, bool enabled);
  void closeOnExecHandles();

  // Copy all live handles from another table into this one (used by fork and
  // spawn for fd inheritance). Bumps each object's refcount via its retain fn.
  // closeOnExec handles are skipped when skipCloseOnExec is true; with
  // filesOnly, only file-like handles (files, pipes, sockets) are copied.
  void cloneFrom(const HandleTable& source, bool skipCloseOnExec, bool filesOnly = false);

  static uint64_t encodeHandle(HandleType type, int slot);
  static bool decodeHandle(uint64_t handle, HandleType* type, int* slot);

private:
  HandleEntry* chunks[MaxChunks] = {};
  int searchFrom = FirstAllocHandle;   // no free slot of allocate()'s below it
  int open = 0;

  HandleEntry* slotEntry(int slot) const;   // nullptr if its chunk is not made
  HandleEntry* makeSlot(int slot);          // makes the chunk; nullptr if out of memory
  bool isValidSlot(int slot) const;
  void clearEntry(int slot);
  void releaseEntry(HandleEntry& entry);
};
