#include <fs/ext4/ext4.hpp>
#include <memory/heap.hpp>
#include <common/string.hpp>
#include <time/time.hpp>
#include <time/tsc_timer.hpp>

// Phase 1 ext2/3/4 read path. See include/fs/ext4/ext4.hpp for the on-disk
// layout. This translation unit intentionally depends only on the heap and the
// freestanding string helpers (no Console/graphics), so it builds with just
// `-I include` and can be exercised by a host test harness that supplies its
// own kmalloc/VNode shims and a file-backed BlockDevice.

using namespace ext4;

namespace {
// RAII guard: begins a journal transaction and, unless commit() is called,
// aborts it on destruction. All no-ops when write-journaling is disabled
// (jBegin/jCommit/jAbort short-circuit), so wrapping every write op is safe.
struct JTxn {
    Ext4FS* fs;
    bool committed;
    explicit JTxn(Ext4FS* f) : fs(f), committed(false) { fs->jBegin(); }
    ~JTxn() { if (!committed) fs->jAbort(); }
    int commit() { committed = true; return fs->jCommit(); }
};
}  // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

Ext4FS::Ext4FS(BlockDevice* device)
    : FileSystem("ext4"), device(device), rootNode(nullptr),
      blockSize(0), inodeSize(0), inodesPerGroup(0), blocksPerGroup(0),
      groupCount(0), firstDataBlock(0), descSize(0),
      has64bit(false), hasExtents(false), hasFileType(false),
      hasMetadataCsum(false), writable(false), mounted(false),
      jWriteEnabled(false), jActive(false), jSkipCheckpoint(false), jOverflow(false),
      jCount(0), jFirst(0), jMaxlen(0), jSeqNext(0), jSbPhys(0) {
    memset(jHash, 0, sizeof(jHash));
    ops.open = nodeOpen;
    ops.close = nodeClose;
    ops.read = nodeRead;
    ops.stat = nodeStat;
    ops.readdir = nodeReaddir;
    ops.lookup = nodeLookup;
    ops.readlink = nodeReadlink;
    ops.statfs = nodeStatfs;
    // Write side (phase 3). The ops are always wired; each one checks `writable`
    // and returns an error on read-only (checksummed) volumes.
    ops.write = nodeWrite;
    ops.create = nodeCreate;
    ops.mkdir = nodeMkdir;
    ops.unlink = nodeUnlink;
    ops.rmdir = nodeRmdir;
    ops.truncate = nodeTruncate;
    ops.chmod = nodeChmod;
    ops.chown = nodeChown;
    ops.utime = nodeUtime;
    ops.rename = nodeRename;
    ops.link = nodeLink;
    ops.symlink = nodeSymlink;
    ops.fsync = nodeFsync;
}

namespace {
constexpr uint32_t kMaxMounted = 4;
Ext4FS* g_mounted[kMaxMounted];
uint32_t g_mountedCount = 0;

void forgetMounted(Ext4FS* fs) {
    for (uint32_t i = 0; i < g_mountedCount; ++i) {
        if (g_mounted[i] == fs) {
            g_mounted[i] = g_mounted[--g_mountedCount];
            return;
        }
    }
}
}  // namespace

uint32_t Ext4FS::mountedCount() { return g_mountedCount; }
Ext4FS* Ext4FS::mountedVolume(uint32_t index) {
    return index < g_mountedCount ? g_mounted[index] : nullptr;
}

Ext4FS::~Ext4FS() {
    if (mounted) {
        sync(false);
    }
    jTxnReset();
    forgetMounted(this);
    if (rootNode) {
        Ext4Node* n = static_cast<Ext4Node*>(rootNode->getData());
        if (n) {
            kfree(n);
        }
        delete rootNode;
    }
    if (runBuf) kfree(runBuf);
    freeBlockCache();
}

int Ext4FS::unmount() {
    return sync(false);
}

int Ext4FS::sync(bool onlyDue) {
    if (!jActive) {
        return 0;
    }
    if (jCount == 0) {
        jActive = false;
        return 0;
    }
    if (onlyDue && !jCommitDue()) {
        return 0;
    }
    return jCommitRunning();
}

VNode* Ext4FS::getRoot() {
    return rootNode;
}

void Ext4FS::releaseNode(VNode* node) {
    if (!node || node == rootNode) {
        return;
    }
    if (node->getData()) {
        kfree(node->getData());
    }
    delete node;
}

// ---------------------------------------------------------------------------
// Low-level I/O
// ---------------------------------------------------------------------------

namespace { constexpr uint64_t kCacheInvalid = ~0ull; }

void Ext4FS::allocBlockCache() {
    if (cacheData || blockSize == 0) return;
    // ~2 MiB working set (direct-mapped). Enough for bitmaps, group descs, inode
    // tables and large directory scans to stay resident during a build.
    uint32_t slots = (2u * 1024u * 1024u) / blockSize;
    if (slots < 64) slots = 64;
    cacheData = static_cast<uint8_t*>(kmalloc(static_cast<uint64_t>(slots) * blockSize));
    cacheTag = static_cast<uint64_t*>(kmalloc(static_cast<uint64_t>(slots) * sizeof(uint64_t)));
    if (!cacheData || !cacheTag) { freeBlockCache(); return; }   // run uncached
    for (uint32_t i = 0; i < slots; ++i) cacheTag[i] = kCacheInvalid;
    cacheSlots = slots;
}

void Ext4FS::freeBlockCache() {
    if (cacheData) kfree(cacheData);
    if (cacheTag) kfree(cacheTag);
    cacheData = nullptr; cacheTag = nullptr; cacheSlots = 0;
}

// Return a pointer to `block`'s cached data, loading it from the device on miss.
// nullptr if caching is disabled or the device read failed.
uint8_t* Ext4FS::cacheSlotFor(uint64_t block) {
    if (!cacheSlots || !device) return nullptr;
    const uint32_t idx = static_cast<uint32_t>(block % cacheSlots);
    uint8_t* data = cacheData + static_cast<uint64_t>(idx) * blockSize;
    if (cacheTag[idx] == block) return data;                 // hit
    if (!device->read(block * static_cast<uint64_t>(blockSize), data, blockSize)) {
        cacheTag[idx] = kCacheInvalid;
        return nullptr;
    }
    cacheTag[idx] = block;                                    // evict + fill
    return data;
}

bool Ext4FS::devReadBytes(uint64_t byteOffset, void* buffer, uint64_t size) {
    if (!device) return false;
    if (!cacheSlots || blockSize == 0) {
        return device->read(byteOffset, buffer, size);
    }
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < size) {
        const uint64_t pos = byteOffset + done;
        const uint64_t block = pos / blockSize;
        const uint32_t boff = static_cast<uint32_t>(pos % blockSize);
        uint32_t chunk = blockSize - boff;
        if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);
        uint8_t* c = cacheSlotFor(block);
        if (c) {
            memcpy(dst + done, c + boff, chunk);
        } else if (!device->read(pos, dst + done, chunk)) {   // fallback on miss failure
            return false;
        }
        done += chunk;
    }
    return true;
}

bool Ext4FS::readBytes(uint64_t byteOffset, void* buffer, uint64_t size) {
    if (!jActive) {
        return devReadBytes(byteOffset, buffer, size);
    }
    // Inside a transaction, reads see buffered (uncommitted) metadata blocks.
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < size) {
        const uint64_t pos = byteOffset + done;
        const uint64_t block = pos / blockSize;
        const uint32_t boff = static_cast<uint32_t>(pos % blockSize);
        uint32_t chunk = blockSize - boff;
        if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);
        const uint8_t* cached = jTxnFind(block);
        if (cached) {
            memcpy(dst + done, cached + boff, chunk);
        } else if (!devReadBytes(pos, dst + done, chunk)) {
            return false;
        }
        done += chunk;
    }
    return true;
}

bool Ext4FS::readBlock(uint64_t block, void* buffer) {
    if (block == 0 || blockSize == 0) {
        return false;
    }
    return readBytes(block * static_cast<uint64_t>(blockSize), buffer, blockSize);
}

bool Ext4FS::readGroupDesc(uint32_t group, Ext4GroupDesc* out) {
    if (group >= groupCount || !out) {
        return false;
    }
    // The group descriptor table lives in the block immediately after the one
    // holding the superblock: block 2 for 1 KiB blocks (superblock in block 1),
    // block 1 otherwise (superblock at offset 1024 inside block 0).
    const uint64_t gdtStartBlock = static_cast<uint64_t>(firstDataBlock) + 1;
    const uint64_t byteOffset =
        gdtStartBlock * blockSize + static_cast<uint64_t>(group) * descSize;
    memset(out, 0, sizeof(*out));
    const uint32_t toRead =
        descSize <= sizeof(*out) ? descSize : static_cast<uint32_t>(sizeof(*out));
    return readBytes(byteOffset, out, toRead);
}

bool Ext4FS::readInode(uint32_t inodeNum, Ext4Inode* out) {
    if (inodeNum == 0 || !out || inodesPerGroup == 0) {
        return false;
    }
    const uint32_t group = (inodeNum - 1) / inodesPerGroup;
    const uint32_t index = (inodeNum - 1) % inodesPerGroup;
    if (group >= groupCount) {
        return false;
    }

    Ext4GroupDesc gd;
    if (!readGroupDesc(group, &gd)) {
        return false;
    }

    const uint64_t inodeTable = static_cast<uint64_t>(gd.bg_inode_table_lo) |
        (has64bit ? (static_cast<uint64_t>(gd.bg_inode_table_hi) << 32) : 0);
    if (inodeTable == 0) {
        return false;
    }

    const uint64_t byteOffset =
        inodeTable * blockSize + static_cast<uint64_t>(index) * inodeSize;
    // The on-disk inode is `inodeSize` bytes (>= 128), but every field the read
    // path needs lives in the first 128 bytes, so only the base is copied.
    memset(out, 0, sizeof(*out));
    return readBytes(byteOffset, out, sizeof(Ext4Inode));
}

// ---------------------------------------------------------------------------
// Logical -> physical block resolution
// ---------------------------------------------------------------------------

uint64_t Ext4FS::readIndirectPointer(uint64_t blockOfPointers, uint32_t index) {
    if (blockOfPointers == 0 || blockSize == 0) {
        return 0;
    }
    uint32_t* buf = static_cast<uint32_t*>(kmalloc(blockSize));
    if (!buf) {
        return 0;
    }
    uint64_t value = 0;
    if (readBlock(blockOfPointers, buf) && index < blockSize / 4) {
        value = buf[index];
    }
    kfree(buf);
    return value;
}

uint64_t Ext4FS::resolveViaBlockMap(const uint32_t iBlock[15], uint64_t lb) {
    const uint64_t ppb = blockSize / 4;   // pointers per indirect block
    if (ppb == 0) {
        return 0;
    }

    if (lb < 12) {
        return iBlock[lb];
    }
    lb -= 12;

    if (lb < ppb) {   // single indirect
        return readIndirectPointer(iBlock[12], static_cast<uint32_t>(lb));
    }
    lb -= ppb;

    if (lb < ppb * ppb) {   // double indirect
        const uint64_t l1 =
            readIndirectPointer(iBlock[13], static_cast<uint32_t>(lb / ppb));
        return readIndirectPointer(l1, static_cast<uint32_t>(lb % ppb));
    }
    lb -= ppb * ppb;

    // triple indirect
    const uint64_t l1 =
        readIndirectPointer(iBlock[14], static_cast<uint32_t>(lb / (ppb * ppb)));
    const uint64_t rem = lb % (ppb * ppb);
    const uint64_t l2 =
        readIndirectPointer(l1, static_cast<uint32_t>(rem / ppb));
    return readIndirectPointer(l2, static_cast<uint32_t>(rem % ppb));
}

uint64_t Ext4FS::resolveViaExtents(const uint32_t iBlock[15], uint64_t logicalBlock) {
    uint8_t* scratch = nullptr;              // lazily allocated for interior nodes
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(iBlock);
    uint32_t curSize = 60;                   // i_block is 60 bytes
    uint64_t result = 0;

    for (int level = 0; level < 8; ++level) {
        const Ext4ExtentHeader* h = reinterpret_cast<const Ext4ExtentHeader*>(cur);
        if (h->eh_magic != kExtentMagic) {
            break;
        }
        uint32_t entries = h->eh_entries;
        const uint32_t capacity =
            (curSize - sizeof(Ext4ExtentHeader)) / sizeof(Ext4Extent);
        if (entries > capacity) {
            entries = capacity;
        }

        if (h->eh_depth == 0) {
            // Leaf: entries are Ext4Extent records covering ranges of blocks.
            const Ext4Extent* ex = reinterpret_cast<const Ext4Extent*>(
                cur + sizeof(Ext4ExtentHeader));
            for (uint32_t i = 0; i < entries; ++i) {
                uint32_t len = ex[i].ee_len;
                if (len > 32768) {
                    len -= 32768;   // uninitialized (preallocated) extent
                }
                if (len == 0) {
                    continue;
                }
                if (logicalBlock >= ex[i].ee_block &&
                    logicalBlock < static_cast<uint64_t>(ex[i].ee_block) + len) {
                    const uint64_t phys =
                        (static_cast<uint64_t>(ex[i].ee_start_hi) << 32) |
                        ex[i].ee_start_lo;
                    result = phys + (logicalBlock - ex[i].ee_block);
                    break;
                }
            }
            break;   // reached a leaf; done (result stays 0 for a hole)
        }

        // Interior node: pick the last index whose covered range starts <= target.
        const Ext4ExtentIdx* idx = reinterpret_cast<const Ext4ExtentIdx*>(
            cur + sizeof(Ext4ExtentHeader));
        int target = -1;
        for (uint32_t i = 0; i < entries; ++i) {
            if (idx[i].ei_block <= logicalBlock) {
                target = static_cast<int>(i);
            } else {
                break;
            }
        }
        if (target < 0) {
            break;
        }
        const uint64_t child = (static_cast<uint64_t>(idx[target].ei_leaf_hi) << 32) |
            idx[target].ei_leaf_lo;
        if (child == 0) {
            break;
        }
        if (!scratch) {
            scratch = static_cast<uint8_t*>(kmalloc(blockSize));
            if (!scratch) {
                break;
            }
        }
        if (!readBlock(child, scratch)) {
            break;
        }
        cur = scratch;
        curSize = blockSize;
    }

    if (scratch) {
        kfree(scratch);
    }
    return result;
}

uint64_t Ext4FS::resolveBlock(const Ext4Node* node, uint64_t logicalBlock) {
    if (node->flags & kInodeExtentsFlag) {
        return resolveViaExtents(node->iBlock, logicalBlock);
    }
    return resolveViaBlockMap(node->iBlock, logicalBlock);
}

// ---------------------------------------------------------------------------
// VNode construction
// ---------------------------------------------------------------------------

FileType Ext4FS::modeToType(uint16_t mode) {
    switch (mode & kModeFmtMask) {
        case kModeDir:  return FileType::Directory;
        case kModeLnk:  return FileType::Symlink;
        case kModeChr:  return FileType::CharDevice;
        case kModeBlk:  return FileType::BlockDevice;
        case kModeFifo: return FileType::Pipe;
        case kModeSock: return FileType::Socket;
        default:        return FileType::Regular;
    }
}

