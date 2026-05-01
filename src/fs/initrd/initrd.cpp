#include <fs/initrd/initrd.hpp>
#include <common/string.hpp>

constexpr uint32_t INITRD_MAGIC = 0x44524E49;
static constexpr uint64_t INITRD_DIRECTORY_INODE_BASE = 0x8000000000000000ULL;

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
    if (node->getType() != FileType::Regular) return -1;
    
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
    
    if (node->getType() == FileType::Directory) {
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

    if (node->getType() != FileType::Directory) return -1;

    const char* prefix = nodePathPrefix(node);
    *read = 0;

    for (uint32_t i = 0; i < fs->header->fileCount && *read < count; i++) {
        InitrdFile* file = &fs->header->files[i];

        char childName[256];
        bool isDirectory = false;
        if (!splitImmediateChild(prefix, file->name, childName, &isDirectory)) {
            continue;
        }

        bool duplicate = false;
        for (uint64_t entryIndex = 0; entryIndex < *read; ++entryIndex) {
            if (strcmp(entries[entryIndex].name, childName) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        strncpy(entries[*read].name, childName, sizeof(entries[*read].name) - 1);
        entries[*read].name[sizeof(entries[*read].name) - 1] = '\0';
        entries[*read].inode = isDirectory ? (INITRD_DIRECTORY_INODE_BASE + i + 1) : (i + 1);
        entries[*read].type = isDirectory ? FileType::Directory : FileType::Regular;
        (*read)++;
    }
    
    return 0;
}

VNode* InitrdFS::nodeLookup(VNode* node, const char* name) {
    if (!node || !name) return nullptr;
    
    InitrdFS* fs = static_cast<InitrdFS*>(node->getFS());
    if (!fs || !fs->header) return nullptr;

    if (node->getType() != FileType::Directory) return nullptr;

    const char* prefix = nodePathPrefix(node);
    for (uint32_t i = 0; i < fs->header->fileCount; i++) {
        InitrdFile* file = &fs->header->files[i];

        char childName[256];
        bool isDirectory = false;
        if (!splitImmediateChild(prefix, file->name, childName, &isDirectory)) {
            continue;
        }

        if (strcmp(childName, name) != 0) {
            continue;
        }

        if (isDirectory) {
            const size_t prefixLength = strlen(prefix);
            const size_t nameLength = strlen(name);
            const size_t fullLength = prefixLength == 0 ? nameLength : prefixLength + 1 + nameLength;
            char* childPrefix = new char[fullLength + 1];
            if (!childPrefix) {
                return nullptr;
            }

            if (prefixLength == 0) {
                strcpy(childPrefix, name);
            } else {
                strcpy(childPrefix, prefix);
                childPrefix[prefixLength] = '/';
                strcpy(childPrefix + prefixLength + 1, name);
            }

            return createDirectoryNode(fs, childPrefix);
        }

        VNode* fileNode = new VNode(fs, i + 1, FileType::Regular);
        fileNode->ops = &fs->ops;
        return fileNode;
    }
    
    return nullptr;
}

const char* InitrdFS::nodePathPrefix(VNode* node) {
    if (!node || node->getInode() == 0) {
        return "";
    }

    const char* prefix = static_cast<const char*>(node->getData());
    return prefix ? prefix : "";
}

bool InitrdFS::splitImmediateChild(const char* prefix, const char* fullPath, char* childName, bool* isDirectory) {
    if (!fullPath || !childName || !isDirectory) {
        return false;
    }

    const size_t prefixLength = prefix ? strlen(prefix) : 0;
    const char* remainder = fullPath;

    if (prefixLength != 0) {
        if (strncmp(fullPath, prefix, prefixLength) != 0 || fullPath[prefixLength] != '/') {
            return false;
        }
        remainder = fullPath + prefixLength + 1;
    }

    if (*remainder == '\0') {
        return false;
    }

    const char* slash = static_cast<const char*>(memchr(remainder, '/', strlen(remainder)));
    size_t childLength = slash ? static_cast<size_t>(slash - remainder) : strlen(remainder);
    if (childLength == 0 || childLength >= 256) {
        return false;
    }

    strncpy(childName, remainder, childLength);
    childName[childLength] = '\0';
    *isDirectory = slash != nullptr;
    return true;
}

VNode* InitrdFS::createDirectoryNode(InitrdFS* fs, const char* prefix) {
    if (!fs) {
        return nullptr;
    }

    VNode* directoryNode = new VNode(fs, INITRD_DIRECTORY_INODE_BASE, FileType::Directory);
    directoryNode->ops = &fs->ops;
    directoryNode->setData(const_cast<char*>(prefix));
    return directoryNode;
}
