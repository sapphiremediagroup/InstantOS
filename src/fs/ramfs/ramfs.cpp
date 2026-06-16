#include <fs/ramfs/ramfs.hpp>
#include <memory/heap.hpp>
#include <time/time.hpp>

namespace {
bool ramfsNameEquals(const char* a, const char* b) {
    int i = 0;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return true;
}

bool ramfsNameIsDotOrDotDot(const char* name) {
    return (name && name[0] == '.' && name[1] == '\0') ||
           (name && name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

RamFSNode* ramfsFindChild(RamFSNode* parent, const char* name) {
    RamFSNode* child = parent ? parent->firstChild : nullptr;
    while (child) {
        if (ramfsNameEquals(name, child->name)) {
            return child;
        }
        child = child->nextSibling;
    }
    return nullptr;
}

bool ramfsDetachChild(RamFSNode* parent, RamFSNode* target) {
    if (!parent || !target) {
        return false;
    }

    RamFSNode* prev = nullptr;
    RamFSNode* child = parent->firstChild;
    while (child) {
        if (child == target) {
            if (prev) {
                prev->nextSibling = child->nextSibling;
            } else {
                parent->firstChild = child->nextSibling;
            }
            child->nextSibling = nullptr;
            return true;
        }
        prev = child;
        child = child->nextSibling;
    }
    return false;
}

bool ramfsIsDescendantOf(RamFSNode* node, RamFSNode* possibleAncestor) {
    while (node) {
        if (node == possibleAncestor) {
            return true;
        }
        node = node->parent;
    }
    return false;
}

void ramfsSetName(RamFSNode* node, const char* name) {
    int i = 0;
    while (name[i] && i < 255) {
        node->name[i] = name[i];
        i++;
    }
    node->name[i] = '\0';
}

uint64_t ramfsNow() {
    return time_get_unix();
}

uint32_t ramfsDirectoryLinks(RamFSNode* node) {
    uint32_t links = 2;
    for (RamFSNode* child = node ? node->firstChild : nullptr; child; child = child->nextSibling) {
        if (child->type == FileType::Directory) {
            links++;
        }
    }
    return links;
}
}

RamFS::RamFS() : FileSystem("ramfs"), rootNode(nullptr), rootData(nullptr), nextInode(1) {
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
    ops.truncate = nodeTruncate;
    ops.rename = nodeRename;
    ops.chmod = nodeChmod;
    ops.utime = nodeUtime;
    ops.link = nodeLink;
    ops.symlink = nodeSymlink;
    ops.readlink = nodeReadlink;
    ops.chown = nodeChown;
    ops.statfs = nodeStatfs;
    ops.mknod = nodeMknod;
}

RamFS::~RamFS() {
    if (rootData) {
        destroyNode(rootData);
    }
    if (rootNode) {
        delete rootNode;
    }
}

int RamFS::mount(const char* path) {
    rootData = createNode("/", FileType::Directory, 0755);
    if (!rootData) return -1;
    
    rootNode = new VNode(this, rootData->inode, FileType::Directory);
    rootNode->setData(rootData);
    rootNode->ops = &ops;
    
    return 0;
}

int RamFS::unmount() {
    return 0;
}

VNode* RamFS::getRoot() {
    return rootNode;
}

RamFSNode* RamFS::createNode(const char* name, FileType type, uint32_t mode) {
    RamFSNode* node = (RamFSNode*)kmalloc(sizeof(RamFSNode));
    if (!node) return nullptr;
    
    int i = 0;
    while (name[i] && i < 255) {
        node->name[i] = name[i];
        i++;
    }
    node->name[i] = '\0';
    
    node->type = type;
    node->inode = nextInode++;
    node->mode = mode;
    node->size = 0;
    node->data = nullptr;
    node->parent = nullptr;
    node->firstChild = nullptr;
    node->nextSibling = nullptr;
    const uint64_t now = ramfsNow();
    node->atime = now;
    node->mtime = now;
    node->ctime = now;
    node->uid = 0;
    node->gid = 0;
    node->rdev = 0;

    if (type == FileType::Regular) {
        RamFSFileData* file = (RamFSFileData*)kmalloc(sizeof(RamFSFileData));
        if (!file) {
            kfree(node);
            return nullptr;
        }
        file->data = nullptr;
        file->size = 0;
        file->links = 1;
        file->mode = mode;
        file->atime = now;
        file->mtime = now;
        file->ctime = now;
        node->data = file;
    }
    
    return node;
}

void RamFS::destroyNode(RamFSNode* node) {
    if (!node) return;
    
    RamFSNode* child = node->firstChild;
    while (child) {
        RamFSNode* next = child->nextSibling;
        destroyNode(child);
        child = next;
    }
    
    if (node->type == FileType::Regular) {
        RamFSFileData* file = (RamFSFileData*)node->data;
        if (file) {
            if (file->links > 0) {
                file->links--;
            }
            if (file->links == 0) {
                if (file->data) {
                    kfree(file->data);
                }
                kfree(file);
            }
        }
    } else if (node->type == FileType::Symlink && node->data) {
        kfree(node->data);
    }
    
    kfree(node);
}

int RamFS::nodeOpen(VNode* node, int flags) {
    return 0;
}

int RamFS::nodeClose(VNode* node) {
    return 0;
}

int64_t RamFS::nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Regular) return -1;
    
