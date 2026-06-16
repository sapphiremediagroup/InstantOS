#pragma once

#include <fs/fat32/fat32.hpp>
#include <stdint.h>

#define MBR_SIGNATURE 0xAA55
#define MBR_PARTITION_TABLE_OFFSET 0x1BE
#define MBR_MAX_PARTITIONS 4

// A single 16-byte MBR primary partition entry as laid out on disk.
struct __attribute__((packed)) MBRPartitionEntry {
    uint8_t status;        // 0x80 = bootable, 0x00 = inactive
    uint8_t firstCHS[3];   // CHS address of first sector (legacy, ignored)
    uint8_t type;          // partition type byte (0x0B/0x0C = FAT32, etc.)
    uint8_t lastCHS[3];    // CHS address of last sector (legacy, ignored)
    uint32_t lbaStart;     // starting sector (LBA)
    uint32_t sectorCount;  // number of sectors
};

// A BlockDevice view over a contiguous region of an underlying device.
// Offsets are translated by startByte; reads/writes are clamped to the
// partition length so a partition can never escape its bounds.
class PartitionBlockDevice : public BlockDevice {
public:
    PartitionBlockDevice(BlockDevice* base, uint64_t startByte, uint64_t lengthByte);
    bool read(uint64_t offset, void* buffer, uint64_t size) override;
    bool write(uint64_t offset, const void* buffer, uint64_t size) override;
    uint64_t getSize() override;

private:
    BlockDevice* base;
    uint64_t startByte;
    uint64_t lengthByte;
};

// Parse the MBR at sector 0 of `device` and locate the first usable FAT
// partition. On success returns true and fills *startByte / *lengthByte with
// the partition's byte offset and length. Returns false if there is no valid
// MBR signature or no FAT partition is found.
bool mbrFindFatPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte, uint8_t* typeOut);

// Returns true if a partition type byte denotes a FAT filesystem we support.
bool mbrIsFatType(uint8_t type);
