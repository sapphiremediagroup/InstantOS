#include <cpu/syscall/syscall.hpp>
#include <cpu/cereal/cereal.hpp>
#include <cpu/process/scheduler.hpp>
#include <time/tsc_timer.hpp>
#include <cpu/tty/pty.hpp>
#include <drivers/usb/ohci.hpp>
#include <fs/vfs/vfs.hpp>
#include <graphics/console.hpp>
#include <interrupts/keyboard.hpp>
#include <ipc/ipc.hpp>
#include <common/string.hpp>

extern int16_t pollSocketHandle(void* object, uint32_t rights, int16_t events);

namespace {
constexpr uint64_t kOpenAccessModeMask = 0x3;
constexpr uint64_t kOpenWriteOnly = 0x1;
constexpr uint64_t kOpenReadWrite = 0x2;
constexpr uint64_t kOpenCloseOnExec = 02000000;
constexpr uint64_t kPipeBufferSize = 4096;
constexpr uint64_t kFcntlGetFD = 1;
constexpr uint64_t kFcntlSetFD = 2;
constexpr uint64_t kFdCloseOnExec = 1;
constexpr uint64_t kFcntlDupFD = 0;
constexpr uint64_t kFcntlGetFL = 3;
constexpr uint64_t kFcntlSetFL = 4;
constexpr uint64_t kFcntlDupFDCloExec = 1030;
constexpr uint64_t kOpenCreate = 0100;
constexpr uint64_t kOpenExclusive = 0200;
constexpr uint64_t kOpenTruncate = 01000;
constexpr uint64_t kOpenAppend = 02000;
constexpr uint64_t kOpenNonBlock = 04000;
constexpr uint64_t kOpenDirectory = 0200000;
constexpr uint64_t kOpenNoFollow = 0400000;
constexpr uint64_t kOpenSupportedFlags = kOpenAccessModeMask | kOpenCreate | kOpenExclusive |
    kOpenTruncate | kOpenAppend | kOpenNonBlock | kOpenDirectory | kOpenNoFollow | kOpenCloseOnExec;
constexpr uint32_t kModeReadBits = 0444;
constexpr uint32_t kModeWriteBits = 0222;
constexpr uint32_t kModeExecBits = 0111;
constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollErr = 0x0008;
constexpr int16_t kPollHup = 0x0010;
constexpr int16_t kPollNval = 0x0020;

enum class PathCopyStatus {
    Ok,
    Invalid,
    Empty,
    TooLong,
};

struct PipeObject {
    char buffer[kPipeBufferSize];
    uint64_t head;
    uint64_t tail;
    uint64_t size;
    uint32_t refs;
    uint32_t readOpen;
    uint32_t writeOpen;
};

struct PipeEndpoint {
    PipeObject* pipe;
    bool writeEnd;
};

// Named-FIFO registry: all opens of the same on-disk FIFO (identified by its
// filesystem + inode) share one PipeObject ring buffer, so a writer and reader
// that open the same path actually communicate. Anonymous pipes do not use this
// table (they create their own PipeObject directly).
struct FifoEntry {
    FileSystem* fs;
    uint64_t inode;
    PipeObject* pipe;
};
constexpr int kMaxFifos = 32;
FifoEntry gFifos[kMaxFifos] = {};

PipeObject* fifoLookup(FileSystem* fs, uint64_t inode) {
    for (int i = 0; i < kMaxFifos; ++i) {
        if (gFifos[i].pipe && gFifos[i].fs == fs && gFifos[i].inode == inode) {
            return gFifos[i].pipe;
        }
    }
    return nullptr;
}

PipeObject* fifoGetOrCreate(FileSystem* fs, uint64_t inode) {
    PipeObject* existing = fifoLookup(fs, inode);
    if (existing) return existing;
    for (int i = 0; i < kMaxFifos; ++i) {
        if (!gFifos[i].pipe) {
            PipeObject* pipe = new PipeObject();
            if (!pipe) return nullptr;
            pipe->head = pipe->tail = pipe->size = 0;
            pipe->refs = 0;
            pipe->readOpen = 0;
            pipe->writeOpen = 0;
            gFifos[i].fs = fs;
            gFifos[i].inode = inode;
            gFifos[i].pipe = pipe;
            return pipe;
        }
    }
    return nullptr;
}

void fifoForget(PipeObject* pipe) {
    for (int i = 0; i < kMaxFifos; ++i) {
        if (gFifos[i].pipe == pipe) {
            gFifos[i].pipe = nullptr;
            gFifos[i].fs = nullptr;
            gFifos[i].inode = 0;
        }
    }
}

void traceStr(const char* text) {
    Cereal::get().write(text);
}

void traceDec(uint64_t value) {
    char buf[21];
    int pos = 0;
    if (value == 0) {
        Cereal::get().write('0');
        return;
    }

    while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
        buf[pos++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        Cereal::get().write(buf[--pos]);
    }
}

void tracePrintableChar(char c) {
    if (c == '\n') {
        Cereal::get().write("\\n");
    } else if (c == '\r') {
        Cereal::get().write("\\r");
    } else if (c == '\b') {
        Cereal::get().write("\\b");
    } else if (c == '\t') {
        Cereal::get().write("\\t");
    } else {
        Cereal::get().write(c);
    }
}

bool appendPathChar(char* out, uint64_t& len, char c) {
    if (len >= 255) {
        return false;
    }
    out[len++] = c;
    out[len] = '\0';
    return true;
}

bool appendPathSegment(char* out, uint64_t& len, const char* segment, uint64_t segmentLen) {
    if (segmentLen == 0 || (segmentLen == 1 && segment[0] == '.')) {
        return true;
    }
    if (segmentLen == 2 && segment[0] == '.' && segment[1] == '.') {
        if (len > 1) {
            if (out[len - 1] == '/') {
                len--;
            }
            while (len > 1 && out[len - 1] != '/') {
                len--;
            }
            if (len > 1 && out[len - 1] == '/') {
                len--;
            }
            out[len] = '\0';
        }
        return true;
    }

    if (len > 1 && !appendPathChar(out, len, '/')) {
        return false;
    }
    for (uint64_t i = 0; i < segmentLen; ++i) {
        if (!appendPathChar(out, len, segment[i])) {
            return false;
        }
    }
    return true;
}

bool normalizeAbsolutePath(const char* input, char* out) {
    if (!input || input[0] != '/') {
        return false;
    }

    uint64_t outLen = 0;
    if (!appendPathChar(out, outLen, '/')) {
        return false;
    }

    uint64_t index = 1;
    while (input[index]) {
        while (input[index] == '/') {
            index++;
        }
        const uint64_t start = index;
        while (input[index] && input[index] != '/') {
            index++;
        }
        if (!appendPathSegment(out, outLen, input + start, index - start)) {
            return false;
        }
    }

    if (outLen == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return true;
}

bool rawPathRequiresDirectory(const char* path) {
    if (!path) {
        return false;
    }

    uint64_t len = 0;
    bool sawNonSlash = false;
    while (path[len]) {
        if (path[len] != '/') {
            sawNonSlash = true;
        }
        len++;
    }
    return sawNonSlash && len > 0 && path[len - 1] == '/';
}

bool dirEntryIsDotOrDotDot(const ::DirEntry& entry) {
    return (entry.name[0] == '.' && entry.name[1] == '\0') ||
           (entry.name[0] == '.' && entry.name[1] == '.' && entry.name[2] == '\0');
}

bool parentPathOf(const char* path, char* parent) {
    if (!path || path[0] != '/') {
        return false;
    }

    uint64_t len = 0;
    while (path[len]) {
        len++;
    }
    if (len == 0) {
        return false;
    }

    uint64_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') {
        slash--;
    }
    if (slash == 0) {
        return false;
    }
    if (slash == 1) {
        parent[0] = '/';
        parent[1] = '\0';
        return true;
    }

    for (uint64_t i = 0; i < slash - 1; ++i) {
        parent[i] = path[i];
    }
    parent[slash - 1] = '\0';
    return true;
}

bool pathParentIsNonDirectory(const char* path) {
    char parent[256];
    if (!parentPathOf(path, parent)) {
        return false;
    }

    FileStats parentStats {};
    return VFS::get().stat(parent, &parentStats) == 0 && parentStats.type != FileType::Directory;
}

bool pathParentDeniesSearch(const char* path) {
    char parent[256];
    if (!parentPathOf(path, parent)) {
        return false;
    }

    FileStats parentStats {};
    return VFS::get().stat(parent, &parentStats) == 0 &&
           parentStats.type == FileType::Directory &&
           (parentStats.mode & kModeExecBits) == 0;
}

bool pathParentDeniesWrite(const char* path) {
    char parent[256];
    if (!parentPathOf(path, parent)) {
        return false;
    }

    FileStats parentStats {};
    return VFS::get().stat(parent, &parentStats) == 0 &&
           parentStats.type == FileType::Directory &&
           (parentStats.mode & kModeWriteBits) == 0;
}

uint64_t missingPathError(const char* path) {
    if (VFS::get().getLastError() == SysErrLoop) {
        return syscall_error(SysErrLoop);
    }
    return syscall_error(pathParentIsNonDirectory(path) ? SysErrNotDirectory : SysErrNoEntry);
}

bool trailingSlashRejectsNonDirectory(const char* path, bool requiresDirectory) {
    if (!requiresDirectory) {
        return false;
    }

    FileStats stats {};
    return VFS::get().stat(path, &stats) == 0 && stats.type != FileType::Directory;
}

bool makeAbsolutePath(const char* path, char* out) {
    if (!path || path[0] == '\0') {
        return false;
    }

    char combined[512];
    uint64_t len = 0;
    if (path[0] == '/') {
        while (path[len] && len < sizeof(combined) - 1) {
            combined[len] = path[len];
            len++;
        }
        if (path[len]) {
            return false;
        }
        combined[len] = '\0';
        return normalizeAbsolutePath(combined, out);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    const char* cwd = current ? current->getCwd() : "/";
    while (cwd[len] && len < sizeof(combined) - 1) {
        combined[len] = cwd[len];
        len++;
    }
    if (cwd[len]) {
        return false;
    }
    if (len == 0 || combined[len - 1] != '/') {
        if (len >= sizeof(combined) - 1) {
            return false;
        }
        combined[len++] = '/';
    }
    for (uint64_t i = 0; path[i]; ++i) {
        if (len >= sizeof(combined) - 1) {
            return false;
        }
        combined[len++] = path[i];
    }
    combined[len] = '\0';
    return normalizeAbsolutePath(combined, out);
}

PathCopyStatus copyUserPathDetailed(uint64_t path, char* pathname, bool* requiresDirectory = nullptr) {
    if (requiresDirectory) {
        *requiresDirectory = false;
    }
    if (!Syscall::isValidUserPointer(path, 1)) {
        return PathCopyStatus::Invalid;
    }

    char raw[256];
    for (uint64_t i = 0; i < sizeof(raw); ++i) {
        char c = '\0';
        if (!Syscall::copyFromUser(&c, path + i, 1)) {
            return PathCopyStatus::Invalid;
        }
        raw[i] = c;
        if (c == '\0') {
            if (i == 0) {
                return PathCopyStatus::Empty;
            }
            if (requiresDirectory) {
                *requiresDirectory = rawPathRequiresDirectory(raw);
            }
            return makeAbsolutePath(raw, pathname) ? PathCopyStatus::Ok : PathCopyStatus::TooLong;
        }
    }
    return PathCopyStatus::TooLong;
}

uint64_t pathCopyError(PathCopyStatus status) {
    return syscall_error(status == PathCopyStatus::TooLong ? SysErrNameTooLong : SysErrInvalid);
}

bool copyUserPathOrError(uint64_t path, char* pathname, uint64_t& error, bool* requiresDirectory = nullptr) {
    const PathCopyStatus status = copyUserPathDetailed(path, pathname, requiresDirectory);
    if (status == PathCopyStatus::Ok) {
        return true;
    }
    error = pathCopyError(status);
    return false;
}

uint32_t fileRightsFromOpenFlags(uint64_t flags) {
    uint32_t rights = HandleRightDuplicate;

    switch (flags & kOpenAccessModeMask) {
        case kOpenWriteOnly:
            rights |= HandleRightWrite;
            break;
        case kOpenReadWrite:
            rights |= HandleRightRead | HandleRightWrite;
            break;
        default:
            rights |= HandleRightRead;
            break;
    }

    return rights;
}

bool openFlagsAllowedByMode(uint64_t flags, uint32_t mode) {
    const bool truncates = (flags & kOpenTruncate) != 0;
    switch (flags & kOpenAccessModeMask) {
        case kOpenWriteOnly:
            return (mode & kModeWriteBits) != 0;
        case kOpenReadWrite:
            return (mode & kModeReadBits) != 0 && (mode & kModeWriteBits) != 0;
        default:
            return (mode & kModeReadBits) != 0 && (!truncates || (mode & kModeWriteBits) != 0);
    }
}

void wakePipeWaiters() {
    Scheduler::get().wakeAllBlockedProcesses();
}

bool blockCurrentForPipe() {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return false;
    }

    current->setState(ProcessState::Blocked);
    Scheduler::get().scheduleFromSyscall();
    return true;
}

PipeEndpoint* pipeEndpoint(VNode* node) {
    return node ? reinterpret_cast<PipeEndpoint*>(node->getData()) : nullptr;
}

int16_t pollPipe(PipeEndpoint* endpoint, int16_t events) {
    if (!endpoint || !endpoint->pipe) {
        return kPollNval;
    }

    PipeObject* pipe = endpoint->pipe;
    int16_t revents = 0;
    if (endpoint->writeEnd) {
        if (pipe->readOpen == 0) {
            revents |= kPollErr;
        } else if ((events & kPollOut) && pipe->size < kPipeBufferSize) {
            revents |= kPollOut;
        }
    } else {
        if ((events & kPollIn) && (pipe->size > 0 || pipe->writeOpen == 0)) {
            revents |= kPollIn;
        }
        if (pipe->writeOpen == 0) {
            revents |= kPollHup;
        }
    }
    return revents;
}

int16_t pollMessageQueue(MessageQueueObject* queue, const HandleEntry& entry, int16_t events) {
    if (!queue) {
        return kPollNval;
    }

    int16_t revents = 0;
    if ((events & kPollIn) && (entry.rights & HandleRightRead) && queue->hasMessages()) {
        revents |= kPollIn;
    }
    if ((events & kPollOut) && (entry.rights & HandleRightWrite) &&
        queue->pendingCount() < MessageQueueObject::MaxMessages) {
        revents |= kPollOut;
    }
    return revents;
}

int16_t pollHandle(Process* current, const PollFD& fd) {
    if (fd.fd < 0) {
        return 0;
    }

    uint64_t resolved = static_cast<uint64_t>(fd.fd);
    bool boundStdio = false;
    if ((fd.fd == 0 || fd.fd == 1 || fd.fd == 2) && current) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, fd.fd);
        if (current->getHandle(encoded) != nullptr) {
            resolved = encoded;
            boundStdio = true;
        }
    }

    if (!boundStdio && fd.fd == 0) {
        USBInput::get().poll();
        Keyboard::get().servicePendingInput();
        return (fd.events & kPollIn) && Keyboard::get().hasKey() ? kPollIn : 0;
    }
    if (!boundStdio && (fd.fd == 1 || fd.fd == 2)) {
        return (fd.events & kPollOut) ? kPollOut : 0;
    }

    if (!current) {
        return kPollNval;
    }

    auto* entry = current->getHandle(resolved);
    if (!entry) {
        return kPollNval;
    }

    if (entry->type == HandleType::EventQueue) {
        return pollMessageQueue(
            reinterpret_cast<MessageQueueObject*>(entry->object),
            *entry,
            fd.events
        );
    }
    if (entry->type == HandleType::Service) {
        auto* service = reinterpret_cast<ServiceObject*>(entry->object);
        return pollMessageQueue(service ? service->getQueue() : nullptr, *entry, fd.events);
    }
    if (entry->type == HandleType::Socket) {
        return pollSocketHandle(entry->object, entry->rights, fd.events);
    }
    if (entry->type != HandleType::File) {
        return kPollNval;
    }

    auto* fileFd = reinterpret_cast<FileDescriptor*>(entry->object);
    VNode* node = fileFd ? fileFd->getNode() : nullptr;
    if (!node) {
        return kPollNval;
    }

    if (node->getType() == FileType::Pipe) {
        return pollPipe(pipeEndpoint(node), fd.events);
    }

    if (node->getType() == FileType::CharDevice) {
        if (PtyDevice* m = ptyDeviceFromMasterNode(node)) {
            return m->pollMaster(fd.events);
        }
        if (PtyDevice* s = ptyDeviceFromSlaveNode(node)) {
            return s->pollSlave(fd.events);
        }
    }

    int16_t revents = 0;
    if ((fd.events & kPollIn) && (entry->rights & HandleRightRead)) {
        revents |= kPollIn;
    }
    if ((fd.events & kPollOut) && (entry->rights & HandleRightWrite)) {
        revents |= kPollOut;
    }
    return revents;
}