FileType Ext4FS::dirEntryType(uint8_t fileType) {
    switch (fileType) {
        case kFtDir:     return FileType::Directory;
        case kFtSymlink: return FileType::Symlink;
        case kFtChrdev:  return FileType::CharDevice;
        case kFtBlkdev:  return FileType::BlockDevice;
        case kFtFifo:    return FileType::Pipe;
        case kFtSocket:  return FileType::Socket;
        default:         return FileType::Regular;
    }
}

VNode* Ext4FS::makeVNode(uint32_t inodeNum, const Ext4Inode* inode) {
    Ext4Node* n = static_cast<Ext4Node*>(kmalloc(sizeof(Ext4Node)));
    if (!n) {
        return nullptr;
    }
    n->inodeNum = inodeNum;
    n->mode = inode->i_mode;
    n->flags = inode->i_flags;
    n->linksCount = inode->i_links_count;
    n->uid = inode->i_uid;
    n->gid = inode->i_gid;
    n->atime = inode->i_atime;
    n->ctime = inode->i_ctime;
    n->mtime = inode->i_mtime;
    n->blocksLo = inode->i_blocks_lo;
    n->generation = inode->i_generation;
    if ((inode->i_mode & kModeFmtMask) == kModeReg) {
        n->size = static_cast<uint64_t>(inode->i_size_lo) |
            (static_cast<uint64_t>(inode->i_size_high) << 32);
    } else {
        // Directories/symlinks/etc. use only the low 32 bits (the high field is
        // repurposed as i_dir_acl for directories).
        n->size = inode->i_size_lo;
    }
    for (int i = 0; i < 15; ++i) {
        n->iBlock[i] = inode->i_block[i];
    }

    VNode* v = new VNode(this, inodeNum, modeToType(inode->i_mode));
    if (!v) {
        kfree(n);
        return nullptr;
    }
    v->setData(n);
    v->ops = &ops;
    return v;
}

// ---------------------------------------------------------------------------
// Mount / probe
// ---------------------------------------------------------------------------

bool Ext4FS::probe(BlockDevice* device) {
    if (!device) {
        return false;
    }
    Ext4Superblock probeSb;
    if (!device->read(kSuperblockOffset, &probeSb, sizeof(probeSb))) {
        return false;
    }
    if (probeSb.s_magic != kSuperMagic) {
        return false;
    }
    if (probeSb.s_log_block_size > 6) {
        return false;
    }
    return probeSb.s_inodes_per_group != 0 && probeSb.s_blocks_per_group != 0;
}

int Ext4FS::mount(const char* path) {
    (void)path;
    if (!device) {
        return -1;
    }
    if (!readBytes(kSuperblockOffset, &sb, sizeof(sb))) {
        return -1;
    }
    if (sb.s_magic != kSuperMagic) {
        return -1;
    }
    if (sb.s_log_block_size > 6) {   // guards against corrupt block-size shift
        return -1;
    }
    blockSize = 1024u << sb.s_log_block_size;
    // Bring up the write-through block cache now that blockSize is known, so
    // group-descriptor reads and journal replay below already benefit.
    if (blockSize >= 1024 && blockSize <= 65536) {
        allocBlockCache();
    }

    if (sb.s_inodes_per_group == 0 || sb.s_blocks_per_group == 0) {
        return -1;
    }
    inodesPerGroup = sb.s_inodes_per_group;
    blocksPerGroup = sb.s_blocks_per_group;
    firstDataBlock = sb.s_first_data_block;

    // Old (rev 0) filesystems always use 128-byte inodes; s_inode_size is only
    // meaningful in the dynamic revision.
    inodeSize = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    if (inodeSize < 128 || inodeSize > blockSize) {
        return -1;
    }

    has64bit = (sb.s_feature_incompat & kIncompat64Bit) != 0;
    hasExtents = (sb.s_feature_incompat & kIncompatExtents) != 0;
    hasFileType = (sb.s_feature_incompat & kIncompatFiletype) != 0;

    // Writing is only supported when we can keep the on-disk format fully
    // consistent without recomputing metadata checksums (crc32c metadata_csum
    // or crc16 uninit_bg group-descriptor checksums). Such volumes mount
    // read-only; the write ops return an error.
    hasMetadataCsum = (sb.s_feature_ro_compat & kRoCompatMetadataCsum) != 0;
    const bool hasGdtCsum = (sb.s_feature_ro_compat & kRoCompatGdtCsum) != 0;
    // metadata_csum volumes are writable (we maintain crc32c on every metadata
    // structure). Legacy uninit_bg (crc16 group-descriptor checksums) without
    // metadata_csum is not supported for writing, so it stays read-only.
    writable = hasMetadataCsum || !hasGdtCsum;
    // Seed for all metadata_csum checksums. (The optional csum_seed incompat
    // feature, which stores an explicit seed, is outside kIncompatSupported and
    // is rejected below, so deriving from the UUID here is always correct.)
    csumSeed = crc32c(0xFFFFFFFFu, sb.s_uuid, 16);

    // Refuse to read a volume that requires an incompat feature we don't model
    // (e.g. meta_bg changes descriptor placement; inline_data/encrypt change
    // data layout). Reading such a volume could silently return garbage.
    if (sb.s_feature_incompat & ~kIncompatSupported) {
        return -1;
    }

    descSize = has64bit ? (sb.s_desc_size ? sb.s_desc_size : 64) : 32;
    if (descSize < 32) {
        return -1;
    }

    const uint64_t totalBlocks = static_cast<uint64_t>(sb.s_blocks_count_lo) |
        (has64bit ? (static_cast<uint64_t>(sb.s_blocks_count_hi) << 32) : 0);
    if (totalBlocks <= firstDataBlock) {
        return -1;
    }
    groupCount = static_cast<uint32_t>(
        (totalBlocks - firstDataBlock + blocksPerGroup - 1) / blocksPerGroup);
    if (groupCount == 0) {
        return -1;
    }

    // Recover a dirty JBD2 journal before trusting any on-disk metadata. Only
    // attempted when the volume is writable (we must write the recovered blocks
    // and reset the journal). Recovery may rewrite the superblock, so re-read it.
    if ((sb.s_feature_compat & kFeatureCompatHasJournal) && writable) {
        if (journalReplay() == 0) {
            readBytes(kSuperblockOffset, &sb, sizeof(sb));
        }
        journalLoadGeometry();   // enable write-side journaling for a simple journal
    }

    Ext4Inode rootInode;
    if (!readInode(kRootInode, &rootInode)) {
        return -1;
    }
    rootNode = makeVNode(kRootInode, &rootInode);
    if (!rootNode) {
        return -1;
    }

    if (writable && !runBuf && blockSize <= kRunBytes) {
        runBuf = static_cast<uint8_t*>(kmalloc(kRunBytes));   // null: writes go one block at a time
    }
    mounted = true;
    if (g_mountedCount < kMaxMounted) {
        g_mounted[g_mountedCount++] = this;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// VNodeOps: read side
// ---------------------------------------------------------------------------

namespace {
void refreshVNode(VNode* v);   // reloads the node's inode (Ext4FS::refreshNode)
}

int Ext4FS::nodeOpen(VNode* node, int flags) {
    (void)node;
    (void)flags;
    return 0;
}

int Ext4FS::nodeClose(VNode* node) {
    (void)node;
    return 0;
}

int64_t Ext4FS::nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset) {
    refreshVNode(node);
    if (!node || !buffer) {
        return -1;
    }
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    if (!n) {
        return -1;
    }
    if ((n->mode & kModeFmtMask) == kModeDir) {
        return -1;   // use readdir for directories
    }
    if (offset >= n->size) {
        return 0;
    }
    if (offset + size > n->size) {
        size = n->size - offset;
    }

    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!fs || fs->blockSize == 0) {
        return -1;
    }

    uint8_t* dest = static_cast<uint8_t*>(buffer);
    const uint64_t bs = fs->blockSize;
    uint64_t done = 0;
    uint64_t logical = offset / bs;
    uint32_t inBlock = static_cast<uint32_t>(offset % bs);
    constexpr uint64_t kMaxRunBlocks = 256;

    while (done < size) {
        const uint64_t phys = fs->resolveBlock(n, logical);
        uint64_t toRead = bs - inBlock;
        if (toRead > size - done) {
            toRead = size - done;
        }
        if (phys != 0 && toRead == bs && size - done >= 2 * bs) {
            // Whole blocks at consecutive addresses are read in one go,
            // straight from the device: file data is never held in the
            // transaction, and the block cache is write-through, so the disk
            // is current. Keeping bulk data out of the cache also leaves it to
            // the metadata it is for.
            uint64_t run = 1;
            while (run < kMaxRunBlocks && size - done >= (run + 1) * bs &&
                   fs->resolveBlock(n, logical + run) == phys + run) {
                ++run;
            }
            if (run > 1) {
                if (!fs->device->read(phys * bs, dest + done, run * bs)) {
                    return done > 0 ? static_cast<int64_t>(done) : -1;
                }
                done += run * bs;
                logical += run;
                continue;
            }
        }
        if (phys == 0) {
            // Sparse hole: reads as zeroes.
            memset(dest + done, 0, toRead);
        } else if (!fs->readBytes(phys * fs->blockSize + inBlock, dest + done, toRead)) {
            return done > 0 ? static_cast<int64_t>(done) : -1;
        }
        done += toRead;
        inBlock = 0;
        ++logical;
    }
    return static_cast<int64_t>(done);
}

int Ext4FS::nodeStat(VNode* node, FileStats* stats) {
    refreshVNode(node);
    if (!node || !stats) {
        return -1;
    }
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    if (!n) {
        return -1;
    }
    stats->size = n->size;
    stats->type = modeToType(n->mode);
    stats->mode = n->mode & 07777;   // permission + setuid/setgid/sticky bits
    stats->inode = n->inodeNum;
    stats->links = n->linksCount;
    stats->atime = n->atime;
    stats->mtime = n->mtime;
    stats->ctime = n->ctime;
    stats->uid = n->uid;
    stats->gid = n->gid;
    stats->rdev = 0;
    stats->dev = reinterpret_cast<uint64_t>(node->getFS());
    return 0;
}

int Ext4FS::readDirInto(VNode* dir, DirEntry* entries, uint64_t count, uint64_t* read) {
    Ext4Node* n = static_cast<Ext4Node*>(dir->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(dir->getFS());
    *read = 0;
    if (fs->blockSize == 0) {
        return -1;
    }

    uint8_t* buf = static_cast<uint8_t*>(kmalloc(fs->blockSize));
    if (!buf) {
        return -1;
    }

    uint64_t filled = 0;
    const uint64_t totalBlocks = (n->size + fs->blockSize - 1) / fs->blockSize;
    for (uint64_t lb = 0; lb < totalBlocks && filled < count; ++lb) {
        const uint64_t phys = fs->resolveBlock(n, lb);
        if (phys == 0) {
            continue;
        }
        if (!fs->readBlock(phys, buf)) {
            break;
        }
        uint32_t off = 0;
        while (off + sizeof(Ext4DirEntry) <= fs->blockSize && filled < count) {
            const Ext4DirEntry* de = reinterpret_cast<const Ext4DirEntry*>(buf + off);
            const uint16_t rec = de->rec_len;
            if (rec < sizeof(Ext4DirEntry)) {
                break;   // corrupt or end-of-block padding
            }
            uint32_t nameLen = de->name_len;
            if (de->inode != 0 && nameLen != 0 &&
                off + sizeof(Ext4DirEntry) + nameLen <= fs->blockSize) {
                if (nameLen > 255) {
                    nameLen = 255;
                }
                const char* nm =
                    reinterpret_cast<const char*>(buf + off + sizeof(Ext4DirEntry));
                for (uint32_t k = 0; k < nameLen; ++k) {
                    entries[filled].name[k] = nm[k];
                }
                entries[filled].name[nameLen] = '\0';
                entries[filled].inode = de->inode;
                entries[filled].type =
                    fs->hasFileType ? dirEntryType(de->file_type) : FileType::Regular;
                ++filled;
            }
            off += rec;
        }
    }

    kfree(buf);
    *read = filled;
    return 0;
}

int Ext4FS::nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read) {
    refreshVNode(node);
    if (!node || !entries || !read) {
        return -1;
    }
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    if (!n || (n->mode & kModeFmtMask) != kModeDir) {
        return -1;
    }
    return static_cast<Ext4FS*>(node->getFS())->readDirInto(node, entries, count, read);
}

VNode* Ext4FS::lookupInDir(VNode* dir, const char* name) {
    Ext4Node* n = static_cast<Ext4Node*>(dir->getData());
    const uint64_t nameLen = strlen(name);
    if (nameLen == 0 || nameLen > 255 || blockSize == 0) {
        return nullptr;
    }

    uint8_t* buf = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!buf) {
        return nullptr;
    }

    uint32_t foundInode = 0;
    const uint64_t totalBlocks = (n->size + blockSize - 1) / blockSize;
    for (uint64_t lb = 0; lb < totalBlocks && foundInode == 0; ++lb) {
        const uint64_t phys = resolveBlock(n, lb);
        if (phys == 0) {
            continue;
        }
        if (!readBlock(phys, buf)) {
            break;
        }
        uint32_t off = 0;
        while (off + sizeof(Ext4DirEntry) <= blockSize) {
            const Ext4DirEntry* de = reinterpret_cast<const Ext4DirEntry*>(buf + off);
            const uint16_t rec = de->rec_len;
            if (rec < sizeof(Ext4DirEntry)) {
                break;
            }
            if (de->inode != 0 && de->name_len == nameLen &&
                off + sizeof(Ext4DirEntry) + nameLen <= blockSize) {
                const char* nm =
                    reinterpret_cast<const char*>(buf + off + sizeof(Ext4DirEntry));
                if (memcmp(nm, name, nameLen) == 0) {
                    foundInode = de->inode;
                    break;
                }
            }
            off += rec;
        }
    }

    kfree(buf);
    if (foundInode == 0) {
        return nullptr;
    }

    Ext4Inode inode;
    if (!readInode(foundInode, &inode)) {
        return nullptr;
    }
    return makeVNode(foundInode, &inode);
}

VNode* Ext4FS::nodeLookup(VNode* node, const char* name) {
    refreshVNode(node);
    if (!node || !name) {
        return nullptr;
    }
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    if (!n || (n->mode & kModeFmtMask) != kModeDir) {
        return nullptr;
    }
    return static_cast<Ext4FS*>(node->getFS())->lookupInDir(node, name);
}

int64_t Ext4FS::nodeReadlink(VNode* node, char* buffer, uint64_t size) {
    refreshVNode(node);
    if (!node || !buffer) {
        return -1;
    }
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    if (!n || (n->mode & kModeFmtMask) != kModeLnk) {
        return -1;
    }
    const uint64_t len = n->size;
    if (len == 0) {
        return 0;
    }

    // Fast symlink: target string is stored inline in the 60-byte i_block area
    // when it fits (this is what mke2fs does for short targets).
    if (len < 60) {
        const char* target = reinterpret_cast<const char*>(n->iBlock);
        const uint64_t toCopy = len < size ? len : size;
        for (uint64_t i = 0; i < toCopy; ++i) {
            buffer[i] = target[i];
        }
        return static_cast<int64_t>(toCopy);
    }

    // Slow symlink: target lives in the first data block.
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    uint8_t* buf = static_cast<uint8_t*>(kmalloc(fs->blockSize));
    if (!buf) {
        return -1;
    }
    int64_t result = -1;
    const uint64_t phys = fs->resolveBlock(n, 0);
    if (phys != 0 && fs->readBlock(phys, buf)) {
        uint64_t toCopy = len < size ? len : size;
        if (toCopy > fs->blockSize) {
            toCopy = fs->blockSize;
        }
        for (uint64_t i = 0; i < toCopy; ++i) {
            buffer[i] = reinterpret_cast<const char*>(buf)[i];
        }
        result = static_cast<int64_t>(toCopy);
    }
    kfree(buf);
    return result;
}

