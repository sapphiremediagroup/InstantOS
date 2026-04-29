#include <fs/fat32/fat32.hpp>
#include <fs/ahci/ahci.hpp>
#include <graphics/console.hpp>
#include <memory/heap.hpp>

FAT32FS::FAT32FS(BlockDevice* device) : FileSystem("fat32"), device(device), rootNode(nullptr), fatStart(0), dataStart(0), clusterSize(0), rootDirCluster(0) {
    ops.open = nodeOpen;
    ops.close = nodeClose;
    ops.read = nodeRead;
    ops.write = nodeWrite;
    ops.stat = nodeStat;
    ops.readdir = nodeReaddir;
    ops.lookup = nodeLookup;
    ops.create = nodeCreate;
    ops.mkdir = nodeMkdir;
    ops.unlink = nodeUnlink;
    ops.rmdir = nodeRmdir;
}

FAT32FS::~FAT32FS() {
    if (rootNode) {
        FAT32Node* node = (FAT32Node*)rootNode->getData();
        if (node) kfree(node);
        delete rootNode;
    }
}

int FAT32FS::mount(const char* path) {
    if (!device) {
        Console::get().drawText("[FAT32] No block device\n");
        return -1;
    }
    
    uint8_t buffer[512];
    if (!device->read(0, buffer, 512)) {
        Console::get().drawText("[FAT32] Failed to read boot sector\n");
        return -1;
    }
    
    for (size_t i = 0; i < sizeof(FAT32BPB) && i < 512; i++) {
        ((uint8_t*)&bpb)[i] = buffer[i];
    }
    
    if (bpb.bytesPerSector == 0 || bpb.sectorsPerCluster == 0) {
        Console::get().drawText("[FAT32] Invalid BPB geometry\n");
        return -1;
    }
    if (bpb.fatSize32 == 0) {
        Console::get().drawText("[FAT32] Not a FAT32 volume\n");
        return -1;
    }
    
    fatStart = bpb.reservedSectors;
    dataStart = bpb.reservedSectors + (bpb.numFATs * bpb.fatSize32);
    clusterSize = bpb.bytesPerSector * bpb.sectorsPerCluster;
    rootDirCluster = bpb.rootCluster;

    Console::get().drawText("[FAT32] bytes/sector: ");
    Console::get().drawNumber(bpb.bytesPerSector);
    Console::get().drawText(", sectors/cluster: ");
    Console::get().drawNumber(bpb.sectorsPerCluster);
    Console::get().drawText(", root cluster: ");
    Console::get().drawNumber(rootDirCluster);
    Console::get().drawText("\n");
    
    FAT32Node* rootData = (FAT32Node*)kmalloc(sizeof(FAT32Node));
    if (!rootData) {
        Console::get().drawText("[FAT32] Failed to allocate root node data\n");
        return -1;
    }
    
    rootData->name[0] = '/';
    rootData->name[1] = '\0';
    rootData->cluster = rootDirCluster;
    rootData->size = 0;
    rootData->attr = FAT32_ATTR_DIRECTORY;
    rootData->isDirectory = true;
    rootData->parentCluster = 0;
    
    rootNode = new VNode(this, rootDirCluster, FileType::Directory);
    if (!rootNode) {
        Console::get().drawText("[FAT32] Failed to allocate root vnode\n");
        kfree(rootData);
        return -1;
    }
    rootNode->setData(rootData);
    rootNode->ops = &ops;
    
    return 0;
}

int FAT32FS::unmount() {
    return 0;
}

VNode* FAT32FS::getRoot() {
    return rootNode;
}

uint64_t FAT32FS::clusterToSector(uint32_t cluster) {
    return dataStart + (cluster - 2) * bpb.sectorsPerCluster;
}

bool FAT32FS::readSector(uint64_t sector, void* buffer) {
    return device->read(sector * bpb.bytesPerSector, buffer, bpb.bytesPerSector);
}

bool FAT32FS::writeSector(uint64_t sector, const void* buffer) {
    return device->write(sector * bpb.bytesPerSector, buffer, bpb.bytesPerSector);
}