int pipeClose(VNode* node) {
    PipeEndpoint* endpoint = pipeEndpoint(node);
    if (!endpoint) {
        return 0;
    }

    PipeObject* pipe = endpoint->pipe;
    if (pipe) {
        if (endpoint->writeEnd) {
            if (pipe->writeOpen > 0) {
                pipe->writeOpen--;
            }
        } else if (pipe->readOpen > 0) {
            pipe->readOpen--;
        }

        if (pipe->refs > 0) {
            pipe->refs--;
        }
        if (pipe->refs == 0) {
            fifoForget(pipe);  // no-op for anonymous pipes (not registered)
            delete pipe;
        }
    }

    delete endpoint;
    node->setData(nullptr);
    wakePipeWaiters();
    return 0;
}

int64_t pipeRead(VNode* node, void* buffer, uint64_t size, uint64_t) {
    PipeEndpoint* endpoint = pipeEndpoint(node);
    if (!endpoint || endpoint->writeEnd || !buffer) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    PipeObject* pipe = endpoint->pipe;
    if (!pipe) {
        return -1;
    }

    while (pipe->size == 0) {
        if (pipe->writeOpen == 0) {
            return 0;
        }
        if (!blockCurrentForPipe()) {
            return -1;
        }
    }

    uint64_t copied = 0;
    char* out = reinterpret_cast<char*>(buffer);
    while (copied < size && pipe->size > 0) {
        out[copied++] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % kPipeBufferSize;
        pipe->size--;
    }

    wakePipeWaiters();
    return static_cast<int64_t>(copied);
}