int Ext4FS::nodeStatfs(VNode* node, FsStats* stats) {
    if (!node || !stats) {
        return -1;
    }
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!fs) {
        return -1;
    }
    const uint64_t totalBlocks = static_cast<uint64_t>(fs->sb.s_blocks_count_lo) |
        (fs->has64bit ? (static_cast<uint64_t>(fs->sb.s_blocks_count_hi) << 32) : 0);
    const uint64_t freeBlocks = static_cast<uint64_t>(fs->sb.s_free_blocks_count_lo) |
        (fs->has64bit ? (static_cast<uint64_t>(fs->sb.s_free_blocks_count_hi) << 32) : 0);

    stats->blockSize = fs->blockSize;
    stats->totalBlocks = totalBlocks;
    stats->freeBlocks = freeBlocks;
    stats->totalInodes = fs->sb.s_inodes_count;
    stats->freeInodes = fs->sb.s_free_inodes_count;
    stats->nameMax = 255;
    stats->fsType = kSuperMagic;
    return 0;
}

// ===========================================================================
// Phase 3: write path
//
// New files/directories are created with the classic ext2 block map (no extent
// tree mutation). All e2fsck-checked invariants are maintained: block/inode
// bitmaps, i_blocks, superblock + group-descriptor free counts, directory link
// counts, and used_dirs_count. Volumes using metadata_csum / uninit_bg are
// mounted read-only (see mount()), so no on-disk checksum upkeep is required.
// ===========================================================================

namespace {
// ext4 stores 32-bit second timestamps. Before the RTC-derived clock is up (or
// in the host test harness) fall back to a fixed plausible value that keeps
// created inodes valid for fsck.
constexpr uint32_t kDefaultTime = 1700000000u;   // 2023-11-14T22:13:20Z

// Current wall-clock time as an ext4 32-bit second timestamp. Build tools such
// as `make` compare mtimes to decide what to rebuild, so writes MUST advance
// the clock rather than stamp a constant.
uint32_t nowTime() {
    uint64_t t = time_get_unix();
    return t ? static_cast<uint32_t>(t) : kDefaultTime;
}
}

bool Ext4FS::devWriteBytes(uint64_t byteOffset, const void* buffer, uint64_t size) {
    if (!device) return false;
    if (!device->write(byteOffset, buffer, size)) return false;
    // Write-through: refresh any cached blocks covering this range so the cache
    // never serves stale data.
    if (cacheSlots && blockSize) {
        const uint8_t* src = static_cast<const uint8_t*>(buffer);
        uint64_t done = 0;
        while (done < size) {
            const uint64_t pos = byteOffset + done;
            const uint64_t block = pos / blockSize;
            const uint32_t boff = static_cast<uint32_t>(pos % blockSize);
            uint32_t chunk = blockSize - boff;
            if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);
            const uint32_t idx = static_cast<uint32_t>(block % cacheSlots);
            if (cacheTag[idx] == block) {
                memcpy(cacheData + static_cast<uint64_t>(idx) * blockSize + boff, src + done, chunk);
            }
            done += chunk;
        }
    }
    return true;
}

bool Ext4FS::writeBytes(uint64_t byteOffset, const void* buffer, uint64_t size) {
    if (!jActive) {
        return devWriteBytes(byteOffset, buffer, size);
    }
    // Inside a transaction, metadata writes buffer into the transaction (at
    // block granularity) instead of hitting the device.
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    uint64_t done = 0;
    while (done < size) {
        const uint64_t pos = byteOffset + done;
        const uint64_t block = pos / blockSize;
        const uint32_t boff = static_cast<uint32_t>(pos % blockSize);
        uint32_t chunk = blockSize - boff;
        if (chunk > size - done) chunk = static_cast<uint32_t>(size - done);
        uint8_t* b = jTxnBlock(block, !(boff == 0 && chunk == blockSize));
        if (!b) return false;   // out of memory
        memcpy(b + boff, src + done, chunk);
        done += chunk;
    }
    return true;
}

bool Ext4FS::writeBlock(uint64_t block, const void* buffer) {
    if (block == 0 || blockSize == 0) {
        return false;
    }
    return writeBytes(block * static_cast<uint64_t>(blockSize), buffer, blockSize);
}

bool Ext4FS::flushSuperblock() {
    superblockSetCsum();
    return writeBytes(kSuperblockOffset, &sb, sizeof(sb));
}

bool Ext4FS::flushGroupDesc(uint32_t group, const Ext4GroupDesc* gd) {
    if (group >= groupCount || !gd) {
        return false;
    }
    Ext4GroupDesc local = *gd;              // groupDescSetCsum mutates bg_checksum
    groupDescSetCsum(group, &local);
    const uint64_t gdtStartBlock = static_cast<uint64_t>(firstDataBlock) + 1;
    const uint64_t off = gdtStartBlock * blockSize + static_cast<uint64_t>(group) * descSize;
    const uint32_t toWrite = descSize <= sizeof(local) ? descSize : static_cast<uint32_t>(sizeof(local));
    return writeBytes(off, &local, toWrite);
}

bool Ext4FS::writeInodeRaw(uint32_t inodeNum, const Ext4Inode* inode, bool zeroExtra) {
    if (inodeNum == 0 || !inode || inodesPerGroup == 0) {
        return false;
    }
    const uint32_t group = (inodeNum - 1) / inodesPerGroup;
    const uint32_t index = (inodeNum - 1) % inodesPerGroup;
    if (group >= groupCount) {
        return false;
    }
    Ext4GroupDesc gd;
    if (!readGroupDesc(group, &gd)) {
        return false;
    }
    const uint64_t inodeTable = static_cast<uint64_t>(gd.bg_inode_table_lo) |
        (has64bit ? (static_cast<uint64_t>(gd.bg_inode_table_hi) << 32) : 0);
    if (inodeTable == 0) {
        return false;
    }
    const uint64_t off = inodeTable * blockSize + static_cast<uint64_t>(index) * inodeSize;

    if (!hasMetadataCsum) {
        if (!writeBytes(off, inode, sizeof(Ext4Inode))) {   // base 128 bytes
            return false;
        }
        if (zeroExtra && inodeSize > sizeof(Ext4Inode)) {
            const uint32_t extra = inodeSize - static_cast<uint32_t>(sizeof(Ext4Inode));
            uint8_t* z = static_cast<uint8_t*>(kmalloc(extra));
            if (!z) return false;
            memset(z, 0, extra);
            const bool ok = writeBytes(off + sizeof(Ext4Inode), z, extra);
            kfree(z);
            if (!ok) return false;
        }
        return true;
    }

    // metadata_csum: assemble the full inode, set i_extra_isize for new inodes,
    // compute the crc32c inode checksum (i_checksum_lo/hi), and write inodeSize
    // bytes. Existing inodes are read first so extra fields are preserved.
    uint8_t* buf = static_cast<uint8_t*>(kmalloc(inodeSize));
    if (!buf) return false;
    if (zeroExtra) {
        memset(buf, 0, inodeSize);
    } else if (!readBytes(off, buf, inodeSize)) {
        kfree(buf);
        return false;
    }
    memcpy(buf, inode, sizeof(Ext4Inode));       // overlay the 128-byte base
    if (zeroExtra && inodeSize >= 0x84) {
        buf[0x80] = 32; buf[0x81] = 0;           // i_extra_isize = 32
    }
    inodeSetCsum(inodeNum, buf);
    const bool ok = writeBytes(off, buf, inodeSize);
    kfree(buf);
    return ok;
}

void Ext4FS::refreshNode(Ext4Node* n) {
    Ext4Inode ino;
    if (!n || !readInode(n->inodeNum, &ino)) {
        return;
    }
    // A freed inode (unlinked while this copy was open) keeps its last state.
    if (ino.i_mode == 0 || ino.i_links_count == 0) {
        return;
    }
    n->mode = ino.i_mode;
    n->flags = ino.i_flags;
    n->linksCount = ino.i_links_count;
    n->uid = ino.i_uid;
    n->gid = ino.i_gid;
    n->atime = ino.i_atime;
    n->ctime = ino.i_ctime;
    n->mtime = ino.i_mtime;
    n->blocksLo = ino.i_blocks_lo;
    n->generation = ino.i_generation;
    n->size = (ino.i_mode & kModeFmtMask) == kModeReg
        ? (static_cast<uint64_t>(ino.i_size_lo) | (static_cast<uint64_t>(ino.i_size_high) << 32))
        : ino.i_size_lo;
    for (int i = 0; i < 15; ++i) {
        n->iBlock[i] = ino.i_block[i];
    }
}

namespace {
void refreshVNode(VNode* v) {
    if (!v) return;
    Ext4FS* fs = static_cast<Ext4FS*>(v->getFS());
    Ext4Node* n = static_cast<Ext4Node*>(v->getData());
    if (fs && n) fs->refreshNode(n);
}
}

bool Ext4FS::syncNodeInode(const Ext4Node* n) {
    // Read-modify-write so untracked on-disk fields (generation, osd, etc.) are
    // preserved.
    Ext4Inode ino;
    if (!readInode(n->inodeNum, &ino)) {
        return false;
    }
    ino.i_mode = n->mode;
    ino.i_uid = static_cast<uint16_t>(n->uid);
    ino.i_gid = static_cast<uint16_t>(n->gid);
    ino.i_links_count = n->linksCount;
    ino.i_size_lo = static_cast<uint32_t>(n->size & 0xFFFFFFFFu);
    ino.i_size_high = ((n->mode & kModeFmtMask) == kModeReg)
        ? static_cast<uint32_t>(n->size >> 32) : 0;
    ino.i_blocks_lo = n->blocksLo;
    ino.i_flags = n->flags;
    ino.i_atime = n->atime;
    ino.i_ctime = n->ctime;
    ino.i_mtime = n->mtime;
    for (int i = 0; i < 15; ++i) {
        ino.i_block[i] = n->iBlock[i];
    }
    return writeInodeRaw(n->inodeNum, &ino, false);
}

uint64_t Ext4FS::groupBlockCount(uint32_t group) const {
    const uint64_t totalBlocks = static_cast<uint64_t>(sb.s_blocks_count_lo) |
        (has64bit ? (static_cast<uint64_t>(sb.s_blocks_count_hi) << 32) : 0);
    const uint64_t groupFirst = static_cast<uint64_t>(firstDataBlock) +
        static_cast<uint64_t>(group) * blocksPerGroup;
    if (totalBlocks <= groupFirst) {
        return 0;
    }
    const uint64_t remaining = totalBlocks - groupFirst;
    return remaining < blocksPerGroup ? remaining : blocksPerGroup;
}
uint64_t Ext4FS::allocBlock(bool zero) {
    if (!writable || groupCount == 0) return 0;
    uint8_t* bmp = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!bmp) return 0;
    // With a running transaction, a block freed in it is still in use in the
    // last committed state (a crash brings that back), so it must not be handed
    // out for file data, which is written in place at once. Such blocks are
    // free in the transaction's bitmap but set in the committed one.
    uint8_t* committed = jActive ? static_cast<uint8_t*>(kmalloc(blockSize)) : nullptr;
    if (jActive && !committed) { kfree(bmp); return 0; }

    uint64_t result = 0;
    const uint32_t startGroup = allocGoalGroup < groupCount ? allocGoalGroup : 0;
    for (uint32_t k = 0; k < groupCount && result == 0; ++k) {
        const uint32_t g = (startGroup + k) % groupCount;
        Ext4GroupDesc gd;
        if (!readGroupDesc(g, &gd)) break;
        uint64_t freeCount = static_cast<uint64_t>(gd.bg_free_blocks_count_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_free_blocks_count_hi) << 16) : 0);
        if (freeCount == 0) continue;
        const uint64_t bitmapBlock = static_cast<uint64_t>(gd.bg_block_bitmap_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_block_bitmap_hi) << 32) : 0);
        if (bitmapBlock == 0 || !readBlock(bitmapBlock, bmp)) continue;
        const bool checkCommitted = committed && jTxnFind(bitmapBlock) &&
            devReadBytes(bitmapBlock * static_cast<uint64_t>(blockSize), committed, blockSize);

        const uint64_t count = groupBlockCount(g);
        uint64_t first = (k == 0) ? allocGoalBit : 0;
        if (first >= count) first = 0;
        uint64_t i = count;   // the free bit found, count if none
        for (uint64_t n = 0; n < count; ++n) {
            uint64_t b = first + n;
            if (b >= count) b -= count;
            uint8_t used = bmp[b >> 3];
            if (checkCommitted) used |= committed[b >> 3];
            if (used == 0xFF && (b & 7) == 0 && b + 8 <= count && n + 8 <= count) {
                n += 7;   // a full byte
                continue;
            }
            if (!(used & (1u << (b & 7)))) { i = b; break; }
        }
        if (i < count) {
            bmp[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
            if (!writeBlock(bitmapBlock, bmp)) {
                kfree(bmp);
                if (committed) kfree(committed);
                return 0;
            }
            if (hasMetadataCsum) {
                const uint32_t c = crc32c(csumSeed, bmp, blocksPerGroup / 8);
                gd.bg_block_bitmap_csum_lo = static_cast<uint16_t>(c & 0xFFFF);
                if (descSize >= 0x3A) gd.bg_block_bitmap_csum_hi = static_cast<uint16_t>((c >> 16) & 0xFFFF);
            }
            --freeCount;
            gd.bg_free_blocks_count_lo = static_cast<uint16_t>(freeCount & 0xFFFF);
            if (has64bit) gd.bg_free_blocks_count_hi = static_cast<uint16_t>((freeCount >> 16) & 0xFFFF);
            flushGroupDesc(g, &gd);
            uint64_t sbFree = static_cast<uint64_t>(sb.s_free_blocks_count_lo) |
                (has64bit ? (static_cast<uint64_t>(sb.s_free_blocks_count_hi) << 32) : 0);
            if (sbFree > 0) --sbFree;
            sb.s_free_blocks_count_lo = static_cast<uint32_t>(sbFree & 0xFFFFFFFFu);
            if (has64bit) sb.s_free_blocks_count_hi = static_cast<uint32_t>(sbFree >> 32);
            flushSuperblock();
            result = static_cast<uint64_t>(firstDataBlock) +
                static_cast<uint64_t>(g) * blocksPerGroup + i;
            allocGoalGroup = g;
            allocGoalBit = i + 1;
        }
    }
    kfree(bmp);
    if (committed) kfree(committed);

    if (result && jActive) {
        // Allocated and freed within this transaction, the block may still be
        // buffered as the metadata it was (an indirect or directory block);
        // checkpointing that copy would overwrite what goes there now.
        jTxnDrop(result);
    }
    if (result && zero) {   // an indirect block: zeros, as metadata
        uint8_t* z = static_cast<uint8_t*>(kmalloc(blockSize));
        bool ok = z != nullptr;
        if (ok) { memset(z, 0, blockSize); ok = writeBlock(result, z); kfree(z); }
        if (!ok) { freeBlockNum(result); return 0; }
    }
    return result;
}

