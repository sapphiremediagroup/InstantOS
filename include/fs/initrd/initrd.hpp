#pragma once

#include <fs/vfs/vfs.hpp>
#include <stdint.h>

struct InitrdFile {
    char name[64];
    uint64_t offset;
    uint64_t size;
};

struct InitrdHeader {
    uint32_t magic;
    uint32_t fileCount;
    InitrdFile files[];
};

class InitrdFS : public FileSystem {
public:
    InitrdFS(void* data, size_t size);
    ~InitrdFS() override;
    
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
    
private:
    void* data;
    size_t dataSize;
    InitrdHeader* header;
    VNode* rootNode;
    VNodeOps ops;
};
