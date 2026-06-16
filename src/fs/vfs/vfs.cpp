#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>
#include <memory/heap.hpp>

namespace {
constexpr int kOpenCreate = 0100;
constexpr int kOpenExclusive = 0200;
constexpr int kOpenTruncate = 01000;
constexpr int kOpenAppend = 02000;
constexpr int kOpenNoFollow = 0400000;
constexpr int kOpenAccessModeMask = 0x3;
constexpr int kOpenWriteOnly = 0x1;
constexpr int kOpenReadWrite = 0x2;
constexpr int kMaxSymlinkDepth = 40;
constexpr int kErrNoEntry = 2;
constexpr int kErrLoop = 40;
constexpr int kErrCrossDevice = 18;  // EXDEV
constexpr int kErrNoSys = 38;        // ENOSYS

bool pathIsRoot(const char* path) {
    return path && path[0] == '/' && path[1] == '\0';
}

uint64_t pathLength(const char* path) {
    uint64_t len = 0;
    while (path && path[len]) len++;
    return len;
}

bool copyPath(char* dest, const char* src) {
    uint64_t i = 0;
    while (src && src[i] && i < 255) {
        dest[i] = src[i];
        i++;
    }
    if (src && src[i]) return false;
    dest[i] = '\0';
    return true;
}

bool appendPath(char* dest, const char* suffix) {
    uint64_t len = pathLength(dest);
    uint64_t i = 0;
    while (suffix && suffix[i]) {
        if (len >= 255) return false;
        dest[len++] = suffix[i++];
    }
    dest[len] = '\0';
    return true;
}

bool parentPathForResolvedPrefix(const char* path, const char* component, char* parent) {
    const uint64_t pathLen = pathLength(path);
    const uint64_t compLen = pathLength(component);
    if (compLen == 0 || compLen > pathLen) return false;
    uint64_t end = pathLen - compLen;
    if (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) {
        parent[0] = '/';
        parent[1] = '\0';
        return true;
    }
    for (uint64_t i = 0; i < end && i < 255; ++i) parent[i] = path[i];
    parent[end] = '\0';
    return true;
}
}

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
        int result = fs->mount(path);
        if (result == 0) {
            rootFS = fs;
        }
        return result;
    }
    
    MountPoint* mp = (MountPoint*)kmalloc(sizeof(MountPoint));
    if (!mp) return -1;
    
    int i = 0;
    while (path[i] && i < 255) {
        mp->path[i] = path[i];
        i++;
    }
    mp->path[i] = '\0';
    int result = fs->mount(path);
    if (result != 0) {
        kfree(mp);
        return result;
    }

    mp->fs = fs;
    mp->next = mountPoints;
    mountPoints = mp;

    return 0;
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

VNode* VFS::resolvePath(const char* path, char* lastComponent, bool followFinal, int symlinkDepth) {
    if (symlinkDepth == 0) setLastError(0);
    if (!initialized) {
        setLastError(kErrNoEntry);
        return nullptr;
    }
    if (symlinkDepth > kMaxSymlinkDepth) {
        setLastError(kErrLoop);
        return nullptr;
    }
    
    if (!path || path[0] != '/') {
        setLastError(kErrNoEntry);
        return nullptr;
    }
    
    char relativePath[256];
    FileSystem* fs = findMount(path, relativePath);
    if (!fs) {
        setLastError(kErrNoEntry);
        return nullptr;
    }
    
    VNode* current = fs->getRoot();
    if (!current) {
        setLastError(kErrNoEntry);
        return nullptr;
    }
    
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
            setLastError(kErrNoEntry);
            return nullptr;
        }
        
        VNode* child = current->ops->lookup(current, token);
        if (!child) {
            setLastError(kErrNoEntry);
            return nullptr;
        }

        if (child->getType() == FileType::Symlink && (followFinal || !isLast)) {
            if (!child->ops || !child->ops->readlink) {
                setLastError(kErrNoEntry);
                return nullptr;
            }
            char target[256];
            int64_t targetLen = child->ops->readlink(child, target, sizeof(target) - 1);
            if (targetLen < 0 || targetLen >= 256) {
                setLastError(kErrNoEntry);
                return nullptr;
            }
            target[targetLen] = '\0';

            char resolved[256];
            if (target[0] == '/') {
                if (!copyPath(resolved, target)) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
            } else {
                char parent[256];
                if (!parentPathForResolvedPrefix(path, token, parent) || !copyPath(resolved, parent)) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
                if (!pathIsRoot(resolved) && !appendPath(resolved, "/")) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
                if (!appendPath(resolved, target)) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
            }
            if (!isLast) {
                if (!pathIsRoot(resolved) && !appendPath(resolved, "/")) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
                if (!appendPath(resolved, next)) {
                    setLastError(kErrNoEntry);
                    return nullptr;
                }
            }
            return resolvePath(resolved, lastComponent, followFinal, symlinkDepth + 1);
        }
        
        current = child;
        token = next;
    }
    
    if (lastComponent) lastComponent[0] = '\0';
    return current;
}