int64_t pipeWrite(VNode* node, const void* buffer, uint64_t size, uint64_t) {
    PipeEndpoint* endpoint = pipeEndpoint(node);
    if (!endpoint || !endpoint->writeEnd || !buffer) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    PipeObject* pipe = endpoint->pipe;
    if (!pipe) {
        return -1;
    }

    uint64_t written = 0;
    const char* in = reinterpret_cast<const char*>(buffer);
    while (written < size) {
        if (pipe->readOpen == 0) {
            return written > 0 ? static_cast<int64_t>(written) : -1;
        }

        while (pipe->size == kPipeBufferSize) {
            if (pipe->readOpen == 0) {
                return written > 0 ? static_cast<int64_t>(written) : -1;
            }
            if (!blockCurrentForPipe()) {
                return written > 0 ? static_cast<int64_t>(written) : -1;
            }
        }

        while (written < size && pipe->size < kPipeBufferSize) {
            pipe->buffer[pipe->head] = in[written++];
            pipe->head = (pipe->head + 1) % kPipeBufferSize;
            pipe->size++;
        }
        wakePipeWaiters();
    }

    return static_cast<int64_t>(written);
}

int pipeStat(VNode*, FileStats* stats) {
    if (!stats) {
        return -1;
    }
    memset(stats, 0, sizeof(*stats));
    stats->type = FileType::Pipe;
    stats->mode = 0010000 | 0600;
    stats->links = 1;
    return 0;
}

bool copyStatToUser(const FileStats& fileStats, uint64_t statbuf) {
    Stat stat {};
    stat.st_dev = fileStats.dev;
    stat.st_ino = fileStats.inode;
    stat.st_mode = fileStats.mode;
    stat.st_nlink = fileStats.links;
    stat.st_uid = fileStats.uid;
    stat.st_gid = fileStats.gid;
    stat.st_rdev = fileStats.rdev;
    stat.st_size = fileStats.size;
    stat.st_blksize = 4096;
    stat.st_blocks = (fileStats.size + 511) / 512;
    stat.st_atime = fileStats.atime;
    stat.st_mtime = fileStats.mtime;
    stat.st_ctime = fileStats.ctime;

    if (fileStats.type == FileType::Directory) {
        stat.st_mode |= 0040000;
    } else if (fileStats.type == FileType::CharDevice) {
        stat.st_mode |= 0020000;
    } else if (fileStats.type == FileType::BlockDevice) {
        stat.st_mode |= 0060000;
    } else if (fileStats.type == FileType::Pipe) {
        stat.st_mode |= 0010000;
    } else if (fileStats.type == FileType::Symlink) {
        stat.st_mode |= 0120000;
    } else {
        stat.st_mode |= 0100000;
    }

    return Syscall::copyToUser(statbuf, &stat, sizeof(Stat));
}