    RamFSFileData* file = (RamFSFileData*)ramNode->data;
    if (!file) return -1;

    if (offset >= file->size) return 0;
    
    uint64_t toRead = size;
    if (offset + toRead > file->size) {
        toRead = file->size - offset;
    }
    
    if (file->data) {
        uint8_t* src = (uint8_t*)file->data + offset;
        uint8_t* dst = (uint8_t*)buffer;
        for (uint64_t i = 0; i < toRead; i++) {
            dst[i] = src[i];
        }
    }

    file->atime = ramfsNow();
    ramNode->atime = file->atime;
    
    return toRead;
}

int64_t RamFS::nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Regular) return -1;
    RamFSFileData* file = (RamFSFileData*)ramNode->data;
    if (!file) return -1;
    
    uint64_t newSize = offset + size;
    if (newSize > file->size) {
        void* newData = kmalloc(newSize);
        if (!newData) return -1;
        
        if (file->data) {
            uint8_t* src = (uint8_t*)file->data;
            uint8_t* dst = (uint8_t*)newData;
            for (uint64_t i = 0; i < file->size; i++) {
                dst[i] = src[i];
            }
            kfree(file->data);
        }
        uint8_t* dst = (uint8_t*)newData;
        for (uint64_t i = file->size; i < newSize; i++) {
            dst[i] = 0;
        }
        
        file->data = newData;
        file->size = newSize;
        ramNode->size = newSize;
    }
    
    uint8_t* dst = (uint8_t*)file->data + offset;
    const uint8_t* src = (const uint8_t*)buffer;
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    const uint64_t now = ramfsNow();
    file->mtime = now;
    file->ctime = now;
    ramNode->mtime = now;
    ramNode->ctime = now;
    
    return size;
}

int RamFS::nodeTruncate(VNode* node, uint64_t size) {
    if (!node) return -1;

    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Regular) return -1;
    RamFSFileData* file = (RamFSFileData*)ramNode->data;
    if (!file) return -1;
    if (size == file->size) return 0;

    void* newData = nullptr;
    if (size != 0) {
        newData = kmalloc(size);
        if (!newData) return -1;

        uint8_t* dst = (uint8_t*)newData;
        uint64_t preserve = file->size < size ? file->size : size;
        if (file->data) {
            uint8_t* src = (uint8_t*)file->data;
            for (uint64_t i = 0; i < preserve; i++) {
                dst[i] = src[i];
            }
        }
        for (uint64_t i = preserve; i < size; i++) {
            dst[i] = 0;
        }
    }

    if (file->data) {
        kfree(file->data);
    }
    file->data = newData;
    file->size = size;
    ramNode->size = size;
    const uint64_t now = ramfsNow();
    file->mtime = now;
    file->ctime = now;
    ramNode->mtime = now;
    ramNode->ctime = now;
    return 0;
}

int RamFS::nodeChmod(VNode* node, uint32_t mode) {
    if (!node) return -1;

    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode) return -1;

    const uint64_t now = ramfsNow();
    if (ramNode->type == FileType::Regular) {
        RamFSFileData* file = (RamFSFileData*)ramNode->data;
        if (!file) return -1;
        file->mode = mode & 07777;
        file->ctime = now;
    }
    ramNode->mode = mode & 07777;
    ramNode->ctime = now;
    return 0;
}

int RamFS::nodeUtime(VNode* node, uint64_t atime, uint64_t mtime) {
    if (!node) return -1;

    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode) return -1;

    const uint64_t now = ramfsNow();
    if (ramNode->type == FileType::Regular) {
        RamFSFileData* file = (RamFSFileData*)ramNode->data;
        if (!file) return -1;
        file->atime = atime;
        file->mtime = mtime;
        file->ctime = now;
    }
    ramNode->atime = atime;
    ramNode->mtime = mtime;
    ramNode->ctime = now;
    return 0;
}