int VFS::open(const char* path, int flags, FileDescriptor** fd, uint32_t mode) {
    if (!initialized || !fd) return -1;
    
    VNode* node = resolvePath(path, nullptr, (flags & kOpenNoFollow) == 0);
    if (!node) {
        if ((flags & kOpenCreate) == 0) return -1;

        char parent[256];
        char name[256];
        splitPath(path, parent, name);
        if (name[0] == '\0' || pathIsRoot(path)) return -1;

        VNode* parentNode = resolvePath(parent, nullptr);
        if (!parentNode || !parentNode->ops || !parentNode->ops->create) return -1;

        if (parentNode->ops->create(parentNode, name, mode, &node) != 0 || !node) {
            return -1;
        }
    } else if ((flags & (kOpenCreate | kOpenExclusive)) == (kOpenCreate | kOpenExclusive)) {
        return -1;
    }
    
    if (node->ops && node->ops->open) {
        int result = node->ops->open(node, flags);
        if (result != 0) return result;
    }

    if ((flags & kOpenTruncate) && node->getType() == FileType::Regular) {
        if (!node->ops || !node->ops->truncate || node->ops->truncate(node, 0) != 0) {
            return -1;
        }
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
    if (!initialized || !fd || !buffer) {
        Console::get().drawText("[VFS] read invalid args initialized=");
        Console::get().drawNumber(initialized ? 1 : 0);
        Console::get().drawText(" fd=");
        Console::get().drawHex(reinterpret_cast<uint64_t>(fd));
        Console::get().drawText(" buffer=");
        Console::get().drawHex(reinterpret_cast<uint64_t>(buffer));
        Console::get().drawText(" size=");
        Console::get().drawNumber(static_cast<int64_t>(size));
        Console::get().drawText("\n");
        return -1;
    }

    if ((fd->getFlags() & kOpenAccessModeMask) == kOpenWriteOnly) {
        return -1;
    }
    
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->read) {
        Console::get().drawText("[VFS] read missing op node=");
        Console::get().drawHex(reinterpret_cast<uint64_t>(node));
        Console::get().drawText(" ops=");
        Console::get().drawHex(node ? reinterpret_cast<uint64_t>(node->ops) : 0);
        Console::get().drawText(" read=");
        Console::get().drawHex((node && node->ops) ? reinterpret_cast<uint64_t>(node->ops->read) : 0);
        Console::get().drawText(" type=");
        Console::get().drawNumber(node ? static_cast<int64_t>(node->getType()) : -1);
        Console::get().drawText(" inode=");
        Console::get().drawNumber(node ? static_cast<int64_t>(node->getInode()) : -1);
        Console::get().drawText("\n");
        return -1;
    }
    
    int64_t result = node->ops->read(node, buffer, size, fd->getOffset());
    if (result < 0) {
        Console::get().drawText("[VFS] read op failed fs=");
        Console::get().drawText(node->getFS() ? node->getFS()->getName() : "<null>");
        Console::get().drawText(" type=");
        Console::get().drawNumber(static_cast<int64_t>(node->getType()));
        Console::get().drawText(" inode=");
        Console::get().drawNumber(static_cast<int64_t>(node->getInode()));
        Console::get().drawText(" offset=");
        Console::get().drawNumber(static_cast<int64_t>(fd->getOffset()));
        Console::get().drawText(" size=");
        Console::get().drawNumber(static_cast<int64_t>(size));
        Console::get().drawText("\n");
    }
    if (result > 0) {
        fd->setOffset(fd->getOffset() + result);
    }
    
    return result;
}

