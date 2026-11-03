#include <fs/partition/partition.hpp>
#include <graphics/console.hpp>

PartitionBlockDevice::PartitionBlockDevice(BlockDevice* base, uint64_t startByte, uint64_t lengthByte)
    : base(base), startByte(startByte), lengthByte(lengthByte) {}

bool PartitionBlockDevice::read(uint64_t offset, void* buffer, uint64_t size) {
    if (!base) return false;
    if (offset > lengthByte || size > lengthByte - offset) return false;
    return base->read(startByte + offset, buffer, size);
}

bool PartitionBlockDevice::write(uint64_t offset, const void* buffer, uint64_t size) {
    if (!base) return false;
    if (offset > lengthByte || size > lengthByte - offset) return false;
    return base->write(startByte + offset, buffer, size);
}

uint64_t PartitionBlockDevice::getSize() {
    return lengthByte;
}

bool PartitionBlockDevice::flush() {
    return base && base->flush();
}

bool mbrIsFatType(uint8_t type) {
    switch (type) {
        case 0x01: // FAT12
        case 0x04: // FAT16 <32M
        case 0x06: // FAT16
        case 0x0B: // FAT32 CHS
        case 0x0C: // FAT32 LBA
        case 0x0E: // FAT16 LBA
            return true;
        default:
            return false;
    }
}

bool mbrFindFatPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte, uint8_t* typeOut) {
    if (!device) return false;

    uint8_t sector[512];
    if (!device->read(0, sector, 512)) {
        Console::get().drawText("[MBR] Failed to read sector 0\n");
        return false;
    }

    uint16_t signature = (uint16_t)sector[510] | ((uint16_t)sector[511] << 8);
    if (signature != MBR_SIGNATURE) {
        Console::get().drawText("[MBR] No MBR signature (0xAA55)\n");
        return false;
    }

    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        MBRPartitionEntry entry;
        const uint8_t* src = sector + MBR_PARTITION_TABLE_OFFSET + i * 16;
        for (size_t j = 0; j < sizeof(MBRPartitionEntry); j++) {
            ((uint8_t*)&entry)[j] = src[j];
        }

        if (entry.type == 0x00 || entry.sectorCount == 0) {
            continue;
        }

        if (!mbrIsFatType(entry.type)) {
            continue;
        }

        if (startByte) *startByte = (uint64_t)entry.lbaStart * 512;
        if (lengthByte) *lengthByte = (uint64_t)entry.sectorCount * 512;
        if (typeOut) *typeOut = entry.type;

        Console::get().drawText("[MBR] FAT partition ");
        Console::get().drawNumber(i);
        Console::get().drawText(" type=0x");
        Console::get().drawNumber(entry.type);
        Console::get().drawText(" lbaStart=");
        Console::get().drawNumber((int64_t)entry.lbaStart);
        Console::get().drawText(" sectors=");
        Console::get().drawNumber((int64_t)entry.sectorCount);
        Console::get().drawText("\n");
        return true;
    }

    Console::get().drawText("[MBR] No FAT partition found\n");
    return false;
}

bool mbrIsLinuxType(uint8_t type) {
    // 0x83 = Linux native (ext2/3/4 and other Linux filesystems).
    return type == 0x83;
}

bool mbrFindPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte, uint8_t* typeOut) {
    if (!device) return false;

    uint8_t sector[512];
    if (!device->read(0, sector, 512)) {
        Console::get().drawText("[MBR] Failed to read sector 0\n");
        return false;
    }

    uint16_t signature = (uint16_t)sector[510] | ((uint16_t)sector[511] << 8);
    if (signature != MBR_SIGNATURE) {
        Console::get().drawText("[MBR] No MBR signature (0xAA55)\n");
        return false;
    }

    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        MBRPartitionEntry entry;
        const uint8_t* src = sector + MBR_PARTITION_TABLE_OFFSET + i * 16;
        for (size_t j = 0; j < sizeof(MBRPartitionEntry); j++) {
            ((uint8_t*)&entry)[j] = src[j];
        }

        if (entry.type == 0x00 || entry.sectorCount == 0) {
            continue;
        }
        if (!mbrIsFatType(entry.type) && !mbrIsLinuxType(entry.type)) {
            continue;
        }

        if (startByte) *startByte = (uint64_t)entry.lbaStart * 512;
        if (lengthByte) *lengthByte = (uint64_t)entry.sectorCount * 512;
        if (typeOut) *typeOut = entry.type;

        Console::get().drawText("[MBR] partition ");
        Console::get().drawNumber(i);
        Console::get().drawText(" type=0x");
        Console::get().drawNumber(entry.type);
        Console::get().drawText(" lbaStart=");
        Console::get().drawNumber((int64_t)entry.lbaStart);
        Console::get().drawText(" sectors=");
        Console::get().drawNumber((int64_t)entry.sectorCount);
        Console::get().drawText("\n");
        return true;
    }

    Console::get().drawText("[MBR] No supported partition found\n");
    return false;
}

// --- GPT ---------------------------------------------------------------------