int RamFS::nodeChown(VNode* node, uint32_t uid, uint32_t gid) {
    if (!node) return -1;
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode) return -1;
    // -1 (0xFFFFFFFF) means "don't change" per POSIX chown semantics.
    if (uid != 0xFFFFFFFFu) ramNode->uid = uid;
    if (gid != 0xFFFFFFFFu) ramNode->gid = gid;
    ramNode->ctime = ramfsNow();
    return 0;
}

int RamFS::nodeStatfs(VNode* node, FsStats* stats) {
    if (!node || !stats) return -1;
    // RamFS is memory-backed with no fixed size; report a nominal volume.
    stats->blockSize = 4096;
    stats->totalBlocks = 0;
    stats->freeBlocks = 0;
    stats->totalInodes = 0;
    stats->freeInodes = 0;
    stats->nameMax = 255;
    stats->fsType = 0x858458f6;  // RAMFS_MAGIC
    return 0;
}

int RamFS::nodeStat(VNode* node, FileStats* stats) {
    if (!node || !stats) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode) return -1;
    
    RamFSFileData* file = ramNode->type == FileType::Regular ? (RamFSFileData*)ramNode->data : nullptr;
    stats->size = file ? file->size : ramNode->size;
    stats->type = ramNode->type;
    stats->mode = file ? file->mode : ramNode->mode;
    stats->inode = ramNode->inode;
    stats->links = file ? file->links : (ramNode->type == FileType::Directory ? ramfsDirectoryLinks(ramNode) : 1);
    stats->atime = file ? file->atime : ramNode->atime;
    stats->mtime = file ? file->mtime : ramNode->mtime;
    stats->ctime = file ? file->ctime : ramNode->ctime;
    stats->uid = ramNode->uid;
    stats->gid = ramNode->gid;
    stats->rdev = ramNode->rdev;
    stats->dev = reinterpret_cast<uint64_t>(node->getFS());
    
    return 0;
}

int RamFS::nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read) {
    if (!node || !entries || !read) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Directory) return -1;
    
    uint64_t index = 0;

    if (index < count) {
        entries[index].name[0] = '.';
        entries[index].name[1] = '\0';
        entries[index].inode = ramNode->inode;
        entries[index].type = FileType::Directory;
        index++;
    }

    if (index < count) {
        entries[index].name[0] = '.';
        entries[index].name[1] = '.';
        entries[index].name[2] = '\0';
        entries[index].inode = ramNode->parent ? ramNode->parent->inode : ramNode->inode;
        entries[index].type = FileType::Directory;
        index++;
    }

    RamFSNode* child = ramNode->firstChild;
    
    while (child && index < count) {
        int i = 0;
        while (child->name[i] && i < 255) {
            entries[index].name[i] = child->name[i];
            i++;
        }
        entries[index].name[i] = '\0';
        
        entries[index].inode = child->inode;
        entries[index].type = child->type;
        
        index++;
        child = child->nextSibling;
    }

    ramNode->atime = ramfsNow();
    
    *read = index;
    return 0;
}

VNode* RamFS::nodeLookup(VNode* node, const char* name) {
    if (!node || !name) return nullptr;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Directory) return nullptr;
    
    RamFSNode* child = ramNode->firstChild;
    while (child) {
        if (ramfsNameEquals(name, child->name)) {
            VNode* childNode = new VNode(node->getFS(), child->inode, child->type);
            childNode->setData(child);
            childNode->ops = node->ops;
            return childNode;
        }
        
        child = child->nextSibling;
    }
    
    return nullptr;
}

int RamFS::nodeCreate(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    if (!parent || !name || !result) return -1;
    if (ramfsNameIsDotOrDotDot(name)) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    if (name[0] == '\0' || ramfsFindChild(parentNode, name)) return -1;
    
    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, FileType::Regular, mode);
    if (!newNode) return -1;
    
    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    const uint64_t now = ramfsNow();
    parentNode->mtime = now;
    parentNode->ctime = now;
    
    VNode* vnode = new VNode(parent->getFS(), newNode->inode, FileType::Regular);
    vnode->setData(newNode);
    vnode->ops = parent->ops;
    
    *result = vnode;
    return 0;
}