bool FAT32FS::readCluster(uint32_t cluster, void* buffer) {
    if (cluster < 2 || cluster >= FAT32_EOC) return false;
    
    uint64_t sector = clusterToSector(cluster);
    uint8_t* buf = (uint8_t*)buffer;
    
    for (uint32_t i = 0; i < bpb.sectorsPerCluster; i++) {
        if (!readSector(sector + i, buf + (i * bpb.bytesPerSector))) {
            return false;
        }
    }
    
    return true;
}

bool FAT32FS::writeCluster(uint32_t cluster, const void* buffer) {
    if (cluster < 2 || cluster >= FAT32_EOC) return false;
    
    uint64_t sector = clusterToSector(cluster);
    const uint8_t* buf = (const uint8_t*)buffer;
    
    for (uint32_t i = 0; i < bpb.sectorsPerCluster; i++) {
        if (!writeSector(sector + i, buf + (i * bpb.bytesPerSector))) {
            return false;
        }
    }
    
    return true;
}

bool FAT32FS::readFATEntry(uint32_t cluster, uint32_t* value) {
    if (!value) return false;
    
    uint32_t fatOffset = cluster * 4;
    uint32_t fatSector = fatStart + (fatOffset / bpb.bytesPerSector);
    uint32_t entryOffset = fatOffset % bpb.bytesPerSector;
    
    uint8_t* buffer = (uint8_t*)kmalloc(bpb.bytesPerSector);
    if (!buffer) return false;
    
    if (!readSector(fatSector, buffer)) {
        kfree(buffer);
        return false;
    }
    
    *value = *(uint32_t*)(buffer + entryOffset) & 0x0FFFFFFF;
    kfree(buffer);
    return true;
}

bool FAT32FS::writeFATEntry(uint32_t cluster, uint32_t value) {
    uint32_t fatOffset = cluster * 4;
    uint32_t fatSector = fatStart + (fatOffset / bpb.bytesPerSector);
    uint32_t entryOffset = fatOffset % bpb.bytesPerSector;
    
    uint8_t* buffer = (uint8_t*)kmalloc(bpb.bytesPerSector);
    if (!buffer) return false;
    
    if (!readSector(fatSector, buffer)) {
        kfree(buffer);
        return false;
    }
    
    uint32_t* entry = (uint32_t*)(buffer + entryOffset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
    
    bool result = writeSector(fatSector, buffer);
    kfree(buffer);
    
    if (result && bpb.numFATs > 1) {
        uint32_t fat2Sector = fatSector + bpb.fatSize32;
        buffer = (uint8_t*)kmalloc(bpb.bytesPerSector);
        if (buffer) {
            if (readSector(fat2Sector, buffer)) {
                entry = (uint32_t*)(buffer + entryOffset);
                *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
                writeSector(fat2Sector, buffer);
            }
            kfree(buffer);
        }
    }
    
    return result;
}

uint32_t FAT32FS::getNextCluster(uint32_t cluster) {
    uint32_t value;
    if (!readFATEntry(cluster, &value)) {
        return FAT32_EOC;
    }
    return value;
}

bool FAT32FS::setNextCluster(uint32_t cluster, uint32_t value) {
    return writeFATEntry(cluster, value);
}

uint32_t FAT32FS::allocateCluster() {
    uint32_t totalClusters = (bpb.totalSectors32 - dataStart) / bpb.sectorsPerCluster;
    
    for (uint32_t i = 2; i < totalClusters; i++) {
        uint32_t value;
        if (readFATEntry(i, &value) && value == FAT32_FREE) {
            if (writeFATEntry(i, FAT32_EOC)) {
                return i;
            }
        }
    }
    
    return 0;
}

bool FAT32FS::freeClusterChain(uint32_t startCluster) {
    uint32_t cluster = startCluster;
    
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = getNextCluster(cluster);
        if (!writeFATEntry(cluster, FAT32_FREE)) {
            return false;
        }
        cluster = next;
    }
    
    return true;
}