VNodeOps pipeOps {
    nullptr,
    pipeClose,
    pipeRead,
    pipeWrite,
    pipeStat,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

// ---- PTY open helpers -----------------------------------------------------

bool pathEquals(const char* a, const char* b) {
    int i = 0;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return true;
}

// Match "/dev/pts/<n>" and return the index, or false.
bool matchPtsPath(const char* path, uint32_t* index) {
    const char* prefix = "/dev/pts/";
    int i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    if (path[i] == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (; path[i]; i++) {
        if (path[i] < '0' || path[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(path[i] - '0');
        if (value >= kMaxPtys) {
            return false;
        }
    }
    *index = value;
    return true;
}

// Allocate a fresh PTY pair, returning the master fd handle in the calling
// process (used for /dev/ptmx).
uint64_t openPtyMaster(Process* current, uint64_t flags) {
    PtyDevice* dev = PtyManager::get().allocate();
    if (!dev) {
        return syscall_error(SysErrNoMemory);
    }
    VNode* node = PtyManager::get().createMasterNode(dev);
    if (!node) {
        PtyManager::get().release(dev);
        return syscall_error(SysErrNoMemory);
    }
    FileDescriptor* fd = new FileDescriptor(node, static_cast<int>(flags));
    if (!fd) {
        gPtyMasterOps.close(node);
        delete node;
        return syscall_error(SysErrNoMemory);
    }
    uint64_t handle = current->allocateFD(fd, HandleRightRead | HandleRightWrite | HandleRightDuplicate,
                                          (flags & kOpenCloseOnExec) != 0);
    if (handle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return syscall_error(SysErrNoMemory);
    }
    return handle;
}

// Open the slave end of an existing pty by index (used for /dev/pts/N).
uint64_t openPtySlave(Process* current, uint32_t index, uint64_t flags) {
    PtyDevice* dev = PtyManager::get().deviceForIndex(index);
    if (!dev) {
        return syscall_error(SysErrNoEntry);
    }
    VNode* node = PtyManager::get().createSlaveNode(dev);
    if (!node) {
        return syscall_error(SysErrNoMemory);
    }
    FileDescriptor* fd = new FileDescriptor(node, static_cast<int>(flags));
    if (!fd) {
        gPtySlaveOps.close(node);
        delete node;
        return syscall_error(SysErrNoMemory);
    }
    uint64_t handle = current->allocateFD(fd, HandleRightRead | HandleRightWrite | HandleRightDuplicate,
                                          (flags & kOpenCloseOnExec) != 0);
    if (handle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return syscall_error(SysErrNoMemory);
    }
    return handle;
}

// Open a named FIFO. All opens of the same FIFO (keyed by fs+inode) share one
// PipeObject. POSIX blocking semantics: open(O_RDONLY) blocks until a writer is
// present and open(O_WRONLY) blocks until a reader is present (unless
// O_NONBLOCK); O_RDWR never blocks. Returns a handle over a pipe-endpoint VNode
// so the existing pipeRead/pipeWrite/pipeClose/pollPipe machinery applies.
uint64_t openFifo(Process* current, VNode* fifoNode, uint64_t flags) {
    PipeObject* pipe = fifoGetOrCreate(fifoNode->getFS(), fifoNode->getInode());
    if (!pipe) {
        return syscall_error(SysErrNoMemory);
    }

    const uint64_t access = flags & kOpenAccessModeMask;
    const bool wantWrite = (access == kOpenWriteOnly) || (access == kOpenReadWrite);
    const bool wantRead = (access != kOpenWriteOnly);  // RO or RW
    const bool nonBlock = (flags & kOpenNonBlock) != 0;

    auto* endpoint = new PipeEndpoint { pipe, wantWrite && !wantRead };
    VNode* node = new VNode(nullptr, fifoNode->getInode(), FileType::Pipe);
    if (!endpoint || !node) {
        delete endpoint;
        delete node;
        return syscall_error(SysErrNoMemory);
    }
    node->refCount = 0;
    node->ops = &pipeOps;
    node->setData(endpoint);

    // Account this open against the shared pipe.
    pipe->refs++;
    if (wantRead) pipe->readOpen++;
    if (wantWrite) pipe->writeOpen++;
    wakePipeWaiters();  // a new peer may unblock the other side's open()

    // POSIX rendezvous: a read-only open blocks until a writer exists; a
    // write-only open blocks until a reader exists. O_RDWR (wantRead &&
    // wantWrite) never blocks.
    if (!nonBlock) {
        if (access == 0) {  // O_RDONLY
            while (pipe->writeOpen == 0) {
                if (!blockCurrentForPipe()) break;
            }
        } else if (access == kOpenWriteOnly) {
            while (pipe->readOpen == 0) {
                if (!blockCurrentForPipe()) break;
            }
        }
    }

    FileDescriptor* fd = new FileDescriptor(node, static_cast<int>(flags));
    if (!fd) {
        pipeClose(node);
        delete node;
        return syscall_error(SysErrNoMemory);
    }

    uint32_t rights = HandleRightDuplicate;
    if (wantRead) rights |= HandleRightRead;
    if (wantWrite) rights |= HandleRightWrite;
    uint64_t handle = current->allocateFD(fd, rights, (flags & kOpenCloseOnExec) != 0);
    if (handle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return syscall_error(SysErrNoMemory);
    }
    return handle;
}
}

uint64_t Syscall::sys_write(uint64_t fileHandle, uint64_t buf, uint64_t count) {
    // If the process has a real file handle bound to a stdio slot (e.g. a tty
    // inherited from its parent), route through it instead of the kernel
    // console. Bare integers 0/1/2 map to encoded File handles in those slots.
    Process* stdioProc = Scheduler::get().getCurrentProcess();
    if ((fileHandle == 1 || fileHandle == 2) && stdioProc) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, static_cast<int>(fileHandle));
        if (stdioProc->getHandle(encoded) != nullptr) {
            fileHandle = encoded;
        }
    }

    if (fileHandle == 1 || fileHandle == 2) {
        if (count == 0) return count;
        if (!isValidUserPointer(buf, count)) return syscall_error(SysErrInvalid);
        
        char chunk[256];
        uint64_t total = 0;
        while (total < count) {
            uint64_t toCopy = count - total;
            if (toCopy > sizeof(chunk)) {
                toCopy = sizeof(chunk);
            }
            if (!copyFromUser(chunk, buf + total, toCopy)) {
                return syscall_error(SysErrInvalid);
            }

            for (uint64_t i = 0; i < toCopy; i++) {
                char temp[2] = { chunk[i], '\0' };
                Console::get().drawText(temp);
            }
            // Mirror stdout/stderr to the serial port so logs from console-less
            // processes (e.g. the NetSurf browser spawned by a launcher) are
            // visible in the serial capture.
            for (uint64_t i = 0; i < toCopy; i++) {
                Cereal::get().write(chunk[i]);
            }
            total += toCopy;
        }
        
        return count;
    }
    
    if (!isValidUserPointer(buf, count)) {
        return syscall_error(SysErrInvalid);
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    FileDescriptor* fileFd = current->getFD(fileHandle, HandleRightWrite);
    if (!fileFd) return syscall_error(SysErrBadFile);
    
    char chunk[512];
    uint64_t total = 0;
    while (total < count) {
        uint64_t toCopy = count - total;
        if (toCopy > sizeof(chunk)) {
            toCopy = sizeof(chunk);
        }
        if (!copyFromUser(chunk, buf + total, toCopy)) {
            return syscall_error(SysErrInvalid);
        }

        int64_t written = VFS::get().write(fileFd, chunk, toCopy);
        if (written < 0) {
            VNode* node = fileFd->getNode();
            if (total > 0) {
                return total;
            }
            if (node && node->getType() == FileType::Pipe) {
                return syscall_error(SysErrPipe);
            }
            if (node && node->getType() == FileType::Directory) {
                return syscall_error(SysErrIsDirectory);
            }
            return syscall_error(SysErrBadFile);
        }
        total += static_cast<uint64_t>(written);
        if (static_cast<uint64_t>(written) != toCopy) {
            break;
        }
    }
    
    return total;
}

uint64_t Syscall::sys_serial_write(uint64_t buf, uint64_t count) {
    if (count == 0) {
        return count;
    }
    if (!isValidUserPointer(buf, count)) {
        return syscall_error(SysErrInvalid);
    }

    char chunk[256];
    uint64_t total = 0;
    while (total < count) {
        uint64_t toCopy = count - total;
        if (toCopy > sizeof(chunk)) {
            toCopy = sizeof(chunk);
        }
        if (!copyFromUser(chunk, buf + total, toCopy)) {
            return syscall_error(SysErrInvalid);
        }

        for (uint64_t i = 0; i < toCopy; ++i) {
            Cereal::get().write(chunk[i]);
        }
        total += toCopy;
    }

    return count;
}

uint64_t Syscall::sys_read(uint64_t fileHandle, uint64_t buf, uint64_t count) {
    // Route stdin through a bound tty/file handle when one exists.
    Process* stdinProc = Scheduler::get().getCurrentProcess();
    if (fileHandle == 0 && stdinProc) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, 0);
        if (stdinProc->getHandle(encoded) != nullptr) {
            fileHandle = encoded;
        }
    }

    if (fileHandle == 0) {
        if (!isValidUserPointer(buf, count)) {
            traceStr("[stdin] read invalid user buffer count=");
            traceDec(count);
            traceStr("\n");
            return syscall_error(SysErrInvalid);
        }

        Process* current = Scheduler::get().getCurrentProcess();
        traceStr("[stdin] read begin pid=");
        traceDec(current ? current->getPID() : 0);
        traceStr(" name=");
        traceStr(current ? current->getName() : "<none>");
        traceStr(" count=");
        traceDec(count);
        traceStr("\n");

        size_t bytesRead = 0;

        while (bytesRead < count) {
            USBInput::get().poll();
            Keyboard::get().servicePendingInput();

            if (!Keyboard::get().hasKey()) {
                current = Scheduler::get().getCurrentProcess();
                if (!current) {
                    traceStr("[stdin] read no current process while blocking\n");
                    return syscall_error(SysErrInvalid);
                }

                traceStr("[stdin] read blocking pid=");
                traceDec(current->getPID());
                traceStr(" name=");
                traceStr(current->getName());
                traceStr(" bytes=");
                traceDec(bytesRead);
                traceStr("\n");
                current->setState(ProcessState::Blocked);
                Scheduler::get().scheduleFromSyscall();
                if (current->hasDeliverableSignal()) {
                    return bytesRead > 0 ? bytesRead : syscall_error(SysErrInterrupted);
                }
                traceStr("[stdin] read woke pid=");
                traceDec(current->getPID());
                traceStr(" name=");
                traceStr(current->getName());
                traceStr("\n");
                continue;
            }

            char c = Keyboard::get().getKey();

            if (!copyToUser(buf + bytesRead, &c, 1)) {
                traceStr("[stdin] read copy_to_user failed byte=");
                traceDec(bytesRead);
                traceStr("\n");
                return syscall_error(SysErrInvalid);
            }
            bytesRead++;
            traceStr("[stdin] read char=");
            tracePrintableChar(c);
            traceStr(" bytes=");
            traceDec(bytesRead);
            traceStr("\n");

            if (!Keyboard::get().hasKey()) {
                break;
            }
        }

        traceStr("[stdin] read end bytes=");
        traceDec(bytesRead);
        traceStr("\n");
        return bytesRead;
    }
    
    if (!isValidUserPointer(buf, count)) {
        return syscall_error(SysErrInvalid);
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    FileDescriptor* fileFd = current->getFD(fileHandle, HandleRightRead);
    if (!fileFd) return syscall_error(SysErrBadFile);
    
    char chunk[512];
    uint64_t total = 0;
    while (total < count) {
        uint64_t toRead = count - total;
        if (toRead > sizeof(chunk)) {
            toRead = sizeof(chunk);
        }

        int64_t bytesRead = VFS::get().read(fileFd, chunk, toRead);
        if (bytesRead < 0) {
            if (total > 0) {
                return total;
            }
            VNode* node = fileFd->getNode();
            return syscall_error(node && node->getType() == FileType::Directory ? SysErrIsDirectory : SysErrBadFile);
        }
        if (bytesRead == 0) {
            break;
        }
        if (!copyToUser(buf + total, chunk, static_cast<size_t>(bytesRead))) {
            return syscall_error(SysErrInvalid);
        }
        total += static_cast<uint64_t>(bytesRead);
        if (static_cast<uint64_t>(bytesRead) != toRead) {
            break;
        }
    }
    
    return total;
}

uint64_t Syscall::sys_open(uint64_t path, uint64_t flags, uint64_t mode) {
    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(path, pathname, error, &requiresDirectory)) {
        return error;
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    if ((flags & ~kOpenSupportedFlags) != 0 || (flags & kOpenAccessModeMask) == kOpenAccessModeMask) {
        return syscall_error(SysErrInvalid);
    }

    // PTY device nodes are synthesized per-open; bypass the generic VFS path so
    // each open gets a fresh, independently-tracked endpoint.
    if (pathEquals(pathname, "/dev/ptmx")) {
        return openPtyMaster(current, flags);
    }
    {
        uint32_t ptsIndex = 0;
        if (matchPtsPath(pathname, &ptsIndex)) {
            return openPtySlave(current, ptsIndex, flags);
        }
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    FileStats linkStats {};
    const bool linkExists = VFS::get().lstat(pathname, &linkStats) == 0;
    if ((flags & (kOpenCreate | kOpenExclusive)) == (kOpenCreate | kOpenExclusive) && linkExists) {
        return syscall_error(SysErrExists);
    }
    if ((flags & kOpenNoFollow) != 0 && linkExists && linkStats.type == FileType::Symlink) {
        return syscall_error(SysErrLoop);
    }
    const bool exists = VFS::get().stat(pathname, &stats) == 0;
    if (trailingSlashRejectsNonDirectory(pathname, requiresDirectory)) {
        return syscall_error(SysErrNotDirectory);
    }
    if ((flags & kOpenTruncate) != 0 && (flags & kOpenAccessModeMask) == 0) {
        return syscall_error(SysErrInvalid);
    }
    if (exists) {
        if ((flags & (kOpenCreate | kOpenExclusive)) == (kOpenCreate | kOpenExclusive)) {
            return syscall_error(SysErrExists);
        }
        if ((flags & kOpenDirectory) != 0 && stats.type != FileType::Directory) {
            return syscall_error(SysErrNotDirectory);
        }
        if (stats.type == FileType::Directory &&
            ((flags & kOpenAccessModeMask) != 0 || (flags & kOpenTruncate) != 0)) {
            return syscall_error(SysErrIsDirectory);
        }
        if (stats.type == FileType::Directory && (stats.mode & kModeReadBits) == 0) {
            return syscall_error(SysErrAccess);
        }
        if (!openFlagsAllowedByMode(flags, stats.mode)) {
            return syscall_error(SysErrAccess);
        }
        // Named FIFO: route through the pipe machinery (shared ring buffer +
        // blocking open/read/write) rather than the generic file path.
        if (stats.type == FileType::Pipe) {
            VNode* fifoNode = VFS::get().lookup(pathname, (flags & kOpenNoFollow) == 0);
            if (fifoNode) {
                return openFifo(current, fifoNode, flags);
            }
        }
    } else {
        if (VFS::get().getLastError() == SysErrLoop) {
            return syscall_error(SysErrLoop);
        }
        char parent[256];
        if (parentPathOf(pathname, parent)) {
            FileStats parentStats {};
            if (VFS::get().stat(parent, &parentStats) != 0) {
                return syscall_error(SysErrNoEntry);
            }
            if (parentStats.type != FileType::Directory) {
                return syscall_error(SysErrNotDirectory);
            }
        }
        if ((flags & kOpenCreate) != 0 && pathParentDeniesWrite(pathname)) {
            return syscall_error(SysErrAccess);
        }
    }
    
    FileDescriptor* fd = nullptr;
    int result = VFS::get().open(pathname, flags, &fd, static_cast<uint32_t>(mode ? mode : 0666));
    
    if (result != 0 || !fd) {
        if (VFS::get().getLastError() == SysErrLoop) {
            return syscall_error(SysErrLoop);
        }
        return syscall_error(SysErrNoEntry);
    }
    
    uint64_t fileHandle = current->allocateFD(fd, fileRightsFromOpenFlags(flags), (flags & kOpenCloseOnExec) != 0);
    if (fileHandle == static_cast<uint64_t>(-1)) {
        VFS::get().close(fd);
        return syscall_error(SysErrNoMemory);
    }
    
    return fileHandle;
}

uint64_t Syscall::sys_close(uint64_t handle) {
    if (handle < 3) return syscall_error(SysErrBadFile);
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    if (current->getFD(handle)) {
        current->closeFD(handle);
        return 0;
    }

    return current->closeHandle(handle) ? 0 : syscall_error(SysErrBadFile);
}

uint64_t Syscall::sys_chdir(uint64_t path) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }
    
    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        char parent[256];
        if (parentPathOf(pathname, parent)) {
            FileStats parentStats {};
            if (VFS::get().stat(parent, &parentStats) == 0 && parentStats.type != FileType::Directory) {
                return syscall_error(SysErrNotDirectory);
            }
        }
        return syscall_error(SysErrNoEntry);
    }
    if (stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }
    if ((stats.mode & kModeExecBits) == 0) {
        return syscall_error(SysErrAccess);
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    current->setCwd(pathname);
    return 0;
}

