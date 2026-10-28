#pragma once

#include <fs/vfs/vfs.hpp>
#include <fs/block/blockdevice.hpp>
#include <stdint.h>
#include <stddef.h>

// ext2/3/4 on-disk structures and the read path driver (Ext4FS).
//
// Phase 1 is read-only: it mounts a volume, parses the superblock and block
// group descriptors, reads inodes, resolves file data blocks via BOTH the
// classic ext2 block map (12 direct + single/double/triple indirect) and ext4
// extent trees, and supports readdir/lookup/read/stat. Write operations are
// intentionally not wired yet.
//
// All multi-byte on-disk fields are little-endian, which matches the x86_64
// target, so fields are read directly without byte-swapping. Every on-disk
// struct is `packed` and guarded by static_asserts on size/offset so a layout
// mistake fails the build rather than corrupting reads.

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace ext4 {

static constexpr uint16_t kSuperMagic = 0xEF53;
static constexpr uint32_t kSuperblockOffset = 1024;   // bytes from FS start
static constexpr uint32_t kSuperblockSize = 1024;     // on-disk superblock size
static constexpr uint32_t kRootInode = 2;             // "/" is always inode 2
static constexpr uint32_t kFirstInode = 11;           // classic first non-reserved
static constexpr uint16_t kExtentMagic = 0xF30A;      // extent header eh_magic

// s_feature_incompat bits
static constexpr uint32_t kIncompatFiletype = 0x0002; // dir entries carry a type
static constexpr uint32_t kIncompatExtents = 0x0040;  // inodes may use extents
static constexpr uint32_t kIncompat64Bit = 0x0080;    // 64-bit block numbers
static constexpr uint32_t kIncompatFlexBg = 0x0200;   // flexible block groups
static constexpr uint32_t kIncompatInlineData = 0x8000;
static constexpr uint32_t kIncompatRecover = 0x0004;          // journal needs recovery
static constexpr uint32_t kFeatureCompatHasJournal = 0x0004;  // s_feature_compat: JBD2 present

// Incompat features this driver understands. A volume that requires any incompat
// bit outside this mask cannot be safely mounted. RECOVER is accepted because we
// replay the journal at mount (see Ext4FS::journalReplay).
static constexpr uint32_t kIncompatSupported =
    kIncompatFiletype | kIncompatExtents | kIncompat64Bit | kIncompatFlexBg | kIncompatRecover;

// s_feature_ro_compat bits that affect write consistency. When either is set we
// mount read-only rather than write without maintaining the on-disk checksums.
static constexpr uint32_t kRoCompatGdtCsum = 0x0010;      // uninit_bg (crc16 group desc)
static constexpr uint32_t kRoCompatMetadataCsum = 0x0400; // crc32c metadata checksums

static constexpr uint8_t kFtDirCsum = 0xDE;   // dir_entry_tail marker (metadata_csum)

// crc32c (Castagnoli, reflected poly 0x82F63B78), matching Linux's crc32c. Used
// for every metadata_csum checksum. This is a running value; callers manage the
// seed (e.g. ~0 for the superblock, or crc32c(~0, uuid) for per-group/inode).
inline uint32_t crc32c(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

// inode i_flags bits
static constexpr uint32_t kInodeIndexFl = 0x00001000;     // EXT4_INDEX_FL (htree directory)
static constexpr uint32_t kInodeExtentsFlag = 0x00080000; // EXT4_EXTENTS_FL
static constexpr uint32_t kInodeInlineDataFlag = 0x10000000;

// Directory entry file_type values (when kIncompatFiletype is set).
static constexpr uint8_t kFtUnknown = 0;
static constexpr uint8_t kFtRegFile = 1;
static constexpr uint8_t kFtDir = 2;
static constexpr uint8_t kFtChrdev = 3;
static constexpr uint8_t kFtBlkdev = 4;
static constexpr uint8_t kFtFifo = 5;
static constexpr uint8_t kFtSocket = 6;
static constexpr uint8_t kFtSymlink = 7;

// i_mode format bits (S_IFMT family).
static constexpr uint16_t kModeFmtMask = 0xF000;
static constexpr uint16_t kModeFifo = 0x1000;
static constexpr uint16_t kModeChr = 0x2000;
static constexpr uint16_t kModeDir = 0x4000;
static constexpr uint16_t kModeBlk = 0x6000;
static constexpr uint16_t kModeReg = 0x8000;
static constexpr uint16_t kModeLnk = 0xA000;
static constexpr uint16_t kModeSock = 0xC000;

} // namespace ext4

// ---------------------------------------------------------------------------
// On-disk structures
// ---------------------------------------------------------------------------

struct Ext4Superblock {
    uint32_t s_inodes_count;            // 0x000
    uint32_t s_blocks_count_lo;         // 0x004
    uint32_t s_r_blocks_count_lo;       // 0x008
    uint32_t s_free_blocks_count_lo;    // 0x00C
    uint32_t s_free_inodes_count;       // 0x010
    uint32_t s_first_data_block;        // 0x014
    uint32_t s_log_block_size;          // 0x018  block size = 1024 << this
    uint32_t s_log_cluster_size;        // 0x01C
    uint32_t s_blocks_per_group;        // 0x020
    uint32_t s_clusters_per_group;      // 0x024
    uint32_t s_inodes_per_group;        // 0x028
    uint32_t s_mtime;                   // 0x02C
    uint32_t s_wtime;                   // 0x030
    uint16_t s_mnt_count;               // 0x034
    uint16_t s_max_mnt_count;           // 0x036
    uint16_t s_magic;                   // 0x038  0xEF53
    uint16_t s_state;                   // 0x03A
    uint16_t s_errors;                  // 0x03C
    uint16_t s_minor_rev_level;         // 0x03E
    uint32_t s_lastcheck;               // 0x040
    uint32_t s_checkinterval;           // 0x044
    uint32_t s_creator_os;              // 0x048
    uint32_t s_rev_level;               // 0x04C
    uint16_t s_def_resuid;              // 0x050
    uint16_t s_def_resgid;              // 0x052
    // -- EXT4_DYNAMIC_REV superblock fields --
    uint32_t s_first_ino;               // 0x054
    uint16_t s_inode_size;              // 0x058
    uint16_t s_block_group_nr;          // 0x05A
    uint32_t s_feature_compat;          // 0x05C
    uint32_t s_feature_incompat;        // 0x060
    uint32_t s_feature_ro_compat;       // 0x064
    uint8_t  s_uuid[16];                // 0x068
    char     s_volume_name[16];         // 0x078
    char     s_last_mounted[64];        // 0x088
    uint32_t s_algorithm_usage_bitmap;  // 0x0C8
    uint8_t  s_prealloc_blocks;         // 0x0CC
    uint8_t  s_prealloc_dir_blocks;     // 0x0CD
    uint16_t s_reserved_gdt_blocks;     // 0x0CE
    uint8_t  s_journal_uuid[16];        // 0x0D0
    uint32_t s_journal_inum;            // 0x0E0
    uint32_t s_journal_dev;             // 0x0E4
    uint32_t s_last_orphan;             // 0x0E8
    uint32_t s_hash_seed[4];            // 0x0EC
    uint8_t  s_def_hash_version;        // 0x0FC
    uint8_t  s_jnl_backup_type;         // 0x0FD
    uint16_t s_desc_size;               // 0x0FE  group descriptor size (64bit)
    uint32_t s_default_mount_opts;      // 0x100
    uint32_t s_first_meta_bg;           // 0x104
    uint32_t s_mkfs_time;               // 0x108
    uint32_t s_jnl_blocks[17];          // 0x10C
    uint32_t s_blocks_count_hi;         // 0x150
    uint32_t s_r_blocks_count_hi;       // 0x154
    uint32_t s_free_blocks_count_hi;    // 0x158
    uint16_t s_min_extra_isize;         // 0x15C
    uint16_t s_want_extra_isize;        // 0x15E
    uint32_t s_flags;                   // 0x160
    uint16_t s_raid_stride;             // 0x164
    uint16_t s_mmp_interval;            // 0x166
    uint64_t s_mmp_block;               // 0x168
    uint32_t s_raid_stripe_width;       // 0x170
    uint8_t  s_log_groups_per_flex;     // 0x174
    uint8_t  s_checksum_type;           // 0x175
    uint16_t s_reserved_pad;            // 0x176
    uint64_t s_kbytes_written;          // 0x178
    uint32_t s_snapshot_inum;           // 0x180
    uint32_t s_snapshot_id;             // 0x184
    uint64_t s_snapshot_r_blocks_count; // 0x188
    uint32_t s_snapshot_list;           // 0x190
    uint32_t s_error_count;             // 0x194
    uint32_t s_first_error_time;        // 0x198
    uint32_t s_first_error_ino;         // 0x19C
    uint64_t s_first_error_block;       // 0x1A0
    uint8_t  s_first_error_func[32];    // 0x1A8
    uint32_t s_first_error_line;        // 0x1C8
    uint32_t s_last_error_time;         // 0x1CC
    uint32_t s_last_error_ino;          // 0x1D0
    uint32_t s_last_error_line;         // 0x1D4
    uint64_t s_last_error_block;        // 0x1D8
    uint8_t  s_last_error_func[32];     // 0x1E0
    uint8_t  s_mount_opts[64];          // 0x200
    uint32_t s_usr_quota_inum;          // 0x240
    uint32_t s_grp_quota_inum;          // 0x244
    uint32_t s_overhead_blocks;         // 0x248
    uint32_t s_backup_bgs[2];           // 0x24C
    uint8_t  s_encrypt_algos[4];        // 0x254
    uint8_t  s_encrypt_pw_salt[16];     // 0x258
    uint32_t s_lpf_ino;                 // 0x268
    uint32_t s_prj_quota_inum;          // 0x26C
    uint32_t s_checksum_seed;           // 0x270
    uint32_t s_reserved[98];            // 0x274 .. 0x3FC
    uint32_t s_checksum;                // 0x3FC
} __attribute__((packed));

static_assert(sizeof(Ext4Superblock) == 1024, "ext4 superblock must be 1024 bytes");
static_assert(offsetof(Ext4Superblock, s_magic) == 0x38, "s_magic offset");
static_assert(offsetof(Ext4Superblock, s_inode_size) == 0x58, "s_inode_size offset");
static_assert(offsetof(Ext4Superblock, s_feature_incompat) == 0x60, "s_feature_incompat offset");
static_assert(offsetof(Ext4Superblock, s_desc_size) == 0xFE, "s_desc_size offset");
static_assert(offsetof(Ext4Superblock, s_blocks_count_hi) == 0x150, "s_blocks_count_hi offset");

struct Ext4GroupDesc {
    uint32_t bg_block_bitmap_lo;        // 0x00
    uint32_t bg_inode_bitmap_lo;        // 0x04
    uint32_t bg_inode_table_lo;         // 0x08
    uint16_t bg_free_blocks_count_lo;   // 0x0C
    uint16_t bg_free_inodes_count_lo;   // 0x0E
    uint16_t bg_used_dirs_count_lo;     // 0x10
    uint16_t bg_flags;                  // 0x12
    uint32_t bg_exclude_bitmap_lo;      // 0x14
    uint16_t bg_block_bitmap_csum_lo;   // 0x18
    uint16_t bg_inode_bitmap_csum_lo;   // 0x1A
    uint16_t bg_itable_unused_lo;       // 0x1C
    uint16_t bg_checksum;               // 0x1E
    // 64-bit portion, present only when s_desc_size > 32.
    uint32_t bg_block_bitmap_hi;        // 0x20
    uint32_t bg_inode_bitmap_hi;        // 0x24
    uint32_t bg_inode_table_hi;         // 0x28
    uint16_t bg_free_blocks_count_hi;   // 0x2C
    uint16_t bg_free_inodes_count_hi;   // 0x2E
    uint16_t bg_used_dirs_count_hi;     // 0x30
    uint16_t bg_itable_unused_hi;       // 0x32
    uint32_t bg_exclude_bitmap_hi;      // 0x34
    uint16_t bg_block_bitmap_csum_hi;   // 0x38
    uint16_t bg_inode_bitmap_csum_hi;   // 0x3A
    uint32_t bg_reserved;               // 0x3C
} __attribute__((packed));

static_assert(sizeof(Ext4GroupDesc) == 64, "ext4 group descriptor (64bit) must be 64 bytes");
static_assert(offsetof(Ext4GroupDesc, bg_inode_table_lo) == 0x08, "bg_inode_table_lo offset");
static_assert(offsetof(Ext4GroupDesc, bg_inode_table_hi) == 0x28, "bg_inode_table_hi offset");

// Base (128-byte) inode. inode size on disk is s_inode_size (>= 128); every
// field the read path needs lives within the first 128 bytes, so reads copy
// only the base regardless of the on-disk inode size.
struct Ext4Inode {
    uint16_t i_mode;         // 0x00
    uint16_t i_uid;          // 0x02
    uint32_t i_size_lo;      // 0x04
    uint32_t i_atime;        // 0x08
    uint32_t i_ctime;        // 0x0C
    uint32_t i_mtime;        // 0x10
    uint32_t i_dtime;        // 0x14
    uint16_t i_gid;          // 0x18
    uint16_t i_links_count;  // 0x1A
    uint32_t i_blocks_lo;    // 0x1C
    uint32_t i_flags;        // 0x20
    uint32_t i_osd1;         // 0x24
    uint32_t i_block[15];    // 0x28  block map roots OR extent tree root (60 B)
    uint32_t i_generation;   // 0x64
    uint32_t i_file_acl_lo;  // 0x68
    uint32_t i_size_high;    // 0x6C  high 32 bits of size for regular files
    uint32_t i_obso_faddr;   // 0x70
    uint8_t  i_osd2[12];     // 0x74
} __attribute__((packed));

static_assert(sizeof(Ext4Inode) == 128, "base ext4 inode must be 128 bytes");
static_assert(offsetof(Ext4Inode, i_block) == 0x28, "i_block offset");
static_assert(offsetof(Ext4Inode, i_size_high) == 0x6C, "i_size_high offset");

// Extent tree structures (live inside i_block[] and in interior extent blocks).
struct Ext4ExtentHeader {
    uint16_t eh_magic;       // 0xF30A
    uint16_t eh_entries;     // number of valid entries following the header
    uint16_t eh_max;         // capacity
    uint16_t eh_depth;       // 0 = leaf (entries are Ext4Extent), >0 = index
    uint32_t eh_generation;
} __attribute__((packed));

struct Ext4ExtentIdx {       // interior node entry (eh_depth > 0)
    uint32_t ei_block;       // covers logical blocks >= ei_block
    uint32_t ei_leaf_lo;     // lower 32 bits of child block
    uint16_t ei_leaf_hi;     // upper 16 bits of child block
    uint16_t ei_unused;
} __attribute__((packed));

struct Ext4Extent {          // leaf entry (eh_depth == 0)
    uint32_t ee_block;       // first logical block this extent covers
    uint16_t ee_len;         // length in blocks (>32768 => uninitialized)
    uint16_t ee_start_hi;    // upper 16 bits of physical start block
    uint32_t ee_start_lo;    // lower 32 bits of physical start block
} __attribute__((packed));

static_assert(sizeof(Ext4ExtentHeader) == 12, "extent header must be 12 bytes");
static_assert(sizeof(Ext4ExtentIdx) == 12, "extent index must be 12 bytes");
static_assert(sizeof(Ext4Extent) == 12, "extent leaf must be 12 bytes");

// Linear directory entry (ext4_dir_entry_2). The name (name_len bytes) follows
// this 8-byte header. HTree-indexed directories still store their leaves as a
// sequence of these, so linear iteration reads them correctly.
struct Ext4DirEntry {
    uint32_t inode;          // 0x00  inode number, 0 => unused slot
    uint16_t rec_len;        // 0x04  distance to the next entry
    uint8_t  name_len;       // 0x06  name length in bytes
    uint8_t  file_type;      // 0x07  Ext4::kFt* (only if kIncompatFiletype)
    // char name[name_len] follows immediately.
} __attribute__((packed));

static_assert(sizeof(Ext4DirEntry) == 8, "ext4 dir entry header must be 8 bytes");

// ---------------------------------------------------------------------------
// In-memory per-vnode data
// ---------------------------------------------------------------------------

// Cached inode data attached to each VNode (via setData). Holds everything the
// read path needs so file reads and stat() don't re-fetch the inode, and keeps
// the raw i_block[] (extent tree root or block-map roots) for block resolution.
struct Ext4Node {
    uint32_t inodeNum;
    uint16_t mode;
    uint32_t flags;
    uint64_t size;
    uint16_t linksCount;
    uint32_t uid;
    uint32_t gid;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t blocksLo;       // i_blocks_lo: 512-byte sectors used (incl. indirect blocks)
    uint32_t generation;     // i_generation, needed for metadata_csum seeds
    uint32_t iBlock[15];     // raw copy of on-disk i_block[]
};

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

class Ext4FS : public FileSystem {
public:
    explicit Ext4FS(BlockDevice* device);
    ~Ext4FS() override;

    int mount(const char* path) override;
    int unmount() override;
    VNode* getRoot() override;
    bool isBlockBacked() const override { return true; }

    // Cheap probe: returns true if `device` starts with a valid ext2/3/4
    // superblock (magic + sane geometry). Used by boot-time FS detection.
    static bool probe(BlockDevice* device);

    // VNodeOps entry points (read-only in phase 1).
    static int nodeOpen(VNode* node, int flags);
    static int nodeClose(VNode* node);
    static int64_t nodeRead(VNode* node, void* buffer, uint64_t size, uint64_t offset);
    static int nodeStat(VNode* node, FileStats* stats);
    static int nodeReaddir(VNode* node, DirEntry* entries, uint64_t count, uint64_t* read);
    static VNode* nodeLookup(VNode* node, const char* name);
    static int64_t nodeReadlink(VNode* node, char* buffer, uint64_t size);
    static int nodeStatfs(VNode* node, FsStats* stats);

    // VNodeOps: write side (phase 3). These return -1 on a read-only volume
    // (metadata_csum/uninit_bg filesystems are mounted read-only so we never
    // have to recompute on-disk checksums). New files/dirs are created with the
    // classic block map (no extent-tree mutation).
    static int64_t nodeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t offset);
    static int nodeCreate(VNode* parent, const char* name, uint32_t mode, VNode** result);
    static int nodeMkdir(VNode* parent, const char* name, uint32_t mode, VNode** result);
    static int nodeUnlink(VNode* parent, const char* name);
    static int nodeRmdir(VNode* parent, const char* name);
    static int nodeTruncate(VNode* node, uint64_t size);
    static int nodeChmod(VNode* node, uint32_t mode);
    static int nodeChown(VNode* node, uint32_t uid, uint32_t gid);
    static int nodeUtime(VNode* node, uint64_t atime, uint64_t mtime);
    static int nodeFsync(VNode* node);
    static int nodeRename(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName);
    static int nodeLink(VNode* oldParent, const char* oldName, VNode* newParent, const char* newName);
    static int nodeSymlink(VNode* parent, const char* name, const char* target, VNode** result);

    bool isMounted() const { return mounted; }
    bool isWritable() const { return writable; }
    uint32_t getBlockSize() const { return blockSize; }
    // Accessors used by the host test harness to confirm which on-disk variant
    // (64-bit group descriptors, extent-mapped inodes) was actually exercised.
    bool is64Bit() const { return has64bit; }
    bool hasExtentFeature() const { return hasExtents; }
    uint16_t descriptorSize() const { return descSize; }

    // Resolve a logical block of an arbitrary inode to its physical block (0 on
    // hole/failure). Exposed for tooling that needs raw block locations (e.g.
    // the test harness locating journal blocks).
    bool resolveInodeBlock(uint32_t inodeNum, uint64_t logicalBlock, uint64_t* physOut);

    // --- write-side journaling transaction control (used by the write ops) ---
    void jBegin();     // open a transaction (no-op unless write-journaling is enabled)
    int  jCommit();    // journal + checkpoint the transaction (0 on success)
    void jAbort();     // discard the transaction (resync in-memory superblock)
    bool journalWriteEnabled() const { return jWriteEnabled; }
    // Test hook: when set, jCommit journals+commits but skips checkpointing,
    // simulating a crash after commit (leaves a dirty journal for recovery).
    void setJournalSkipCheckpoint(bool v) { jSkipCheckpoint = v; }

private:
    // --- block / structure I/O ---
    bool readBytes(uint64_t byteOffset, void* buffer, uint64_t size);
    bool readBlock(uint64_t block, void* buffer);
    bool readGroupDesc(uint32_t group, Ext4GroupDesc* out);
    bool readInode(uint32_t inodeNum, Ext4Inode* out);

    // --- logical -> physical block resolution ---
    // Returns 0 for a hole/failure (block 0 is never a valid data block).
    uint64_t resolveBlock(const Ext4Node* node, uint64_t logicalBlock);
    uint64_t resolveViaBlockMap(const uint32_t iBlock[15], uint64_t logicalBlock);
    uint64_t resolveViaExtents(const uint32_t iBlock[15], uint64_t logicalBlock);
    // Follow an indirect block map level. `blockOfPointers` is a physical block
    // full of uint32_t child pointers; `index` selects one.
    uint64_t readIndirectPointer(uint64_t blockOfPointers, uint32_t index);

    // --- vnode construction ---
    VNode* makeVNode(uint32_t inodeNum, const Ext4Inode* inode);
    static FileType modeToType(uint16_t mode);
    static FileType dirEntryType(uint8_t fileType);

    // --- directory iteration ---
    int readDirInto(VNode* dir, DirEntry* entries, uint64_t count, uint64_t* read);
    VNode* lookupInDir(VNode* dir, const char* name);

    // --- write helpers (phase 3) ---
    bool writeBytes(uint64_t byteOffset, const void* buffer, uint64_t size);
    bool writeBlock(uint64_t block, const void* buffer);
    bool writeInodeRaw(uint32_t inodeNum, const Ext4Inode* inode, bool zeroExtra);
    bool syncNodeInode(const Ext4Node* node);   // read-modify-write tracked fields
    bool flushSuperblock();
    bool flushGroupDesc(uint32_t group, const Ext4GroupDesc* gd);
    uint64_t groupBlockCount(uint32_t group) const;  // data blocks in a group
    uint64_t allocBlock();                 // physical block (zeroed) or 0
    uint32_t allocInode(bool isDir);       // inode number or 0
    void freeBlockNum(uint64_t block);
    void freeInodeNum(uint32_t inodeNum, bool isDir);
    bool bmapSet(Ext4Node* node, uint64_t logicalBlock, uint64_t physBlock);
    bool ensureIndirect(uint32_t* slot, Ext4Node* node);
    uint32_t readPointer(uint64_t block, uint32_t index);
    bool writePointer(uint64_t block, uint32_t index, uint32_t value);
    void freeInodeData(Ext4Node* node);    // free all data + indirect blocks
    void freeIndirect(uint64_t block, int levels);
    void bmapTruncate(Ext4Node* node, uint64_t keepBlocks);
    bool convertDirToLinear(VNode* dir);   // HTree -> linear so it can be modified
    int dirInsert(VNode* dir, const char* name, uint32_t inodeNum, uint8_t fileType);
    int dirRemove(VNode* dir, const char* name, uint32_t* removedInode);
    bool dirIsEmpty(Ext4Node* dirNode);
    VNode* createEntry(VNode* parent, const char* name, uint32_t mode, bool isDir,
                       const char* symlinkTarget);

    // --- metadata_csum (crc32c) helpers; no-ops when the feature is absent ---
    void superblockSetCsum();
    void groupDescSetCsum(uint32_t group, Ext4GroupDesc* gd);
    uint32_t inodeSeed(uint32_t inodeNum, uint32_t generation);
    void inodeSetCsum(uint32_t inodeNum, uint8_t* rawInode);   // rawInode is inodeSize bytes
    void dirBlockSetCsum(uint32_t dirInode, uint32_t generation, uint8_t* block);
    bool writeDirBlock(uint32_t dirInode, uint32_t generation, uint64_t phys, uint8_t* block);

    // --- JBD2 journal recovery (phase 4) ---
    int journalReplay();   // replay a dirty journal; returns 0 on success or clean
    uint32_t journalDescTagCount(const uint8_t* descBlock, bool csumV3, bool has64);

    // --- write-side journaling (phase 4b; simple/uncsummed journals only) ---
    bool devReadBytes(uint64_t byteOffset, void* buffer, uint64_t size);
    bool devWriteBytes(uint64_t byteOffset, const void* buffer, uint64_t size);

    // Write-through block cache. Every device access funnels through
    // devReadBytes/devWriteBytes, so caching there is automatically coherent
    // (writes update the cached copy and the device; reads populate). Keeps
    // directory/bitmap/inode-table scans off the disk at build scale.
    uint8_t* cacheSlotFor(uint64_t block);   // loaded block data, or nullptr
    void allocBlockCache();
    void freeBlockCache();

    uint8_t* jTxnFind(uint64_t block);    // buffered dirty block, or nullptr
    uint8_t* jTxnBlock(uint64_t block);   // find-or-load a dirty block (for writing)
    int jCheckpoint();                    // write dirty blocks to final locations
    bool journalLoadGeometry();           // cache journal layout at mount

    BlockDevice* device;
    VNode* rootNode;
    VNodeOps ops{};

    // Block cache storage (allocated at mount once blockSize is known).
    uint8_t* cacheData = nullptr;   // cacheSlots * blockSize bytes
    uint64_t* cacheTag = nullptr;   // owning block per slot (~0 == empty)
    uint32_t cacheSlots = 0;        // 0 disables caching (falls back to direct I/O)

    // Geometry derived from the superblock at mount time.
    Ext4Superblock sb;
    uint32_t blockSize;       // bytes per block (1024 << s_log_block_size)
    uint32_t inodeSize;       // s_inode_size (>= 128)
    uint32_t inodesPerGroup;
    uint32_t blocksPerGroup;
    uint32_t groupCount;
    uint32_t firstDataBlock;  // s_first_data_block (0 or 1)
    uint32_t csumSeed;        // metadata_csum seed = crc32c(~0, s_uuid)
    uint16_t descSize;        // group descriptor size (32 or 64)
    bool has64bit;
    bool hasExtents;
    bool hasFileType;
    bool hasMetadataCsum;
    bool writable;
    bool mounted;

    // write-side journaling state
    static constexpr int kJTxnCap = 256;   // max dirty blocks per transaction
    bool jWriteEnabled;      // journal present, writable, and a simple (uncsummed) format
    bool jActive;            // a transaction is currently open
    bool jSkipCheckpoint;    // test hook: commit but do not checkpoint
    bool jOverflow;          // transaction exceeded kJTxnCap
    int jCount;              // number of buffered dirty blocks
    uint64_t jBlockNum[kJTxnCap];
    uint8_t* jBlockBuf[kJTxnCap];
    Ext4Node jNode;          // journal inode (for resolving journal blocks)
    uint32_t jFirst;         // journal s_first (first log block)
    uint32_t jMaxlen;        // journal s_maxlen
    uint32_t jSeqNext;       // next transaction sequence number
    uint64_t jSbPhys;        // physical block of the journal superblock
    uint8_t  jUuid[16];      // journal s_uuid (for descriptor tags)
};
