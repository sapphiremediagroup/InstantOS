#pragma once

#include <fs/vfs/vfs.hpp>

struct RamFSNode {
    char name[256];
    FileType type;
    uint64_t inode;
    uint32_t mode;
    uint64_t size;
    void* data;
    RamFSNode* parent;
    RamFSNode* firstChild;
    RamFSNode* nextSibling;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
};

class RamFS : public FileSystem {
public:
    RamFS();
    ~RamFS() override;
    
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
    
private:
    VNode* rootNode;
    RamFSNode* rootData;
    uint64_t nextInode;
    VNodeOps ops;
    
    RamFSNode* createNode(const char* name, FileType type, uint32_t mode);
    void destroyNode(RamFSNode* node);
};
