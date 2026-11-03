#include <fs/devfs/devfs.hpp>
#include <cpu/tty/pty.hpp>
#include <memory/heap.hpp>
#include <common/string.hpp>
#include <common/krandom.hpp>
#include <graphics/console.hpp>
#include <cpu/cereal/cereal.hpp>
#include <graphics/intel_gen9_drm.hpp>

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

// /dev/dri/renderD128 as stat sees it: Linux's DRM major, render minor 128
// (what libdrm checks before it reads /sys/dev/char/226:128).
int devRenderStat(VNode* node, FileStats* stats) {
    if (devCharStat(node, stats) != 0) {
        return -1;
    }
    stats->rdev = (226u << 8) | 128u;
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
    if (nameEquals(name, "console")) {
        return gActiveDevFS->getConsoleNode();
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
    if (nameEquals(name, "dri") && drm::available()) {
        return gActiveDevFS->getDriDirNode();
    }
    return nullptr;
}

VNode* devDriLookup(VNode*, const char* name) {
    if (!gActiveDevFS || !name || !drm::available()) {
        return nullptr;
    }
    return nameEquals(name, "renderD128") ? gActiveDevFS->getRenderNode() : nullptr;
}

// A directory listing from fixed names; those whose bit is set in dirsMask are directories.
int fixedReaddir(const char* const* names, uint64_t total, uint64_t dirsMask, FileType other, DirEntry* entries,
                 uint64_t count, uint64_t* read) {
    uint64_t produced = 0;
    for (uint64_t i = 0; i < total && produced < count; i++) {
        DirEntry& e = entries[produced];
        memset(&e, 0, sizeof(e));
        int j = 0;
        while (names[i][j] && j < 255) {
            e.name[j] = names[i][j];
            j++;
        }
        e.name[j] = '\0';
        e.type = (dirsMask & (1ull << i)) ? FileType::Directory : other;
        produced++;
    }
    if (read) {
        *read = produced;
    }
    return 0;
}

int devDriReaddir(VNode*, DirEntry* entries, uint64_t count, uint64_t* read) {
    static const char* names[] = { ".", "..", "renderD128" };
    return fixedReaddir(names, drm::available() ? 3 : 2, 0x3, FileType::CharDevice, entries, count, read);
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
    static const char* names[] = { ".", "..", "ptmx", "pts", "tty", "console", "null", "zero", "urandom", "random", "dri" };
    if (drm::available()) {
        return fixedReaddir(names, 11, (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 10), FileType::CharDevice,
                            entries, count, read);
    }
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

// /dev/console: a real, dup-able char device for userland stdio. Read returns
// EOF; write mirrors to the framebuffer console and the serial port (matching
// the kernel's implicit fd 1/2 path). Giving processes a bound console handle
// lets them dup()/dup2() their stdout/stderr (e.g. autoconf `exec 6>&1`), which
// the bare implicit console (no handle) cannot support.
int64_t devConsoleRead(VNode*, void*, uint64_t, uint64_t) { return 0; }
int64_t devConsoleWrite(VNode*, const void* buffer, uint64_t size, uint64_t) {
    if (!buffer) return -1;
    const char* p = static_cast<const char*>(buffer);
    // Console::drawText already mirrors each char to the serial port
    // (writeCharUnlocked -> Cereal), so do NOT also write to Cereal here (that
    // would double every byte on the serial capture).
    for (uint64_t i = 0; i < size; i++) {
        char temp[2] = { p[i], '\0' };
        Console::get().drawText(temp);
    }
    return (int64_t)size;
}

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
                 consoleNode(nullptr),
                 nullNode(nullptr), zeroNode(nullptr), urandomNode(nullptr), randomNode(nullptr),
                 driDirNode(nullptr), renderNode(nullptr) {
    memset(&rootOps, 0, sizeof(rootOps));
    memset(&ptmxOps, 0, sizeof(ptmxOps));
    memset(&ptsDirOps, 0, sizeof(ptsDirOps));
    memset(&consoleOps, 0, sizeof(consoleOps));
    memset(&nullOps, 0, sizeof(nullOps));
    memset(&zeroOps, 0, sizeof(zeroOps));
    memset(&randomOps, 0, sizeof(randomOps));
    memset(&driDirOps, 0, sizeof(driDirOps));
    memset(&renderOps, 0, sizeof(renderOps));
    driDirOps.stat = devDirStat;
    driDirOps.lookup = devDriLookup;
    driDirOps.readdir = devDriReaddir;
    renderOps.stat = devRenderStat;
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

    consoleOps.stat = devCharStat;
    consoleOps.read = devConsoleRead;
    consoleOps.write = devConsoleWrite;

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
    consoleNode = new VNode(this, KindConsole, FileType::CharDevice);
    nullNode = new VNode(this, KindNull, FileType::CharDevice);
    zeroNode = new VNode(this, KindZero, FileType::CharDevice);
    urandomNode = new VNode(this, KindUrandom, FileType::CharDevice);
    randomNode = new VNode(this, KindRandom, FileType::CharDevice);
    driDirNode = new VNode(this, KindDriDir, FileType::Directory);
    renderNode = new VNode(this, KindRenderNode, FileType::CharDevice);
    if (driDirNode) { driDirNode->ops = &driDirOps; driDirNode->refCount = 1; }
    if (renderNode) { renderNode->ops = &renderOps; renderNode->refCount = 1; }

    if (rootNode) { rootNode->ops = &rootOps; rootNode->refCount = 1; }
    if (ptmxNode) { ptmxNode->ops = &ptmxOps; ptmxNode->refCount = 1; }
    if (ptsDirNode) { ptsDirNode->ops = &ptsDirOps; ptsDirNode->refCount = 1; }
    if (ttyNode) { ttyNode->ops = &ptmxOps; ttyNode->refCount = 1; }
    if (consoleNode) { consoleNode->ops = &consoleOps; consoleNode->refCount = 1; }
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
    delete consoleNode;
    delete nullNode;
    delete zeroNode;
    delete urandomNode;
    delete randomNode;
    delete driDirNode;
    delete renderNode;
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
