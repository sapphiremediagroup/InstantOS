#include <fs/storage/storage.hpp>
#include <fs/fat32/fat32.hpp>
#include <fs/vfs/vfs.hpp>
#include <memory/heap.hpp>

namespace {

StorageInfo storageInfo = {};
BlockDevice* storageDevice = nullptr;
FAT32FS* mountedFat32 = nullptr;

void copyString(char* destination, uint64_t destinationSize, const char* source)
{
    if (!destination || destinationSize == 0) {
        return;
    }

    uint64_t i = 0;
    if (source) {
        while (i + 1 < destinationSize && source[i] != '\0') {
            destination[i] = source[i];
            i++;
        }
    }
    destination[i] = '\0';
}

void zero(void* buffer, uint64_t size)
{
    auto* bytes = reinterpret_cast<uint8_t*>(buffer);
    for (uint64_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

void putFatEntry(uint8_t* sector, uint32_t index, uint32_t value)
{
    const uint32_t offset = index * 4;
    sector[offset + 0] = value & 0xFF;
    sector[offset + 1] = (value >> 8) & 0xFF;
    sector[offset + 2] = (value >> 16) & 0xFF;
    sector[offset + 3] = (value >> 24) & 0xFF;
}

uint32_t ceilDiv(uint64_t value, uint64_t divisor)
{
    return static_cast<uint32_t>((value + divisor - 1) / divisor);
}

uint8_t chooseSectorsPerCluster(uint64_t totalSectors)
{
    if (totalSectors < 64 * 1024) return 1;
    if (totalSectors < 512 * 1024) return 4;
    if (totalSectors < 2 * 1024 * 1024) return 8;
    return 16;
}

}

namespace KernelStorage {

void reset()
{
    storageInfo = {};
    storageDevice = nullptr;
    mountedFat32 = nullptr;
}

void setDevice(const char* name, BlockDevice* device, uint64_t totalSize, uint32_t sectorSize, bool readable, bool writable)
{
    storageDevice = device;
    storageInfo.totalSize = totalSize;
    storageInfo.sectorSize = sectorSize;
    storageInfo.flags |= StorageInfoPresent;
    if (readable) {
        storageInfo.flags |= StorageInfoReadable;
    }
    if (writable) {
        storageInfo.flags |= StorageInfoWritable;
    }
    copyString(storageInfo.deviceName, sizeof(storageInfo.deviceName), name);
}

void setMount(const char* path, const char* fsType, int mountError)
{
    storageInfo.mountError = mountError;
    copyString(storageInfo.mountPath, sizeof(storageInfo.mountPath), path);
    copyString(storageInfo.fsType, sizeof(storageInfo.fsType), fsType);

    if (mountError == 0) {
        storageInfo.flags |= StorageInfoMounted | StorageInfoFormatted;
    } else {
        storageInfo.flags &= ~(StorageInfoMounted | StorageInfoFormatted);
    }
}

StorageInfo snapshot()
{
    return storageInfo;
}

int formatFat32()
{
    if (!storageDevice || storageInfo.sectorSize != 512 || storageInfo.totalSize < 4 * 1024 * 1024) {
        return -1;
    }
    if ((storageInfo.flags & StorageInfoWritable) == 0) {
        return -1;
    }
    if ((storageInfo.flags & StorageInfoMounted) != 0) {
        return -1;
    }

    const uint32_t totalSectors = static_cast<uint32_t>(storageInfo.totalSize / 512);
    const uint32_t reservedSectors = 32;
    const uint32_t numFats = 2;
    const uint8_t sectorsPerCluster = chooseSectorsPerCluster(totalSectors);

    uint32_t fatSectors = 1;
    for (int i = 0; i < 8; ++i) {
        const uint32_t dataSectors = totalSectors - reservedSectors - numFats * fatSectors;
        const uint32_t clusters = dataSectors / sectorsPerCluster;
        fatSectors = ceilDiv((static_cast<uint64_t>(clusters) + 2) * 4, 512);
    }

    uint8_t sector[512];
    zero(sector, sizeof(sector));

    auto* bpb = reinterpret_cast<FAT32BPB*>(sector);
    bpb->jmp[0] = 0xEB;
    bpb->jmp[1] = 0x58;
    bpb->jmp[2] = 0x90;
    const char oem[8] = {'I', 'N', 'S', 'T', 'A', 'N', 'T', ' '};
    for (int i = 0; i < 8; ++i) bpb->oem[i] = oem[i];
    bpb->bytesPerSector = 512;
    bpb->sectorsPerCluster = sectorsPerCluster;
    bpb->reservedSectors = reservedSectors;
    bpb->numFATs = numFats;
    bpb->rootEntryCount = 0;
    bpb->media = 0xF8;
    bpb->fatSize16 = 0;
    bpb->sectorsPerTrack = 63;
    bpb->numHeads = 255;
    bpb->totalSectors32 = totalSectors;
    bpb->fatSize32 = fatSectors;
    bpb->rootCluster = 2;
    bpb->fsInfo = 1;
    bpb->backupBootSector = 6;
    bpb->driveNumber = 0x80;
    bpb->bootSignature = 0x29;
    bpb->volumeID = 0x4953544F;
    const char label[11] = {'I','N','S','T','A','N','T','O','S',' ',' '};
    for (int i = 0; i < 11; ++i) bpb->volumeLabel[i] = label[i];
    const char fsType[8] = {'F','A','T','3','2',' ',' ',' '};
    for (int i = 0; i < 8; ++i) bpb->fsType[i] = fsType[i];
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (!storageDevice->write(0, sector, sizeof(sector))) {
        return -1;
    }
    if (!storageDevice->write(6 * 512, sector, sizeof(sector))) {
        return -1;
    }

    zero(sector, sizeof(sector));
    sector[0] = 0x52;
    sector[1] = 0x52;
    sector[2] = 0x61;
    sector[3] = 0x41;
    sector[484] = 0x72;
    sector[485] = 0x72;
    sector[486] = 0x41;
    sector[487] = 0x61;
    sector[488] = 0xFF;
    sector[489] = 0xFF;
    sector[490] = 0xFF;
    sector[491] = 0xFF;
    sector[492] = 0x03;
    sector[493] = 0x00;
    sector[494] = 0x00;
    sector[495] = 0x00;
    sector[510] = 0x55;
    sector[511] = 0xAA;
    if (!storageDevice->write(512, sector, sizeof(sector))) {
        return -1;
    }

    zero(sector, sizeof(sector));
    for (uint32_t fat = 0; fat < numFats; ++fat) {
        const uint64_t fatStart = (reservedSectors + fat * fatSectors) * 512ULL;
        for (uint32_t sectorIndex = 0; sectorIndex < fatSectors; ++sectorIndex) {
            zero(sector, sizeof(sector));
            if (sectorIndex == 0) {
                putFatEntry(sector, 0, 0x0FFFFFF8);
                putFatEntry(sector, 1, 0x0FFFFFFF);
                putFatEntry(sector, 2, 0x0FFFFFFF);
            }
            if (!storageDevice->write(fatStart + sectorIndex * 512ULL, sector, sizeof(sector))) {
                return -1;
            }
        }
    }

    const uint64_t rootClusterOffset = (reservedSectors + numFats * fatSectors) * 512ULL;
    zero(sector, sizeof(sector));
    for (uint32_t i = 0; i < sectorsPerCluster; ++i) {
        if (!storageDevice->write(rootClusterOffset + i * 512ULL, sector, sizeof(sector))) {
            return -1;
        }
    }

    storageInfo.flags |= StorageInfoFormatted;
    storageInfo.mountError = 0;
    copyString(storageInfo.fsType, sizeof(storageInfo.fsType), "fat32");
    return 0;
}

int mountFat32()
{
    if ((storageInfo.flags & StorageInfoMounted) != 0) {
        return 0;
    }
    if (!storageDevice) {
        setMount("/", "fat32", -1);
        return -1;
    }

    auto* fs = new FAT32FS(storageDevice);
    if (!fs) {
        setMount("/", "fat32", -1);
        return -1;
    }

    const int result = VFS::get().mount(fs, "/");
    if (result != 0) {
        delete fs;
        setMount("/", "fat32", result);
        return result;
    }

    mountedFat32 = fs;
    setMount("/", "fat32", 0);
    return 0;
}

}