uint32_t Ext4FS::allocInode(bool isDir) {
    if (!writable) return 0;
    uint8_t* bmp = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!bmp) return 0;

    uint32_t result = 0;
    for (uint32_t g = 0; g < groupCount && result == 0; ++g) {
        Ext4GroupDesc gd;
        if (!readGroupDesc(g, &gd)) break;
        uint64_t freeCount = static_cast<uint64_t>(gd.bg_free_inodes_count_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_free_inodes_count_hi) << 16) : 0);
        if (freeCount == 0) continue;
        const uint64_t bitmapBlock = static_cast<uint64_t>(gd.bg_inode_bitmap_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_inode_bitmap_hi) << 32) : 0);
        if (bitmapBlock == 0 || !readBlock(bitmapBlock, bmp)) continue;

        for (uint32_t i = 0; i < inodesPerGroup; ++i) {
            const uint32_t inodeNum = g * inodesPerGroup + i + 1;
            if (inodeNum < kFirstInode) continue;             // reserved inodes
            if (bmp[i >> 3] & (1u << (i & 7))) continue;      // in use
            bmp[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
            if (!writeBlock(bitmapBlock, bmp)) { kfree(bmp); return 0; }
            if (hasMetadataCsum) {
                const uint32_t c = crc32c(csumSeed, bmp, inodesPerGroup / 8);
                gd.bg_inode_bitmap_csum_lo = static_cast<uint16_t>(c & 0xFFFF);
                if (descSize >= 0x3C) gd.bg_inode_bitmap_csum_hi = static_cast<uint16_t>((c >> 16) & 0xFFFF);
                // Maintain bg_itable_unused (trailing never-used inodes) and clear
                // the INODE_UNINIT flag; both are validated by e2fsck.
                uint64_t unused = static_cast<uint64_t>(gd.bg_itable_unused_lo) |
                    (has64bit ? (static_cast<uint64_t>(gd.bg_itable_unused_hi) << 16) : 0);
                const uint64_t newUnused = inodesPerGroup - (static_cast<uint64_t>(i) + 1);
                if (newUnused < unused) {
                    gd.bg_itable_unused_lo = static_cast<uint16_t>(newUnused & 0xFFFF);
                    if (has64bit) gd.bg_itable_unused_hi = static_cast<uint16_t>((newUnused >> 16) & 0xFFFF);
                }
                gd.bg_flags &= static_cast<uint16_t>(~0x0001u);   // clear EXT4_BG_INODE_UNINIT
            }
            --freeCount;
            gd.bg_free_inodes_count_lo = static_cast<uint16_t>(freeCount & 0xFFFF);
            if (has64bit) gd.bg_free_inodes_count_hi = static_cast<uint16_t>((freeCount >> 16) & 0xFFFF);
            if (isDir) {
                uint64_t dirs = static_cast<uint64_t>(gd.bg_used_dirs_count_lo) |
                    (has64bit ? (static_cast<uint64_t>(gd.bg_used_dirs_count_hi) << 16) : 0);
                ++dirs;
                gd.bg_used_dirs_count_lo = static_cast<uint16_t>(dirs & 0xFFFF);
                if (has64bit) gd.bg_used_dirs_count_hi = static_cast<uint16_t>((dirs >> 16) & 0xFFFF);
            }
            flushGroupDesc(g, &gd);
            if (sb.s_free_inodes_count > 0) --sb.s_free_inodes_count;
            flushSuperblock();
            result = inodeNum;
            break;
        }
    }
    kfree(bmp);
    return result;
}

void Ext4FS::freeBlockNum(uint64_t block) {
    if (block == 0 || block < firstDataBlock || blocksPerGroup == 0) return;
    const uint32_t g = static_cast<uint32_t>((block - firstDataBlock) / blocksPerGroup);
    const uint64_t idx = (block - firstDataBlock) % blocksPerGroup;
    if (g >= groupCount) return;
    Ext4GroupDesc gd;
    if (!readGroupDesc(g, &gd)) return;
    const uint64_t bitmapBlock = static_cast<uint64_t>(gd.bg_block_bitmap_lo) |
        (has64bit ? (static_cast<uint64_t>(gd.bg_block_bitmap_hi) << 32) : 0);
    uint8_t* bmp = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!bmp) return;
    if (readBlock(bitmapBlock, bmp) && (bmp[idx >> 3] & (1u << (idx & 7)))) {
        bmp[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7)));
        writeBlock(bitmapBlock, bmp);
        if (hasMetadataCsum) {
            const uint32_t c = crc32c(csumSeed, bmp, blocksPerGroup / 8);
            gd.bg_block_bitmap_csum_lo = static_cast<uint16_t>(c & 0xFFFF);
            if (descSize >= 0x3A) gd.bg_block_bitmap_csum_hi = static_cast<uint16_t>((c >> 16) & 0xFFFF);
        }
        uint64_t freeCount = static_cast<uint64_t>(gd.bg_free_blocks_count_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_free_blocks_count_hi) << 16) : 0);
        ++freeCount;
        gd.bg_free_blocks_count_lo = static_cast<uint16_t>(freeCount & 0xFFFF);
        if (has64bit) gd.bg_free_blocks_count_hi = static_cast<uint16_t>((freeCount >> 16) & 0xFFFF);
        flushGroupDesc(g, &gd);
        uint64_t sbFree = static_cast<uint64_t>(sb.s_free_blocks_count_lo) |
            (has64bit ? (static_cast<uint64_t>(sb.s_free_blocks_count_hi) << 32) : 0);
        ++sbFree;
        sb.s_free_blocks_count_lo = static_cast<uint32_t>(sbFree & 0xFFFFFFFFu);
        if (has64bit) sb.s_free_blocks_count_hi = static_cast<uint32_t>(sbFree >> 32);
        flushSuperblock();
    }
    kfree(bmp);
}

void Ext4FS::freeInodeNum(uint32_t inodeNum, bool isDir) {
    if (inodeNum == 0 || inodesPerGroup == 0) return;
    const uint32_t g = (inodeNum - 1) / inodesPerGroup;
    const uint32_t idx = (inodeNum - 1) % inodesPerGroup;
    if (g >= groupCount) return;
    Ext4GroupDesc gd;
    if (!readGroupDesc(g, &gd)) return;
    const uint64_t bitmapBlock = static_cast<uint64_t>(gd.bg_inode_bitmap_lo) |
        (has64bit ? (static_cast<uint64_t>(gd.bg_inode_bitmap_hi) << 32) : 0);
    uint8_t* bmp = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!bmp) return;
    if (readBlock(bitmapBlock, bmp) && (bmp[idx >> 3] & (1u << (idx & 7)))) {
        bmp[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7)));
        writeBlock(bitmapBlock, bmp);
        if (hasMetadataCsum) {
            const uint32_t c = crc32c(csumSeed, bmp, inodesPerGroup / 8);
            gd.bg_inode_bitmap_csum_lo = static_cast<uint16_t>(c & 0xFFFF);
            if (descSize >= 0x3C) gd.bg_inode_bitmap_csum_hi = static_cast<uint16_t>((c >> 16) & 0xFFFF);
        }
        uint64_t freeCount = static_cast<uint64_t>(gd.bg_free_inodes_count_lo) |
            (has64bit ? (static_cast<uint64_t>(gd.bg_free_inodes_count_hi) << 16) : 0);
        ++freeCount;
        gd.bg_free_inodes_count_lo = static_cast<uint16_t>(freeCount & 0xFFFF);
        if (has64bit) gd.bg_free_inodes_count_hi = static_cast<uint16_t>((freeCount >> 16) & 0xFFFF);
        if (isDir) {
            uint64_t dirs = static_cast<uint64_t>(gd.bg_used_dirs_count_lo) |
                (has64bit ? (static_cast<uint64_t>(gd.bg_used_dirs_count_hi) << 16) : 0);
            if (dirs > 0) --dirs;
            gd.bg_used_dirs_count_lo = static_cast<uint16_t>(dirs & 0xFFFF);
            if (has64bit) gd.bg_used_dirs_count_hi = static_cast<uint16_t>((dirs >> 16) & 0xFFFF);
        }
        flushGroupDesc(g, &gd);
        ++sb.s_free_inodes_count;
        flushSuperblock();
    }
    kfree(bmp);
}

uint32_t Ext4FS::readPointer(uint64_t block, uint32_t index) {
    if (block == 0 || index >= blockSize / 4) return 0;
    uint32_t* buf = static_cast<uint32_t*>(kmalloc(blockSize));
    if (!buf) return 0;
    uint32_t value = 0;
    if (readBlock(block, buf)) value = buf[index];
    kfree(buf);
    return value;
}

bool Ext4FS::writePointer(uint64_t block, uint32_t index, uint32_t value) {
    if (block == 0 || index >= blockSize / 4) return false;
    uint32_t* buf = static_cast<uint32_t*>(kmalloc(blockSize));
    if (!buf) return false;
    bool ok = readBlock(block, buf);
    if (ok) { buf[index] = value; ok = writeBlock(block, buf); }
    kfree(buf);
    return ok;
}

bool Ext4FS::ensureIndirect(uint32_t* slot, Ext4Node* node) {
    if (*slot != 0) return true;
    const uint64_t nb = allocBlock(true);   // zeroed: no pointers yet
    if (nb == 0) return false;
    *slot = static_cast<uint32_t>(nb);
    node->blocksLo += blockSize / 512;
    return true;
}

bool Ext4FS::bmapSet(Ext4Node* node, uint64_t lb, uint64_t phys) {
    const uint64_t ppb = blockSize / 4;
    if (lb < 12) {
        node->iBlock[lb] = static_cast<uint32_t>(phys);
        return true;
    }
    lb -= 12;
    if (lb < ppb) {   // single indirect
        if (!ensureIndirect(&node->iBlock[12], node)) return false;
        return writePointer(node->iBlock[12], static_cast<uint32_t>(lb), static_cast<uint32_t>(phys));
    }
    lb -= ppb;
    if (lb < ppb * ppb) {   // double indirect
        if (!ensureIndirect(&node->iBlock[13], node)) return false;
        const uint32_t l1index = static_cast<uint32_t>(lb / ppb);
        const uint32_t l2index = static_cast<uint32_t>(lb % ppb);
        uint32_t l1 = readPointer(node->iBlock[13], l1index);
        if (l1 == 0) {
            const uint64_t nb = allocBlock(true);
            if (nb == 0) return false;
            l1 = static_cast<uint32_t>(nb);
            node->blocksLo += blockSize / 512;
            if (!writePointer(node->iBlock[13], l1index, l1)) return false;
        }
        return writePointer(l1, l2index, static_cast<uint32_t>(phys));
    }
    return false;   // triple indirect not supported for writes (files > ~64 MiB)
}

void Ext4FS::freeIndirect(uint64_t block, int levels) {
    if (block == 0) return;
    uint32_t* buf = static_cast<uint32_t*>(kmalloc(blockSize));
    if (buf && readBlock(block, buf)) {
        const uint64_t ppb = blockSize / 4;
        for (uint64_t i = 0; i < ppb; ++i) {
            const uint32_t p = buf[i];
            if (!p) continue;
            if (levels > 1) freeIndirect(p, levels - 1);
            else freeBlockNum(p);
        }
    }
    if (buf) kfree(buf);
    freeBlockNum(block);
}

void Ext4FS::freeInodeData(Ext4Node* n) {
    // Fast symlink: target is inline in i_block, no data blocks.
    if ((n->mode & kModeFmtMask) == kModeLnk && n->blocksLo == 0) return;

    if (n->flags & kInodeExtentsFlag) {
        // Depth-0 extent files (our created files are block-mapped, but a test
        // may unlink an mke2fs file): free the mapped data blocks.
        const uint64_t numBlocks = blockSize ? (n->size + blockSize - 1) / blockSize : 0;
        for (uint64_t lb = 0; lb < numBlocks; ++lb) {
            const uint64_t p = resolveViaExtents(n->iBlock, lb);
            if (p) freeBlockNum(p);
        }
        return;
    }
    for (int i = 0; i < 12; ++i) {
        if (n->iBlock[i]) freeBlockNum(n->iBlock[i]);
    }
    if (n->iBlock[12]) freeIndirect(n->iBlock[12], 1);
    if (n->iBlock[13]) freeIndirect(n->iBlock[13], 2);
    if (n->iBlock[14]) freeIndirect(n->iBlock[14], 3);
}