uint64_t Syscall::sys_getcwd(uint64_t buf, uint64_t size) {
    if (!isValidUserPointer(buf, size)) {
        return syscall_error(SysErrInvalid);
    }
    
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);
    
    const char* cwd = current->getCwd();
    size_t cwdLen = 0;
    while (cwd[cwdLen]) cwdLen++;
    
    if (cwdLen + 1 > size) {
        return syscall_error(SysErrRange);
    }
    
    if (!copyToUser(buf, cwd, cwdLen + 1)) {
        return syscall_error(SysErrInvalid);
    }
    
    return buf;
}

uint64_t Syscall::sys_mkdir(uint64_t path, uint64_t mode) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) == 0) {
        return syscall_error(SysErrExists);
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }
    if (pathParentDeniesWrite(pathname)) {
        return syscall_error(SysErrAccess);
    }

    char parent[256];
    if (parentPathOf(pathname, parent)) {
        FileStats parentStats {};
        if (VFS::get().stat(parent, &parentStats) != 0) {
            return syscall_error(SysErrNoEntry);
        }
        if (parentStats.type != FileType::Directory) {
            return syscall_error(SysErrNotDirectory);
        }
    }
    
    int result = VFS::get().mkdir(pathname, mode);
    return result == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_rmdir(uint64_t path) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }
    if (pathParentDeniesWrite(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        char parent[256];
        if (parentPathOf(pathname, parent)) {
            FileStats parentStats {};
            if (VFS::get().stat(parent, &parentStats) == 0 && parentStats.type != FileType::Directory) {
                return syscall_error(SysErrNotDirectory);
            }
        }
        return syscall_error(SysErrNoEntry);
    }
    if (stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }

    DirEntry entries[4] {};
    uint64_t readCount = 0;
    if (VFS::get().readdir(pathname, entries, 4, &readCount) == 0) {
        for (uint64_t i = 0; i < readCount; ++i) {
            if (!dirEntryIsDotOrDotDot(entries[i])) {
                return syscall_error(SysErrNotEmpty);
            }
        }
    }
    
    int result = VFS::get().rmdir(pathname);
    return result == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_unlink(uint64_t path) {
    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(path, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }
    if (pathParentDeniesWrite(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        return missingPathError(pathname);
    }
    if (requiresDirectory && stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }
    if (stats.type == FileType::Directory) {
        return syscall_error(SysErrIsDirectory);
    }
    
    int result = VFS::get().unlink(pathname);
    return result == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_link(uint64_t oldPath, uint64_t newPath) {
    char oldPathname[256];
    char newPathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(oldPath, oldPathname, error) ||
        !copyUserPathOrError(newPath, newPathname, error)) {
        return error;
    }

    if (pathParentDeniesSearch(oldPathname) || pathParentDeniesSearch(newPathname)) {
        return syscall_error(SysErrAccess);
    }
    if (pathParentDeniesWrite(newPathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats oldStats {};
    if (VFS::get().stat(oldPathname, &oldStats) != 0) {
        return syscall_error(SysErrNoEntry);
    }
    if (oldStats.type == FileType::Directory) {
        return syscall_error(SysErrInvalid);
    }

    FileStats newStats {};
    if (VFS::get().stat(newPathname, &newStats) == 0) {
        return syscall_error(SysErrExists);
    }

    char newParent[256];
    if (parentPathOf(newPathname, newParent)) {
        FileStats parentStats {};
        if (VFS::get().stat(newParent, &parentStats) != 0) {
            return syscall_error(SysErrNoEntry);
        }
        if (parentStats.type != FileType::Directory) {
            return syscall_error(SysErrNotDirectory);
        }
    }

    return VFS::get().link(oldPathname, newPathname) == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_symlink(uint64_t target, uint64_t linkPath) {
    char targetPath[256];
    char linkPathname[256];
    uint64_t error = 0;
    if (!copyUserString(target, targetPath, sizeof(targetPath)) || targetPath[0] == '\0') {
        return syscall_error(SysErrInvalid);
    }
    if (!copyUserPathOrError(linkPath, linkPathname, error)) {
        return error;
    }

    if (pathParentDeniesSearch(linkPathname) || pathParentDeniesWrite(linkPathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats existing {};
    if (VFS::get().lstat(linkPathname, &existing) == 0) {
        return syscall_error(SysErrExists);
    }

    char parent[256];
    if (parentPathOf(linkPathname, parent)) {
        FileStats parentStats {};
        if (VFS::get().stat(parent, &parentStats) != 0) {
            return syscall_error(SysErrNoEntry);
        }
        if (parentStats.type != FileType::Directory) {
            return syscall_error(SysErrNotDirectory);
        }
    }

    return VFS::get().symlink(targetPath, linkPathname) == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_readlink(uint64_t path, uint64_t buffer, uint64_t size) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }
    if (size == 0) {
        return syscall_error(SysErrInvalid);
    }
    if (size > 0 && !isValidUserPointer(buffer, size)) {
        return syscall_error(SysErrInvalid);
    }
    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().lstat(pathname, &stats) != 0) {
        return missingPathError(pathname);
    }
    if (stats.type != FileType::Symlink) {
        return syscall_error(SysErrInvalid);
    }

    char local[256];
    int64_t result = VFS::get().readlink(pathname, local, size < sizeof(local) ? size : sizeof(local));
    if (result < 0) {
        return VFS::get().getLastError() == SysErrLoop ? syscall_error(SysErrLoop) : syscall_error(SysErrInvalid);
    }
    if (result > 0 && !copyToUser(buffer, local, static_cast<size_t>(result))) {
        return syscall_error(SysErrInvalid);
    }
    return static_cast<uint64_t>(result);
}

uint64_t Syscall::sys_stat(uint64_t path, uint64_t statbuf) {
    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(path, pathname, error, &requiresDirectory)) {
        return error;
    }
    
    if (!isValidUserPointer(statbuf, sizeof(Stat))) {
        return syscall_error(SysErrInvalid);
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }
    
    FileStats fileStats {};
    if (VFS::get().stat(pathname, &fileStats) != 0) {
        if (VFS::get().getLastError() == SysErrLoop) {
            return syscall_error(SysErrLoop);
        }
        char parent[256];
        if (parentPathOf(pathname, parent)) {
            FileStats parentStats {};
            if (VFS::get().stat(parent, &parentStats) == 0 && parentStats.type != FileType::Directory) {
                return syscall_error(SysErrNotDirectory);
            }
        }
        return syscall_error(SysErrNoEntry);
    }
    if (requiresDirectory && fileStats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }

    return copyStatToUser(fileStats, statbuf) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_lstat(uint64_t path, uint64_t statbuf) {
    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(path, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (!isValidUserPointer(statbuf, sizeof(Stat))) {
        return syscall_error(SysErrInvalid);
    }
    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats fileStats {};
    if (VFS::get().lstat(pathname, &fileStats) != 0) {
        return missingPathError(pathname);
    }
    if (requiresDirectory && fileStats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }

    return copyStatToUser(fileStats, statbuf) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_fstat(uint64_t handle, uint64_t statbuf) {
    if (!isValidUserPointer(statbuf, sizeof(Stat))) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();

    // Prefer a real file handle bound to a stdio slot.
    if ((handle == 0 || handle == 1 || handle == 2) && current) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, static_cast<int>(handle));
        if (current->getHandle(encoded) != nullptr) {
            handle = encoded;
        }
    }

    if (handle == 0 || handle == 1 || handle == 2) {
        FileStats stats {};
        stats.type = FileType::CharDevice;
        stats.mode = 0020000 | 0600;
        stats.links = 1;
        return copyStatToUser(stats, statbuf) ? 0 : syscall_error(SysErrInvalid);
    }

    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    FileDescriptor* fileFd = current->getFD(handle);
    if (!fileFd || !fileFd->getNode() || !fileFd->getNode()->ops || !fileFd->getNode()->ops->stat) {
        return syscall_error(SysErrBadFile);
    }

    FileStats fileStats {};
    if (fileFd->getNode()->ops->stat(fileFd->getNode(), &fileStats) != 0) {
        return syscall_error(SysErrBadFile);
    }

    return copyStatToUser(fileStats, statbuf) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_dup(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    return current->duplicateHandle(handle);
}

uint64_t Syscall::sys_dup2(uint64_t oldHandle, uint64_t newHandle) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return syscall_error(SysErrInvalid);

    // A bare small integer target (0/1/2) refers to a reserved stdio slot.
    // Translate it to the encoded File handle for that slot so duplicateTo()
    // installs the source into the right place (mirrors sys_write/sys_ioctl).
    if (newHandle <= 2) {
        newHandle = HandleTable::encodeHandle(HandleType::File, static_cast<int>(newHandle));
    }

    if (!current->duplicateHandleTo(oldHandle, newHandle)) {
        return syscall_error(SysErrBadFile);
    }

    return newHandle;
}

uint64_t Syscall::sys_pipe(uint64_t pipeHandles) {
    if (!isValidUserPointer(pipeHandles, sizeof(uint64_t) * 2)) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    PipeObject* pipe = new PipeObject {};
    PipeEndpoint* readEndpoint = new PipeEndpoint { pipe, false };
    PipeEndpoint* writeEndpoint = new PipeEndpoint { pipe, true };
    VNode* readNode = new VNode(nullptr, 0, FileType::Pipe);
    VNode* writeNode = new VNode(nullptr, 0, FileType::Pipe);
    if (!pipe || !readEndpoint || !writeEndpoint || !readNode || !writeNode) {
        delete readNode;
        delete writeNode;
        delete readEndpoint;
        delete writeEndpoint;
        delete pipe;
        return syscall_error(SysErrNoMemory);
    }

    pipe->refs = 2;
    pipe->readOpen = 1;
    pipe->writeOpen = 1;

    readNode->refCount = 0;
    readNode->ops = &pipeOps;
    readNode->setData(readEndpoint);
    writeNode->refCount = 0;
    writeNode->ops = &pipeOps;
    writeNode->setData(writeEndpoint);

    FileDescriptor* readFd = new FileDescriptor(readNode, 0);
    FileDescriptor* writeFd = new FileDescriptor(writeNode, 1);
    if (!readFd || !writeFd) {
        if (readFd) {
            VFS::get().close(readFd);
        } else {
            pipeClose(readNode);
            delete readNode;
        }
        if (writeFd) {
            VFS::get().close(writeFd);
        } else {
            pipeClose(writeNode);
            delete writeNode;
        }
        return syscall_error(SysErrNoMemory);
    }

    uint64_t handles[2];
    handles[0] = current->allocateFD(readFd, HandleRightRead | HandleRightDuplicate);
    if (handles[0] == static_cast<uint64_t>(-1)) {
        VFS::get().close(readFd);
        VFS::get().close(writeFd);
        return syscall_error(SysErrNoSpace);
    }

    handles[1] = current->allocateFD(writeFd, HandleRightWrite | HandleRightDuplicate);
    if (handles[1] == static_cast<uint64_t>(-1)) {
        current->closeHandle(handles[0]);
        VFS::get().close(writeFd);
        return syscall_error(SysErrNoSpace);
    }

    if (!copyToUser(pipeHandles, handles, sizeof(handles))) {
        current->closeHandle(handles[0]);
        current->closeHandle(handles[1]);
        return syscall_error(SysErrInvalid);
    }

    return 0;
}

uint64_t Syscall::sys_fcntl(uint64_t handle, uint64_t command, uint64_t value) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrBadFile);
    }

    // Bare stdio fds 0/1/2 map to encoded File handles in the reserved slots.
    if (handle <= 2) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, static_cast<int>(handle));
        if (current->getHandle(encoded) != nullptr) {
            handle = encoded;
        }
    }

    if (!current->getHandle(handle)) {
        return syscall_error(SysErrBadFile);
    }

    switch (command) {
        case kFcntlGetFD: {
            bool closeOnExec = false;
            if (!current->getHandleCloseOnExec(handle, &closeOnExec)) {
                return syscall_error(SysErrBadFile);
            }
            return closeOnExec ? kFdCloseOnExec : 0;
        }
        case kFcntlSetFD:
            if (!current->setHandleCloseOnExec(handle, (value & kFdCloseOnExec) != 0)) {
                return syscall_error(SysErrBadFile);
            }
            return 0;
        case kFcntlGetFL:
            // The kernel does not track per-fd status flags; report read/write,
            // no special modes (O_NONBLOCK etc.).
            return kOpenReadWrite;
        case kFcntlSetFL:
            // Accept and ignore status-flag changes (e.g. O_NONBLOCK toggles).
            return 0;
        case kFcntlDupFD:
        case kFcntlDupFDCloExec:
            // F_DUPFD[_CLOEXEC]: duplicate to a new handle. The kernel uses
            // encoded handles rather than lowest-numbered fds; the mlibc
            // userspace fd table maps the returned handle to a small fd.
            return current->duplicateHandle(handle);
        default:
            return syscall_error(SysErrInvalid);
    }
}

