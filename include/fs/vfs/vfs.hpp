#pragma once

#include <stdint.h>
#include <stddef.h>

enum class FileType {
    Regular,
    Directory,
    CharDevice,
    BlockDevice,
    Symlink,
    Pipe,
    Socket
};

enum class SeekMode {
    Set,
    Current,
    End
};

struct FileStats {
    uint64_t size;
    FileType type;
    uint32_t mode;
    uint64_t inode;
    uint32_t links;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
};

struct DirEntry {
    char name[256];
    uint64_t inode;
    FileType type;
};

class VNode;
class FileSystem;

struct VNodeOps {
    int (*open)(VNode* node, int flags);
    int (*close)(VNode* node);
    int64_t (*read)(VNode* node, void* buffer, uint64_t size, uint64_t offset);
    int64_t (*write)(VNode* node, const void* buffer, uint64_t size, uint64_t offset);
    int (*stat)(VNode* node, FileStats* stats);
    int (*readdir)(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read);
    VNode* (*lookup)(VNode* node, const char* name);
    int (*create)(VNode* parent, const char* name, uint32_t mode, VNode** result);
    int (*mkdir)(VNode* parent, const char* name, uint32_t mode, VNode** result);
    int (*unlink)(VNode* parent, const char* name);
    int (*rmdir)(VNode* parent, const char* name);
};

class VNode {
public:
    VNode(FileSystem* fs, uint64_t inode, FileType type);
    ~VNode();
    
    FileSystem* getFS() { return fs; }
    uint64_t getInode() { return inode; }
    FileType getType() { return type; }
    void* getData() { return data; }
    void setData(void* d) { data = d; }
    
    VNodeOps* ops;
    uint32_t refCount;
    
private:
    FileSystem* fs;
    uint64_t inode;
    FileType type;
    void* data;
};

class FileSystem {
public:
    FileSystem(const char* name);
    virtual ~FileSystem();
    
    virtual int mount(const char* path) = 0;
    virtual int unmount() = 0;
    virtual VNode* getRoot() = 0;
    
    const char* getName() { return name; }
    
protected:
    char name[64];
};

class FileDescriptor {
public:
    FileDescriptor(VNode* node, int flags);
    ~FileDescriptor();
    
    VNode* getNode() { return node; }
    int getFlags() { return flags; }
    uint64_t getOffset() { return offset; }
    void setOffset(uint64_t off) { offset = off; }
    void retain() { refCount++; }
    bool release() { return --refCount == 0; }
    
private:
    VNode* node;
    int flags;
    uint64_t offset;
    uint32_t refCount;
};

struct MountPoint {
    char path[256];
    FileSystem* fs;
    MountPoint* next;
};

class VFS {
public:
    VFS() : rootFS(nullptr), mountPoints(nullptr), initialized(false) {}
    
    static VFS& get();
    
    void initialize();
    
    int mount(FileSystem* fs, const char* path);
    int unmount(const char* path);
    
    int open(const char* path, int flags, FileDescriptor** fd);
    void retain(FileDescriptor* fd);
    int close(FileDescriptor* fd);
    int64_t read(FileDescriptor* fd, void* buffer, uint64_t size);
    int64_t write(FileDescriptor* fd, const void* buffer, uint64_t size);
    int64_t seek(FileDescriptor* fd, int64_t offset, SeekMode mode);
    int stat(const char* path, FileStats* stats);
    int readdir(const char* path, DirEntry* entries, uint64_t count, uint64_t* read);
    
    int create(const char* path, uint32_t mode);
    int mkdir(const char* path, uint32_t mode);
    int unlink(const char* path);
    int rmdir(const char* path);
    
private:
    VNode* resolvePath(const char* path, char* lastComponent);
    void splitPath(const char* path, char* parent, char* name);
    FileSystem* findMount(const char* path, char* relativePath);
    
    FileSystem* rootFS;
    MountPoint* mountPoints;
    bool initialized;
};