void Ext4FS::bmapTruncate(Ext4Node* n, uint64_t keep) {
    const uint64_t ppb = blockSize / 4;
    const uint32_t bpf = blockSize / 512;   // 512-sectors per fs block

    for (uint32_t i = 0; i < 12; ++i) {
        if (i >= keep && n->iBlock[i]) {
            freeBlockNum(n->iBlock[i]);
            n->iBlock[i] = 0;
            if (n->blocksLo >= bpf) n->blocksLo -= bpf;
        }
    }

    if (n->iBlock[12]) {   // single indirect covers logical 12 .. 12+ppb-1
        const uint64_t base = 12;
        bool anyKept = false;
        uint32_t* buf = static_cast<uint32_t*>(kmalloc(blockSize));
        if (buf && readBlock(n->iBlock[12], buf)) {
            bool dirty = false;
            for (uint64_t j = 0; j < ppb; ++j) {
                if (base + j >= keep) {
                    if (buf[j]) { freeBlockNum(buf[j]); buf[j] = 0; dirty = true; if (n->blocksLo >= bpf) n->blocksLo -= bpf; }
                } else if (buf[j]) {
                    anyKept = true;
                }
            }
            if (dirty) writeBlock(n->iBlock[12], buf);
        }
        if (buf) kfree(buf);
        if (!anyKept && base >= keep) {
            freeBlockNum(n->iBlock[12]); n->iBlock[12] = 0;
            if (n->blocksLo >= bpf) n->blocksLo -= bpf;
        }
    }

    if (n->iBlock[13]) {   // double indirect
        const uint64_t base = 12 + ppb;
        bool anyL1 = false;
        uint32_t* l1 = static_cast<uint32_t*>(kmalloc(blockSize));
        if (l1 && readBlock(n->iBlock[13], l1)) {
            bool l1dirty = false;
            for (uint64_t a = 0; a < ppb; ++a) {
                if (!l1[a]) continue;
                const uint64_t l2base = base + a * ppb;
                bool anyL2 = false;
                uint32_t* l2 = static_cast<uint32_t*>(kmalloc(blockSize));
                if (l2 && readBlock(l1[a], l2)) {
                    bool l2dirty = false;
                    for (uint64_t b = 0; b < ppb; ++b) {
                        if (l2base + b >= keep) {
                            if (l2[b]) { freeBlockNum(l2[b]); l2[b] = 0; l2dirty = true; if (n->blocksLo >= bpf) n->blocksLo -= bpf; }
                        } else if (l2[b]) {
                            anyL2 = true;
                        }
                    }
                    if (l2dirty) writeBlock(l1[a], l2);
                }
                if (l2) kfree(l2);
                if (!anyL2 && l2base >= keep) {
                    freeBlockNum(l1[a]); l1[a] = 0; l1dirty = true;
                    if (n->blocksLo >= bpf) n->blocksLo -= bpf;
                } else {
                    anyL1 = true;
                }
            }
            if (l1dirty) writeBlock(n->iBlock[13], l1);
        }
        if (l1) kfree(l1);
        if (!anyL1 && base >= keep) {
            freeBlockNum(n->iBlock[13]); n->iBlock[13] = 0;
            if (n->blocksLo >= bpf) n->blocksLo -= bpf;
        }
    }
}
// Convert an HTree (indexed) directory to a plain linear directory so it can be
// modified: read every entry via the mapping-agnostic linear scan, free the old
// blocks, rebuild a linear "."/".." block, and re-insert each entry. Only
// depth-0 index trees are handled (deeper trees return false so the caller fails
// the op rather than risk corruption). The InstantOS image uses ^dir_index so
// this path is dormant in the OS and exercised host-side (ext4def.img).
bool Ext4FS::convertDirToLinear(VNode* dirV) {
    Ext4Node* dir = static_cast<Ext4Node*>(dirV->getData());
    if (!(dir->flags & kInodeIndexFl)) return true;

    uint8_t* b0 = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!b0) return false;
    const uint64_t p0 = resolveBlock(dir, 0);
    if (p0 == 0 || !readBlock(p0, b0)) { kfree(b0); return false; }
    if (b0[30] != 0) { kfree(b0); return false; }        // dx_root indirect_levels: deep tree
    Ext4DirEntry* d0 = reinterpret_cast<Ext4DirEntry*>(b0);
    Ext4DirEntry* dd0 = reinterpret_cast<Ext4DirEntry*>(b0 + d0->rec_len);
    const uint32_t parentInode = dd0->inode;
    kfree(b0);
    if (parentInode == 0) return false;

    const uint64_t totalBlocks = (dir->size + blockSize - 1) / blockSize;
    uint8_t* blk = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!blk) return false;

    struct Collected { uint32_t inode; uint8_t ftype; uint8_t nameLen; char name[256]; };
    // Two passes: count, then collect (bounded kmalloc).
    uint32_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        Collected* list = nullptr;
        if (pass == 1) {
            list = count ? static_cast<Collected*>(kmalloc(count * sizeof(Collected))) : nullptr;
            if (count && !list) { kfree(blk); return false; }
        }
        uint32_t idx = 0;
        for (uint64_t lb = 0; lb < totalBlocks; ++lb) {
            const uint64_t phys = resolveBlock(dir, lb);
            if (phys == 0 || !readBlock(phys, blk)) continue;
            uint32_t off = 0;
            while (off + 8 <= blockSize) {
                Ext4DirEntry* de = reinterpret_cast<Ext4DirEntry*>(blk + off);
                const uint16_t rec = de->rec_len;
                if (rec < 8 || off + rec > blockSize) break;
                if (de->inode != 0 && de->name_len > 0) {
                    const char* nm = reinterpret_cast<const char*>(blk + off + 8);
                    const bool dot = (de->name_len == 1 && nm[0] == '.');
                    const bool ddot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                    if (!dot && !ddot) {
                        if (pass == 0) ++count;
                        else if (idx < count) {
                            list[idx].inode = de->inode;
                            list[idx].ftype = de->file_type;
                            list[idx].nameLen = de->name_len;
                            memcpy(list[idx].name, nm, de->name_len);
                            list[idx].name[de->name_len] = '\0';
                            ++idx;
                        }
                    }
                }
                off += rec;
            }
        }
        if (pass == 0) continue;

        // Reset the directory to an empty linear "."/".." block.
        freeInodeData(dir);
        for (int i = 0; i < 15; ++i) dir->iBlock[i] = 0;
        dir->flags &= ~(kInodeIndexFl | kInodeExtentsFlag);
        dir->size = 0;
        dir->blocksLo = 0;

        const uint64_t nb = allocBlock();
        if (nb == 0) { kfree(blk); if (list) kfree(list); return false; }
        memset(blk, 0, blockSize);
        const uint32_t dirUsable = hasMetadataCsum ? (blockSize - 12) : blockSize;
        Ext4DirEntry* dot = reinterpret_cast<Ext4DirEntry*>(blk);
        dot->inode = dir->inodeNum; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = hasFileType ? kFtDir : 0; blk[8] = '.';
        Ext4DirEntry* dd = reinterpret_cast<Ext4DirEntry*>(blk + 12);
        dd->inode = parentInode; dd->rec_len = static_cast<uint16_t>(dirUsable - 12);
        dd->name_len = 2; dd->file_type = hasFileType ? kFtDir : 0;
        blk[20] = '.'; blk[21] = '.';
        bool ok = bmapSet(dir, 0, nb);
        dir->blocksLo += blockSize / 512;
        dir->size = blockSize;
        if (ok) ok = writeDirBlock(dir->inodeNum, dir->generation, nb, blk);
        kfree(blk);
        if (ok) ok = syncNodeInode(dir);

        for (uint32_t i = 0; i < count && ok; ++i) {
            if (dirInsert(dirV, list[i].name, list[i].inode, list[i].ftype) != 0) ok = false;
        }
        if (list) kfree(list);
        return ok;
    }
    kfree(blk);
    return false;
}

int Ext4FS::dirInsert(VNode* dirV, const char* name, uint32_t inodeNum, uint8_t fileType) {
    Ext4Node* dir = static_cast<Ext4Node*>(dirV->getData());
    if (dir->flags & kInodeIndexFl) {   // HTree: convert to linear, then insert
        if (!convertDirToLinear(dirV)) return -1;
    }
    const uint32_t nameLen = static_cast<uint32_t>(strlen(name));
    if (nameLen == 0 || nameLen > 255) return -1;
    const uint32_t needed = 8 + ((nameLen + 3) & ~3u);   // 4-byte aligned entry
    // Reserve the last 12 bytes for the dir_entry_tail checksum on metadata_csum.
    const uint32_t limit = hasMetadataCsum ? (blockSize - 12) : blockSize;

    uint8_t* blk = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!blk) return -1;

    const uint64_t totalBlocks = (dir->size + blockSize - 1) / blockSize;
    for (uint64_t lb = 0; lb < totalBlocks; ++lb) {
        const uint64_t phys = resolveBlock(dir, lb);
        if (phys == 0) continue;
        if (!readBlock(phys, blk)) { kfree(blk); return -1; }

        uint32_t off = 0;
        while (off + 8 <= limit) {
            Ext4DirEntry* de = reinterpret_cast<Ext4DirEntry*>(blk + off);
            const uint16_t rec = de->rec_len;
            if (rec < 8 || off + rec > blockSize) break;
            const uint32_t used = (de->inode == 0) ? 0 : (8 + ((de->name_len + 3) & ~3u));
            if (rec - used >= needed) {
                uint32_t newOff;
                if (de->inode == 0) {
                    newOff = off;
                    Ext4DirEntry* ne = reinterpret_cast<Ext4DirEntry*>(blk + newOff);
                    ne->inode = inodeNum;
                    ne->rec_len = rec;
                    ne->name_len = static_cast<uint8_t>(nameLen);
                    ne->file_type = hasFileType ? fileType : 0;
                    memcpy(blk + newOff + 8, name, nameLen);
                } else {
                    const uint16_t oldRec = rec;
                    de->rec_len = static_cast<uint16_t>(used);
                    newOff = off + used;
                    Ext4DirEntry* ne = reinterpret_cast<Ext4DirEntry*>(blk + newOff);
                    ne->inode = inodeNum;
                    ne->rec_len = static_cast<uint16_t>(oldRec - used);
                    ne->name_len = static_cast<uint8_t>(nameLen);
                    ne->file_type = hasFileType ? fileType : 0;
                    memcpy(blk + newOff + 8, name, nameLen);
                }
                bool ok = writeDirBlock(dir->inodeNum, dir->generation, phys, blk);
                if (ok) { dir->mtime = dir->ctime = nowTime(); ok = syncNodeInode(dir); }
                kfree(blk);
                return ok ? 0 : -1;
            }
            off += rec;
        }
    }

    // No room in existing blocks: append a new directory block.
    const uint64_t nb = allocBlock();
    if (nb == 0) { kfree(blk); return -1; }
    dir->blocksLo += blockSize / 512;
    if (!bmapSet(dir, totalBlocks, nb)) { kfree(blk); return -1; }
    memset(blk, 0, blockSize);
    Ext4DirEntry* ne = reinterpret_cast<Ext4DirEntry*>(blk);
    ne->inode = inodeNum;
    ne->rec_len = static_cast<uint16_t>(limit);
    ne->name_len = static_cast<uint8_t>(nameLen);
    ne->file_type = hasFileType ? fileType : 0;
    memcpy(blk + 8, name, nameLen);
    bool ok = writeDirBlock(dir->inodeNum, dir->generation, nb, blk);
    kfree(blk);
    dir->size += blockSize;
    dir->mtime = dir->ctime = nowTime();
    if (ok) ok = syncNodeInode(dir);
    return ok ? 0 : -1;
}

int Ext4FS::dirRemove(VNode* dirV, const char* name, uint32_t* removedInode) {
    Ext4Node* dir = static_cast<Ext4Node*>(dirV->getData());
    if (dir->flags & kInodeIndexFl) {   // HTree: convert to linear, then remove
        if (!convertDirToLinear(dirV)) return -1;
    }
    const uint32_t nameLen = static_cast<uint32_t>(strlen(name));
    if (nameLen == 0 || nameLen > 255) return -1;

    uint8_t* blk = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!blk) return -1;

    const uint64_t totalBlocks = (dir->size + blockSize - 1) / blockSize;
    for (uint64_t lb = 0; lb < totalBlocks; ++lb) {
        const uint64_t phys = resolveBlock(dir, lb);
        if (phys == 0) continue;
        if (!readBlock(phys, blk)) { kfree(blk); return -1; }

        uint32_t off = 0;
        int prevOff = -1;
        while (off + 8 <= blockSize) {
            Ext4DirEntry* de = reinterpret_cast<Ext4DirEntry*>(blk + off);
            const uint16_t rec = de->rec_len;
            if (rec < 8 || off + rec > blockSize) break;
            if (de->inode != 0 && de->name_len == nameLen &&
                memcmp(blk + off + 8, name, nameLen) == 0) {
                if (removedInode) *removedInode = de->inode;
                if (prevOff >= 0) {
                    Ext4DirEntry* prev = reinterpret_cast<Ext4DirEntry*>(blk + prevOff);
                    prev->rec_len = static_cast<uint16_t>(prev->rec_len + rec);
                } else {
                    de->inode = 0;   // first entry in block: tombstone
                }
                bool ok = writeDirBlock(dir->inodeNum, dir->generation, phys, blk);
                if (ok) { dir->mtime = dir->ctime = nowTime(); ok = syncNodeInode(dir); }
                kfree(blk);
                return ok ? 0 : -1;
            }
            prevOff = static_cast<int>(off);
            off += rec;
        }
    }
    kfree(blk);
    return -1;   // not found
}

bool Ext4FS::dirIsEmpty(Ext4Node* dir) {
    uint8_t* blk = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!blk) return false;
    bool empty = true;
    const uint64_t totalBlocks = (dir->size + blockSize - 1) / blockSize;
    for (uint64_t lb = 0; lb < totalBlocks && empty; ++lb) {
        const uint64_t phys = resolveBlock(dir, lb);
        if (phys == 0) continue;
        if (!readBlock(phys, blk)) { empty = false; break; }
        uint32_t off = 0;
        while (off + 8 <= blockSize) {
            Ext4DirEntry* de = reinterpret_cast<Ext4DirEntry*>(blk + off);
            const uint16_t rec = de->rec_len;
            if (rec < 8 || off + rec > blockSize) break;
            if (de->inode != 0 && de->name_len > 0) {
                const char* nm = reinterpret_cast<const char*>(blk + off + 8);
                const bool dot = (de->name_len == 1 && nm[0] == '.');
                const bool dotdot = (de->name_len == 2 && nm[0] == '.' && nm[1] == '.');
                if (!dot && !dotdot) { empty = false; break; }
            }
            off += rec;
        }
    }
    kfree(blk);
    return empty;
}