void FAT32FS::shortNameFromLong(const char* longName, char* shortName) {
    for (int i = 0; i < 11; i++) {
        shortName[i] = ' ';
    }
    
    int nameLen = 0;
    while (longName[nameLen] && longName[nameLen] != '.') nameLen++;
    
    int extLen = 0;
    if (longName[nameLen] == '.') {
        int extStart = nameLen + 1;
        while (longName[extStart + extLen]) extLen++;
    }
    
    for (int i = 0; i < 8 && i < nameLen; i++) {
        char c = longName[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        shortName[i] = c;
    }
    
    if (extLen > 0) {
        for (int i = 0; i < 3 && i < extLen; i++) {
            char c = longName[nameLen + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            shortName[8 + i] = c;
        }
    }
}

uint8_t FAT32FS::lfnChecksum(const char* shortName) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) << 7) + (sum >> 1) + shortName[i];
    }
    return sum;
}

FAT32Node* FAT32FS::createNodeFromEntry(FAT32DirEntry* entry, const char* longName) {
    FAT32Node* node = (FAT32Node*)kmalloc(sizeof(FAT32Node));
    if (!node) return nullptr;
    
    if (longName && longName[0]) {
        int i = 0;
        while (longName[i] && i < 255) {
            node->name[i] = longName[i];
            i++;
        }
        node->name[i] = '\0';
    } else {
        int nameLen = 0;
        for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
            char c = entry->name[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            node->name[nameLen++] = c;
        }
        
        if (entry->name[8] != ' ') {
            node->name[nameLen++] = '.';
            for (int i = 8; i < 11 && entry->name[i] != ' '; i++) {
                char c = entry->name[i];
                if (c >= 'A' && c <= 'Z') c += 32;
                node->name[nameLen++] = c;
            }
        }
        node->name[nameLen] = '\0';
    }
    
    node->cluster = ((uint32_t)entry->fstClusHI << 16) | entry->fstClusLO;
    node->size = entry->fileSize;
    node->attr = entry->attr;
    node->isDirectory = (entry->attr & FAT32_ATTR_DIRECTORY) != 0;
    node->parentCluster = 0;
    
    return node;
}