int RamFS::nodeMknod(VNode* parent, const char* name, uint32_t mode, uint64_t dev, VNode** result) {
    if (!parent || !name || !result) return -1;
    if (ramfsNameIsDotOrDotDot(name)) return -1;

    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    if (name[0] == '\0' || ramfsFindChild(parentNode, name)) return -1;

    // Pick the node type from the format bits of `mode`.
    constexpr uint32_t kSIFMT  = 0xF000;
    constexpr uint32_t kSIFIFO = 0x1000;
    constexpr uint32_t kSIFCHR = 0x2000;
    constexpr uint32_t kSIFBLK = 0x6000;
    constexpr uint32_t kSIFREG = 0x8000;
    FileType type;
    switch (mode & kSIFMT) {
        case kSIFIFO: type = FileType::Pipe; break;
        case kSIFCHR: type = FileType::CharDevice; break;
        case kSIFBLK: type = FileType::BlockDevice; break;
        case 0:       // POSIX: mode without a type means regular file
        case kSIFREG: type = FileType::Regular; break;
        default: return -1;  // sockets/unsupported
    }

    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, type, mode & 07777);
    if (!newNode) return -1;
    newNode->rdev = (type == FileType::CharDevice || type == FileType::BlockDevice) ? dev : 0;

    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    const uint64_t now = ramfsNow();
    parentNode->mtime = now;
    parentNode->ctime = now;

    VNode* vnode = new VNode(parent->getFS(), newNode->inode, type);
    vnode->setData(newNode);
    vnode->ops = parent->ops;

    *result = vnode;
    return 0;
}

int RamFS::nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    if (!parent || !name || !result) return -1;
    if (ramfsNameIsDotOrDotDot(name)) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    if (name[0] == '\0' || ramfsFindChild(parentNode, name)) return -1;
    
    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, FileType::Directory, mode);
    if (!newNode) return -1;
    
    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    const uint64_t now = ramfsNow();
    parentNode->mtime = now;
    parentNode->ctime = now;
    
    VNode* vnode = new VNode(parent->getFS(), newNode->inode, FileType::Directory);
    vnode->setData(newNode);
    vnode->ops = parent->ops;
    
    *result = vnode;
    return 0;
}

int RamFS::nodeUnlink(VNode* parent, const char* name) {
    if (!parent || !name) return -1;
    if (ramfsNameIsDotOrDotDot(name)) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFSNode* prev = nullptr;
    RamFSNode* child = parentNode->firstChild;
    
    while (child) {
        if (ramfsNameEquals(name, child->name)) {
            if (child->type == FileType::Directory) return -1;
            
            if (prev) {
                prev->nextSibling = child->nextSibling;
            } else {
                parentNode->firstChild = child->nextSibling;
            }
            const uint64_t now = ramfsNow();
            parentNode->mtime = now;
            parentNode->ctime = now;
            
            RamFS* fs = (RamFS*)parent->getFS();
            fs->destroyNode(child);
            return 0;
        }
        
        prev = child;
        child = child->nextSibling;
    }
    
    return -1;
}

int RamFS::nodeRmdir(VNode* parent, const char* name) {
    if (!parent || !name) return -1;
    if (ramfsNameIsDotOrDotDot(name)) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFSNode* prev = nullptr;
    RamFSNode* child = parentNode->firstChild;
    
    while (child) {
        if (ramfsNameEquals(name, child->name)) {
            if (child->type != FileType::Directory) return -1;
            if (child->firstChild) return -1;
            
            if (prev) {
                prev->nextSibling = child->nextSibling;
            } else {
                parentNode->firstChild = child->nextSibling;
            }
            const uint64_t now = ramfsNow();
            parentNode->mtime = now;
            parentNode->ctime = now;
            
            RamFS* fs = (RamFS*)parent->getFS();
            fs->destroyNode(child);
            return 0;
        }
        
        prev = child;
        child = child->nextSibling;
    }
    
    return -1;
}