VNode* Ext4FS::createEntry(VNode* parent, const char* name, uint32_t mode,
                           bool isDir, const char* symlinkTarget) {
    if (!writable || !parent || !name) return nullptr;
    Ext4Node* p = static_cast<Ext4Node*>(parent->getData());
    if (!p || (p->mode & kModeFmtMask) != kModeDir) return nullptr;
    const uint32_t nameLen = static_cast<uint32_t>(strlen(name));
    if (nameLen == 0 || nameLen > 255) return nullptr;

    // Reject if the name already exists.
    if (VNode* ex = lookupInDir(parent, name)) {
        if (ex->getData()) kfree(ex->getData());
        delete ex;
        return nullptr;
    }

    const uint32_t inodeNum = allocInode(isDir);
    if (inodeNum == 0) return nullptr;

    Ext4Inode ino;
    memset(&ino, 0, sizeof(ino));
    const uint16_t fmt = isDir ? kModeDir : (symlinkTarget ? kModeLnk : kModeReg);
    ino.i_mode = static_cast<uint16_t>((mode & 07777) | fmt);
    ino.i_links_count = isDir ? 2 : 1;
    ino.i_atime = ino.i_ctime = ino.i_mtime = nowTime();
    ino.i_flags = 0;   // block-mapped

    const uint8_t fileType = isDir ? kFtDir : (symlinkTarget ? kFtSymlink : kFtRegFile);

    if (isDir) {
        const uint64_t blk = allocBlock();
        if (blk == 0) { freeInodeNum(inodeNum, true); return nullptr; }
        ino.i_block[0] = static_cast<uint32_t>(blk);
        ino.i_size_lo = blockSize;
        ino.i_blocks_lo = blockSize / 512;
        uint8_t* buf = static_cast<uint8_t*>(kmalloc(blockSize));
        if (!buf) { freeBlockNum(blk); freeInodeNum(inodeNum, true); return nullptr; }
        memset(buf, 0, blockSize);
        const uint32_t dirUsable = hasMetadataCsum ? (blockSize - 12) : blockSize;
        Ext4DirEntry* dot = reinterpret_cast<Ext4DirEntry*>(buf);
        dot->inode = inodeNum; dot->rec_len = 12; dot->name_len = 1;
        dot->file_type = hasFileType ? kFtDir : 0; buf[8] = '.';
        Ext4DirEntry* dd = reinterpret_cast<Ext4DirEntry*>(buf + 12);
        dd->inode = p->inodeNum; dd->rec_len = static_cast<uint16_t>(dirUsable - 12);
        dd->name_len = 2; dd->file_type = hasFileType ? kFtDir : 0;
        buf[20] = '.'; buf[21] = '.';
        const bool ok = writeDirBlock(inodeNum, 0, blk, buf);   // new dir: generation 0
        kfree(buf);
        if (!ok) { freeBlockNum(blk); freeInodeNum(inodeNum, true); return nullptr; }
    } else if (symlinkTarget) {
        const uint32_t len = static_cast<uint32_t>(strlen(symlinkTarget));
        ino.i_size_lo = len;
        if (len < 60) {
            memcpy(ino.i_block, symlinkTarget, len);   // fast symlink
        } else {
            const uint64_t blk = allocBlock();
            if (blk == 0) { freeInodeNum(inodeNum, false); return nullptr; }
            ino.i_block[0] = static_cast<uint32_t>(blk);
            ino.i_blocks_lo = blockSize / 512;
            uint8_t* buf = static_cast<uint8_t*>(kmalloc(blockSize));
            if (!buf) { freeBlockNum(blk); freeInodeNum(inodeNum, false); return nullptr; }
            memset(buf, 0, blockSize);
            memcpy(buf, symlinkTarget, len > blockSize ? blockSize : len);
            const bool ok = writeBlock(blk, buf);
            kfree(buf);
            if (!ok) { freeBlockNum(blk); freeInodeNum(inodeNum, false); return nullptr; }
        }
    }

    if (!writeInodeRaw(inodeNum, &ino, true)) {
        freeInodeNum(inodeNum, isDir);
        return nullptr;
    }
    if (dirInsert(parent, name, inodeNum, fileType) != 0) {
        freeInodeNum(inodeNum, isDir);
        return nullptr;
    }
    if (isDir) {
        ++p->linksCount;   // the new dir's ".." references the parent
        syncNodeInode(p);
    }
    return makeVNode(inodeNum, &ino);
}
int64_t Ext4FS::nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset) {
    refreshVNode(node);
    if (!node || !buffer) return -1;
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!n || !fs || !fs->writable) return -1;
    if ((n->mode & kModeFmtMask) == kModeDir) return -1;
    if (size == 0) return 0;

    const bool extentFile = (n->flags & kInodeExtentsFlag) != 0;
    if (extentFile && offset + size > n->size) {
        return -1;   // growing extent-mapped files is out of scope (phase 3)
    }

    JTxn txn(fs);
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    const uint64_t bs = fs->blockSize;
    uint64_t done = 0;
    uint64_t logical = offset / bs;
    uint32_t inBlock = static_cast<uint32_t>(offset % bs);

    // File data bypasses the journal (ordered mode) and goes straight to its
    // blocks. Whole blocks at consecutive physical addresses are written as
    // one run (the allocator hands out consecutive blocks), so a large write
    // is a few device commands rather than one per block.
    uint64_t runPhys = 0;     // first block of the pending run of whole blocks
    uint64_t runDone = 0;     // `done` where the run starts
    uint64_t runBlocks = 0;
    constexpr uint64_t kMaxRunBlocks = 256;   // 1 MiB at 4 KiB blocks
    bool failed = false;
    auto flushRun = [&]() -> bool {
        if (runBlocks == 0) return true;
        const bool ok = fs->devWriteBytes(runPhys * bs, src + runDone, runBlocks * bs);
        if (!ok) done = runDone;   // nothing of the run counts as written
        runBlocks = 0;
        return ok;
    };
    uint8_t* partial = nullptr;   // a new block written in part: zeros around the data

    while (done < size) {
        uint64_t phys = fs->resolveBlock(n, logical);
        bool fresh = false;
        if (phys == 0) {
            if (extentFile) break;
            const uint64_t nb = fs->allocBlock();
            if (nb == 0) break;
            n->blocksLo += bs / 512;
            if (!fs->bmapSet(n, logical, nb)) break;
            phys = nb;
            fresh = true;
        }
        uint64_t toWrite = bs - inBlock;
        if (toWrite > size - done) toWrite = size - done;
        if (toWrite == bs) {
            if (runBlocks != 0 && phys == runPhys + runBlocks && runBlocks < kMaxRunBlocks) {
                ++runBlocks;
            } else {
                if (!flushRun()) { failed = true; break; }
                runPhys = phys;
                runDone = done;
                runBlocks = 1;
            }
        } else {
            if (!flushRun()) { failed = true; break; }
            bool ok;
            if (fresh) {
                // The rest of a new block must read as zeros, whatever the
                // disk held there before.
                if (!partial) partial = static_cast<uint8_t*>(kmalloc(bs));
                ok = partial != nullptr;
                if (ok) {
                    memset(partial, 0, bs);
                    memcpy(partial + inBlock, src + done, toWrite);
                    ok = fs->devWriteBytes(phys * bs, partial, bs);
                }
            } else {
                ok = fs->devWriteBytes(phys * bs + inBlock, src + done, toWrite);
            }
            if (!ok) { failed = true; break; }
        }
        done += toWrite;
        inBlock = 0;
        ++logical;
    }
    if (!failed) flushRun();
    if (partial) kfree(partial);

    if (offset + done > n->size) n->size = offset + done;
    n->mtime = n->ctime = nowTime();   // data write advances mtime (and ctime)
    fs->syncNodeInode(n);
    if (txn.commit() != 0) return -1;
    return done > 0 ? static_cast<int64_t>(done) : -1;
}