bool FAT32FS::findEntry(uint32_t dirCluster, const char* name, FAT32DirEntry* entry, uint32_t* entryCluster, uint32_t* entryOffset) {
    uint32_t cluster = dirCluster;
    char longName[256] = {0};
    int lfnIndex = 0;
    
    while (cluster >= 2 && cluster < FAT32_EOC) {
        void* buffer = kmalloc(clusterSize);
        if (!buffer) return false;
        
        if (!readCluster(cluster, buffer)) {
            kfree(buffer);
            return false;
        }
        
        FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
        uint32_t entriesPerCluster = clusterSize / sizeof(FAT32DirEntry);
        
        for (uint32_t i = 0; i < entriesPerCluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(buffer);
                return false;
            }
            
            if (entries[i].name[0] == 0xE5) {
                longName[0] = '\0';
                lfnIndex = 0;
                continue;
            }
            
            if (entries[i].attr == FAT32_ATTR_LONG_NAME) {
                FAT32LFNEntry* lfn = (FAT32LFNEntry*)&entries[i];
                int order = lfn->order & 0x1F;
                int offset = (order - 1) * 13;
                
                for (int j = 0; j < 5; j++) {
                    if (lfn->name1[j] == 0 || lfn->name1[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name1[j];
                }
                for (int j = 0; j < 6; j++) {
                    if (lfn->name2[j] == 0 || lfn->name2[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name2[j];
                }
                for (int j = 0; j < 2; j++) {
                    if (lfn->name3[j] == 0 || lfn->name3[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name3[j];
                }
                longName[offset] = '\0';
                continue;
            }
            
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                longName[0] = '\0';
                continue;
            }
            
            bool match = false;
            if (longName[0]) {
                int j = 0;
                while (name[j] && longName[j] && name[j] == longName[j]) j++;
                match = (name[j] == '\0' && longName[j] == '\0');
            } else {
                char shortName[256];
                int nameLen = 0;
                for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                    char c = entries[i].name[j];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    shortName[nameLen++] = c;
                }
                if (entries[i].name[8] != ' ') {
                    shortName[nameLen++] = '.';
                    for (int j = 8; j < 11 && entries[i].name[j] != ' '; j++) {
                        char c = entries[i].name[j];
                        if (c >= 'A' && c <= 'Z') c += 32;
                        shortName[nameLen++] = c;
                    }
                }
                shortName[nameLen] = '\0';
                
                int j = 0;
                while (name[j] && shortName[j] && name[j] == shortName[j]) j++;
                match = (name[j] == '\0' && shortName[j] == '\0');
            }
            
            if (match) {
                for (int j = 0; j < 32; j++) {
                    ((uint8_t*)entry)[j] = ((uint8_t*)&entries[i])[j];
                }
                if (entryCluster) *entryCluster = cluster;
                if (entryOffset) *entryOffset = i * sizeof(FAT32DirEntry);
                kfree(buffer);
                return true;
            }
            
            longName[0] = '\0';
        }
        
        kfree(buffer);
        cluster = getNextCluster(cluster);
    }
    
    return false;
}

bool FAT32FS::createEntry(uint32_t dirCluster, const char* name, uint8_t attr, uint32_t cluster, FAT32DirEntry* entry) {
    char shortName[11];
    shortNameFromLong(name, shortName);
    
    uint32_t currentCluster = dirCluster;
    uint32_t lastCluster = dirCluster;
    
    while (currentCluster >= 2 && currentCluster < FAT32_EOC) {
        void* buffer = kmalloc(clusterSize);
        if (!buffer) return false;
        
        if (!readCluster(currentCluster, buffer)) {
            kfree(buffer);
            return false;
        }
        
        FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
        uint32_t entriesPerCluster = clusterSize / sizeof(FAT32DirEntry);
        
        for (uint32_t i = 0; i < entriesPerCluster; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                for (int j = 0; j < 11; j++) {
                    entries[i].name[j] = shortName[j];
                }
                entries[i].attr = attr;
                entries[i].ntRes = 0;
                entries[i].crtTimeTenth = 0;
                entries[i].crtTime = 0;
                entries[i].crtDate = 0;
                entries[i].lstAccDate = 0;
                entries[i].fstClusHI = (cluster >> 16) & 0xFFFF;
                entries[i].wrtTime = 0;
                entries[i].wrtDate = 0;
                entries[i].fstClusLO = cluster & 0xFFFF;
                entries[i].fileSize = 0;
                
                if (entry) {
                    for (int j = 0; j < 32; j++) {
                        ((uint8_t*)entry)[j] = ((uint8_t*)&entries[i])[j];
                    }
                }
                
                bool result = writeCluster(currentCluster, buffer);
                kfree(buffer);
                return result;
            }
        }
        
        kfree(buffer);
        lastCluster = currentCluster;
        currentCluster = getNextCluster(currentCluster);
    }
    
    uint32_t newCluster = allocateCluster();
    if (newCluster == 0) return false;
    
    setNextCluster(lastCluster, newCluster);
    
    void* buffer = kmalloc(clusterSize);
    if (!buffer) return false;
    
    for (uint32_t i = 0; i < clusterSize; i++) {
        ((uint8_t*)buffer)[i] = 0;
    }
    
    FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
    for (int j = 0; j < 11; j++) {
        entries[0].name[j] = shortName[j];
    }
    entries[0].attr = attr;
    entries[0].ntRes = 0;
    entries[0].crtTimeTenth = 0;
    entries[0].crtTime = 0;
    entries[0].crtDate = 0;
    entries[0].lstAccDate = 0;
    entries[0].fstClusHI = (cluster >> 16) & 0xFFFF;
    entries[0].wrtTime = 0;
    entries[0].wrtDate = 0;
    entries[0].fstClusLO = cluster & 0xFFFF;
    entries[0].fileSize = 0;
    
    if (entry) {
        for (int j = 0; j < 32; j++) {
            ((uint8_t*)entry)[j] = ((uint8_t*)&entries[0])[j];
        }
    }
    
    bool result = writeCluster(newCluster, buffer);
    kfree(buffer);
    return result;
}

bool FAT32FS::deleteEntry(uint32_t dirCluster, const char* name) {
    uint32_t cluster = dirCluster;
    
    while (cluster >= 2 && cluster < FAT32_EOC) {
        void* buffer = kmalloc(clusterSize);
        if (!buffer) return false;
        
        if (!readCluster(cluster, buffer)) {
            kfree(buffer);
            return false;
        }
        
        FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
        uint32_t entriesPerCluster = clusterSize / sizeof(FAT32DirEntry);
        
        for (uint32_t i = 0; i < entriesPerCluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(buffer);
                return false;
            }
            
            if (entries[i].name[0] == 0xE5 || entries[i].attr == FAT32_ATTR_LONG_NAME) {
                continue;
            }
            
            char shortName[256];
            int nameLen = 0;
            for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                char c = entries[i].name[j];
                if (c >= 'A' && c <= 'Z') c += 32;
                shortName[nameLen++] = c;
            }
            if (entries[i].name[8] != ' ') {
                shortName[nameLen++] = '.';
                for (int j = 8; j < 11 && entries[i].name[j] != ' '; j++) {
                    char c = entries[i].name[j];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    shortName[nameLen++] = c;
                }
            }
            shortName[nameLen] = '\0';
            
            int j = 0;
            while (name[j] && shortName[j] && name[j] == shortName[j]) j++;
            
            if (name[j] == '\0' && shortName[j] == '\0') {
                entries[i].name[0] = 0xE5;
                bool result = writeCluster(cluster, buffer);
                kfree(buffer);
                return result;
            }
        }
        
        kfree(buffer);
        cluster = getNextCluster(cluster);
    }
    
    return false;
}

