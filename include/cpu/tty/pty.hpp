#pragma once

#include <stdint.h>
#include <stddef.h>

class VNode;
struct VNodeOps;

// Linux-ABI termios. Must match outside/.../mlibc/abis/linux/termios.h and
// the userspace ilibcxx view so the same struct crosses the syscall boundary.
constexpr uint32_t kPtyNCCS = 32;

struct KernelTermios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[kPtyNCCS];
    uint32_t c_ibaud;
    uint32_t c_obaud;
};

struct KernelWinsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

// c_cc indices (Linux ABI)
enum {
    PTY_VINTR = 0,
    PTY_VQUIT = 1,
    PTY_VERASE = 2,
    PTY_VKILL = 3,
    PTY_VEOF = 4,
    PTY_VTIME = 5,
    PTY_VMIN = 6,
    PTY_VSTART = 8,
    PTY_VSTOP = 9,
    PTY_VSUSP = 10,
    PTY_VEOL = 11,
    PTY_VWERASE = 14,
};

// c_iflag
constexpr uint32_t PTY_ICRNL = 0000400;
constexpr uint32_t PTY_INLCR = 0000100;
constexpr uint32_t PTY_IGNCR = 0000200;
constexpr uint32_t PTY_IXON = 0002000;
constexpr uint32_t PTY_ISTRIP = 0000040;
// c_oflag
constexpr uint32_t PTY_OPOST = 0000001;
constexpr uint32_t PTY_ONLCR = 0000004;
constexpr uint32_t PTY_OCRNL = 0000010;
// c_lflag
constexpr uint32_t PTY_ISIG = 0000001;
constexpr uint32_t PTY_ICANON = 0000002;
constexpr uint32_t PTY_ECHO = 0000010;
constexpr uint32_t PTY_ECHOE = 0000020;
constexpr uint32_t PTY_ECHOK = 0000040;
constexpr uint32_t PTY_ECHONL = 0000100;

// ioctl request numbers (Linux values where possible)
constexpr uint64_t PTY_TCGETS = 0x5401;
constexpr uint64_t PTY_TCSETS = 0x5402;
constexpr uint64_t PTY_TCSETSW = 0x5403;
constexpr uint64_t PTY_TCSETSF = 0x5404;
constexpr uint64_t PTY_TIOCSCTTY = 0x540E;
constexpr uint64_t PTY_TIOCGPGRP = 0x540F;
constexpr uint64_t PTY_TIOCSPGRP = 0x5410;
constexpr uint64_t PTY_TIOCGWINSZ = 0x5413;
constexpr uint64_t PTY_TIOCSWINSZ = 0x5414;
constexpr uint64_t PTY_TIOCGPTN = 0x80045430;   // get pty number (master)
constexpr uint64_t PTY_TIOCSPTLCK = 0x40045431;  // lock/unlock (no-op)

constexpr uint64_t kPtyBufferSize = 8192;
constexpr uint64_t kPtyLineBufferSize = 4096;
constexpr uint32_t kMaxPtys = 64;

struct PtyRing {
    char buffer[kPtyBufferSize];
    uint64_t head;
    uint64_t tail;
    uint64_t size;
};

class PtyDevice {
public:
    PtyDevice(uint32_t index);

    uint32_t getIndex() const { return index; }

    // Reference / open-count management.
    void openMaster();
    void closeMaster();
    void openSlave();
    void closeSlave();
    bool masterOpen() const { return masterOpenCount > 0; }
    bool slaveOpen() const { return slaveOpenCount > 0; }
    bool isOrphaned() const { return masterOpenCount == 0 && slaveOpenCount == 0; }

    // Master endpoint: app writes keystrokes here (run through line discipline
    // into the slave-input ring); app reads processed slave output from here.
    int64_t masterWrite(const char* data, uint64_t size);
    int64_t masterRead(char* data, uint64_t size);

    // Slave endpoint: the child reads its stdin here, writes stdout/stderr here.
    int64_t slaveRead(char* data, uint64_t size);
    int64_t slaveWrite(const char* data, uint64_t size);

    int16_t pollMaster(int16_t events) const;
    int16_t pollSlave(int16_t events) const;

    // ioctl support.
    void getTermios(KernelTermios* out) const;
    void setTermios(const KernelTermios* in);
    void getWinsize(KernelWinsize* out) const;
    void setWinsize(const KernelWinsize* in);
    uint32_t getForegroundPgid() const { return foregroundPgid; }
    void setForegroundPgid(uint32_t pgid) { foregroundPgid = pgid; }
    uint32_t getSession() const { return controllingSession; }
    void setSession(uint32_t sid) { controllingSession = sid; }

private:
    // Line discipline helpers (operate on the master->slave direction).
    void inputChar(char c);
    void commitCanonicalLine(bool addToBuffer, char terminator);
    void echoChar(char c);                 // echo into output ring (master read side)
    void outputProcessChar(char c);        // OPOST processing into output ring
    uint64_t canonicalReadable() const;    // bytes available for a slave read

    bool ringPush(PtyRing& ring, char c);
    int64_t ringPop(PtyRing& ring, char* out, uint64_t max);
    uint64_t ringAvailable(const PtyRing& ring) const { return kPtyBufferSize - ring.size; }

    uint32_t index;
    uint32_t masterOpenCount;
    uint32_t slaveOpenCount;

    KernelTermios termios;
    KernelWinsize winsize;
    uint32_t foregroundPgid;
    uint32_t controllingSession;

    // master -> slave (slave's stdin) after line discipline
    PtyRing inputRing;
    // slave -> master (what the terminal displays)
    PtyRing outputRing;

    // canonical-mode line assembly (not yet committed to inputRing)
    char lineBuffer[kPtyLineBufferSize];
    uint64_t lineLength;
    // number of completed lines / EOFs sitting in inputRing that a canonical
    // read is allowed to return (canonical reads stop at line boundaries).
    uint64_t canonicalSegments;
};

class PtyManager {
public:
    PtyManager();
    static PtyManager& get();

    // Allocate a new master/slave pair. Returns the device or nullptr.
    PtyDevice* allocate();
    PtyDevice* deviceForIndex(uint32_t index);
    void release(PtyDevice* device);

    // Build the master and slave VNodes for a freshly allocated device.
    VNode* createMasterNode(PtyDevice* device);
    VNode* createSlaveNode(PtyDevice* device);

private:
    PtyDevice* devices[kMaxPtys];
};

// VNode op tables, exported so devfs / fs.cpp can identify pty nodes.
extern VNodeOps gPtyMasterOps;
extern VNodeOps gPtySlaveOps;

// Helpers used by sys_ioctl to recover the device from a VNode.
PtyDevice* ptyDeviceFromMasterNode(VNode* node);
PtyDevice* ptyDeviceFromSlaveNode(VNode* node);
