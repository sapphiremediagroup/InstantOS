#include <fs/vfs/vfs.hpp>
#include <memory/heap.hpp>

VFS vfsInstance;

VFS& VFS::get() {
    return vfsInstance;
}

void VFS::initialize() {
    if (initialized) return;
    
    rootFS = nullptr;
    initialized = true;
}

VNode::VNode(FileSystem* fs, uint64_t inode, FileType type) 
    : fs(fs), inode(inode), type(type), data(nullptr), refCount(1), ops(nullptr) {
}

VNode::~VNode() {
}

FileSystem::FileSystem(const char* name) {
    for (int i = 0; i < 64 && name[i]; i++) {
        this->name[i] = name[i];
    }
}

FileSystem::~FileSystem() {
}

FileDescriptor::FileDescriptor(VNode* node, int flags) 
    : node(node), flags(flags), offset(0), refCount(1) {
    if (node) {
        node->refCount++;
    }
}

FileDescriptor::~FileDescriptor() {
    if (node) {
        node->refCount--;
        if (node->refCount == 0) {
            delete node;
        }
    }
}

int VFS::mount(FileSystem* fs, const char* path) {
    if (!initialized || !fs) return -1;
    
    if (path[0] == '/' && path[1] == '\0') {
        rootFS = fs;
        return fs->mount(path);
    }
    
    MountPoint* mp = (MountPoint*)kmalloc(sizeof(MountPoint));
    if (!mp) return -1;
    
    int i = 0;
    while (path[i] && i < 255) {
        mp->path[i] = path[i];
        i++;
    }
    mp->path[i] = '\0';
    mp->fs = fs;
    mp->next = mountPoints;
    mountPoints = mp;
    
    return fs->mount(path);
}

int VFS::unmount(const char* path) {
    if (!initialized || !rootFS) return -1;
    
    if (path[0] == '/' && path[1] == '\0') {
        int result = rootFS->unmount();
        if (result == 0) {
            rootFS = nullptr;
        }
        return result;
    }
    
    return -1;
}

void VFS::splitPath(const char* path, char* parent, char* name) {
    int len = 0;
    while (path[len]) len++;
    
    int lastSlash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            lastSlash = i;
            break;
        }
    }
    
    if (lastSlash == -1) {
        parent[0] = '.';
        parent[1] = '\0';
        for (int i = 0; i <= len; i++) {
            name[i] = path[i];
        }
    } else if (lastSlash == 0) {
        parent[0] = '/';
        parent[1] = '\0';
        for (int i = 0; i < len - 1; i++) {
            name[i] = path[i + 1];
        }
        name[len - 1] = '\0';
    } else {
        for (int i = 0; i < lastSlash; i++) {
            parent[i] = path[i];
        }
        parent[lastSlash] = '\0';
        
        for (int i = lastSlash + 1; i <= len; i++) {
            name[i - lastSlash - 1] = path[i];
        }
    }
}

FileSystem* VFS::findMount(const char* path, char* relativePath) {
    MountPoint* mp = mountPoints;
    MountPoint* bestMatch = nullptr;
    int bestLen = 0;
    
    while (mp) {
        int len = 0;
        while (mp->path[len]) len++;
        
        bool match = true;
        for (int i = 0; i < len; i++) {
            if (path[i] != mp->path[i]) {
                match = false;
                break;
            }
        }
        
        if (match && (path[len] == '/' || path[len] == '\0') && len > bestLen) {
            bestMatch = mp;
            bestLen = len;
        }
        
        mp = mp->next;
    }
    
    if (bestMatch) {
        int i = 0;
        if (path[bestLen] == '/') {
            while (path[bestLen + i]) {
                relativePath[i] = path[bestLen + i];
                i++;
            }
        } else {
            relativePath[0] = '/';
            i = 1;
        }
        relativePath[i] = '\0';
        return bestMatch->fs;
    }
    
    int i = 0;
    while (path[i]) {
        relativePath[i] = path[i];
        i++;
    }
    relativePath[i] = '\0';
    return rootFS;
}

VNode* VFS::resolvePath(const char* path, char* lastComponent) {
    if (!initialized) return nullptr;
    
    if (!path || path[0] != '/') return nullptr;
    
    char relativePath[256];
    FileSystem* fs = findMount(path, relativePath);
    if (!fs) return nullptr;
    
    VNode* current = fs->getRoot();
    if (!current) return nullptr;
    
    if (relativePath[1] == '\0') {
        if (lastComponent) lastComponent[0] = '\0';
        return current;
    }
    
    char pathCopy[256];
    int i = 0;
    while (relativePath[i] && i < 255) {
        pathCopy[i] = relativePath[i];
        i++;
    }
    pathCopy[i] = '\0';
    
    char* token = pathCopy + 1;
    char* next = nullptr;
    
    while (token && *token) {
        next = token;
        while (*next && *next != '/') next++;
        
        bool isLast = (*next == '\0');
        if (!isLast) {
            *next = '\0';
            next++;
        }
        
        if (isLast && lastComponent) {
            i = 0;
            while (token[i]) {
                lastComponent[i] = token[i];
                i++;
            }
            lastComponent[i] = '\0';
            return current;
        }
        
        if (!current->ops || !current->ops->lookup) {
            return nullptr;
        }
        
        VNode* child = current->ops->lookup(current, token);
        if (!child) {
            return nullptr;
        }
        
        current = child;
        token = next;
    }
    
    if (lastComponent) lastComponent[0] = '\0';
    return current;
}

