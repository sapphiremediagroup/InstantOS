#pragma once

#include <fs/vfs/vfs.hpp>

class SharedPages;

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
    uint32_t refs;       // VNodes over it (open files, lookups under way)
    bool unlinked;       // out of the tree: goes with the last of them
};

// What a regular file's names (hard links) have in common.
struct RamFSFileData {
    SharedPages* pages;  // its contents, which mappings share
    uint32_t links;      // names
    uint32_t nodes;      // RamFSNodes: the names, and unlinked ones still open
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
    void releaseNode(VNode* node) override;   // lookups hand out fresh VNodes over the tree's nodes
    
    // A regular file's memory, or nullptr if the node is not a RamFS file.
    static SharedPages* pagesOf(VNode* node);

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
    // Takes a node that is out of the tree away: now, or when it is closed.
    static void dropNode(RamFSNode* node);
    static void freeNode(RamFSNode* node);
    static VNode* makeVNode(FileSystem* fs, RamFSNode* node, VNodeOps* ops);
};