int64_t VFS::write(FileDescriptor* fd, const void* buffer, uint64_t size) {
    if (!initialized || !fd || !buffer) return -1;

    const int accessMode = fd->getFlags() & kOpenAccessModeMask;
    if (accessMode != kOpenWriteOnly && accessMode != kOpenReadWrite) return -1;
    
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->write) return -1;

    if ((fd->getFlags() & kOpenAppend) && node->getType() == FileType::Regular) {
        FileStats stats {};
        if (!node->ops->stat || node->ops->stat(node, &stats) != 0) {
            return -1;
        }
        fd->setOffset(stats.size);
    }
    
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

    // TTYs/char devices and pipes are not seekable. Returning a negative value
    // makes sys_seek() report ESPIPE, which is what mlibc relies on to classify
    // the stream as pipe-like (and thus pick line/no buffering for a tty).
    if (node->getType() == FileType::CharDevice || node->getType() == FileType::Pipe) {
        return -1;
    }
    
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

int VFS::truncate(FileDescriptor* fd, uint64_t size) {
    if (!initialized || !fd) return -1;

    VNode* node = fd->getNode();
    if (!node || node->getType() != FileType::Regular) return -1;
    if (!node->ops || !node->ops->truncate) return -1;

    return node->ops->truncate(node, size);
}

int VFS::truncate(const char* path, uint64_t size) {
    if (!initialized) return -1;

    VNode* node = resolvePath(path, nullptr);
    if (!node || node->getType() != FileType::Regular) return -1;
    if (!node->ops || !node->ops->truncate) return -1;

    return node->ops->truncate(node, size);
}

int VFS::chmod(FileDescriptor* fd, uint32_t mode) {
    if (!initialized || !fd) return -1;

    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->chmod) return -1;

    return node->ops->chmod(node, mode);
}

int VFS::chmod(const char* path, uint32_t mode) {
    if (!initialized) return -1;

    VNode* node = resolvePath(path, nullptr);
    if (!node || !node->ops || !node->ops->chmod) return -1;

    return node->ops->chmod(node, mode);
}

int VFS::utime(FileDescriptor* fd, uint64_t atime, uint64_t mtime) {
    if (!initialized || !fd) return -1;

    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->utime) return -1;

    return node->ops->utime(node, atime, mtime);
}

int VFS::utime(const char* path, uint64_t atime, uint64_t mtime) {
    if (!initialized) return -1;

    VNode* node = resolvePath(path, nullptr);
    if (!node || !node->ops || !node->ops->utime) return -1;

    return node->ops->utime(node, atime, mtime);
}

int VFS::stat(const char* path, FileStats* stats) {
    if (!initialized || !stats) return -1;
    
    VNode* node = resolvePath(path, nullptr);
    if (!node) return -1;
    
    if (!node->ops || !node->ops->stat) return -1;
    
    return node->ops->stat(node, stats);
}

int VFS::statfs(const char* path, FsStats* stats) {
    if (!initialized || !stats) { setLastError(kErrNoEntry); return -1; }
    VNode* node = resolvePath(path, nullptr);
    if (!node) { setLastError(kErrNoEntry); return -1; }
    if (!node->ops || !node->ops->statfs) { setLastError(kErrNoSys); return -1; }
    return node->ops->statfs(node, stats);
}

int VFS::statfs(FileDescriptor* fd, FsStats* stats) {
    if (!initialized || !fd || !stats) { setLastError(kErrNoEntry); return -1; }
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->statfs) { setLastError(kErrNoSys); return -1; }
    return node->ops->statfs(node, stats);
}

int VFS::chown(const char* path, uint32_t uid, uint32_t gid, bool followSymlink) {
    if (!initialized || !path) { setLastError(kErrNoEntry); return -1; }
    VNode* node = resolvePath(path, nullptr, followSymlink);
    if (!node) { setLastError(kErrNoEntry); return -1; }
    if (!node->ops || !node->ops->chown) { setLastError(kErrNoSys); return -1; }
    return node->ops->chown(node, uid, gid);
}