int RamFS::nodeRename(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName) {
    if (!oldParent || !oldName || !newParent || !newName) return -1;
    if (newName[0] == '\0') return -1;
    if (ramfsNameIsDotOrDotDot(oldName) || ramfsNameIsDotOrDotDot(newName)) return -1;

    RamFSNode* oldParentNode = (RamFSNode*)oldParent->getData();
    RamFSNode* newParentNode = (RamFSNode*)newParent->getData();
    if (!oldParentNode || !newParentNode) return -1;
    if (oldParentNode->type != FileType::Directory || newParentNode->type != FileType::Directory) return -1;

    RamFSNode* source = ramfsFindChild(oldParentNode, oldName);
    if (!source) return -1;

    if (oldParentNode == newParentNode && ramfsNameEquals(oldName, newName)) {
        return 0;
    }

    if (source->type == FileType::Directory && ramfsIsDescendantOf(newParentNode, source)) {
        return -1;
    }

    RamFSNode* target = ramfsFindChild(newParentNode, newName);
    if (target) {
        if (target == source) {
            return 0;
        }
        if (source->type == FileType::Regular && target->type == FileType::Regular && source->data == target->data) {
            return 0;
        }
        if (target->type == FileType::Directory && target->firstChild) {
            return -1;
        }
        if (source->type == FileType::Directory && target->type != FileType::Directory) {
            return -1;
        }
        if (source->type != FileType::Directory && target->type == FileType::Directory) {
            return -1;
        }
        if (!ramfsDetachChild(newParentNode, target)) {
            return -1;
        }
        RamFS* fs = (RamFS*)newParent->getFS();
        fs->destroyNode(target);
    }

    if (!ramfsDetachChild(oldParentNode, source)) {
        return -1;
    }

    ramfsSetName(source, newName);
    source->parent = newParentNode;
    source->nextSibling = newParentNode->firstChild;
    newParentNode->firstChild = source;
    const uint64_t now = ramfsNow();
    source->ctime = now;
    oldParentNode->mtime = now;
    oldParentNode->ctime = now;
    newParentNode->mtime = now;
    newParentNode->ctime = now;
    return 0;
}

int RamFS::nodeLink(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName) {
    if (!oldParent || !oldName || !newParent || !newName) return -1;
    if (oldName[0] == '\0' || newName[0] == '\0') return -1;
    if (ramfsNameIsDotOrDotDot(oldName) || ramfsNameIsDotOrDotDot(newName)) return -1;

    RamFSNode* oldParentNode = (RamFSNode*)oldParent->getData();
    RamFSNode* newParentNode = (RamFSNode*)newParent->getData();
    if (!oldParentNode || !newParentNode) return -1;
    if (oldParentNode->type != FileType::Directory || newParentNode->type != FileType::Directory) return -1;
    if (ramfsFindChild(newParentNode, newName)) return -1;

    RamFSNode* source = ramfsFindChild(oldParentNode, oldName);
    if (!source || source->type != FileType::Regular) return -1;

    RamFSFileData* file = (RamFSFileData*)source->data;
    if (!file) return -1;

    RamFSNode* linkNode = (RamFSNode*)kmalloc(sizeof(RamFSNode));
    if (!linkNode) return -1;

    ramfsSetName(linkNode, newName);
    linkNode->type = FileType::Regular;
    linkNode->inode = source->inode;
    linkNode->mode = file->mode;
    linkNode->size = file->size;
    linkNode->data = file;
    linkNode->parent = newParentNode;
    linkNode->firstChild = nullptr;
    linkNode->nextSibling = newParentNode->firstChild;
    linkNode->atime = file->atime;
    linkNode->mtime = file->mtime;
    const uint64_t now = ramfsNow();
    linkNode->ctime = now;

    file->links++;
    file->ctime = now;
    newParentNode->firstChild = linkNode;
    newParentNode->mtime = now;
    newParentNode->ctime = now;
    source->ctime = now;
    return 0;
}

int RamFS::nodeSymlink(VNode* parent, const char* name, const char* target, VNode** result) {
    if (!parent || !name || !target || !result) return -1;
    if (name[0] == '\0' || ramfsNameIsDotOrDotDot(name)) return -1;

    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    if (ramfsFindChild(parentNode, name)) return -1;

    uint64_t len = 0;
    while (target[len]) len++;
    if (len > 255) return -1;

    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, FileType::Symlink, 0777);
    if (!newNode) return -1;

    char* stored = (char*)kmalloc(len + 1);
    if (!stored) {
        fs->destroyNode(newNode);
        return -1;
    }
    for (uint64_t i = 0; i < len; ++i) stored[i] = target[i];
    stored[len] = '\0';
    newNode->data = stored;
    newNode->size = len;
    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    const uint64_t now = ramfsNow();
    parentNode->mtime = now;
    parentNode->ctime = now;

    VNode* vnode = new VNode(parent->getFS(), newNode->inode, FileType::Symlink);
    vnode->setData(newNode);
    vnode->ops = parent->ops;
    *result = vnode;
    return 0;
}

int64_t RamFS::nodeReadlink(VNode* node, char* buffer, uint64_t size) {
    if (!node || !buffer) return -1;
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Symlink || !ramNode->data) return -1;

    uint64_t copied = ramNode->size < size ? ramNode->size : size;
    const char* target = (const char*)ramNode->data;
    for (uint64_t i = 0; i < copied; ++i) buffer[i] = target[i];
    ramNode->atime = ramfsNow();
    return copied;
}
