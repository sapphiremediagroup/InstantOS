#pragma once

#include <fs/vfs/vfs.hpp>
#include <stdint.h>

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LONG_NAME 0x0F

#define FAT32_EOC 0x0FFFFFF8
#define FAT32_FREE 0x00000000
#define FAT32_BAD 0x0FFFFFF7

struct FAT32BPB {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t media;
    uint16_t fatSize16;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    uint32_t fatSize32;
    uint16_t extFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;
    uint16_t fsInfo;
    uint16_t backupBootSector;
    uint8_t reserved[12];
    uint8_t driveNumber;
    uint8_t reserved1;
    uint8_t bootSignature;
    uint32_t volumeID;
    char volumeLabel[11];
    char fsType[8];
} __attribute__((packed));

struct FAT32DirEntry {
    char name[11];
    uint8_t attr;
    uint8_t ntRes;
    uint8_t crtTimeTenth;
    uint16_t crtTime;
    uint16_t crtDate;
    uint16_t lstAccDate;
    uint16_t fstClusHI;
    uint16_t wrtTime;
    uint16_t wrtDate;
    uint16_t fstClusLO;
    uint32_t fileSize;
} __attribute__((packed));

struct FAT32LFNEntry {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t fstClusLO;
    uint16_t name3[2];
} __attribute__((packed));

struct FAT32Node {
    char name[256];
    uint32_t cluster;
    uint32_t size;
    uint8_t attr;
    bool isDirectory;
    uint32_t parentCluster;
};

class BlockDevice {
public:
    virtual ~BlockDevice() {}
    virtual bool read(uint64_t offset, void* buffer, uint64_t size) = 0;
    virtual bool write(uint64_t offset, const void* buffer, uint64_t size) = 0;
    virtual uint64_t getSize() = 0;
};



class FAT32FS : public FileSystem {
public:
    FAT32FS(BlockDevice* device);
    ~FAT32FS() override;
    
    int mount(const char* path) override;
    int unmount() override;
    VNode* getRoot() override;
    
    static int nodeOpen(VNode* node, int flags);
    static int nodeClose(VNode* node);
    static int64_t nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset);
    static int64_t nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset);
    static int nodeStat(VNode* node, FileStats* stats);
    static int nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read);
    static VNode* nodeLookup(VNode* node, const char* name);
    static int nodeCreate(VNode* parent, const char* name, uint32_t mode, VNode** result);
    static int nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result);
    static int nodeUnlink(VNode* parent, const char* name);
    static int nodeRmdir(VNode* parent, const char* name);
    
    bool readCluster(uint32_t cluster, void* buffer);
    bool writeCluster(uint32_t cluster, const void* buffer);
    uint32_t getNextCluster(uint32_t cluster);
    bool setNextCluster(uint32_t cluster, uint32_t value);
    uint32_t allocateCluster();
    bool freeClusterChain(uint32_t startCluster);
    

    
private:
    BlockDevice* device;
    FAT32BPB bpb;
    VNode* rootNode;
    VNodeOps ops;
    
    uint32_t fatStart;
    uint32_t dataStart;
    uint32_t clusterSize;
    uint32_t rootDirCluster;
    
    uint64_t clusterToSector(uint32_t cluster);
    bool readSector(uint64_t sector, void* buffer);
    bool writeSector(uint64_t sector, const void* buffer);
    bool readFATEntry(uint32_t cluster, uint32_t* value);
    bool writeFATEntry(uint32_t cluster, uint32_t value);
    
    FAT32Node* createNodeFromEntry(FAT32DirEntry* entry, const char* longName);
    bool findEntry(uint32_t dirCluster, const char* name, FAT32DirEntry* entry, uint32_t* entryCluster, uint32_t* entryOffset);
    bool createEntry(uint32_t dirCluster, const char* name, uint8_t attr, uint32_t cluster, FAT32DirEntry* entry);
    bool deleteEntry(uint32_t dirCluster, const char* name);
    void shortNameFromLong(const char* longName, char* shortName);
    uint8_t lfnChecksum(const char* shortName);
};
