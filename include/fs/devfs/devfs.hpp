#pragma once

#include <fs/vfs/vfs.hpp>

// Minimal in-memory device filesystem mounted at /dev.
//
// It exposes a tiny fixed set of device nodes:
//   /dev/ptmx     - the PTY master multiplexer (opened via sys_open special case)
//   /dev/pts      - directory holding allocated PTY slaves (/dev/pts/N)
//   /dev/tty      - controlling terminal alias (handled in sys_open)
//
// The actual PTY master/slave VNodes are created in the syscall layer
// (sys_open) so their lifetime is bound to the FileDescriptor. DevFS only
// needs to make the paths resolvable for stat()/readdir()/lookup().
class DevFS : public FileSystem {
public:
    DevFS();
    ~DevFS() override;

    int mount(const char* path) override;
    int unmount() override;
    VNode* getRoot() override;

    // Marker node kinds, stored as the VNode inode so sys_open can identify
    // what a resolved /dev node represents.
    enum DevKind : uint64_t {
        KindRoot = 0,
        KindPtmx = 1,
        KindPtsDir = 2,
        KindPtsSlave = 3,  // inode high bits carry the slave index
        KindTty = 4,
        KindNull = 5,
        KindZero = 6,
        KindUrandom = 7,
        KindRandom = 8,
    };

    static constexpr uint64_t kPtsSlaveInodeBase = 0x1000;

    VNode* getPtmxNode() { return ptmxNode; }
    VNode* getPtsDirNode() { return ptsDirNode; }
    VNode* getTtyNode() { return ttyNode; }
    VNode* getNullNode() { return nullNode; }
    VNode* getZeroNode() { return zeroNode; }
    VNode* getUrandomNode() { return urandomNode; }
    VNode* getRandomNode() { return randomNode; }

    // Returns a cached marker VNode for /dev/pts/<index>, or nullptr if no such
    // pty exists. The marker's inode encodes the slave index.
    VNode* ptsSlaveMarker(uint32_t index);

    VNodeOps* charOps() { return &ptmxOps; }

private:
    VNode* rootNode;
    VNode* ptmxNode;
    VNode* ptsDirNode;
    VNode* ttyNode;
    VNode* nullNode;
    VNode* zeroNode;
    VNode* urandomNode;
    VNode* randomNode;
    VNode* ptsSlaveNodes[64];
    VNodeOps rootOps;
    VNodeOps ptmxOps;
    VNodeOps ptsDirOps;
    VNodeOps nullOps;    // /dev/null (read EOF, write discard)
    VNodeOps zeroOps;    // /dev/zero (read zeros, write discard)
    VNodeOps randomOps;  // /dev/urandom, /dev/random (entropy read)
};