namespace {

bool gptSignatureOk(const uint8_t* block) {
    // "EFI PART"
    return block[0] == 'E' && block[1] == 'F' && block[2] == 'I' && block[3] == ' ' &&
           block[4] == 'P' && block[5] == 'A' && block[6] == 'R' && block[7] == 'T';
}

uint32_t rd32le(const uint8_t* p, int off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

uint64_t rd64le(const uint8_t* p, int off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[off + i] << (8 * i);
    }
    return v;
}

bool guidIsZero(const uint8_t* g) {
    for (int i = 0; i < 16; i++) {
        if (g[i]) return false;
    }
    return true;
}

bool guidEqual(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

}  // namespace

// GUIDs as GPT stores them (the first three fields little-endian).
// Windows "basic data": ebd0a0a2-b9e5-4433-87c0-68b6b72699c7.
static const uint8_t kGptBasicDataGuid[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static const char kInstantOSLabel[11] = { 'I', 'N', 'S', 'T', 'A', 'N', 'T', 'O', 'S', ' ', ' ' };

static bool isInstantOSLabel(const uint8_t* label) {
    for (int i = 0; i < 11; i++) {
        if (label[i] != (uint8_t)kInstantOSLabel[i]) return false;
    }
    return true;
}

static uint16_t rd16le(const uint8_t* p, int off) {
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

// Whether the FAT32 volume at `startByte` is labelled INSTANTOS. The label is
// in two places: the boot sector's extended BPB, and a volume-label entry in
// the root directory. Windows' format writes "NO NAME" in the first and the
// real label only in the second, so both are checked.
static bool fat32LabelIsInstantOS(BlockDevice* device, uint64_t startByte) {
    uint8_t boot[512];
    if (!device->read(startByte, boot, 512) || boot[510] != 0x55 || boot[511] != 0xAA) {
        return false;
    }
    static const char kFat32[8] = { 'F', 'A', 'T', '3', '2', ' ', ' ', ' ' };
    for (int i = 0; i < 8; i++) {
        if (boot[0x52 + i] != (uint8_t)kFat32[i]) return false;
    }
    if (isInstantOSLabel(boot + 0x47)) {
        return true;
    }

    // The first cluster of the root directory.
    const uint32_t bytesPerSector = rd16le(boot, 0x0B);
    const uint32_t sectorsPerCluster = boot[0x0D];
    const uint32_t reserved = rd16le(boot, 0x0E);
    const uint32_t fats = boot[0x10];
    const uint32_t fatSize = rd32le(boot, 0x24);
    const uint32_t rootCluster = rd32le(boot, 0x2C);
    if (bytesPerSector != 512 || sectorsPerCluster == 0 || rootCluster < 2 || fats == 0) {
        return false;
    }
    const uint64_t dataStart = (uint64_t)reserved + (uint64_t)fats * fatSize;
    const uint64_t rootStart = startByte + (dataStart + (uint64_t)(rootCluster - 2) * sectorsPerCluster) * 512;
    uint8_t sector[512];
    for (uint32_t s = 0; s < sectorsPerCluster; s++) {
        if (!device->read(rootStart + (uint64_t)s * 512, sector, 512)) {
            return false;
        }
        for (int e = 0; e < 512; e += 32) {
            const uint8_t* entry = sector + e;
            if (entry[0] == 0x00) {
                return false;   // end of the directory
            }
            const uint8_t attr = entry[11];
            if (entry[0] != 0xE5 && attr != 0x0F && (attr & 0x08) != 0) {
                return isInstantOSLabel(entry);
            }
        }
    }
    return false;
}

// 9566fa50-7665-4f44-962e-113fd99cdc87.
const uint8_t kGptInstantOSDataGuid[16] = {
    0x50, 0xFA, 0x66, 0x95, 0x65, 0x76, 0x44, 0x4F,
    0x96, 0x2E, 0x11, 0x3F, 0xD9, 0x9C, 0xDC, 0x87
};

bool gptIsPresent(BlockDevice* device) {
    if (!device) return false;
    uint8_t block[512];
    if (!device->read((uint64_t)GPT_HEADER_LBA * 512, block, 512)) {
        return false;
    }
    return gptSignatureOk(block);
}

GptSearch gptFindInstantOSPartition(BlockDevice* device, uint64_t* startByte, uint64_t* lengthByte) {
    if (!device) return GptSearch::NoGpt;

    uint8_t hdr[512];
    if (!device->read((uint64_t)GPT_HEADER_LBA * 512, hdr, 512)) {
        return GptSearch::NoGpt;
    }
    if (!gptSignatureOk(hdr)) {
        return GptSearch::NoGpt;
    }

    const uint64_t entryLba = rd64le(hdr, 0x48);
    uint32_t numEntries = rd32le(hdr, 0x50);
    const uint32_t entrySize = rd32le(hdr, 0x54);
    if (entrySize < 128 || entrySize > 512 || entryLba == 0) {
        Console::get().drawText("[GPT] Invalid entry geometry\n");
        return GptSearch::NotInstantOS;
    }
    if (numEntries > 256) {   // cap the scan against corrupt/huge values
        numEntries = 256;
    }

    uint8_t entry[512];
    for (uint32_t i = 0; i < numEntries; i++) {
        const uint64_t off = entryLba * 512 + (uint64_t)i * entrySize;
        if (!device->read(off, entry, entrySize)) {
            break;
        }
        const bool ours = guidEqual(entry, kGptInstantOSDataGuid);
        if (!ours && !guidEqual(entry, kGptBasicDataGuid)) {
            continue;
        }
        const uint64_t firstLba = rd64le(entry, 0x20);
        const uint64_t lastLba = rd64le(entry, 0x28);
        if (lastLba < firstLba) {
            continue;
        }
        if (!ours && !fat32LabelIsInstantOS(device, firstLba * 512)) {
            continue;
        }
        if (startByte) *startByte = firstLba * 512;
        if (lengthByte) *lengthByte = (lastLba - firstLba + 1) * 512;
        return GptSearch::Found;
    }
    return GptSearch::NotInstantOS;
}
