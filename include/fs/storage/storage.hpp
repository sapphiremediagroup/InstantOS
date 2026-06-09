#pragma once

#include <cpu/syscall/syscall.hpp>

class BlockDevice;
class FAT32FS;

namespace KernelStorage {

void reset();
void setDevice(const char* name, BlockDevice* device, uint64_t totalSize, uint32_t sectorSize, bool readable, bool writable);
void setMount(const char* path, const char* fsType, int mountError);
StorageInfo snapshot();
int formatFat32();
int mountFat32();

}