int FAT32FS::nodeOpen(VNode* node, int flags) {
    return 0;
}

int FAT32FS::nodeClose(VNode* node) {
    return 0;
}

int64_t FAT32FS::nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    FAT32Node* fatNode = (FAT32Node*)node->getData();
    if (!fatNode || fatNode->isDirectory) return -1;
    
    if (offset >= fatNode->size) return 0;
    
    if (offset + size > fatNode->size) {
        size = fatNode->size - offset;
    }
    
    FAT32FS* fs = (FAT32FS*)node->getFS();
    uint64_t bytesRead = 0;
    uint8_t* dest = (uint8_t*)buffer;
    
    uint32_t cluster = fatNode->cluster;
    uint64_t clusterOffset = offset / fs->clusterSize;
    
    for (uint64_t i = 0; i < clusterOffset && cluster >= 2 && cluster < FAT32_EOC; i++) {
        cluster = fs->getNextCluster(cluster);
    }
    
    uint64_t offsetInCluster = offset % fs->clusterSize;
    
    while (bytesRead < size && cluster >= 2 && cluster < FAT32_EOC) {
        void* clusterBuffer = kmalloc(fs->clusterSize);
        if (!clusterBuffer) return bytesRead;
        
        if (!fs->readCluster(cluster, clusterBuffer)) {
            kfree(clusterBuffer);
            return bytesRead;
        }
        
        uint64_t toRead = fs->clusterSize - offsetInCluster;
        if (toRead > size - bytesRead) {
            toRead = size - bytesRead;
        }
        
        uint8_t* src = (uint8_t*)clusterBuffer + offsetInCluster;
        for (uint64_t i = 0; i < toRead; i++) {
            dest[bytesRead + i] = src[i];
        }
        
        kfree(clusterBuffer);
        bytesRead += toRead;
        offsetInCluster = 0;
        cluster = fs->getNextCluster(cluster);
    }
    
    return bytesRead;
}

