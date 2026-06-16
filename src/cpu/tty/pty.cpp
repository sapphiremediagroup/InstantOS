#include <cpu/tty/pty.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/process/process.hpp>
#include <fs/vfs/vfs.hpp>
#include <memory/heap.hpp>
#include <common/string.hpp>
#include <graphics/console.hpp>

namespace {
constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollHup = 0x0010;
constexpr int16_t kPollNval = 0x0020;

void wakeTtyWaiters() {
    Scheduler::get().wakeAllBlockedProcesses();
}

bool blockCurrent() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return false;
    }
    current->setState(ProcessState::Blocked);
    Scheduler::get().scheduleFromSyscall();
    return !current->hasDeliverableSignal();
}

// Deliver a signal to every process in the foreground process group.
void signalForeground(uint32_t pgid, int sig) {
    if (pgid == 0) {
        return;
    }
    for (Process* p = Scheduler::get().getAllProcessesHead(); p; p = p->allNext) {
        // Without full pgid tracking we treat the leader PID as the group id.
        if (p->getPID() == pgid) {
            p->sendSignal(sig);
        }
    }
}

void defaultTermios(KernelTermios* t) {
    memset(t, 0, sizeof(*t));
    t->c_iflag = PTY_ICRNL | PTY_IXON;
    t->c_oflag = PTY_OPOST | PTY_ONLCR;
    t->c_cflag = 0000060 | 0000200;  // CS8 | CREAD
    t->c_lflag = PTY_ISIG | PTY_ICANON | PTY_ECHO | PTY_ECHOE | PTY_ECHOK;
    t->c_cc[PTY_VINTR] = 3;    // ^C
    t->c_cc[PTY_VQUIT] = 28;   // ^\.
    t->c_cc[PTY_VERASE] = 127; // DEL
    t->c_cc[PTY_VKILL] = 21;   // ^U
    t->c_cc[PTY_VEOF] = 4;     // ^D
    t->c_cc[PTY_VSUSP] = 26;   // ^Z
    t->c_cc[PTY_VMIN] = 1;
    t->c_cc[PTY_VTIME] = 0;
    t->c_cc[PTY_VEOL] = 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// PtyDevice
// ---------------------------------------------------------------------------

PtyDevice::PtyDevice(uint32_t deviceIndex)
    : index(deviceIndex),
      masterOpenCount(0),
      slaveOpenCount(0),
      foregroundPgid(0),
      controllingSession(0),
      lineLength(0),
      canonicalSegments(0) {
    defaultTermios(&termios);
    memset(&winsize, 0, sizeof(winsize));
    winsize.ws_row = 24;
    winsize.ws_col = 80;
    memset(&inputRing, 0, sizeof(inputRing));
    memset(&outputRing, 0, sizeof(outputRing));
    memset(lineBuffer, 0, sizeof(lineBuffer));
}

void PtyDevice::openMaster() { masterOpenCount++; }
void PtyDevice::openSlave() { slaveOpenCount++; }

void PtyDevice::closeMaster() {
    if (masterOpenCount > 0) {
        masterOpenCount--;
    }
    wakeTtyWaiters();
}

void PtyDevice::closeSlave() {
    if (slaveOpenCount > 0) {
        slaveOpenCount--;
    }
    wakeTtyWaiters();
}

bool PtyDevice::ringPush(PtyRing& ring, char c) {
    if (ring.size >= kPtyBufferSize) {
        return false;
    }
    ring.buffer[ring.head] = c;
    ring.head = (ring.head + 1) % kPtyBufferSize;
    ring.size++;
    return true;
}

int64_t PtyDevice::ringPop(PtyRing& ring, char* out, uint64_t max) {
    uint64_t copied = 0;
    while (copied < max && ring.size > 0) {
        out[copied++] = ring.buffer[ring.tail];
        ring.tail = (ring.tail + 1) % kPtyBufferSize;
        ring.size--;
    }
    return static_cast<int64_t>(copied);
}

// Echo a character to the output ring (visible on the master / terminal).
void PtyDevice::echoChar(char c) {
    if ((termios.c_lflag & PTY_ECHO) == 0) {
        return;
    }
    // Render control chars (except \n, \t) as ^X for readability.
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 32 && c != '\n' && c != '\t' && c != '\r') {
        outputProcessChar('^');
        outputProcessChar(static_cast<char>('@' + uc));
        return;
    }
    outputProcessChar(c);
}