int VFS::chown(FileDescriptor* fd, uint32_t uid, uint32_t gid) {
    if (!initialized || !fd) { setLastError(kErrNoEntry); return -1; }
    VNode* node = fd->getNode();
    if (!node || !node->ops || !node->ops->chown) { setLastError(kErrNoSys); return -1; }
    return node->ops->chown(node, uid, gid);
}

int VFS::mknod(const char* path, uint32_t mode, uint64_t dev) {
    if (!initialized || !path) { setLastError(kErrNoEntry); return -1; }

    char parent[256];
    char name[256];
    splitPath(path, parent, name);
    if (name[0] == '\0') { setLastError(kErrNoEntry); return -1; }

    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode) { setLastError(kErrNoEntry); return -1; }
    if (!parentNode->ops || !parentNode->ops->mknod) { setLastError(kErrNoSys); return -1; }

    VNode* result = nullptr;
    int rc = parentNode->ops->mknod(parentNode, name, mode, dev, &result);
    if (rc == 0 && result) {
        // The node is persisted in the FS tree; we don't keep the VNode here.
        delete result;
    }
    if (rc != 0) setLastError(kErrNoEntry);
    return rc;
}

int VFS::lstat(const char* path, FileStats* stats) {
    if (!initialized || !stats) return -1;

    VNode* node = resolvePath(path, nullptr, false);
    if (!node) return -1;

    if (!node->ops || !node->ops->stat) return -1;

    return node->ops->stat(node, stats);
}

int64_t VFS::readlink(const char* path, char* buffer, uint64_t size) {
    if (!initialized || !buffer) return -1;

    VNode* node = resolvePath(path, nullptr, false);
    if (!node || node->getType() != FileType::Symlink) return -1;
    if (!node->ops || !node->ops->readlink) return -1;

    return node->ops->readlink(node, buffer, size);
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

int VFS::rename(const char* oldPath, const char* newPath) {
    if (!initialized || !oldPath || !newPath) { setLastError(kErrNoEntry); return -1; }

    char oldParent[256];
    char oldName[256];
    char newParent[256];
    char newName[256];
    splitPath(oldPath, oldParent, oldName);
    splitPath(newPath, newParent, newName);
    if (oldName[0] == '\0' || newName[0] == '\0') { setLastError(kErrNoEntry); return -1; }

    VNode* oldParentNode = resolvePath(oldParent, nullptr);
    VNode* newParentNode = resolvePath(newParent, nullptr);
    if (!oldParentNode || !newParentNode) { setLastError(kErrNoEntry); return -1; }
    if (oldParentNode->getFS() != newParentNode->getFS()) { setLastError(kErrCrossDevice); return -1; }
    if (!oldParentNode->ops || !oldParentNode->ops->rename) { setLastError(kErrNoSys); return -1; }

    int rc = oldParentNode->ops->rename(oldParentNode, oldName, newParentNode, newName);
    if (rc != 0) setLastError(kErrNoEntry);
    return rc;
}

int VFS::link(const char* oldPath, const char* newPath) {
    if (!initialized || !oldPath || !newPath) return -1;

    char oldParent[256];
    char oldName[256];
    char newParent[256];
    char newName[256];
    splitPath(oldPath, oldParent, oldName);
    splitPath(newPath, newParent, newName);
    if (oldName[0] == '\0' || newName[0] == '\0' || pathIsRoot(oldPath) || pathIsRoot(newPath)) {
        return -1;
    }

    VNode* oldParentNode = resolvePath(oldParent, nullptr);
    VNode* newParentNode = resolvePath(newParent, nullptr);
    if (!oldParentNode || !newParentNode) return -1;
    if (oldParentNode->getFS() != newParentNode->getFS()) return -1;
    if (!oldParentNode->ops || !oldParentNode->ops->link) return -1;

    return oldParentNode->ops->link(oldParentNode, oldName, newParentNode, newName);
}

int VFS::symlink(const char* target, const char* linkPath) {
    if (!initialized || !target || !linkPath) return -1;

    char parent[256];
    char name[256];
    splitPath(linkPath, parent, name);
    if (name[0] == '\0' || pathIsRoot(linkPath)) return -1;

    VNode* parentNode = resolvePath(parent, nullptr);
    if (!parentNode || !parentNode->ops || !parentNode->ops->symlink) return -1;

    VNode* newNode = nullptr;
    return parentNode->ops->symlink(parentNode, name, target, &newNode);
}