int64_t FAT32FS::nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    FAT32Node* fatNode = (FAT32Node*)node->getData();
    if (!fatNode || fatNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)node->getFS();
    uint64_t bytesWritten = 0;
    const uint8_t* src = (const uint8_t*)buffer;
    
    uint32_t cluster = fatNode->cluster;
    
    if (cluster == 0) {
        cluster = fs->allocateCluster();
        if (cluster == 0) return -1;
        fatNode->cluster = cluster;
    }
    
    uint64_t clusterOffset = offset / fs->clusterSize;
    
    for (uint64_t i = 0; i < clusterOffset; i++) {
        uint32_t next = fs->getNextCluster(cluster);
        if (next >= FAT32_EOC) {
            next = fs->allocateCluster();
            if (next == 0) return bytesWritten;
            fs->setNextCluster(cluster, next);
        }
        cluster = next;
    }
    
    uint64_t offsetInCluster = offset % fs->clusterSize;
    
    while (bytesWritten < size) {
        void* clusterBuffer = kmalloc(fs->clusterSize);
        if (!clusterBuffer) return bytesWritten;
        
        if (offsetInCluster > 0 || (size - bytesWritten) < fs->clusterSize) {
            if (!fs->readCluster(cluster, clusterBuffer)) {
                for (uint32_t i = 0; i < fs->clusterSize; i++) {
                    ((uint8_t*)clusterBuffer)[i] = 0;
                }
            }
        }
        
        uint64_t toWrite = fs->clusterSize - offsetInCluster;
        if (toWrite > size - bytesWritten) {
            toWrite = size - bytesWritten;
        }
        
        uint8_t* dest = (uint8_t*)clusterBuffer + offsetInCluster;
        for (uint64_t i = 0; i < toWrite; i++) {
            dest[i] = src[bytesWritten + i];
        }
        
        if (!fs->writeCluster(cluster, clusterBuffer)) {
            kfree(clusterBuffer);
            return bytesWritten;
        }
        
        kfree(clusterBuffer);
        bytesWritten += toWrite;
        offsetInCluster = 0;
        
        if (bytesWritten < size) {
            uint32_t next = fs->getNextCluster(cluster);
            if (next >= FAT32_EOC) {
                next = fs->allocateCluster();
                if (next == 0) return bytesWritten;
                fs->setNextCluster(cluster, next);
            }
            cluster = next;
        }
    }
    
    if (offset + size > fatNode->size) {
        fatNode->size = offset + size;
    }
    
    return bytesWritten;
}

int FAT32FS::nodeStat(VNode* node, FileStats* stats) {
    if (!node || !stats) return -1;
    
    FAT32Node* fatNode = (FAT32Node*)node->getData();
    if (!fatNode) return -1;
    
    stats->size = fatNode->size;
    stats->inode = fatNode->cluster;
    stats->mode = 0644;
    stats->links = 1;
    stats->atime = 0;
    stats->mtime = 0;
    stats->ctime = 0;
    
    if (fatNode->isDirectory) {
        stats->type = FileType::Directory;
        stats->mode = 0755;
    } else {
        stats->type = FileType::Regular;
    }
    
    return 0;
}