// OPOST output processing (NL -> CRLF when ONLCR).
void PtyDevice::outputProcessChar(char c) {
    if ((termios.c_oflag & PTY_OPOST) != 0) {
        if (c == '\n' && (termios.c_oflag & PTY_ONLCR) != 0) {
            ringPush(outputRing, '\r');
            ringPush(outputRing, '\n');
            return;
        }
        if (c == '\r' && (termios.c_oflag & PTY_OCRNL) != 0) {
            ringPush(outputRing, '\n');
            return;
        }
    }
    ringPush(outputRing, c);
}

void PtyDevice::commitCanonicalLine(bool addTerminator, char terminator) {
    for (uint64_t i = 0; i < lineLength; i++) {
        ringPush(inputRing, lineBuffer[i]);
    }
    if (addTerminator) {
        ringPush(inputRing, terminator);
    }
    lineLength = 0;
    canonicalSegments++;
}

// Process one character coming from the master (a keystroke) through the line
// discipline and into the slave's input ring.
void PtyDevice::inputChar(char c) {
    // Input translation (c_iflag).
    if (c == '\r') {
        if (termios.c_iflag & PTY_IGNCR) {
            return;
        }
        if (termios.c_iflag & PTY_ICRNL) {
            c = '\n';
        }
    } else if (c == '\n' && (termios.c_iflag & PTY_INLCR)) {
        c = '\r';
    }
    if (termios.c_iflag & PTY_ISTRIP) {
        c = static_cast<char>(c & 0x7F);
    }

    // Signal generation (ISIG).
    if (termios.c_lflag & PTY_ISIG) {
        if (c == static_cast<char>(termios.c_cc[PTY_VINTR])) {
            signalForeground(foregroundPgid, SIGINT);
            echoChar(c);
            return;
        }
        if (c == static_cast<char>(termios.c_cc[PTY_VQUIT])) {
            signalForeground(foregroundPgid, SIGQUIT);
            echoChar(c);
            return;
        }
    }

    if (termios.c_lflag & PTY_ICANON) {
        // Canonical (cooked) mode.
        const char eof = static_cast<char>(termios.c_cc[PTY_VEOF]);
        const char erase = static_cast<char>(termios.c_cc[PTY_VERASE]);
        const char kill = static_cast<char>(termios.c_cc[PTY_VKILL]);

        if (c == erase) {
            if (lineLength > 0) {
                lineLength--;
                if (termios.c_lflag & PTY_ECHOE) {
                    outputProcessChar('\b');
                    outputProcessChar(' ');
                    outputProcessChar('\b');
                }
            }
            return;
        }
        if (c == kill) {
            while (lineLength > 0) {
                lineLength--;
                if (termios.c_lflag & PTY_ECHOE) {
                    outputProcessChar('\b');
                    outputProcessChar(' ');
                    outputProcessChar('\b');
                }
            }
            return;
        }
        if (c == eof) {
            // EOF: deliver whatever is buffered as a line with no newline; if
            // the line is empty this is a zero-length read (real EOF).
            echoChar(c);
            commitCanonicalLine(false, 0);
            wakeTtyWaiters();
            return;
        }
        if (c == '\n' || (termios.c_cc[PTY_VEOL] != 0 &&
                          c == static_cast<char>(termios.c_cc[PTY_VEOL]))) {
            if (lineLength < kPtyLineBufferSize) {
                lineBuffer[lineLength++] = c;
            }
            echoChar(c);
            commitCanonicalLine(false, 0);
            wakeTtyWaiters();
            return;
        }

        if (lineLength < kPtyLineBufferSize) {
            lineBuffer[lineLength++] = c;
            echoChar(c);
        }
        return;
    }

    // Raw mode: char goes straight to the slave input.
    ringPush(inputRing, c);
    echoChar(c);
    wakeTtyWaiters();
}

int64_t PtyDevice::masterWrite(const char* data, uint64_t size) {
    if (!data) {
        return -1;
    }
    for (uint64_t i = 0; i < size; i++) {
        inputChar(data[i]);
    }
    wakeTtyWaiters();
    return static_cast<int64_t>(size);
}

int64_t PtyDevice::masterRead(char* data, uint64_t size) {
    if (!data) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    while (outputRing.size == 0) {
        if (!slaveOpen()) {
            return 0;  // slave gone: EOF on the master.
        }
        if (!blockCurrent()) {
            return -1;
        }
    }
    int64_t n = ringPop(outputRing, data, size);
    wakeTtyWaiters();
    return n;
}

uint64_t PtyDevice::canonicalReadable() const {
    // In canonical mode a read may only consume up to the next committed line.
    return inputRing.size;
}

