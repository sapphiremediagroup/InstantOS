#pragma once

#include <stddef.h>
#include <stdint.h>

class AHCIController;
class BlockDevice;

// The GPT partitions of every AHCI disk, scanned once at boot, for the
// installer (instant-install) and the /dev/diskNpM nodes. Only partitions that
// are InstantOS's by the rule in partition.hpp (the InstantOS data type, or
// Windows basic data holding FAT32 labelled INSTANTOS or ext4 labelled
// InstantOS) can be written, mounted
// or re-typed, so nothing here can touch another system's partitions; the
// rule is applied at the boot scan and holds for the session, so a partition
// that was InstantOS's stays so while instant-install formats it.
class DiskRegistry {
public:
    struct Partition {
        uint32_t disk;          // AHCI port order among active disks, from 0
        uint32_t index;         // GPT entry number, from 1
        uint64_t startByte;
        uint64_t lengthByte;
        uint8_t typeGuid[16];   // as GPT stores them
        uint8_t uniqueGuid[16];
        char name[40];          // the GPT name, ASCII
        bool instantOS;         // eligible (see above)
        bool esp;               // the EFI system partition
        bool mounted;           // the root, or mounted by PartitionMount
        BlockDevice* device;    // a PartitionBlockDevice (eligible ones only)
    };

    static constexpr size_t kMaxPartitions = 64;

    // rootPort/rootStart: the partition mounted as / (port -1: none).
    static void scan(AHCIController* controller, int rootPort, uint64_t rootStart);

    static size_t count();
    static Partition* at(size_t i);
    static Partition* find(uint32_t disk, uint32_t index);

    // Gives an eligible, unmounted partition the InstantOS data type in both
    // GPT copies (CRCs recomputed). 0 or a negative errno.
    static int setInstantOSType(Partition& p);
};