int FAT32FS::nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read) {
    if (!node || !entries || !read) return -1;
    
    FAT32Node* fatNode = (FAT32Node*)node->getData();
    if (!fatNode || !fatNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)node->getFS();
    uint32_t cluster = fatNode->cluster;
    uint64_t entryCount = 0;
    char longName[256] = {0};
    
    while (cluster >= 2 && cluster < FAT32_EOC && entryCount < count) {
        void* buffer = kmalloc(fs->clusterSize);
        if (!buffer) break;
        
        if (!fs->readCluster(cluster, buffer)) {
            kfree(buffer);
            break;
        }
        
        FAT32DirEntry* dirEntries = (FAT32DirEntry*)buffer;
        uint32_t entriesPerCluster = fs->clusterSize / sizeof(FAT32DirEntry);
        
        for (uint32_t i = 0; i < entriesPerCluster && entryCount < count; i++) {
            if (dirEntries[i].name[0] == 0x00) {
                kfree(buffer);
                *read = entryCount;
                return 0;
            }
            
            if (dirEntries[i].name[0] == 0xE5) {
                longName[0] = '\0';
                continue;
            }
            
            if (dirEntries[i].attr == FAT32_ATTR_LONG_NAME) {
                FAT32LFNEntry* lfn = (FAT32LFNEntry*)&dirEntries[i];
                int order = lfn->order & 0x1F;
                int offset = (order - 1) * 13;
                
                for (int j = 0; j < 5; j++) {
                    if (lfn->name1[j] == 0 || lfn->name1[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name1[j];
                }
                for (int j = 0; j < 6; j++) {
                    if (lfn->name2[j] == 0 || lfn->name2[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name2[j];
                }
                for (int j = 0; j < 2; j++) {
                    if (lfn->name3[j] == 0 || lfn->name3[j] == 0xFFFF) break;
                    longName[offset++] = (char)lfn->name3[j];
                }
                longName[offset] = '\0';
                continue;
            }
            
            if (dirEntries[i].attr & FAT32_ATTR_VOLUME_ID) {
                longName[0] = '\0';
                continue;
            }
            
            if (longName[0]) {
                int j = 0;
                while (longName[j] && j < 255) {
                    entries[entryCount].name[j] = longName[j];
                    j++;
                }
                entries[entryCount].name[j] = '\0';
            } else {
                int nameLen = 0;
                for (int j = 0; j < 8 && dirEntries[i].name[j] != ' '; j++) {
                    char c = dirEntries[i].name[j];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    entries[entryCount].name[nameLen++] = c;
                }
                if (dirEntries[i].name[8] != ' ') {
                    entries[entryCount].name[nameLen++] = '.';
                    for (int j = 8; j < 11 && dirEntries[i].name[j] != ' '; j++) {
                        char c = dirEntries[i].name[j];
                        if (c >= 'A' && c <= 'Z') c += 32;
                        entries[entryCount].name[nameLen++] = c;
                    }
                }
                entries[entryCount].name[nameLen] = '\0';
            }
            
            entries[entryCount].inode = ((uint32_t)dirEntries[i].fstClusHI << 16) | dirEntries[i].fstClusLO;
            entries[entryCount].type = (dirEntries[i].attr & FAT32_ATTR_DIRECTORY) ? FileType::Directory : FileType::Regular;
            
            entryCount++;
            longName[0] = '\0';
        }
        
        kfree(buffer);
        cluster = fs->getNextCluster(cluster);
    }
    
    *read = entryCount;
    return 0;
}

VNode* FAT32FS::nodeLookup(VNode* node, const char* name) {
    if (!node || !name) return nullptr;
    
    FAT32Node* fatNode = (FAT32Node*)node->getData();
    if (!fatNode || !fatNode->isDirectory) return nullptr;
    
    FAT32FS* fs = (FAT32FS*)node->getFS();
    FAT32DirEntry entry;
    
    if (!fs->findEntry(fatNode->cluster, name, &entry, nullptr, nullptr)) {
        return nullptr;
    }
    
    FAT32Node* childNode = fs->createNodeFromEntry(&entry, name);
    if (!childNode) return nullptr;
    
    childNode->parentCluster = fatNode->cluster;
    
    VNode* vnode = new VNode(fs, childNode->cluster, childNode->isDirectory ? FileType::Directory : FileType::Regular);
    vnode->setData(childNode);
    vnode->ops = &fs->ops;
    
    return vnode;
}

int FAT32FS::nodeCreate(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    if (!parent || !name || !result) return -1;
    
    FAT32Node* parentNode = (FAT32Node*)parent->getData();
    if (!parentNode || !parentNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)parent->getFS();
    
    uint32_t newCluster = fs->allocateCluster();
    if (newCluster == 0) return -1;
    
    FAT32DirEntry entry;
    if (!fs->createEntry(parentNode->cluster, name, FAT32_ATTR_ARCHIVE, newCluster, &entry)) {
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    FAT32Node* newNode = fs->createNodeFromEntry(&entry, name);
    if (!newNode) {
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    newNode->parentCluster = parentNode->cluster;
    
    VNode* vnode = new VNode(fs, newCluster, FileType::Regular);
    vnode->setData(newNode);
    vnode->ops = &fs->ops;
    
    *result = vnode;
    return 0;
}

int FAT32FS::nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    if (!parent || !name || !result) return -1;
    
    FAT32Node* parentNode = (FAT32Node*)parent->getData();
    if (!parentNode || !parentNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)parent->getFS();
    
    uint32_t newCluster = fs->allocateCluster();
    if (newCluster == 0) return -1;
    
    void* buffer = kmalloc(fs->clusterSize);
    if (!buffer) {
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    for (uint32_t i = 0; i < fs->clusterSize; i++) {
        ((uint8_t*)buffer)[i] = 0;
    }
    
    FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
    
    for (int i = 0; i < 11; i++) {
        entries[0].name[i] = (i == 0) ? '.' : ' ';
    }
    entries[0].attr = FAT32_ATTR_DIRECTORY;
    entries[0].fstClusHI = (newCluster >> 16) & 0xFFFF;
    entries[0].fstClusLO = newCluster & 0xFFFF;
    
    for (int i = 0; i < 11; i++) {
        entries[1].name[i] = (i < 2) ? '.' : ' ';
    }
    entries[1].attr = FAT32_ATTR_DIRECTORY;
    entries[1].fstClusHI = (parentNode->cluster >> 16) & 0xFFFF;
    entries[1].fstClusLO = parentNode->cluster & 0xFFFF;
    
    if (!fs->writeCluster(newCluster, buffer)) {
        kfree(buffer);
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    kfree(buffer);
    
    FAT32DirEntry entry;
    if (!fs->createEntry(parentNode->cluster, name, FAT32_ATTR_DIRECTORY, newCluster, &entry)) {
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    FAT32Node* newNode = fs->createNodeFromEntry(&entry, name);
    if (!newNode) {
        fs->freeClusterChain(newCluster);
        return -1;
    }
    
    newNode->parentCluster = parentNode->cluster;
    
    VNode* vnode = new VNode(fs, newCluster, FileType::Directory);
    vnode->setData(newNode);
    vnode->ops = &fs->ops;
    
    *result = vnode;
    return 0;
}

int FAT32FS::nodeUnlink(VNode* parent, const char* name) {
    if (!parent || !name) return -1;
    
    FAT32Node* parentNode = (FAT32Node*)parent->getData();
    if (!parentNode || !parentNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)parent->getFS();
    
    FAT32DirEntry entry;
    if (!fs->findEntry(parentNode->cluster, name, &entry, nullptr, nullptr)) {
        return -1;
    }
    
    if (entry.attr & FAT32_ATTR_DIRECTORY) {
        return -1;
    }
    
    uint32_t cluster = ((uint32_t)entry.fstClusHI << 16) | entry.fstClusLO;
    
    if (!fs->deleteEntry(parentNode->cluster, name)) {
        return -1;
    }
    
    if (cluster >= 2) {
        fs->freeClusterChain(cluster);
    }
    
    return 0;
}

int FAT32FS::nodeRmdir(VNode* parent, const char* name) {
    if (!parent || !name) return -1;
    
    FAT32Node* parentNode = (FAT32Node*)parent->getData();
    if (!parentNode || !parentNode->isDirectory) return -1;
    
    FAT32FS* fs = (FAT32FS*)parent->getFS();
    
    FAT32DirEntry entry;
    if (!fs->findEntry(parentNode->cluster, name, &entry, nullptr, nullptr)) {
        return -1;
    }
    
    if (!(entry.attr & FAT32_ATTR_DIRECTORY)) {
        return -1;
    }
    
    uint32_t cluster = ((uint32_t)entry.fstClusHI << 16) | entry.fstClusLO;
    
    void* buffer = kmalloc(fs->clusterSize);
    if (!buffer) return -1;
    
    if (!fs->readCluster(cluster, buffer)) {
        kfree(buffer);
        return -1;
    }
    
    FAT32DirEntry* entries = (FAT32DirEntry*)buffer;
    uint32_t entriesPerCluster = fs->clusterSize / sizeof(FAT32DirEntry);
    
    for (uint32_t i = 2; i < entriesPerCluster; i++) {
        if (entries[i].name[0] == 0x00) break;
        if ((unsigned char)entries[i].name[0] != 0xE5 && entries[i].attr != FAT32_ATTR_LONG_NAME) {
            kfree(buffer);
            return -1;
        }
    }
    
    kfree(buffer);
    
    if (!fs->deleteEntry(parentNode->cluster, name)) {
        return -1;
    }
    
    if (cluster >= 2) {
        fs->freeClusterChain(cluster);
    }
    
    return 0;
}
