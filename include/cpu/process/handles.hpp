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
  Pipe
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
};

class HandleTable {
public:
  static constexpr int MaxHandles = 1024;
  static constexpr int FirstAllocHandle = 3;
  static constexpr uint64_t TypeShift = 48;
  static constexpr uint64_t SlotMask = 0xFFFFFFFFULL;

  HandleTable();
  ~HandleTable();

  uint64_t allocate(HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release);
  uint64_t allocateAt(uint64_t handle, HandleType type, uint32_t rights, void* object, HandleRetainFn retain, HandleReleaseFn release);
  bool close(uint64_t handle);
  bool close(uint64_t handle, HandleType expectedType);
  void closeAll();

  HandleEntry* get(uint64_t handle);
  const HandleEntry* get(uint64_t handle) const;
  void* getObject(uint64_t handle, HandleType expectedType, uint32_t requiredRights = HandleRightNone);

  uint64_t duplicate(uint64_t handle);
  uint64_t duplicate(uint64_t handle, HandleType expectedType);
  bool duplicateTo(uint64_t oldHandle, uint64_t newHandle);
  bool duplicateTo(uint64_t oldHandle, uint64_t newHandle, HandleType expectedType);

  static uint64_t encodeHandle(HandleType type, int slot);
  static bool decodeHandle(uint64_t handle, HandleType* type, int* slot);

private:
  HandleEntry entries[MaxHandles];

  bool isValidSlot(int slot) const;
  void clearEntry(int slot);
  void releaseEntry(HandleEntry& entry);
};
