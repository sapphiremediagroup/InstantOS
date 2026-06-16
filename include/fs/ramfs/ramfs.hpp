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
    uint32_t uid;
    uint32_t gid;
    uint64_t rdev;
};

struct RamFSFileData {
    void* data;
    uint64_t size;
    uint32_t links;
    uint32_t mode;
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
    static int nodeTruncate(VNode* node, uint64_t size);
    static int nodeRename(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName);
    static int nodeChmod(VNode* node, uint32_t mode);
    static int nodeUtime(VNode* node, uint64_t atime, uint64_t mtime);
    static int nodeChown(VNode* node, uint32_t uid, uint32_t gid);
    static int nodeStatfs(VNode* node, FsStats* stats);
    static int nodeMknod(VNode* parent, const char* name, uint32_t mode, uint64_t dev, VNode** result);
    static int nodeLink(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName);
    static int nodeSymlink(VNode* parent, const char* name, const char* target, VNode** result);
    static int64_t nodeReadlink(VNode* node, char* buffer, uint64_t size);
    
private:
    VNode* rootNode;
    RamFSNode* rootData;
    uint64_t nextInode;
    VNodeOps ops{};
    
    RamFSNode* createNode(const char* name, FileType type, uint32_t mode);
    void destroyNode(RamFSNode* node);
};
