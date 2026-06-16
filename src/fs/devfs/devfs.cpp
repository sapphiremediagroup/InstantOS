#include <fs/devfs/devfs.hpp>
#include <cpu/tty/pty.hpp>
#include <memory/heap.hpp>
#include <common/string.hpp>
#include <common/krandom.hpp>

namespace {
DevFS* gActiveDevFS = nullptr;

bool nameEquals(const char* a, const char* b) {
    int i = 0;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return true;
}

// Parse a decimal pts index ("0".."63"). Returns false if not a pure number.
bool parsePtsIndex(const char* name, uint32_t* out) {
    if (!name || name[0] == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(name[i] - '0');
        if (value >= 64) {
            return false;
        }
    }
    *out = value;
    return true;
}

void writeDecimal(char* out, uint32_t value) {
    char tmp[16];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0) {
        tmp[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    int o = 0;
    while (n > 0) {
        out[o++] = tmp[--n];
    }
    out[o] = '\0';
}

int devDirStat(VNode*, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::Directory;
    stats->mode = 0040000 | 0755;
    stats->links = 2;
    stats->dev = reinterpret_cast<uint64_t>(gActiveDevFS);
    return 0;
}

int devCharStat(VNode* node, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::CharDevice;
    stats->mode = 0020000 | 0666;
    stats->links = 1;
    stats->inode = node ? node->getInode() : 0;
    stats->dev = reinterpret_cast<uint64_t>(gActiveDevFS);
    return 0;
}

int devStatfs(VNode*, FsStats* stats) {
    if (!stats) return -1;
    // DevFS is a pseudo-filesystem with no storage.
    stats->blockSize = 4096;
    stats->totalBlocks = 0;
    stats->freeBlocks = 0;
    stats->totalInodes = 0;
    stats->freeInodes = 0;
    stats->nameMax = 255;
    stats->fsType = 0x1373;  // arbitrary devfs magic
    return 0;
}

VNode* devRootLookup(VNode*, const char* name) {
    if (!gActiveDevFS || !name) {
        return nullptr;
    }
    if (nameEquals(name, "ptmx")) {
        return gActiveDevFS->getPtmxNode();
    }
    if (nameEquals(name, "pts")) {
        return gActiveDevFS->getPtsDirNode();
    }
    if (nameEquals(name, "tty")) {
        return gActiveDevFS->getTtyNode();
    }
    if (nameEquals(name, "null")) {
        return gActiveDevFS->getNullNode();
    }
    if (nameEquals(name, "zero")) {
        return gActiveDevFS->getZeroNode();
    }
    if (nameEquals(name, "urandom")) {
        return gActiveDevFS->getUrandomNode();
    }
    if (nameEquals(name, "random")) {
        return gActiveDevFS->getRandomNode();
    }
    return nullptr;
}

VNode* devPtsLookup(VNode*, const char* name) {
    if (!gActiveDevFS || !name) {
        return nullptr;
    }
    uint32_t index = 0;
    if (!parsePtsIndex(name, &index)) {
        return nullptr;
    }
    if (!PtyManager::get().deviceForIndex(index)) {
        return nullptr;
    }
    return gActiveDevFS->ptsSlaveMarker(index);
}

int devRootReaddir(VNode*, DirEntry* entries, uint64_t count, uint64_t* read) {
    static const char* names[] = { ".", "..", "ptmx", "pts", "tty", "null", "zero", "urandom", "random" };
    constexpr uint64_t kCount = 9;
    uint64_t produced = 0;
    for (uint64_t i = 0; i < kCount && produced < count; i++) {
        DirEntry& e = entries[produced];
        memset(&e, 0, sizeof(e));
        int j = 0;
        while (names[i][j] && j < 255) {
            e.name[j] = names[i][j];
            j++;
        }
        e.name[j] = '\0';
        e.type = (i <= 1 || i == 3) ? FileType::Directory : FileType::CharDevice;
        produced++;
    }
    if (read) {
        *read = produced;
    }
    return 0;
}

int devPtsReaddir(VNode*, DirEntry* entries, uint64_t count, uint64_t* read) {
    uint64_t produced = 0;
    const char* dots[] = { ".", ".." };
    for (uint64_t i = 0; i < 2 && produced < count; i++) {
        DirEntry& e = entries[produced];
        memset(&e, 0, sizeof(e));
        e.name[0] = dots[i][0];
        if (i == 1) {
            e.name[1] = '.';
        }
        e.type = FileType::Directory;
        produced++;
    }
    for (uint32_t idx = 0; idx < 64 && produced < count; idx++) {
        if (!PtyManager::get().deviceForIndex(idx)) {
            continue;
        }
        DirEntry& e = entries[produced];
        memset(&e, 0, sizeof(e));
        writeDecimal(e.name, idx);
        e.type = FileType::CharDevice;
        produced++;
    }
    if (read) {
        *read = produced;
    }
    return 0;
}

// /dev/null: read returns EOF (0), write discards everything.
int64_t devNullRead(VNode*, void*, uint64_t, uint64_t) { return 0; }
int64_t devNullWrite(VNode*, const void*, uint64_t size, uint64_t) { return (int64_t)size; }

// /dev/zero: read fills zeros, write discards.
int64_t devZeroRead(VNode*, void* buffer, uint64_t size, uint64_t) {
    if (buffer && size) memset(buffer, 0, size);
    return (int64_t)size;
}

// /dev/urandom and /dev/random: read fills with kernel entropy.
int64_t devRandomRead(VNode*, void* buffer, uint64_t size, uint64_t) {
    if (!buffer) return -1;
    if (size) kernel_fill_entropy(buffer, size);
    return (int64_t)size;
}
}  // namespace

DevFS::DevFS() : FileSystem("devfs"),
                 rootNode(nullptr), ptmxNode(nullptr), ptsDirNode(nullptr), ttyNode(nullptr),
                 nullNode(nullptr), zeroNode(nullptr), urandomNode(nullptr), randomNode(nullptr) {
    memset(&rootOps, 0, sizeof(rootOps));
    memset(&ptmxOps, 0, sizeof(ptmxOps));
    memset(&ptsDirOps, 0, sizeof(ptsDirOps));
    memset(&nullOps, 0, sizeof(nullOps));
    memset(&zeroOps, 0, sizeof(zeroOps));
    memset(&randomOps, 0, sizeof(randomOps));
    for (int i = 0; i < 64; i++) {
        ptsSlaveNodes[i] = nullptr;
    }

    rootOps.stat = devDirStat;
    rootOps.lookup = devRootLookup;
    rootOps.readdir = devRootReaddir;
    rootOps.statfs = devStatfs;

    ptmxOps.stat = devCharStat;

    ptsDirOps.stat = devDirStat;
    ptsDirOps.lookup = devPtsLookup;
    ptsDirOps.readdir = devPtsReaddir;

    // /dev/null + /dev/zero + /dev/urandom + /dev/random char devices.
    nullOps.stat = devCharStat;
    nullOps.read = devNullRead;
    nullOps.write = devNullWrite;

    zeroOps.stat = devCharStat;
    zeroOps.read = devZeroRead;
    zeroOps.write = devNullWrite;

    randomOps.stat = devCharStat;
    randomOps.read = devRandomRead;
    randomOps.write = devNullWrite;

    rootNode = new VNode(this, KindRoot, FileType::Directory);
    ptmxNode = new VNode(this, KindPtmx, FileType::CharDevice);
    ptsDirNode = new VNode(this, KindPtsDir, FileType::Directory);
    ttyNode = new VNode(this, KindTty, FileType::CharDevice);
    nullNode = new VNode(this, KindNull, FileType::CharDevice);
    zeroNode = new VNode(this, KindZero, FileType::CharDevice);
    urandomNode = new VNode(this, KindUrandom, FileType::CharDevice);
    randomNode = new VNode(this, KindRandom, FileType::CharDevice);

    if (rootNode) { rootNode->ops = &rootOps; rootNode->refCount = 1; }
    if (ptmxNode) { ptmxNode->ops = &ptmxOps; ptmxNode->refCount = 1; }
    if (ptsDirNode) { ptsDirNode->ops = &ptsDirOps; ptsDirNode->refCount = 1; }
    if (ttyNode) { ttyNode->ops = &ptmxOps; ttyNode->refCount = 1; }
    if (nullNode) { nullNode->ops = &nullOps; nullNode->refCount = 1; }
    if (zeroNode) { zeroNode->ops = &zeroOps; zeroNode->refCount = 1; }
    if (urandomNode) { urandomNode->ops = &randomOps; urandomNode->refCount = 1; }
    if (randomNode) { randomNode->ops = &randomOps; randomNode->refCount = 1; }

    gActiveDevFS = this;
}

DevFS::~DevFS() {
    if (gActiveDevFS == this) {
        gActiveDevFS = nullptr;
    }
    delete rootNode;
    delete ptmxNode;
    delete ptsDirNode;
    delete ttyNode;
    delete nullNode;
    delete zeroNode;
    delete urandomNode;
    delete randomNode;
    for (int i = 0; i < 64; i++) {
        delete ptsSlaveNodes[i];
    }
}

int DevFS::mount(const char*) { return 0; }
int DevFS::unmount() { return 0; }
VNode* DevFS::getRoot() { return rootNode; }

VNode* DevFS::ptsSlaveMarker(uint32_t index) {
    if (index >= 64) {
        return nullptr;
    }
    if (!ptsSlaveNodes[index]) {
        VNode* node = new VNode(this, kPtsSlaveInodeBase + index, FileType::CharDevice);
        if (!node) {
            return nullptr;
        }
        node->ops = &ptmxOps;  // stat-only marker
        node->refCount = 1;
        ptsSlaveNodes[index] = node;
    }
    return ptsSlaveNodes[index];
}
