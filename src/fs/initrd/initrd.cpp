#include <fs/initrd/initrd.hpp>
#include <common/string.hpp>

constexpr uint32_t INITRD_MAGIC = 0x44524E49;

InitrdFS::InitrdFS(void* data, size_t size) : FileSystem("initrd"), data(data), dataSize(size), header(nullptr), rootNode(nullptr) {
    if (data && size >= sizeof(InitrdHeader)) {
        header = static_cast<InitrdHeader*>(data);
        if (header->magic != INITRD_MAGIC) {
            header = nullptr;
        }
    }
    
    ops.open = nodeOpen;
    ops.close = nodeClose;
    ops.read = nodeRead;
    ops.write = nodeWrite;
    ops.stat = nodeStat;
    ops.readdir = nodeReaddir;
    ops.lookup = nodeLookup;
    ops.create = nullptr;
    ops.mkdir = nullptr;
    ops.unlink = nullptr;
    ops.rmdir = nullptr;
}

InitrdFS::~InitrdFS() {
    if (rootNode) {
        delete rootNode;
    }
}

int InitrdFS::mount(const char* path) {
    if (!header) return -1;
    
    rootNode = new VNode(this, 0, FileType::Directory);
    rootNode->ops = &ops;
    
    return 0;
}

int InitrdFS::unmount() {
    return 0;
}

VNode* InitrdFS::getRoot() {
    return rootNode;
}

int InitrdFS::nodeOpen(VNode* node, int flags) {
    return 0;
}

int InitrdFS::nodeClose(VNode* node) {
    return 0;
}

int64_t InitrdFS::nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset) {
    if (!node || !buffer) return -1;
    
    InitrdFS* fs = static_cast<InitrdFS*>(node->getFS());
    if (!fs || !fs->header) return -1;
    
    uint64_t inode = node->getInode();
    if (inode == 0 || inode > fs->header->fileCount) return -1;
    
    InitrdFile* file = &fs->header->files[inode - 1];
    
    if (offset >= file->size) return 0;
    
    uint64_t toRead = size;
    if (offset + toRead > file->size) {
        toRead = file->size - offset;
    }
    
    uint8_t* fileData = static_cast<uint8_t*>(fs->data) + file->offset + offset;
    memcpy(buffer, fileData, toRead);
    
    return toRead;
}

int64_t InitrdFS::nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    return -1;
}

int InitrdFS::nodeStat(VNode* node, FileStats* stats) {
    if (!node || !stats) return -1;
    
    InitrdFS* fs = static_cast<InitrdFS*>(node->getFS());
    if (!fs || !fs->header) return -1;
    
    stats->type = node->getType();
    stats->inode = node->getInode();
    
    if (node->getInode() == 0) {
        stats->size = 0;
        stats->mode = 0755;
    } else {
        uint64_t inode = node->getInode();
        if (inode > fs->header->fileCount) return -1;
        
        InitrdFile* file = &fs->header->files[inode - 1];
        stats->size = file->size;
        stats->mode = 0644;
    }
    
    return 0;
}

int InitrdFS::nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read) {
    if (!node || !entries || !read) return -1;
    
    InitrdFS* fs = static_cast<InitrdFS*>(node->getFS());
    if (!fs || !fs->header) return -1;
    
    if (node->getInode() != 0) return -1;
    
    *read = 0;
    for (uint32_t i = 0; i < fs->header->fileCount && *read < count; i++) {
        InitrdFile* file = &fs->header->files[i];
        
        int j = 0;
        while (file->name[j] && j < 63) {
            entries[*read].name[j] = file->name[j];
            j++;
        }
        entries[*read].name[j] = '\0';
        
        entries[*read].inode = i + 1;
        entries[*read].type = FileType::Regular;
        
        (*read)++;
    }
    
    return 0;
}

VNode* InitrdFS::nodeLookup(VNode* node, const char* name) {
    if (!node || !name) return nullptr;
    
    InitrdFS* fs = static_cast<InitrdFS*>(node->getFS());
    if (!fs || !fs->header) return nullptr;
    
    if (node->getInode() != 0) return nullptr;
    
    for (uint32_t i = 0; i < fs->header->fileCount; i++) {
        InitrdFile* file = &fs->header->files[i];
        
        bool match = true;
        int j = 0;
        while (name[j] && file->name[j]) {
            if (name[j] != file->name[j]) {
                match = false;
                break;
            }
            j++;
        }
        
        if (match && name[j] == '\0' && file->name[j] == '\0') {
            VNode* fileNode = new VNode(fs, i + 1, FileType::Regular);
            fileNode->ops = &fs->ops;
            return fileNode;
        }
    }
    
    return nullptr;
}