uint64_t Syscall::sys_ioctl(uint64_t handle, uint64_t request, uint64_t arg) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    // Bare stdio fds 0/1/2 map to encoded File handles in the reserved stdio
    // slots (e.g. a tty inherited from the parent). Translate so ioctls such as
    // isatty()/tcgetattr() on stdin reach the bound device.
    if (handle <= 2) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, static_cast<int>(handle));
        if (current->getHandle(encoded) != nullptr) {
            handle = encoded;
        }
    }

    HandleEntry* entry = current->getHandle(handle);
    if (!entry || entry->type != HandleType::File) {
        return syscall_error(SysErrBadFile);
    }
    auto* fileFd = reinterpret_cast<FileDescriptor*>(entry->object);
    VNode* node = fileFd ? fileFd->getNode() : nullptr;
    if (!node || node->getType() != FileType::CharDevice) {
        return syscall_error(SysErrInvalid);
    }

    PtyDevice* master = ptyDeviceFromMasterNode(node);
    PtyDevice* slave = ptyDeviceFromSlaveNode(node);
    PtyDevice* dev = master ? master : slave;
    if (!dev) {
        return syscall_error(SysErrInvalid);
    }

    switch (request) {
        case PTY_TCGETS: {
            KernelTermios t;
            dev->getTermios(&t);
            if (!copyToUser(arg, &t, sizeof(t))) {
                return syscall_error(SysErrInvalid);
            }
            return 0;
        }
        case PTY_TCSETS:
        case PTY_TCSETSW:
        case PTY_TCSETSF: {
            KernelTermios t;
            if (!copyFromUser(&t, arg, sizeof(t))) {
                return syscall_error(SysErrInvalid);
            }
            dev->setTermios(&t);
            return 0;
        }
        case PTY_TIOCGWINSZ: {
            KernelWinsize ws;
            dev->getWinsize(&ws);
            if (!copyToUser(arg, &ws, sizeof(ws))) {
                return syscall_error(SysErrInvalid);
            }
            return 0;
        }
        case PTY_TIOCSWINSZ: {
            KernelWinsize ws;
            if (!copyFromUser(&ws, arg, sizeof(ws))) {
                return syscall_error(SysErrInvalid);
            }
            dev->setWinsize(&ws);
            return 0;
        }
        case PTY_TIOCGPTN: {
            uint32_t n = dev->getIndex();
            if (!copyToUser(arg, &n, sizeof(n))) {
                return syscall_error(SysErrInvalid);
            }
            return 0;
        }
        case PTY_TIOCSPTLCK:
            return 0;  // we never lock the slave
        case PTY_TIOCGPGRP: {
            uint32_t pgrp = dev->getForegroundPgid();
            if (!copyToUser(arg, &pgrp, sizeof(pgrp))) {
                return syscall_error(SysErrInvalid);
            }
            return 0;
        }
        case PTY_TIOCSPGRP: {
            uint32_t pgrp = 0;
            if (!copyFromUser(&pgrp, arg, sizeof(pgrp))) {
                return syscall_error(SysErrInvalid);
            }
            dev->setForegroundPgid(pgrp);
            return 0;
        }
        case PTY_TIOCSCTTY:
            dev->setSession(current->getSessionID());
            dev->setForegroundPgid(current->getPID());
            return 0;
        default:
            return syscall_error(SysErrInvalid);
    }
}