int64_t PtyDevice::slaveRead(char* data, uint64_t size) {
    if (!data) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    const bool canonical = (termios.c_lflag & PTY_ICANON) != 0;

    if (canonical) {
        // Wait until at least one line/EOF segment is available.
        while (canonicalSegments == 0) {
            if (!masterOpen()) {
                return 0;
            }
            if (!blockCurrent()) {
                return -1;
            }
        }
        // Pop bytes from inputRing up to and including the next '\n', bounded
        // by size. A segment with no newline (EOF) yields what's there.
        uint64_t copied = 0;
        while (copied < size && inputRing.size > 0) {
            char c = inputRing.buffer[inputRing.tail];
            inputRing.tail = (inputRing.tail + 1) % kPtyBufferSize;
            inputRing.size--;
            data[copied++] = c;
            if (c == '\n') {
                break;
            }
        }
        if (canonicalSegments > 0) {
            canonicalSegments--;
        }
        wakeTtyWaiters();
        return static_cast<int64_t>(copied);
    }

    // Raw mode: honour VMIN (treat VTIME as 0 for now).
    uint64_t vmin = termios.c_cc[PTY_VMIN];
    while (inputRing.size == 0) {
        if (!masterOpen()) {
            return 0;
        }
        if (vmin == 0) {
            return 0;  // non-blocking semantics
        }
        if (!blockCurrent()) {
            return -1;
        }
    }
    int64_t n = ringPop(inputRing, data, size);
    wakeTtyWaiters();
    return n;
}

int64_t PtyDevice::slaveWrite(const char* data, uint64_t size) {
    if (!data) {
        return -1;
    }
    for (uint64_t i = 0; i < size; i++) {
        // Block while the master output buffer is full and the master is alive.
        while (outputRing.size >= kPtyBufferSize) {
            if (!masterOpen()) {
                return static_cast<int64_t>(i);
            }
            if (!blockCurrent()) {
                return i > 0 ? static_cast<int64_t>(i) : -1;
            }
        }
        outputProcessChar(data[i]);
    }
    wakeTtyWaiters();
    return static_cast<int64_t>(size);
}

int16_t PtyDevice::pollMaster(int16_t events) const {
    int16_t revents = 0;
    if ((events & kPollIn) && (outputRing.size > 0 || !slaveOpen())) {
        revents |= kPollIn;
    }
    if ((events & kPollOut) && inputRing.size < kPtyBufferSize) {
        revents |= kPollOut;
    }
    if (!slaveOpen()) {
        revents |= kPollHup;
    }
    return revents;
}

int16_t PtyDevice::pollSlave(int16_t events) const {
    const bool canonical = (termios.c_lflag & PTY_ICANON) != 0;
    const bool readable = canonical ? (canonicalSegments > 0) : (inputRing.size > 0);
    int16_t revents = 0;
    if ((events & kPollIn) && (readable || !masterOpen())) {
        revents |= kPollIn;
    }
    if ((events & kPollOut) && outputRing.size < kPtyBufferSize) {
        revents |= kPollOut;
    }
    if (!masterOpen()) {
        revents |= kPollHup;
    }
    return revents;
}

void PtyDevice::getTermios(KernelTermios* out) const {
    if (out) {
        *out = termios;
    }
}

void PtyDevice::setTermios(const KernelTermios* in) {
    if (!in) {
        return;
    }
    termios = *in;
    if (termios.c_cc[PTY_VMIN] == 0 && termios.c_cc[PTY_VTIME] == 0) {
        // Avoid a busy non-blocking spin from raw readers that forgot VMIN.
        termios.c_cc[PTY_VMIN] = 1;
    }
    wakeTtyWaiters();
}

void PtyDevice::getWinsize(KernelWinsize* out) const {
    if (out) {
        *out = winsize;
    }
}

void PtyDevice::setWinsize(const KernelWinsize* in) {
    if (in) {
        winsize = *in;
        // A real kernel raises SIGWINCH to the foreground group here.
        signalForeground(foregroundPgid, 28 /* SIGWINCH */);
    }
}

// ---------------------------------------------------------------------------
// VNode glue
// ---------------------------------------------------------------------------

