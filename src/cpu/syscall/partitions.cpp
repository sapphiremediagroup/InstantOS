// PartitionList / PartitionSetType / PartitionMount: the disks' GPT partitions
// (DiskRegistry), for instant-install. Only InstantOS partitions can be
// re-typed or mounted; nothing here reaches another system's.
#include <cpu/syscall/syscall.hpp>

#include <common/string.hpp>
#include <cpu/process/process.hpp>
#include <cpu/process/scheduler.hpp>
#include <fs/ext4/ext4.hpp>
#include <fs/partition/disk_registry.hpp>
#include <fs/vfs/vfs.hpp>

namespace {
bool privileged() {
    Process* current = Scheduler::get().getCurrentProcess();
    return current && current->isPrivileged();
}

uint64_t errnoResult(int negative) {
    return static_cast<uint64_t>(static_cast<int64_t>(negative));
}
}  // namespace

uint64_t Syscall::sys_partition_list(uint64_t buffer, uint64_t capacity) {
    const size_t count = DiskRegistry::count();
    for (size_t i = 0; buffer != 0 && i < count && i < capacity; ++i) {
        const DiskRegistry::Partition* p = DiskRegistry::at(i);
        PartitionRecord record = {};
        record.disk = p->disk;
        record.index = p->index;
        record.startByte = p->startByte;
        record.lengthByte = p->lengthByte;
        memcpy(record.typeGuid, p->typeGuid, 16);
        memcpy(record.uniqueGuid, p->uniqueGuid, 16);
        memcpy(record.name, p->name, sizeof(record.name));
        record.flags = (p->instantOS ? PartitionInstantOS : 0u) | (p->esp ? PartitionEsp : 0u) |
                       (p->mounted ? PartitionMounted : 0u);
        if (!copyToUser(buffer + i * sizeof(PartitionRecord), &record, sizeof(record))) {
            return syscall_error(SysErrInvalid);
        }
    }
    return count;
}

uint64_t Syscall::sys_partition_set_type(uint64_t disk, uint64_t index) {
    if (!privileged()) {
        return syscall_error(SysErrPermission);
    }
    DiskRegistry::Partition* p = DiskRegistry::find(static_cast<uint32_t>(disk), static_cast<uint32_t>(index));
    if (!p) {
        return syscall_error(SysErrNoEntry);
    }
    const int result = DiskRegistry::setInstantOSType(*p);
    return result == 0 ? 0 : errnoResult(result);
}

uint64_t Syscall::sys_partition_mount(uint64_t disk, uint64_t index, uint64_t path) {
    if (!privileged()) {
        return syscall_error(SysErrPermission);
    }
    DiskRegistry::Partition* p = DiskRegistry::find(static_cast<uint32_t>(disk), static_cast<uint32_t>(index));
    if (!p) {
        return syscall_error(SysErrNoEntry);
    }
    if (!p->instantOS || !p->device) {
        return syscall_error(SysErrAccess);   // not InstantOS's: never mounted
    }
    if (p->mounted) {
        return syscall_error(SysErrBusy);
    }
    char where[256];
    if (path == 0 || !copyStringFromUser(path, where, sizeof(where)) || where[0] != '/') {
        return syscall_error(SysErrInvalid);
    }
    if (!Ext4FS::probe(p->device)) {
        return syscall_error(SysErrInvalid);   // only ext4 (instant-install's)
    }
    auto* fs = new Ext4FS(p->device);
    if (!fs) {
        return syscall_error(SysErrNoMemory);
    }
    const int result = VFS::get().mount(fs, where);
    if (result != 0) {
        delete fs;
        return syscall_error(SysErrIO);
    }
    p->mounted = true;
    return 0;
}