uint64_t Syscall::sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeoutMs) {
    if (nfds > HandleTable::MaxHandles) {
        return syscall_error(SysErrInvalid);
    }
    if (nfds != 0 && !isValidUserPointer(fds, nfds * sizeof(PollFD))) {
        return syscall_error(SysErrInvalid);
    }

    Process* current = Scheduler::get().getCurrentProcess();

    // One scan over all descriptors; returns the ready count and writes revents.
    auto scanOnce = [&](uint64_t* outReady) -> bool {
        PollFD local[64];
        uint64_t ready = 0;
        uint64_t index = 0;
        while (index < nfds) {
            uint64_t batch = nfds - index;
            if (batch > 64) {
                batch = 64;
            }
            const uint64_t bytes = batch * sizeof(PollFD);
            if (!copyFromUser(local, fds + index * sizeof(PollFD), bytes)) {
                return false;
            }

            for (uint64_t i = 0; i < batch; ++i) {
                local[i].revents = pollHandle(current, local[i]);
                if (local[i].revents != 0) {
                    ready++;
                }
            }

            if (!copyToUser(fds + index * sizeof(PollFD), local, bytes)) {
                return false;
            }
            index += batch;
        }
        *outReady = ready;
        return true;
    };

    // timeoutMs: 0 = non-blocking (single scan); kPollWaitForever blocks until
    // a descriptor is ready (or a signal arrives). A finite non-zero timeout
    // blocks but arms a sleep deadline so the scheduler's timer wakes us after
    // timeoutMs even if no descriptor becomes ready (required so callers like
    // the NetSurf event loop wake to re-run their own scheduler instead of
    // busy-spinning).
    constexpr uint64_t kPollWaitForever = static_cast<uint64_t>(-1);

    for (;;) {
        uint64_t ready = 0;
        if (!scanOnce(&ready)) {
            return syscall_error(SysErrInvalid);
        }
        if (ready > 0 || timeoutMs == 0) {
            return ready;
        }

        // Block until woken by an I/O event, the sleep deadline, or a signal.
        current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return 0;
        }
        if (timeoutMs != kPollWaitForever) {
            current->sleepUntil(time_get_uptime_ms() + timeoutMs);
        }
        current->setState(ProcessState::Blocked);
        Scheduler::get().scheduleFromSyscall();
        current->clearSleep();
        if (current->hasDeliverableSignal()) {
            return syscall_error(SysErrInterrupted);
        }

        if (timeoutMs != kPollWaitForever) {
            // Finite timeout: we either hit the deadline or were woken by I/O.
            // Re-scan once and return whatever is ready (0 is fine -> caller
            // re-polls / re-runs its scheduler).
            uint64_t ready2 = 0;
            if (!scanOnce(&ready2)) {
                return syscall_error(SysErrInvalid);
            }
            return ready2;
        }
    }
}

uint64_t Syscall::sys_truncate(uint64_t target, uint64_t size, uint64_t byHandle) {
    if (byHandle) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return syscall_error(SysErrInvalid);
        }
        FileDescriptor* fileFd = current->getFD(target, HandleRightWrite);
        if (!fileFd) {
            fileFd = current->getFD(target);
            if (fileFd && fileFd->getNode() && fileFd->getNode()->getType() == FileType::Directory) {
                return syscall_error(SysErrIsDirectory);
            }
            return syscall_error(SysErrBadFile);
        }
        if (fileFd->getNode() && fileFd->getNode()->getType() == FileType::Directory) {
            return syscall_error(SysErrIsDirectory);
        }
        return VFS::get().truncate(fileFd, size) == 0 ? 0 : syscall_error(SysErrInvalid);
    }

    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(target, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        return missingPathError(pathname);
    }
    if (requiresDirectory && stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }
    if (stats.type == FileType::Directory) {
        return syscall_error(SysErrIsDirectory);
    }

    return VFS::get().truncate(pathname, size) == 0 ? 0 : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_rename(uint64_t oldPath, uint64_t newPath) {
    char oldPathname[256];
    char newPathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(oldPath, oldPathname, error) ||
        !copyUserPathOrError(newPath, newPathname, error)) {
        return error;
    }

    if (pathParentDeniesSearch(oldPathname) || pathParentDeniesSearch(newPathname)) {
        return syscall_error(SysErrAccess);
    }
    if (pathParentDeniesWrite(oldPathname) || pathParentDeniesWrite(newPathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats oldStats {};
    if (VFS::get().stat(oldPathname, &oldStats) != 0 && pathParentIsNonDirectory(oldPathname)) {
        return syscall_error(SysErrNotDirectory);
    }
    if (pathParentIsNonDirectory(newPathname)) {
        return syscall_error(SysErrNotDirectory);
    }

    if (VFS::get().rename(oldPathname, newPathname) == 0) {
        return 0;
    }
    // Map the VFS error to a meaningful errno instead of a blanket ENOENT.
    switch (VFS::get().getLastError()) {
        case 18: return syscall_error(SysErrCrossDevice);   // EXDEV
        case 38: return syscall_error(SysErrNoSys);          // ENOSYS (fs lacks rename)
        default: return syscall_error(SysErrNoEntry);        // ENOENT
    }
}

uint64_t Syscall::sys_access(uint64_t path, uint64_t mode) {
    // POSIX access()/faccessat() permission probe.
    //   mode bits: F_OK=0, X_OK=1, W_OK=2, R_OK=4.
    // The kernel resolves the path (following symlinks), checks existence, then
    // verifies the requested permission bits against the file's mode. With a
    // single-owner model we test the owner permission bits (rwx = 0400/0200/0100).
    constexpr uint64_t kFOk = 0;
    constexpr uint64_t kXOk = 1;
    constexpr uint64_t kWOk = 2;
    constexpr uint64_t kROk = 4;

    if (mode & ~(kXOk | kWOk | kROk)) {
        return syscall_error(SysErrInvalid);
    }

    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(path, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        return missingPathError(pathname);
    }
    if (requiresDirectory && stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }

    // F_OK: existence only.
    if (mode == kFOk) {
        return 0;
    }

    // Owner permission bits from the file mode.
    const bool canRead = (stats.mode & 0400) != 0;
    const bool canWrite = (stats.mode & 0200) != 0;
    const bool canExec = (stats.mode & 0100) != 0;

    if (((mode & kROk) && !canRead) ||
        ((mode & kWOk) && !canWrite) ||
        ((mode & kXOk) && !canExec)) {
        return syscall_error(SysErrAccess);
    }
    return 0;
}

uint64_t Syscall::sys_statfs(uint64_t target, uint64_t byHandle, uint64_t statbuf) {
    if (!isValidUserPointer(statbuf, sizeof(KernelStatfs))) {
        return syscall_error(SysErrInvalid);
    }

    FsStats fsStats {};
    if (byHandle) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) return syscall_error(SysErrInvalid);
        FileDescriptor* fileFd = current->getFD(target);
        if (!fileFd) return syscall_error(SysErrBadFile);
        if (VFS::get().statfs(fileFd, &fsStats) != 0) {
            return VFS::get().getLastError() == 38 ? syscall_error(SysErrNoSys)
                                                   : syscall_error(SysErrBadFile);
        }
    } else {
        char pathname[256];
        uint64_t error = 0;
        if (!copyUserPathOrError(target, pathname, error)) {
            return error;
        }
        if (pathParentDeniesSearch(pathname)) {
            return syscall_error(SysErrAccess);
        }
        if (VFS::get().statfs(pathname, &fsStats) != 0) {
            return VFS::get().getLastError() == 38 ? syscall_error(SysErrNoSys)
                                                   : missingPathError(pathname);
        }
    }

    KernelStatfs out {};
    out.blockSize = fsStats.blockSize;
    out.totalBlocks = fsStats.totalBlocks;
    out.freeBlocks = fsStats.freeBlocks;
    out.totalInodes = fsStats.totalInodes;
    out.freeInodes = fsStats.freeInodes;
    out.nameMax = fsStats.nameMax;
    out.fsType = fsStats.fsType;
    out.reserved = 0;
    return copyToUser(statbuf, &out, sizeof(out)) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_chown(uint64_t target, uint64_t byHandle, uint64_t uid, uint64_t gid,
                            uint64_t flags) {
    const uint32_t newUid = static_cast<uint32_t>(uid);
    const uint32_t newGid = static_cast<uint32_t>(gid);

    if (byHandle) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) return syscall_error(SysErrInvalid);
        FileDescriptor* fileFd = current->getFD(target);
        if (!fileFd) return syscall_error(SysErrBadFile);
        if (VFS::get().chown(fileFd, newUid, newGid) != 0) {
            return VFS::get().getLastError() == 38 ? syscall_error(SysErrNoSys)
                                                   : syscall_error(SysErrBadFile);
        }
        return 0;
    }

    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(target, pathname, error)) {
        return error;
    }
    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }
    // AT_SYMLINK_NOFOLLOW (0x100) -> lchown semantics (do not follow final link).
    const bool followSymlink = (flags & 0x100) == 0;
    if (VFS::get().chown(pathname, newUid, newGid, followSymlink) != 0) {
        return VFS::get().getLastError() == 38 ? syscall_error(SysErrNoSys)
                                               : missingPathError(pathname);
    }
    return 0;
}

