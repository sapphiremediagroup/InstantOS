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