int Ext4FS::nodeCreate(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    refreshVNode(parent);
    if (!parent) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(parent->getFS());
    if (!fs || !fs->writable) return -1;
    JTxn txn(fs);
    VNode* v = fs->createEntry(parent, name, mode, false, nullptr);
    if (!v) return -1;
    if (result) *result = v;
    else { if (v->getData()) kfree(v->getData()); delete v; }
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result) {
    refreshVNode(parent);
    if (!parent) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(parent->getFS());
    if (!fs || !fs->writable) return -1;
    JTxn txn(fs);
    VNode* v = fs->createEntry(parent, name, mode, true, nullptr);
    if (!v) return -1;
    if (result) *result = v;
    else { if (v->getData()) kfree(v->getData()); delete v; }
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeSymlink(VNode* parent, const char* name, const char* target, VNode** result) {
    refreshVNode(parent);
    if (!parent || !target) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(parent->getFS());
    if (!fs || !fs->writable) return -1;
    JTxn txn(fs);
    VNode* v = fs->createEntry(parent, name, 0777, false, target);
    if (!v) return -1;
    if (result) *result = v;
    else { if (v->getData()) kfree(v->getData()); delete v; }
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeChmod(VNode* node, uint32_t mode) {
    refreshVNode(node);
    if (!node) return -1;
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!n || !fs || !fs->writable) return -1;
    JTxn txn(fs);
    n->mode = static_cast<uint16_t>((n->mode & kModeFmtMask) | (mode & 07777));
    n->ctime = nowTime();
    if (!fs->syncNodeInode(n)) return -1;
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeChown(VNode* node, uint32_t uid, uint32_t gid) {
    refreshVNode(node);
    if (!node) return -1;
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!n || !fs || !fs->writable) return -1;
    JTxn txn(fs);
    n->uid = uid;
    n->gid = gid;
    n->ctime = nowTime();
    if (!fs->syncNodeInode(n)) return -1;
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeFsync(VNode* node) {
    refreshVNode(node);
    if (!node) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!fs || !fs->device) return -1;
    // File data is already on the device; commit the running transaction's
    // metadata, then push the device's write cache to stable storage.
    const int committed = fs->sync(false);
    return (fs->device->flush() && committed == 0) ? 0 : -1;
}

int Ext4FS::nodeUtime(VNode* node, uint64_t atime, uint64_t mtime) {
    refreshVNode(node);
    if (!node) return -1;
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!n || !fs || !fs->writable) return -1;
    JTxn txn(fs);
    n->atime = static_cast<uint32_t>(atime);
    n->mtime = static_cast<uint32_t>(mtime);
    n->ctime = nowTime();   // changing timestamps updates ctime
    if (!fs->syncNodeInode(n)) return -1;
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeTruncate(VNode* node, uint64_t newSize) {
    refreshVNode(node);
    if (!node) return -1;
    Ext4Node* n = static_cast<Ext4Node*>(node->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(node->getFS());
    if (!n || !fs || !fs->writable) return -1;
    if ((n->mode & kModeFmtMask) != kModeReg) return -1;
    if (n->flags & kInodeExtentsFlag) {
        if (newSize == n->size) return 0;
        // O_TRUNC / truncate-to-zero converts an extent-mapped file to an empty
        // block-mapped file so it can be rewritten (build tools O_TRUNC outputs).
        // Only depth-0 extent trees are convertible without leaking index blocks;
        // deeper trees (large fragmented files) remain unsupported.
        const uint16_t ehDepth = static_cast<uint16_t>(n->iBlock[1] >> 16);
        if (newSize != 0 || ehDepth != 0) return -1;
        JTxn txn(fs);
        fs->freeInodeData(n);                    // free depth-0 extent data blocks
        for (int i = 0; i < 15; ++i) n->iBlock[i] = 0;
        n->flags &= ~kInodeExtentsFlag;          // now block-mapped
        n->size = 0;
        n->blocksLo = 0;
        n->mtime = n->ctime = nowTime();
        if (!fs->syncNodeInode(n)) return -1;
        return txn.commit() == 0 ? 0 : -1;
    }
    JTxn txn(fs);
    if (newSize < n->size) {
        const uint64_t keepBlocks = (newSize + fs->blockSize - 1) / fs->blockSize;
        fs->bmapTruncate(n, keepBlocks);
        // The new last block keeps bytes past the new end: zero them, or a
        // later extension (truncate up, a write past the end) would show them.
        const uint32_t tail = static_cast<uint32_t>(newSize % fs->blockSize);
        const uint64_t last = tail ? fs->resolveBlock(n, newSize / fs->blockSize) : 0;
        if (last) {
            uint8_t* z = static_cast<uint8_t*>(kmalloc(fs->blockSize - tail));
            if (z) {
                memset(z, 0, fs->blockSize - tail);
                fs->devWriteBytes(last * fs->blockSize + tail, z, fs->blockSize - tail);
                kfree(z);
            }
        }
    }
    // Growing produces a sparse file (holes read as zero, allocated on write).
    n->size = newSize;
    n->mtime = n->ctime = nowTime();
    if (!fs->syncNodeInode(n)) return -1;
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeUnlink(VNode* parent, const char* name) {
    refreshVNode(parent);
    if (!parent || !name) return -1;
    Ext4Node* p = static_cast<Ext4Node*>(parent->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(parent->getFS());
    if (!p || !fs || !fs->writable || (p->mode & kModeFmtMask) != kModeDir) return -1;

    JTxn txn(fs);
    VNode* target = fs->lookupInDir(parent, name);
    if (!target) return -1;
    Ext4Node* tn = static_cast<Ext4Node*>(target->getData());
    if ((tn->mode & kModeFmtMask) == kModeDir) {   // directories go through rmdir
        kfree(tn); delete target; return -1;
    }
    const uint32_t tino = tn->inodeNum;
    kfree(tn); delete target;

    uint32_t removed = 0;
    if (fs->dirRemove(parent, name, &removed) != 0) return -1;

    Ext4Inode ino;
    if (fs->readInode(tino, &ino)) {
        if (ino.i_links_count > 0) --ino.i_links_count;
        if (ino.i_links_count == 0) {
            Ext4Node fn;
            memset(&fn, 0, sizeof(fn));
            fn.inodeNum = tino;
            fn.mode = ino.i_mode;
            fn.flags = ino.i_flags;
            fn.blocksLo = ino.i_blocks_lo;
            fn.size = static_cast<uint64_t>(ino.i_size_lo) |
                (((ino.i_mode & kModeFmtMask) == kModeReg) ? (static_cast<uint64_t>(ino.i_size_high) << 32) : 0);
            for (int i = 0; i < 15; ++i) fn.iBlock[i] = ino.i_block[i];
            fs->freeInodeData(&fn);
            ino.i_links_count = 0;
            ino.i_blocks_lo = 0;
            ino.i_dtime = nowTime();
            fs->writeInodeRaw(tino, &ino, false);
            fs->freeInodeNum(tino, false);
        } else {
            fs->writeInodeRaw(tino, &ino, false);
        }
    }
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeRmdir(VNode* parent, const char* name) {
    refreshVNode(parent);
    if (!parent || !name) return -1;
    Ext4Node* p = static_cast<Ext4Node*>(parent->getData());
    Ext4FS* fs = static_cast<Ext4FS*>(parent->getFS());
    if (!p || !fs || !fs->writable || (p->mode & kModeFmtMask) != kModeDir) return -1;

    JTxn txn(fs);
    VNode* target = fs->lookupInDir(parent, name);
    if (!target) return -1;
    Ext4Node* tn = static_cast<Ext4Node*>(target->getData());
    if ((tn->mode & kModeFmtMask) != kModeDir) { kfree(tn); delete target; return -1; }
    if (!fs->dirIsEmpty(tn)) { kfree(tn); delete target; return -1; }

    const uint32_t tino = tn->inodeNum;
    uint32_t removed = 0;
    if (fs->dirRemove(parent, name, &removed) != 0) { kfree(tn); delete target; return -1; }

    fs->freeInodeData(tn);   // free the directory's data block(s)
    Ext4Inode ino;
    if (fs->readInode(tino, &ino)) {
        ino.i_links_count = 0;
        ino.i_blocks_lo = 0;
        ino.i_dtime = nowTime();
        fs->writeInodeRaw(tino, &ino, false);
    }
    fs->freeInodeNum(tino, true);

    if (p->linksCount > 0) --p->linksCount;   // removed dir's ".." no longer references parent
    fs->syncNodeInode(p);

    kfree(tn); delete target;
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeLink(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName) {
    refreshVNode(oldParent); refreshVNode(newParent);
    if (!oldParent || !oldName || !newParent || !newName) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(oldParent->getFS());
    if (!fs || !fs->writable || fs != static_cast<Ext4FS*>(newParent->getFS())) return -1;

    JTxn txn(fs);
    VNode* src = fs->lookupInDir(oldParent, oldName);
    if (!src) return -1;
    Ext4Node* sn = static_cast<Ext4Node*>(src->getData());
    if ((sn->mode & kModeFmtMask) == kModeDir) { kfree(sn); delete src; return -1; }
    const uint32_t ino = sn->inodeNum;
    const uint8_t ft = fs->hasFileType
        ? ((sn->mode & kModeFmtMask) == kModeLnk ? kFtSymlink : kFtRegFile) : 0;
    kfree(sn); delete src;

    if (fs->dirInsert(newParent, newName, ino, ft) != 0) return -1;
    Ext4Inode i2;
    if (fs->readInode(ino, &i2)) { ++i2.i_links_count; fs->writeInodeRaw(ino, &i2, false); }
    return txn.commit() == 0 ? 0 : -1;
}

int Ext4FS::nodeRename(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName) {
    refreshVNode(oldParent); refreshVNode(newParent);
    if (!oldParent || !oldName || !newParent || !newName) return -1;
    Ext4FS* fs = static_cast<Ext4FS*>(oldParent->getFS());
    if (!fs || !fs->writable || fs != static_cast<Ext4FS*>(newParent->getFS())) return -1;

    JTxn txn(fs);
    VNode* src = fs->lookupInDir(oldParent, oldName);
    if (!src) return -1;
    Ext4Node* sn = static_cast<Ext4Node*>(src->getData());
    const uint32_t ino = sn->inodeNum;
    const bool srcIsDir = (sn->mode & kModeFmtMask) == kModeDir;
    const uint8_t ft = fs->hasFileType
        ? (srcIsDir ? kFtDir : ((sn->mode & kModeFmtMask) == kModeLnk ? kFtSymlink : kFtRegFile)) : 0;

    // If the destination already exists, remove it first (only files/symlinks;
    // refuse to clobber a directory).
    if (VNode* dst = fs->lookupInDir(newParent, newName)) {
        Ext4Node* dn = static_cast<Ext4Node*>(dst->getData());
        const bool dstDir = (dn->mode & kModeFmtMask) == kModeDir;
        const uint32_t dino = dn->inodeNum;
        kfree(dn); delete dst;
        if (dstDir) { kfree(sn); delete src; return -1; }
        uint32_t rm = 0;
        fs->dirRemove(newParent, newName, &rm);
        Ext4Inode di;
        if (fs->readInode(dino, &di)) {
            if (di.i_links_count > 0) --di.i_links_count;
            if (di.i_links_count == 0) {
                Ext4Node fn; memset(&fn, 0, sizeof(fn));
                fn.inodeNum = dino; fn.mode = di.i_mode; fn.flags = di.i_flags;
                fn.blocksLo = di.i_blocks_lo;
                fn.size = static_cast<uint64_t>(di.i_size_lo);
                for (int i = 0; i < 15; ++i) fn.iBlock[i] = di.i_block[i];
                fs->freeInodeData(&fn);
                di.i_links_count = 0; di.i_blocks_lo = 0; di.i_dtime = nowTime();
                fs->writeInodeRaw(dino, &di, false);
                fs->freeInodeNum(dino, false);
            } else {
                fs->writeInodeRaw(dino, &di, false);
            }
        }
    }

    // oldParent and newParent can be two VNodes of one directory (ext4 makes a
    // VNode per lookup). dirInsert may append a block through newParent's copy,
    // so reload oldParent's before dirRemove writes it back, or the old size
    // drops that block and the renamed entry with it (P7.2 lost mpfr's fma.lo).
    const bool sameDir = static_cast<Ext4Node*>(oldParent->getData())->inodeNum ==
                         static_cast<Ext4Node*>(newParent->getData())->inodeNum;
    if (fs->dirInsert(newParent, newName, ino, ft) != 0) { kfree(sn); delete src; return -1; }
    refreshVNode(oldParent);
    uint32_t rm = 0;
    fs->dirRemove(oldParent, oldName, &rm);

    // Moving a directory across parents: fix its ".." and both link counts.
    if (srcIsDir && !sameDir) {
        Ext4Node* op = static_cast<Ext4Node*>(oldParent->getData());
        Ext4Node* np = static_cast<Ext4Node*>(newParent->getData());
        uint8_t* buf = static_cast<uint8_t*>(kmalloc(fs->blockSize));
        const uint64_t phys = fs->resolveBlock(sn, 0);
        if (buf && phys && fs->readBlock(phys, buf)) {
            uint32_t off = 0;
            while (off + 8 <= fs->blockSize) {
                Ext4DirEntry* de = reinterpret_cast<Ext4DirEntry*>(buf + off);
                const uint16_t rec = de->rec_len;
                if (rec < 8) break;
                if (de->name_len == 2) {
                    const char* nm = reinterpret_cast<const char*>(buf + off + 8);
                    if (nm[0] == '.' && nm[1] == '.') { de->inode = np->inodeNum; fs->writeDirBlock(sn->inodeNum, sn->generation, phys, buf); break; }
                }
                off += rec;
            }
        }
        if (buf) kfree(buf);
        if (op->linksCount > 0) --op->linksCount;
        fs->syncNodeInode(op);
        ++np->linksCount;
        fs->syncNodeInode(np);
    }

    kfree(sn); delete src;
    return txn.commit() == 0 ? 0 : -1;
}

// ===========================================================================
// metadata_csum (crc32c) checksum helpers. All are no-ops when the feature is
// absent, so the write path is identical either way.
// ===========================================================================

void Ext4FS::superblockSetCsum() {
    if (!hasMetadataCsum) return;
    // crc32c(~0) over the first 1020 bytes (everything before s_checksum @ 0x3FC).
    sb.s_checksum = crc32c(0xFFFFFFFFu, &sb, 0x3FC);
}

void Ext4FS::groupDescSetCsum(uint32_t group, Ext4GroupDesc* gd) {
    if (!hasMetadataCsum) return;
    const uint32_t leGroup = group;                       // le32 on x86
    uint32_t crc = crc32c(csumSeed, &leGroup, sizeof(leGroup));
    crc = crc32c(crc, gd, 0x1E);                           // up to bg_checksum
    const uint16_t zero = 0;
    crc = crc32c(crc, &zero, sizeof(zero));                // checksum field as zero
    if (descSize > 0x20) {
        crc = crc32c(crc, reinterpret_cast<const uint8_t*>(gd) + 0x20, descSize - 0x20);
    }
    gd->bg_checksum = static_cast<uint16_t>(crc & 0xFFFF);
}

uint32_t Ext4FS::inodeSeed(uint32_t inodeNum, uint32_t generation) {
    const uint32_t inum = inodeNum;                        // le32 on x86
    uint32_t c = crc32c(csumSeed, &inum, sizeof(inum));
    c = crc32c(c, &generation, sizeof(generation));
    return c;
}

void Ext4FS::inodeSetCsum(uint32_t inodeNum, uint8_t* raw) {
    if (!hasMetadataCsum) return;
    // i_generation @ 0x64 (little-endian).
    const uint32_t gen = static_cast<uint32_t>(raw[0x64]) |
        (static_cast<uint32_t>(raw[0x65]) << 8) |
        (static_cast<uint32_t>(raw[0x66]) << 16) |
        (static_cast<uint32_t>(raw[0x67]) << 24);
    // i_checksum_hi @ 0x82 participates only when i_extra_isize covers it.
    const uint16_t extraIsize = (inodeSize > 128)
        ? static_cast<uint16_t>(raw[0x80] | (raw[0x81] << 8)) : 0;
    const bool includeHi = (inodeSize > 128) && (extraIsize >= 4);
    // Checksum the inode with the checksum fields treated as zero, then store.
    raw[0x7C] = 0; raw[0x7D] = 0;                           // i_checksum_lo
    if (includeHi) { raw[0x82] = 0; raw[0x83] = 0; }        // i_checksum_hi
    uint32_t crc = inodeSeed(inodeNum, gen);
    crc = crc32c(crc, raw, inodeSize);
    raw[0x7C] = static_cast<uint8_t>(crc & 0xFF);
    raw[0x7D] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    if (includeHi) {
        raw[0x82] = static_cast<uint8_t>((crc >> 16) & 0xFF);
        raw[0x83] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    }
}

void Ext4FS::dirBlockSetCsum(uint32_t dirInode, uint32_t generation, uint8_t* block) {
    if (!hasMetadataCsum) return;
    uint32_t crc = inodeSeed(dirInode, generation);
    crc = crc32c(crc, block, blockSize - 12);              // all bytes before the tail
    block[blockSize - 4] = static_cast<uint8_t>(crc & 0xFF);
    block[blockSize - 3] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    block[blockSize - 2] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    block[blockSize - 1] = static_cast<uint8_t>((crc >> 24) & 0xFF);
}

bool Ext4FS::writeDirBlock(uint32_t dirInode, uint32_t generation, uint64_t phys, uint8_t* block) {
    if (hasMetadataCsum) {
        // Install/refresh the dir_entry_tail (a fake entry marking the checksum),
        // then compute its checksum.
        const uint32_t t = blockSize - 12;
        block[t + 0] = 0; block[t + 1] = 0; block[t + 2] = 0; block[t + 3] = 0;  // inode = 0
        block[t + 4] = 12; block[t + 5] = 0;   // rec_len = 12
        block[t + 6] = 0;                       // name_len = 0
        block[t + 7] = kFtDirCsum;              // file_type = 0xDE
        dirBlockSetCsum(dirInode, generation, block);
    }
    return writeBlock(phys, block);
}

// ===========================================================================
// Phase 4: JBD2 journal recovery (replay on mount)
//
// JBD2 on-disk structures are big-endian. On a dirty journal (superblock
// s_start != 0) the committed transactions are replayed to their final block
// locations before the filesystem is otherwise trusted, then the journal is
// reset to clean and the RECOVER feature flag is cleared. Journal-side checksums
// (csum_v2/v3) are parsed for layout but not verified; this handles the common
// case (single/few transactions) sufficiently for crash recovery.
// ===========================================================================

namespace {
constexpr uint32_t kJbd2Magic = 0xC03B3998u;
constexpr uint32_t kJbd2Descriptor = 1;
constexpr uint32_t kJbd2Commit = 2;
constexpr uint32_t kJbd2Revoke = 5;
constexpr uint16_t kJbd2FlagEscape = 1;
constexpr uint16_t kJbd2FlagSameUuid = 2;
constexpr uint16_t kJbd2FlagLastTag = 8;

uint32_t jbe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t jbe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
void jwbe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
void jwbe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
uint32_t jbNext(uint32_t block, uint32_t first, uint32_t maxlen) {
    const uint32_t n = block + 1;
    return n >= maxlen ? first : n;
}
}  // namespace

bool Ext4FS::resolveInodeBlock(uint32_t inodeNum, uint64_t logicalBlock, uint64_t* physOut) {
    Ext4Inode ino;
    if (!readInode(inodeNum, &ino)) {
        return false;
    }
    Ext4Node n;
    memset(&n, 0, sizeof(n));
    n.inodeNum = inodeNum;
    n.mode = ino.i_mode;
    n.flags = ino.i_flags;
    n.size = static_cast<uint64_t>(ino.i_size_lo) | (static_cast<uint64_t>(ino.i_size_high) << 32);
    for (int i = 0; i < 15; ++i) n.iBlock[i] = ino.i_block[i];
    const uint64_t p = resolveBlock(&n, logicalBlock);
    if (physOut) *physOut = p;
    return p != 0;
}

uint32_t Ext4FS::journalDescTagCount(const uint8_t* jblk, bool v3, bool j64) {
    uint32_t off = 12, count = 0;
    for (;;) {
        const uint32_t tagsz = v3 ? 16u : (j64 ? 12u : 8u);
        if (off + tagsz > blockSize) break;
        const uint16_t flags = v3 ? static_cast<uint16_t>(jbe32(jblk + off + 4))
                                  : jbe16(jblk + off + 6);
        ++count;
        off += tagsz;
        if (!(flags & kJbd2FlagSameUuid)) off += 16;   // UUID follows
        if (flags & kJbd2FlagLastTag) break;
    }
    return count;
}

int Ext4FS::journalReplay() {
    const uint32_t jinum = sb.s_journal_inum ? sb.s_journal_inum : 8;
    Ext4Inode jino;
    if (!readInode(jinum, &jino)) return -1;

    Ext4Node jn;
    memset(&jn, 0, sizeof(jn));
    jn.inodeNum = jinum;
    jn.mode = jino.i_mode;
    jn.flags = jino.i_flags;
    jn.size = static_cast<uint64_t>(jino.i_size_lo) | (static_cast<uint64_t>(jino.i_size_high) << 32);
    for (int i = 0; i < 15; ++i) jn.iBlock[i] = jino.i_block[i];

    uint8_t* jsb = static_cast<uint8_t*>(kmalloc(blockSize));
    uint8_t* jblk = static_cast<uint8_t*>(kmalloc(blockSize));
    uint8_t* dblk = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!jsb || !jblk || !dblk) {
        if (jsb) kfree(jsb);
        if (jblk) kfree(jblk);
        if (dblk) kfree(dblk);
        return -1;
    }

    int result = 0;   // default: nothing to do
    const uint64_t p0 = resolveBlock(&jn, 0);
    if (p0 != 0 && readBlock(p0, jsb) && jbe32(jsb) == kJbd2Magic) {
        const uint32_t jBlockSize = jbe32(jsb + 12);
        const uint32_t jMaxlen = jbe32(jsb + 16);
        const uint32_t jFirst  = jbe32(jsb + 20);
        const uint32_t jSeq    = jbe32(jsb + 24);
        const uint32_t jStart  = jbe32(jsb + 28);
        const uint32_t jIncompat = jbe32(jsb + 40);
        const bool j64 = (jIncompat & 0x2) != 0;   // JBD2_FEATURE_INCOMPAT_64BIT
        const bool jv3 = (jIncompat & 0x10) != 0;  // JBD2_FEATURE_INCOMPAT_CSUM_V3

        if (jStart != 0 && jBlockSize == blockSize && jMaxlen != 0) {
            // Pass A: walk transactions to find the last committed sequence.
            uint32_t cur = jStart, seq = jSeq, lastCommitted = jSeq - 1;
            for (;;) {
                const uint64_t pj = resolveBlock(&jn, cur);
                if (pj == 0 || !readBlock(pj, jblk)) break;
                if (jbe32(jblk) != kJbd2Magic || jbe32(jblk + 8) != seq) break;
                const uint32_t bt = jbe32(jblk + 4);
                if (bt == kJbd2Descriptor) {
                    const uint32_t nTags = journalDescTagCount(jblk, jv3, j64);
                    cur = jbNext(cur, jFirst, jMaxlen);
                    for (uint32_t k = 0; k < nTags; ++k) cur = jbNext(cur, jFirst, jMaxlen);
                } else if (bt == kJbd2Revoke) {
                    cur = jbNext(cur, jFirst, jMaxlen);
                } else if (bt == kJbd2Commit) {
                    lastCommitted = seq;
                    ++seq;
                    cur = jbNext(cur, jFirst, jMaxlen);
                } else {
                    break;
                }
            }

            // Pass B: replay committed transactions [jSeq, lastCommitted].
            cur = jStart;
            seq = jSeq;
            while (seq <= lastCommitted) {
                const uint64_t pj = resolveBlock(&jn, cur);
                if (pj == 0 || !readBlock(pj, jblk)) break;
                if (jbe32(jblk) != kJbd2Magic || jbe32(jblk + 8) != seq) break;
                const uint32_t bt = jbe32(jblk + 4);
                if (bt == kJbd2Descriptor) {
                    uint32_t off = 12;
                    uint32_t dataJb = jbNext(cur, jFirst, jMaxlen);
                    bool last = false;
                    while (!last) {
                        const uint32_t tagsz = jv3 ? 16u : (j64 ? 12u : 8u);
                        if (off + tagsz > blockSize) break;
                        uint64_t target;
                        uint16_t flags;
                        if (jv3) {
                            const uint32_t tb = jbe32(jblk + off);
                            const uint32_t fl = jbe32(jblk + off + 4);
                            const uint32_t hi = jbe32(jblk + off + 8);
                            target = tb | (static_cast<uint64_t>(hi) << 32);
                            flags = static_cast<uint16_t>(fl);
                        } else {
                            const uint32_t tb = jbe32(jblk + off);
                            flags = jbe16(jblk + off + 6);
                            const uint32_t hi = j64 ? jbe32(jblk + off + 8) : 0;
                            target = tb | (static_cast<uint64_t>(hi) << 32);
                        }
                        const uint64_t pjd = resolveBlock(&jn, dataJb);
                        if (pjd != 0 && readBlock(pjd, dblk)) {
                            if (flags & kJbd2FlagEscape) {   // restore the escaped magic word
                                dblk[0] = 0xC0; dblk[1] = 0x3B; dblk[2] = 0x39; dblk[3] = 0x98;
                            }
                            if (target != 0 && writeBlock(target, dblk)) ++replayedBlocks_;
                        }
                        dataJb = jbNext(dataJb, jFirst, jMaxlen);
                        off += tagsz;
                        if (!(flags & kJbd2FlagSameUuid)) off += 16;
                        if (flags & kJbd2FlagLastTag) last = true;
                    }
                    cur = dataJb;
                } else if (bt == kJbd2Revoke) {
                    cur = jbNext(cur, jFirst, jMaxlen);
                } else if (bt == kJbd2Commit) {
                    ++seq;
                    cur = jbNext(cur, jFirst, jMaxlen);
                } else {
                    break;
                }
            }

            replayedTransactions_ = lastCommitted + 1 - jSeq;

            // Reset the journal to clean and clear the fs RECOVER flag.
            jwbe32(jsb + 28, 0);                    // s_start = 0
            jwbe32(jsb + 24, lastCommitted + 1);    // s_sequence = next expected
            writeBlock(p0, jsb);
            // The replay may have rewritten the superblock block (write-side
            // journaling journals it). Re-read that fresh copy before clearing
            // RECOVER so we don't clobber replayed fields (e.g. free counts).
            devReadBytes(kSuperblockOffset, &sb, sizeof(sb));
            sb.s_feature_incompat &= ~kIncompatRecover;
            flushSuperblock();
        }
    }

    kfree(jsb);
    kfree(jblk);
    kfree(dblk);
    return result;
}

// ===========================================================================
// Phase 4b: write-side JBD2 transactions (ordered mode; simple journals only)
//
// Each top-level write op runs inside one transaction: metadata writes buffer
// into an in-memory block cache (file data bypasses the journal and is written
// directly). On commit the buffered blocks are written to the journal as one
// transaction (descriptor + data + commit) and the journal is marked dirty;
// then they are checkpointed to their final locations and the journal reset.
// A crash between commit and checkpoint is repaired by journalReplay().
// ===========================================================================

bool Ext4FS::journalLoadGeometry() {
    const uint32_t jinum = sb.s_journal_inum ? sb.s_journal_inum : 8;
    Ext4Inode jino;
    if (!readInode(jinum, &jino)) return false;
    memset(&jNode, 0, sizeof(jNode));
    jNode.inodeNum = jinum;
    jNode.mode = jino.i_mode;
    jNode.flags = jino.i_flags;
    jNode.size = static_cast<uint64_t>(jino.i_size_lo) | (static_cast<uint64_t>(jino.i_size_high) << 32);
    for (int i = 0; i < 15; ++i) jNode.iBlock[i] = jino.i_block[i];

    const uint64_t p0 = resolveBlock(&jNode, 0);
    if (p0 == 0) return false;
    jSbPhys = p0;

    uint8_t* jsb = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!jsb) return false;
    bool ok = readBlock(p0, jsb) && jbe32(jsb) == kJbd2Magic;
    if (ok) {
        const uint32_t jBlockSize = jbe32(jsb + 12);
        jMaxlen = jbe32(jsb + 16);
        jFirst = jbe32(jsb + 20);
        jSeqNext = jbe32(jsb + 24);
        const uint32_t jIncompat = jbe32(jsb + 40);
        const bool j64 = (jIncompat & 0x2) != 0;
        const bool jv3 = (jIncompat & 0x10) != 0;
        const bool jasync = (jIncompat & 0x4) != 0;
        memcpy(jUuid, jsb + 48, 16);
        // Only journal writes for the simple format we can emit exactly.
        if (jBlockSize == blockSize && !j64 && !jv3 && !jasync && jMaxlen > 4 && jFirst >= 1) {
            jWriteEnabled = true;
        }
    }
    kfree(jsb);
    return jWriteEnabled;
}

// The running transaction's blocks are found through a small open-addressing
// table (reads look there first, and a transaction holds up to kJTxnCap).
namespace {
inline uint32_t jHashSlot(uint64_t block, int size) {
    return static_cast<uint32_t>((block * 0x9E3779B97F4A7C15ull) >> 40) & static_cast<uint32_t>(size - 1);
}
}  // namespace

uint8_t* Ext4FS::jTxnFind(uint64_t block) {
    if (jCount == 0) return nullptr;
    for (uint32_t s = jHashSlot(block, kJHashSize);; s = (s + 1) & (kJHashSize - 1)) {
        const int16_t e = jHash[s];
        if (e == 0) return nullptr;
        if (jBlockNum[e - 1] == block) return jBlockBuf[e - 1];
    }
}

void Ext4FS::jTxnReset() {
    for (int i = 0; i < jCount; ++i) kfree(jBlockBuf[i]);
    jCount = 0;
    memset(jHash, 0, sizeof(jHash));
}

void Ext4FS::jTxnDrop(uint64_t block) {
    int found = -1;
    for (int i = 0; i < jCount; ++i) {
        if (jBlockNum[i] == block) { found = i; break; }
    }
    if (found < 0) return;
    kfree(jBlockBuf[found]);
    jBlockNum[found] = jBlockNum[jCount - 1];
    jBlockBuf[found] = jBlockBuf[jCount - 1];
    --jCount;
    // Rare (a block allocated and freed in one transaction): rebuild the index.
    memset(jHash, 0, sizeof(jHash));
    for (int i = 0; i < jCount; ++i) {
        uint32_t s = jHashSlot(jBlockNum[i], kJHashSize);
        while (jHash[s] != 0) s = (s + 1) & (kJHashSize - 1);
        jHash[s] = static_cast<int16_t>(i + 1);
    }
}

uint32_t Ext4FS::jTxnCapacity() const {
    // One descriptor block holds every tag: a 12-byte header, 8 bytes a tag,
    // and the UUID after the first. The log also needs the descriptor and the
    // commit block.
    uint32_t cap = kJTxnCap;
    const uint32_t tags = (blockSize - 12 - 16) / 8;
    if (tags < cap) cap = tags;
    if (jMaxlen > 3 && jMaxlen - 3 < cap) cap = jMaxlen - 3;
    return cap;
}

void Ext4FS::jOpen() {
    jActive = true;
    jOverflow = false;
    jOpenedMs = time_get_uptime_ms();
}

bool Ext4FS::jCommitDue() const {
    return time_get_uptime_ms() >= jOpenedMs + kCommitMs;
}

uint8_t* Ext4FS::jTxnBlock(uint64_t block, bool load) {
    if (uint8_t* existing = jTxnFind(block)) return existing;
    if (static_cast<uint32_t>(jCount) >= jTxnCapacity()) {
        // Full in the middle of an op (jBegin commits well before that, so only
        // a very large op gets here): commit what is there and go on in a new
        // transaction. That op is then not atomic across a crash.
        if (jCommitRunning() != 0) { jOverflow = true; return nullptr; }
        jOpen();
    }
    uint8_t* buf = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!buf) { jOverflow = true; return nullptr; }
    if (!load || !devReadBytes(block * static_cast<uint64_t>(blockSize), buf, blockSize)) {
        memset(buf, 0, blockSize);
    }
    jBlockNum[jCount] = block;
    jBlockBuf[jCount] = buf;
    uint32_t s = jHashSlot(block, kJHashSize);
    while (jHash[s] != 0) s = (s + 1) & (kJHashSize - 1);
    jHash[s] = static_cast<int16_t>(jCount + 1);
    ++jCount;
    return buf;
}

void Ext4FS::jBegin() {
    if (!jWriteEnabled) return;
    if (!jActive) {
        jTxnReset();
        jOpen();
        return;
    }
    // Leave room for this op: commit first when the transaction is filling up.
    // (A failed commit has written its blocks through; either way go on.)
    if (static_cast<uint32_t>(jCount) * 4 >= jTxnCapacity() * 3) {
        jCommitRunning();
        jOpen();
    }
}

int Ext4FS::jCommit() {
    if (!jActive) return 0;
    if (jSkipCheckpoint || jOverflow ||
        static_cast<uint32_t>(jCount) * 4 >= jTxnCapacity() * 3 || jCommitDue()) {
        return jCommitRunning();
    }
    return 0;
}

void Ext4FS::jAbort() {
    // The op's changes so far stay: they are consistent as far as they go (each
    // helper updates bitmap, descriptor and counts together, and the failure
    // paths free what they allocated), as without a journal.
    (void)jCommit();
}

bool Ext4FS::runAdd(uint64_t phys, const uint8_t* data) {
    if (!runBuf) {
        return devWriteBytes(phys * static_cast<uint64_t>(blockSize), data, blockSize);
    }
    const uint32_t cap = kRunBytes / blockSize;
    if (runCount != 0 && (phys != runStart + runCount || runCount == cap)) {
        if (!runFlush()) return false;
    }
    if (runCount == 0) runStart = phys;
    memcpy(runBuf + static_cast<uint64_t>(runCount) * blockSize, data, blockSize);
    ++runCount;
    return true;
}

bool Ext4FS::runFlush() {
    if (runCount == 0) return true;
    const bool ok = devWriteBytes(runStart * static_cast<uint64_t>(blockSize), runBuf,
                                  static_cast<uint64_t>(runCount) * blockSize);
    runCount = 0;
    return ok;
}

bool Ext4FS::setRecoverFlag(bool on) {
    // s_feature_incompat is at 1024 + 0x60, in the superblock's first sector:
    // rewrite that whole sector (one command; four bytes would be a read and a
    // write of it).
    uint8_t* sector = sectorBuf;
    if (!devReadBytes(kSuperblockOffset, sector, sizeof(sectorBuf))) return false;
    const uint8_t old = sector[0x60];
    sector[0x60] = on ? static_cast<uint8_t>(old | kIncompatRecover)
                      : static_cast<uint8_t>(old & ~kIncompatRecover);
    if (sector[0x60] == old) return true;
    return devWriteBytes(kSuperblockOffset, sector, sizeof(sectorBuf));
}

int Ext4FS::jCheckpoint() {
    // Write the buffered blocks to their final locations, in address order so
    // neighbours (inode table blocks, bitmaps) go out as one write, then reset
    // the journal.
    int16_t* order = jOrder;
    for (int i = 0; i < jCount; ++i) {
        int j = i;
        while (j > 0 && jBlockNum[order[j - 1]] > jBlockNum[i]) { order[j] = order[j - 1]; --j; }
        order[j] = static_cast<int16_t>(i);
    }
    bool ok = true;
    for (int k = 0; k < jCount; ++k) {
        ok = runAdd(jBlockNum[order[k]], jBlockBuf[order[k]]) && ok;
    }
    ok = runFlush() && ok;
    jTxnReset();

    uint8_t* jsb = static_cast<uint8_t*>(kmalloc(blockSize));
    if (jsb) {
        if (devReadBytes(jSbPhys * static_cast<uint64_t>(blockSize), jsb, blockSize)) {
            jwbe32(jsb + 28, 0);          // s_start = 0 (journal clean)
            jwbe32(jsb + 24, jSeqNext);   // s_sequence = next
            devWriteBytes(jSbPhys * static_cast<uint64_t>(blockSize), jsb, blockSize);
        }
        kfree(jsb);
    }
    setRecoverFlag(false);
    return ok ? 0 : -1;
}

int Ext4FS::jCommitRunning() {
    if (!jActive) return 0;
    jActive = false;   // subsequent I/O in this function is direct
    jOverflow = false;
    if (jCount == 0) return 0;   // nothing to journal
    ++jCommits;

    const uint32_t seq = jSeqNext;
    const uint32_t need = 1u + static_cast<uint32_t>(jCount) + 1u;   // desc + data + commit
    const bool tagsFit = (12u + 16u + static_cast<uint32_t>(jCount) * 8u) <= blockSize;
    if (jMaxlen == 0 || need >= jMaxlen || !tagsFit) {
        // Too large to journal in one transaction: write through directly.
        return jCheckpoint();
    }

    uint8_t* desc = static_cast<uint8_t*>(kmalloc(blockSize));
    uint8_t* tmp = static_cast<uint8_t*>(kmalloc(blockSize));
    if (!desc || !tmp) {
        if (desc) kfree(desc);
        if (tmp) kfree(tmp);
        return jCheckpoint();   // fall back
    }

    // Descriptor block: one tag per buffered block.
    memset(desc, 0, blockSize);
    jwbe32(desc + 0, kJbd2Magic);
    jwbe32(desc + 4, 1);      // descriptor
    jwbe32(desc + 8, seq);
    uint32_t off = 12;
    for (int i = 0; i < jCount; ++i) {
        const uint8_t* b = jBlockBuf[i];
        const bool escape = (b[0] == 0xC0 && b[1] == 0x3B && b[2] == 0x39 && b[3] == 0x98);
        uint16_t flags = 0;
        if (escape) flags |= kJbd2FlagEscape;
        if (i != 0) flags |= kJbd2FlagSameUuid;
        if (i == jCount - 1) flags |= kJbd2FlagLastTag;
        jwbe32(desc + off, static_cast<uint32_t>(jBlockNum[i]));
        jwbe16(desc + off + 4, 0);        // t_checksum (unused: no csum journal)
        jwbe16(desc + off + 6, flags);
        off += 8;
        if (i == 0) { memcpy(desc + off, jUuid, 16); off += 16; }
    }

    // The log: descriptor and data blocks, coalesced where the journal's blocks
    // are contiguous on disk (mke2fs makes them so).
    bool ok = runAdd(resolveBlock(&jNode, jFirst), desc);
    for (int i = 0; i < jCount && ok; ++i) {
        const uint64_t pj = resolveBlock(&jNode, jFirst + 1 + static_cast<uint32_t>(i));
        const uint8_t* b = jBlockBuf[i];
        if (b[0] == 0xC0 && b[1] == 0x3B && b[2] == 0x39 && b[3] == 0x98) {
            memcpy(tmp, b, blockSize);    // escaped copy
            tmp[0] = tmp[1] = tmp[2] = tmp[3] = 0;
            ok = runAdd(pj, tmp);
        } else {
            ok = runAdd(pj, b);
        }
    }
    ok = runFlush() && ok;

    // Commit block, after the rest of the log.
    if (ok) {
        memset(tmp, 0, blockSize);
        jwbe32(tmp + 0, kJbd2Magic);
        jwbe32(tmp + 4, 2);      // commit
        jwbe32(tmp + 8, seq);
        const uint64_t pcommit = resolveBlock(&jNode, jFirst + 1 + static_cast<uint32_t>(jCount));
        ok = devWriteBytes(pcommit * static_cast<uint64_t>(blockSize), tmp, blockSize);
    }
    kfree(desc);
    kfree(tmp);
    if (!ok) {
        return jCheckpoint();    // the log is incomplete: never mark it dirty
    }

    // Commit point: mark the journal dirty and set the fs RECOVER flag.
    uint8_t* jsb = static_cast<uint8_t*>(kmalloc(blockSize));
    if (jsb) {
        if (devReadBytes(jSbPhys * static_cast<uint64_t>(blockSize), jsb, blockSize)) {
            jwbe32(jsb + 28, jFirst);   // s_start
            jwbe32(jsb + 24, seq);      // s_sequence
            devWriteBytes(jSbPhys * static_cast<uint64_t>(blockSize), jsb, blockSize);
        }
        kfree(jsb);
    }
    setRecoverFlag(true);
    jSeqNext = seq + 1;

    if (jSkipCheckpoint) {
        // Simulate a crash after commit: leave the journal dirty for recovery.
        jTxnReset();
        return 0;
    }
    return jCheckpoint();
}