int VFS::open(const char* path, int flags, FileDescriptor** fd) {
    if (!initialized || !fd) return -1;
    
    VNode* node = resolvePath(path, nullptr);
    if (!node) return -1;
    
    if (node->ops && node->ops->open) {
        int result = node->ops->open(node, flags);
        if (result != 0) return result;
    }
    
    *fd = new FileDescriptor(node, flags);
    return 0;
}

void VFS::retain(FileDescriptor* fd) {
    if (fd) {
        fd->retain();
    }
}

int VFS::close(FileDescriptor* fd) {
    if (!initialized || !fd) return -1;

    if (!fd->release()) {
        return 0;
    }
    
    VNode* node = fd->getNode();
    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }
    
    delete fd;
    return 0;
}

int64_t VFS::read(FileDescriptor* fd, void* buffer, uint64_t size) {
    if (!initialized || !fd || !buffer) return -1;
    
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->read) return -1;
    
    int64_t result = node->ops->read(node, buffer, size, fd->getOffset());
    if (result > 0) {
        fd->setOffset(fd->getOffset() + result);
    }
    
    return result;
}

int64_t VFS::write(FileDescriptor* fd, const void* buffer, uint64_t size) {
    if (!initialized || !fd || !buffer) return -1;
    
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->write) return -1;
    
    int64_t result = node->ops->write(node, buffer, size, fd->getOffset());
    if (result > 0) {
        fd->setOffset(fd->getOffset() + result);
    }
    
    return result;
}

int64_t VFS::seek(FileDescriptor* fd, int64_t offset, SeekMode mode) {
    if (!initialized || !fd) return -1;
    
    VNode* node = fd->getNode();
    if (!node) return -1;
    
    FileStats stats;
    if (node->ops && node->ops->stat) {
        if (node->ops->stat(node, &stats) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    
    int64_t newOffset = 0;
    
    switch (mode) {
        case SeekMode::Set:
            newOffset = offset;
            break;
        case SeekMode::Current:
            newOffset = fd->getOffset() + offset;
            break;
        case SeekMode::End:
            newOffset = stats.size + offset;
            break;
    }
    
    if (newOffset < 0) return -1;
    
    fd->setOffset(newOffset);
    return newOffset;
}

int VFS::stat(const char* path, FileStats* stats) {
    if (!initialized || !stats) return -1;
    
    VNode* node = resolvePath(path, nullptr);
    if (!node) return -1;
    
    if (!node->ops || !node->ops->stat) return -1;
    
    return node->ops->stat(node, stats);
}

int VFS::readdir(const char* path, DirEntry* entries, uint64_t count, uint64_t* read) {
    if (!initialized || !entries || !read) return -1;
    
    VNode* node = resolvePath(path, nullptr);
    if (!node) return -1;
    
    if (node->getType() != FileType::Directory) return -1;
    
    if (!node->ops || !node->ops->readdir) return -1;
    
    return node->ops->readdir(node, entries, count, read);
}

int VFS::create(const char* path, uint32_t mode) {
    if (!initialized) return -1;
    
    char parent[256];
    char name[256];
    splitPath(path, parent, name);
    
    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode) return -1;
    
    if (!parentNode->ops || !parentNode->ops->create) return -1;
    
    VNode* newNode = nullptr;
    return parentNode->ops->create(parentNode, name, mode, &newNode);
}

int VFS::mkdir(const char* path, uint32_t mode) {
    if (!initialized) return -1;
    
    char parent[256];
    char name[256];
    splitPath(path, parent, name);
    
    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode) return -1;
    
    if (!parentNode->ops || !parentNode->ops->mkdir) return -1;
    
    VNode* newNode = nullptr;
    return parentNode->ops->mkdir(parentNode, name, mode, &newNode);
}

int VFS::unlink(const char* path) {
    if (!initialized) return -1;
    
    char parent[256];
    char name[256];
    splitPath(path, parent, name);
    
    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode) return -1;
    
    if (!parentNode->ops || !parentNode->ops->unlink) return -1;
    
    return parentNode->ops->unlink(parentNode, name);
}

int VFS::rmdir(const char* path) {
    if (!initialized) return -1;
    
    char parent[256];
    char name[256];
    splitPath(path, parent, name);
    
    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode) return -1;
    
    if (!parentNode->ops || !parentNode->ops->rmdir) return -1;
    
    return parentNode->ops->rmdir(parentNode, name);
}
