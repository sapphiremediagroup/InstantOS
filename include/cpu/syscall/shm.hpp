#pragma once

#include <stdint.h>

class Process;
class SharedPages;
class VNode;

// The pages behind a file whose MAP_SHARED mappings are the same memory in
// every process: a memfd, or a regular RamFS file. nullptr for other files.
SharedPages* sharedPagesOf(VNode* node);
// False once a seal forbids writing (memfds only).
bool sharedPagesMayWrite(VNode* node);
// False if a seal forbids giving it this size (memfds only).
bool sharedPagesMayResize(VNode* node, uint64_t size);
// fcntl F_ADD_SEALS (add) and F_GET_SEALS.
uint64_t memfdSeals(Process* process, uint64_t handle, bool add, uint64_t seals);
