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
    bool flush() override;

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

// Returns true if a partition type byte denotes a Linux filesystem (ext2/3/4).
bool mbrIsLinuxType(uint8_t type);

// Parse the MBR and locate the first usable partition holding a filesystem we
// support (FAT or Linux/ext*). On success fills *startByte / *lengthByte /
// *typeOut and returns true; returns false if there is no valid MBR signature
// or no supported partition is present. The caller probes the partition to
// decide which filesystem driver to mount.
bool mbrFindPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte, uint8_t* typeOut);

// --- GPT (GUID Partition Table): the partitioning scheme used by UEFI disks ---

#define GPT_HEADER_LBA 1
#define GPT_ENTRY_NAME_LEN 36   // UTF-16 code units

// Returns true if `device` has a primary GPT header ("EFI PART" at LBA 1).
bool gptIsPresent(BlockDevice* device);

// The GPT partitions InstantOS mounts: type 9566fa50-7665-4f44-962e-113fd99cdc87
// ("InstantOS data"), or a Windows "basic data" partition holding a FAT32
// volume labelled INSTANTOS (Windows can make that one and keeps showing it).
// A GPT disk is a real computer's disk, shared with other systems, so nothing
// else on it is ever mounted.
extern const uint8_t kGptInstantOSDataGuid[16];

enum class GptSearch {
    NoGpt,          // no valid GPT: an MBR or unpartitioned (test) image
    NotInstantOS,   // a GPT disk without an InstantOS data partition: leave it alone
    Found,
};

// Looks for the first InstantOS partition (above) on a GPT disk and fills
// *startByte / *lengthByte (bytes; 512-byte sectors). The caller probes it to
// pick the filesystem driver.
GptSearch gptFindInstantOSPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte);
