#include <fs/ramfs/ramfs.hpp>
#include <memory/heap.hpp>

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
    node->atime = 0;
    node->mtime = 0;
    node->ctime = 0;
    
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
    
    if (node->data) {
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
    
    if (offset >= ramNode->size) return 0;
    
    uint64_t toRead = size;
    if (offset + toRead > ramNode->size) {
        toRead = ramNode->size - offset;
    }
    
    if (ramNode->data) {
        uint8_t* src = (uint8_t*)ramNode->data + offset;
        uint8_t* dst = (uint8_t*)buffer;
        for (uint64_t i = 0; i < toRead; i++) {
            dst[i] = src[i];
        }
    }
    
    return toRead;
}

int64_t RamFS::nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Regular) return -1;
    
    uint64_t newSize = offset + size;
    if (newSize > ramNode->size) {
        void* newData = kmalloc(newSize);
        if (!newData) return -1;
        
        if (ramNode->data) {
            uint8_t* src = (uint8_t*)ramNode->data;
            uint8_t* dst = (uint8_t*)newData;
            for (uint64_t i = 0; i < ramNode->size; i++) {
                dst[i] = src[i];
            }
            kfree(ramNode->data);
        }
        
        ramNode->data = newData;
        ramNode->size = newSize;
    }
    
    uint8_t* dst = (uint8_t*)ramNode->data + offset;
    const uint8_t* src = (const uint8_t*)buffer;
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    
    return size;
}

int RamFS::nodeStat(VNode* node, FileStats* stats) {
    if (!node || !stats) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode) return -1;
    
    stats->size = ramNode->size;
    stats->type = ramNode->type;
    stats->mode = ramNode->mode;
    stats->inode = ramNode->inode;
    stats->links = 1;
    stats->atime = ramNode->atime;
    stats->mtime = ramNode->mtime;
    stats->ctime = ramNode->ctime;
    
    return 0;
}

int RamFS::nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read) {
    if (!node || !entries || !read) return -1;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Directory) return -1;
    
    uint64_t index = 0;
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
    
    *read = index;
    return 0;
}

VNode* RamFS::nodeLookup(VNode* node, const char* name) {
    if (!node || !name) return nullptr;
    
    RamFSNode* ramNode = (RamFSNode*)node->getData();
    if (!ramNode || ramNode->type != FileType::Directory) return nullptr;
    
    RamFSNode* child = ramNode->firstChild;
    while (child) {
        bool match = true;
        int i = 0;
        while (name[i] || child->name[i]) {
            if (name[i] != child->name[i]) {
                match = false;
                break;
            }
            i++;
        }
        
        if (match) {
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
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, FileType::Regular, mode);
    if (!newNode) return -1;
    
    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    
    VNode* vnode = new VNode(parent->getFS(), newNode->inode, FileType::Regular);
    vnode->setData(newNode);
    vnode->ops = parent->ops;
    
    *result = vnode;
    return 0;
}

int RamFS::nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    if (!parent || !name || !result) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFS* fs = (RamFS*)parent->getFS();
    RamFSNode* newNode = fs->createNode(name, FileType::Directory, mode);
    if (!newNode) return -1;
    
    newNode->parent = parentNode;
    newNode->nextSibling = parentNode->firstChild;
    parentNode->firstChild = newNode;
    
    VNode* vnode = new VNode(parent->getFS(), newNode->inode, FileType::Directory);
    vnode->setData(newNode);
    vnode->ops = parent->ops;
    
    *result = vnode;
    return 0;
}

int RamFS::nodeUnlink(VNode* parent, const char* name) {
    if (!parent || !name) return -1;
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFSNode* prev = nullptr;
    RamFSNode* child = parentNode->firstChild;
    
    while (child) {
        bool match = true;
        int i = 0;
        while (name[i] || child->name[i]) {
            if (name[i] != child->name[i]) {
                match = false;
                break;
            }
            i++;
        }
        
        if (match) {
            if (child->type == FileType::Directory) return -1;
            
            if (prev) {
                prev->nextSibling = child->nextSibling;
            } else {
                parentNode->firstChild = child->nextSibling;
            }
            
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
    
    RamFSNode* parentNode = (RamFSNode*)parent->getData();
    if (!parentNode || parentNode->type != FileType::Directory) return -1;
    
    RamFSNode* prev = nullptr;
    RamFSNode* child = parentNode->firstChild;
    
    while (child) {
        bool match = true;
        int i = 0;
        while (name[i] || child->name[i]) {
            if (name[i] != child->name[i]) {
                match = false;
                break;
            }
            i++;
        }
        
        if (match) {
            if (child->type != FileType::Directory) return -1;
            if (child->firstChild) return -1;
            
            if (prev) {
                prev->nextSibling = child->nextSibling;
            } else {
                parentNode->firstChild = child->nextSibling;
            }
            
            RamFS* fs = (RamFS*)parent->getFS();
            fs->destroyNode(child);
            return 0;
        }
        
        prev = child;
        child = child->nextSibling;
    }
    
    return -1;
}
