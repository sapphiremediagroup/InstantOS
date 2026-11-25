#include <fs/partition/disk_registry.hpp>

#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <fs/ahci/ahci.hpp>
#include <fs/partition/partition.hpp>
#include <fs/storage/storage_lock.hpp>
#include <memory/heap.hpp>

namespace {

DiskRegistry::Partition gPartitions[DiskRegistry::kMaxPartitions];
size_t gCount = 0;
BlockDevice* gDisks[32];   // the whole disk of each partition's `disk`

// EFI system partition: c12a7328-f81f-11d2-ba4b-00a0c93ec93b.
constexpr uint8_t kEspGuid[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                  0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t rd64(const uint8_t* p) {
    return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}

void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// CRC-32 (IEEE), as the GPT uses.
uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool headerOk(const uint8_t* h) {
    if (h[0] != 'E' || h[1] != 'F' || h[2] != 'I' || h[3] != ' ' || h[4] != 'P' || h[5] != 'A' || h[6] != 'R' ||
        h[7] != 'T') {
        return false;
    }
    const uint32_t size = rd32(h + 12);
    if (size < 92 || size > 512) {
        return false;
    }
    uint8_t copy[512];
    memcpy(copy, h, size);
    wr32(copy + 16, 0);
    return crc32(copy, size) == rd32(h + 16);
}

// One GPT copy (the header at headerLba): writes the entry array and the
// header with fresh CRCs. The caller checked the header.
bool writeGptCopy(BlockDevice* disk, uint64_t headerLba, const uint8_t* entries, size_t entriesBytes) {
    uint8_t header[512];
    if (!disk->read(headerLba * 512, header, 512) || !headerOk(header)) {
        return false;
    }
    const uint64_t entryLba = rd64(header + 72);
    if (!disk->write(entryLba * 512, entries, entriesBytes)) {
        return false;
    }
    wr32(header + 88, crc32(entries, entriesBytes));
    wr32(header + 16, 0);
    wr32(header + 16, crc32(header, rd32(header + 12)));
    return disk->write(headerLba * 512, header, 512);
}

}  // namespace

void DiskRegistry::scan(AHCIController* controller, int rootPort, uint64_t rootStart) {
    gCount = 0;
    if (!controller) {
        return;
    }
    StorageLock::Section storage;
    uint32_t disk = 0;
    for (int port = 0; port < controller->getPortCount() && disk < 32; ++port) {
        AHCIPort* ahciPort = controller->getPort(port);
        if (!ahciPort || !ahciPort->isActive()) {
            continue;
        }
        SATABlockDevice* device = new SATABlockDevice(ahciPort);
        gDisks[disk] = device;
        uint8_t header[512];
        if (device->read(512, header, 512) && headerOk(header)) {
            const uint64_t entryLba = rd64(header + 72);
            uint32_t entries = rd32(header + 80);
            const uint32_t entrySize = rd32(header + 84);
            if (entries > 256) {
                entries = 256;
            }
            uint8_t entry[512];
            for (uint32_t i = 0; i < entries && entrySize >= 128 && entrySize <= 512 && gCount < kMaxPartitions; ++i) {
                if (!device->read(entryLba * 512 + static_cast<uint64_t>(i) * entrySize, entry, entrySize)) {
                    break;
                }
                bool empty = true;
                for (int b = 0; b < 16; ++b) {
                    empty = empty && entry[b] == 0;
                }
                if (empty) {
                    continue;
                }
                Partition& p = gPartitions[gCount++];
                p = {};
                p.disk = disk;
                p.index = i + 1;
                p.startByte = rd64(entry + 0x20) * 512;
                p.lengthByte = (rd64(entry + 0x28) - rd64(entry + 0x20) + 1) * 512;
                memcpy(p.typeGuid, entry, 16);
                memcpy(p.uniqueGuid, entry + 0x10, 16);
                for (int c = 0; c < 36 && c < static_cast<int>(sizeof(p.name)) - 1; ++c) {
                    const uint16_t ch = static_cast<uint16_t>(entry[0x38 + c * 2] | (entry[0x39 + c * 2] << 8));
                    if (ch == 0) {
                        break;
                    }
                    p.name[c] = ch < 0x80 ? static_cast<char>(ch) : '?';
                }
                p.esp = memcmp(p.typeGuid, kEspGuid, 16) == 0;
                p.instantOS = gptEntryIsInstantOS(device, entry);
                p.mounted = port == rootPort && p.startByte == rootStart;
                if (p.instantOS) {
                    p.device = new PartitionBlockDevice(device, p.startByte, p.lengthByte);
                }
            }
        }
        ++disk;
    }
    Cereal::get().write("[storage] partitions: ");
    for (size_t i = 0; i < gCount; ++i) {
        const Partition& p = gPartitions[i];
        char text[48];
        size_t n = 0;
        auto put = [&](const char* s) {
            while (*s && n + 1 < sizeof(text)) {
                text[n++] = *s++;
            }
        };
        put("disk");
        text[n++] = static_cast<char>('0' + p.disk % 10);
        put("p");
        if (p.index >= 10) {
            text[n++] = static_cast<char>('0' + p.index / 10 % 10);
        }
        text[n++] = static_cast<char>('0' + p.index % 10);
        put(p.instantOS ? (p.mounted ? "(instantos,/) " : "(instantos) ") : (p.esp ? "(esp) " : " "));
        text[n] = 0;
        Cereal::get().write(text);
    }
    Cereal::get().write("\n");
}

size_t DiskRegistry::count() {
    return gCount;
}

DiskRegistry::Partition* DiskRegistry::at(size_t i) {
    return i < gCount ? &gPartitions[i] : nullptr;
}

DiskRegistry::Partition* DiskRegistry::find(uint32_t disk, uint32_t index) {
    for (size_t i = 0; i < gCount; ++i) {
        if (gPartitions[i].disk == disk && gPartitions[i].index == index) {
            return &gPartitions[i];
        }
    }
    return nullptr;
}

int DiskRegistry::setInstantOSType(Partition& p) {
    if (!p.instantOS) {
        return -13;   // EACCES: not InstantOS's
    }
    if (p.mounted) {
        return -16;   // EBUSY
    }
    BlockDevice* disk = p.disk < 32 ? gDisks[p.disk] : nullptr;
    if (!disk) {
        return -19;
    }
    StorageLock::Section storage;
    uint8_t header[512];
    if (!disk->read(512, header, 512) || !headerOk(header)) {
        return -5;
    }
    const uint64_t entryLba = rd64(header + 72);
    const uint32_t entries = rd32(header + 80);
    const uint32_t entrySize = rd32(header + 84);
    const uint64_t backupLba = rd64(header + 32);
    const size_t bytes = static_cast<size_t>(entries) * entrySize;
    if (entrySize < 128 || entrySize > 512 || bytes == 0 || bytes > 64 * 1024 || p.index == 0 || p.index > entries) {
        return -22;
    }
    auto* table = static_cast<uint8_t*>(kmalloc(bytes));
    if (!table) {
        return -12;
    }
    int result = -5;
    if (disk->read(entryLba * 512, table, bytes) && crc32(table, bytes) == rd32(header + 88)) {
        uint8_t* entry = table + static_cast<size_t>(p.index - 1) * entrySize;
        if (memcmp(entry + 0x10, p.uniqueGuid, 16) == 0) {
            memcpy(entry, kGptInstantOSDataGuid, 16);
            // The backup first: if the primary write then fails, firmware and
            // tools still find a consistent table (they repair from it).
            const bool backup = writeGptCopy(disk, backupLba, table, bytes);
            const bool primary = writeGptCopy(disk, 1, table, bytes);
            disk->flush();
            if (backup && primary) {
                memcpy(p.typeGuid, kGptInstantOSDataGuid, 16);
                result = 0;
            }
        } else {
            result = -22;
        }
    }
    kfree(table);
    Cereal::get().write(result == 0 ? "[storage] partition given the InstantOS type\n"
                                    : "[storage] setting the InstantOS type failed\n");
    return result;
}