namespace {
struct PtyNodeData {
    PtyDevice* device;
    bool master;
};

PtyNodeData* nodeData(VNode* node) {
    return node ? reinterpret_cast<PtyNodeData*>(node->getData()) : nullptr;
}

int ptyMasterClose(VNode* node) {
    PtyNodeData* d = nodeData(node);
    if (d && d->device) {
        d->device->closeMaster();
        if (d->device->isOrphaned()) {
            PtyManager::get().release(d->device);
        }
    }
    delete d;
    if (node) {
        node->setData(nullptr);
    }
    return 0;
}

int64_t ptyMasterRead(VNode* node, void* buffer, uint64_t size, uint64_t) {
    PtyNodeData* d = nodeData(node);
    if (!d || !d->device) {
        return -1;
    }
    return d->device->masterRead(reinterpret_cast<char*>(buffer), size);
}

int64_t ptyMasterWrite(VNode* node, const void* buffer, uint64_t size, uint64_t) {
    PtyNodeData* d = nodeData(node);
    if (!d || !d->device) {
        return -1;
    }
    return d->device->masterWrite(reinterpret_cast<const char*>(buffer), size);
}

int ptyMasterStat(VNode*, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::CharDevice;
    stats->mode = 0020000 | 0600;
    stats->links = 1;
    return 0;
}

int ptySlaveClose(VNode* node) {
    PtyNodeData* d = nodeData(node);
    if (d && d->device) {
        d->device->closeSlave();
        if (d->device->isOrphaned()) {
            PtyManager::get().release(d->device);
        }
    }
    delete d;
    if (node) {
        node->setData(nullptr);
    }
    return 0;
}

int64_t ptySlaveRead(VNode* node, void* buffer, uint64_t size, uint64_t) {
    PtyNodeData* d = nodeData(node);
    if (!d || !d->device) {
        return -1;
    }
    return d->device->slaveRead(reinterpret_cast<char*>(buffer), size);
}

int64_t ptySlaveWrite(VNode* node, const void* buffer, uint64_t size, uint64_t) {
    PtyNodeData* d = nodeData(node);
    if (!d || !d->device) {
        return -1;
    }
    return d->device->slaveWrite(reinterpret_cast<const char*>(buffer), size);
}

int ptySlaveStat(VNode*, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::CharDevice;
    stats->mode = 0020000 | 0620;
    stats->links = 1;
    return 0;
}
}  // namespace

VNodeOps gPtyMasterOps {
    nullptr,            // open
    ptyMasterClose,
    ptyMasterRead,
    ptyMasterWrite,
    ptyMasterStat,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr
};

VNodeOps gPtySlaveOps {
    nullptr,
    ptySlaveClose,
    ptySlaveRead,
    ptySlaveWrite,
    ptySlaveStat,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr
};

PtyDevice* ptyDeviceFromMasterNode(VNode* node) {
    PtyNodeData* d = nodeData(node);
    return (d && d->master) ? d->device : nullptr;
}

PtyDevice* ptyDeviceFromSlaveNode(VNode* node) {
    PtyNodeData* d = nodeData(node);
    return (d && !d->master) ? d->device : nullptr;
}

// ---------------------------------------------------------------------------
// PtyManager
// ---------------------------------------------------------------------------

PtyManager& PtyManager::get() {
    static PtyManager instance;
    return instance;
}

PtyManager::PtyManager() {
    for (uint32_t i = 0; i < kMaxPtys; i++) {
        devices[i] = nullptr;
    }
}

PtyDevice* PtyManager::allocate() {
    for (uint32_t i = 0; i < kMaxPtys; i++) {
        if (devices[i] == nullptr) {
            PtyDevice* dev = new PtyDevice(i);
            if (!dev) {
                return nullptr;
            }
            devices[i] = dev;
            return dev;
        }
    }
    return nullptr;
}

PtyDevice* PtyManager::deviceForIndex(uint32_t index) {
    if (index >= kMaxPtys) {
        return nullptr;
    }
    return devices[index];
}

void PtyManager::release(PtyDevice* device) {
    if (!device) {
        return;
    }
    uint32_t idx = device->getIndex();
    if (idx < kMaxPtys && devices[idx] == device) {
        devices[idx] = nullptr;
    }
    delete device;
}

VNode* PtyManager::createMasterNode(PtyDevice* device) {
    if (!device) {
        return nullptr;
    }
    VNode* node = new VNode(nullptr, device->getIndex(), FileType::CharDevice);
    PtyNodeData* data = new PtyNodeData { device, true };
    if (!node || !data) {
        delete node;
        delete data;
        return nullptr;
    }
    node->refCount = 0;
    node->ops = &gPtyMasterOps;
    node->setData(data);
    device->openMaster();
    return node;
}

VNode* PtyManager::createSlaveNode(PtyDevice* device) {
    if (!device) {
        return nullptr;
    }
    VNode* node = new VNode(nullptr, device->getIndex(), FileType::CharDevice);
    PtyNodeData* data = new PtyNodeData { device, false };
    if (!node || !data) {
        delete node;
        delete data;
        return nullptr;
    }
    node->refCount = 0;
    node->ops = &gPtySlaveOps;
    node->setData(data);
    device->openSlave();
    return node;
}