uint64_t Syscall::sys_mknod(uint64_t path, uint64_t mode, uint64_t dev) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }
    if (pathParentDeniesSearch(pathname) || pathParentDeniesWrite(pathname)) {
        return syscall_error(SysErrAccess);
    }

    // Reject if the target already exists.
    FileStats existing {};
    if (VFS::get().lstat(pathname, &existing) == 0) {
        return syscall_error(SysErrExists);
    }

    if (VFS::get().mknod(pathname, static_cast<uint32_t>(mode), dev) != 0) {
        switch (VFS::get().getLastError()) {
            case 38: return syscall_error(SysErrNoSys);    // fs lacks mknod
            case 17: return syscall_error(SysErrExists);
            default: return syscall_error(SysErrNoEntry);
        }
    }
    return 0;
}

uint64_t Syscall::sys_chmod(uint64_t target, uint64_t mode, uint64_t byHandle) {
    if (byHandle) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return syscall_error(SysErrInvalid);
        }
        FileDescriptor* fileFd = current->getFD(target);
        if (!fileFd) {
            return syscall_error(SysErrBadFile);
        }
        return VFS::get().chmod(fileFd, static_cast<uint32_t>(mode)) == 0 ? 0 : syscall_error(SysErrInvalid);
    }

    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(target, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    if (trailingSlashRejectsNonDirectory(pathname, requiresDirectory)) {
        return syscall_error(SysErrNotDirectory);
    }

    return VFS::get().chmod(pathname, static_cast<uint32_t>(mode)) == 0 ? 0 : missingPathError(pathname);
}

uint64_t Syscall::sys_utime(uint64_t target, uint64_t atime, uint64_t mtime, uint64_t byHandle) {
    if (byHandle) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return syscall_error(SysErrInvalid);
        }
        FileDescriptor* fileFd = current->getFD(target);
        if (!fileFd) {
            return syscall_error(SysErrBadFile);
        }
        return VFS::get().utime(fileFd, atime, mtime) == 0 ? 0 : syscall_error(SysErrInvalid);
    }

    char pathname[256];
    uint64_t error = 0;
    bool requiresDirectory = false;
    if (!copyUserPathOrError(target, pathname, error, &requiresDirectory)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    if (trailingSlashRejectsNonDirectory(pathname, requiresDirectory)) {
        return syscall_error(SysErrNotDirectory);
    }

    return VFS::get().utime(pathname, atime, mtime) == 0 ? 0 : missingPathError(pathname);
}

uint64_t Syscall::sys_seek(uint64_t handle, uint64_t offset, uint64_t whence) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    // Bare stdio fds 0/1/2 map to encoded File handles in the reserved stdio
    // slots (e.g. a tty inherited from the parent). Translate so seek() (used by
    // mlibc to detect pipe-like vs file-like streams via SEEK_CUR) reaches the
    // bound device and returns ESPIPE for ttys/pipes instead of EBADF.
    if (handle <= 2) {
        uint64_t encoded = HandleTable::encodeHandle(HandleType::File, static_cast<int>(handle));
        if (current->getHandle(encoded) != nullptr) {
            handle = encoded;
        } else {
            // No File bound to this stdio slot: it is the kernel console, which
            // is not seekable. Report ESPIPE so libc treats it as a pipe-like
            // (non-seekable) stream rather than a hard EBADF error. Without this
            // mlibc's fd_file::determine_type() fails and stdio writes are lost.
            return syscall_error(SysErrPipe);
        }
    }

    FileDescriptor* fileFd = current->getFD(handle, HandleRightRead);
    if (!fileFd) {
        fileFd = current->getFD(handle, HandleRightWrite);
    }
    if (!fileFd) {
        return syscall_error(SysErrBadFile);
    }

    SeekMode mode = SeekMode::Set;
    switch (whence) {
        case 0:
            mode = SeekMode::Set;
            break;
        case 1:
            mode = SeekMode::Current;
            break;
        case 2:
            mode = SeekMode::End;
            break;
        default:
            return syscall_error(SysErrInvalid);
    }

    const int64_t signedOffset = static_cast<int64_t>(offset);
    const int64_t result = VFS::get().seek(fileFd, signedOffset, mode);
    return result < 0 ? syscall_error(SysErrPipe) : static_cast<uint64_t>(result);
}

uint64_t Syscall::sys_readdir(uint64_t path, uint64_t entries, uint64_t count) {
    char pathname[256];
    uint64_t error = 0;
    if (!copyUserPathOrError(path, pathname, error)) {
        return error;
    }

    if (pathParentDeniesSearch(pathname)) {
        return syscall_error(SysErrAccess);
    }

    FileStats stats {};
    if (VFS::get().stat(pathname, &stats) != 0) {
        return missingPathError(pathname);
    }
    if (stats.type != FileType::Directory) {
        return syscall_error(SysErrNotDirectory);
    }
    if ((stats.mode & kModeReadBits) == 0) {
        return syscall_error(SysErrAccess);
    }
    
    if (count > (~0ULL / sizeof(DirEntry))) {
        return syscall_error(SysErrRange);
    }

    if (!isValidUserPointer(entries, count * sizeof(DirEntry))) {
        return syscall_error(SysErrInvalid);
    }
    
    ::DirEntry* vfsEntries = new ::DirEntry[count];
    if (!vfsEntries) {
        return syscall_error(SysErrNoMemory);
    }
    
    uint64_t readCount = 0;
    int result = VFS::get().readdir(pathname, vfsEntries, count, &readCount);
    
    if (result != 0) {
        delete[] vfsEntries;
        return syscall_error(SysErrNoEntry);
    }

    if (readCount == 0) {
        delete[] vfsEntries;
        return 0;
    }
    
    DirEntry* outEntries = new DirEntry[readCount];
    if (!outEntries) {
        delete[] vfsEntries;
        return syscall_error(SysErrNoMemory);
    }

    for (uint64_t i = 0; i < readCount; i++) {
        size_t j = 0;
        while (vfsEntries[i].name[j] && j < 255) {
            outEntries[i].name[j] = vfsEntries[i].name[j];
            j++;
        }
        outEntries[i].name[j] = '\0';
        
        outEntries[i].inode = vfsEntries[i].inode;
        outEntries[i].type = vfsEntries[i].type;
    }
    
    bool copied = copyToUser(entries, outEntries, readCount * sizeof(DirEntry));
    delete[] outEntries;
    delete[] vfsEntries;
    return copied ? readCount : syscall_error(SysErrInvalid);
}
