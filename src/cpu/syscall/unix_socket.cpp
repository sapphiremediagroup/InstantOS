// AF_UNIX sockets: what a window system needs to hand a program's GPU buffers
// (dma-buf fds) and fences (sync_file fds) to the compositor, and what
// Wayland, D-Bus and most local servers speak.
//
// - SOCK_STREAM and SOCK_SEQPACKET, bound to a path (kept in a kernel table,
//   absolute after the cwd; no file is made in the file system) or to an
//   abstract name (sun_path[0] == 0); listen/accept/connect, socketpair (also
//   SOCK_DGRAM, which gets seqpacket's record boundaries).
// - SCM_RIGHTS: SocketSendMsg carries file-like handles (files, pipes,
//   sockets: what fork and spawn would pass on) with the bytes; each is
//   retained while in flight and becomes a new handle of the receiver when the
//   bytes it came with are read. As on Linux a stream read never runs past
//   bytes that carry handles into later ones, and handles nobody takes (a
//   plain read) are dropped.
// - SO_PEERCRED: the pid/uid/gid of the peer when it connected.
//
// Each receive queue holds up to 256 KiB. Every state change wakes the
// sleepers (Scheduler::wakeAllBlockedProcesses), as the IPv4 sockets do.
#include <cpu/syscall/unix_socket.hpp>
#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <common/string.hpp>
#include <time/tsc_timer.hpp>

namespace unixsock {
namespace {

constexpr int kSockStream = 1;
constexpr int kSockDgram = 2;
constexpr int kSockSeqpacket = 5;
constexpr int kSockNonblock = 04000;
constexpr int kSockCloexec = 02000000;
constexpr uint64_t kMsgPeek = 0x2;
constexpr uint64_t kMsgDontWait = 0x40;
constexpr uint64_t kMsgWaitAll = 0x100;
constexpr uint64_t kMsgCmsgCloexec = 0x40000000;
constexpr int kSolSocket = 1;
constexpr int kSoType = 3;
constexpr int kSoError = 4;
constexpr int kSoSendBuffer = 7;
constexpr int kSoReceiveBuffer = 8;
constexpr int kSoPassCred = 16;
constexpr int kSoPeerCred = 17;
constexpr int kSoReceiveTimeout = 20;
constexpr int kSoSendTimeout = 21;
constexpr int kSoAcceptConn = 30;
constexpr int kSoDomain = 39;
constexpr int kSoProtocol = 38;
constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollHup = 0x0010;
constexpr int16_t kPollRdHup = 0x2000;
constexpr uint64_t kQueueCapacity = 256 * 1024;
constexpr uint64_t kPathMax = 108;                   // sizeof(sockaddr_un::sun_path)
constexpr uint64_t kAddressMax = 2 + kPathMax;
constexpr uint32_t kSocketRights =
    HandleRightRead | HandleRightWrite | HandleRightDuplicate | HandleRightControl | HandleRightWait;

struct Ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct Timeval {
    int64_t seconds;
    int64_t microseconds;
};

// Bytes sent in one call, with the handles that came with them.
struct Chunk {
    uint8_t* data;
    uint64_t length;
    uint64_t offset;         // stream: how much was read already
    HandleEntry* rights;
    uint32_t rightsCount;
    Chunk* next;
};

} // namespace

struct UnixSocket {
    uint32_t refs;
    int type;
    bool nonBlocking;
    bool bound;
    bool listening;
    bool connected;          // has (or had) a peer
    bool readShut;           // shutdown(SHUT_RD) here
    bool writeShut;          // shutdown(SHUT_WR) here
    bool peerWriteShut;      // the peer will send nothing more (shutdown or gone)
    bool passCred;
    int backlog;
    uint8_t address[kAddressMax];   // as bound: family + sun_path (abstract: leading 0)
    uint64_t addressLength;
    char key[kPathMax + 1];         // the name in the table: absolute path, or abstract bytes
    uint64_t keyLength;
    bool abstract;
    UnixSocket* peer;
    Ucred peerCred;
    Chunk* rxHead;
    Chunk* rxTail;
    uint64_t rxBytes;
    UnixSocket* acceptHead;  // connections waiting for accept (each holds a reference)
    UnixSocket* acceptTail;
    UnixSocket* acceptNext;
    uint32_t acceptCount;
    uint64_t receiveTimeoutMs;
    uint64_t sendTimeoutMs;
    UnixSocket* nextBound;
};

namespace {

UnixSocket* gBound = nullptr;   // sockets with a name

void releaseSocket(void* object);
void retainSocket(void* object);

void wakeAll() {
    Scheduler::get().wakeAllBlockedProcesses();
}

void releaseRights(HandleEntry* rights, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (rights[i].release) {
            rights[i].release(rights[i].object);
        }
    }
    delete[] rights;
}

void freeChunk(Chunk* chunk) {
    if (chunk->rights) {
        releaseRights(chunk->rights, chunk->rightsCount);
    }
    delete[] chunk->data;
    delete chunk;
}

void unlinkBound(UnixSocket* socket) {
    for (UnixSocket** link = &gBound; *link; link = &(*link)->nextBound) {
        if (*link == socket) {
            *link = socket->nextBound;
            socket->nextBound = nullptr;
            return;
        }
    }
}

// The peer stops sending: what is queued can still be read, then EOF.
void detachPeer(UnixSocket* socket) {
    UnixSocket* peer = socket->peer;
    if (!peer) {
        return;
    }
    socket->peer = nullptr;
    if (peer->peer == socket) {
        peer->peer = nullptr;
        peer->peerWriteShut = true;
    }
}

void destroy(UnixSocket* socket) {
    unlinkBound(socket);
    detachPeer(socket);
    // Connections nobody accepted: their clients see EOF.
    UnixSocket* pending = socket->acceptHead;
    socket->acceptHead = socket->acceptTail = nullptr;
    while (pending) {
        UnixSocket* next = pending->acceptNext;
        pending->acceptNext = nullptr;
        releaseSocket(pending);
        pending = next;
    }
    Chunk* chunk = socket->rxHead;
    while (chunk) {
        Chunk* next = chunk->next;
        freeChunk(chunk);
        chunk = next;
    }
    delete socket;
    wakeAll();
}

void retainSocket(void* object) {
    if (auto* socket = static_cast<UnixSocket*>(object)) {
        socket->refs++;
    }
}

void releaseSocket(void* object) {
    auto* socket = static_cast<UnixSocket*>(object);
    if (!socket) {
        return;
    }
    if (socket->refs > 0) {
        socket->refs--;
    }
    if (socket->refs == 0) {
        destroy(socket);
    }
}

UnixSocket* newSocket(int type) {
    auto* socket = new UnixSocket {};
    if (!socket) {
        return nullptr;
    }
    socket->refs = 1;
    socket->type = type;
    return socket;
}

Ucred credOf(Process* process) {
    Ucred cred {};
    if (process) {
        cred.pid = static_cast<int32_t>(process->getPID());
        cred.uid = process->getUID();
        cred.gid = process->getGID();
    }
    return cred;
}

uint64_t installSocket(UnixSocket* socket, bool cloexec) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }
    const uint64_t handle = current->allocateHandle(HandleType::Socket, kSocketRights, socket, retainSocket, releaseSocket);
    if (handle == static_cast<uint64_t>(-1)) {
        return syscall_error(SysErrNoMemory);
    }
    if (cloexec) {
        current->setHandleCloseOnExec(handle, true);
    }
    return handle;
}

uint64_t deadline(uint64_t timeoutMs) {
    return timeoutMs == 0 ? 0 : time_get_uptime_ms() + timeoutMs;
}

// Sleep until something changes, the deadline passes (-EAGAIN, as Linux's
// SO_RCVTIMEO) or a signal comes (-EINTR). 0: look again.
uint64_t waitForEvent(UnixSocket* socket, uint64_t deadlineMs) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }
    if (current->hasDeliverableSignal()) {
        return syscall_error(SysErrInterrupted);
    }
    if (deadlineMs != 0) {
        if (time_get_uptime_ms() >= deadlineMs) {
            return syscall_error(SysErrAgain);
        }
        current->sleepUntil(deadlineMs, true);
    }
    retainSocket(socket);
    current->setState(ProcessState::Blocked);
    Scheduler::get().scheduleFromSyscall();
    current->clearSleep();
    const bool closed = socket->refs == 1;
    releaseSocket(socket);
    if (closed) {
        return syscall_error(SysErrBadFile);
    }
    if (current->hasDeliverableSignal()) {
        return syscall_error(SysErrInterrupted);
    }
    return 0;
}

// Parses a sockaddr_un into the name the table keys on.
uint64_t parseAddress(uint64_t address, uint64_t length, uint8_t* raw, uint64_t* rawLength,
                      char* key, uint64_t* keyLength, bool* abstract) {
    if (address == 0 || length <= 2 || length > kAddressMax) {
        return syscall_error(SysErrInvalid);
    }
    memset(raw, 0, kAddressMax);
    if (!Syscall::copyFromUser(raw, address, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }
    uint16_t family = 0;
    memcpy(&family, raw, sizeof(family));
    if (family != kAfUnix) {
        return syscall_error(SysErrAddressFamilyNotSupported);
    }
    const char* path = reinterpret_cast<const char*>(raw + 2);
    const uint64_t pathRoom = length - 2;
    if (path[0] == '\0') {
        // Abstract: every byte of the given length is the name.
        *abstract = true;
        memcpy(key, path, pathRoom);
        *keyLength = pathRoom;
        *rawLength = length;
        return 0;
    }
    *abstract = false;
    uint64_t pathLength = 0;
    while (pathLength < pathRoom && path[pathLength] != '\0') {
        pathLength++;
    }
    char relative[kPathMax + 1];
    memcpy(relative, path, pathLength);
    relative[pathLength] = '\0';
    char absolute[512];
    if (!Syscall::makeAbsolutePath(relative, absolute)) {
        return syscall_error(SysErrNameTooLong);
    }
    const uint64_t absoluteLength = strlen(absolute);
    if (absoluteLength > kPathMax) {
        return syscall_error(SysErrNameTooLong);
    }
    memcpy(key, absolute, absoluteLength + 1);
    *keyLength = absoluteLength;
    *rawLength = 2 + pathLength + 1;   // as Linux reports it: the path and its NUL
    if (*rawLength > kAddressMax) {
        *rawLength = kAddressMax;
    }
    return 0;
}

UnixSocket* findBound(const char* key, uint64_t keyLength, bool abstract) {
    for (UnixSocket* s = gBound; s; s = s->nextBound) {
        if (s->abstract == abstract && s->keyLength == keyLength && memcmp(s->key, key, keyLength) == 0) {
            return s;
        }
    }
    return nullptr;
}

// sockaddr out with Linux's length rules: *lengthPtr is the room in and the
// full length out; the copy is cut to the room.
uint64_t copyAddressOut(const uint8_t* raw, uint64_t rawLength, uint64_t address, uint64_t lengthPtr) {
    if (address == 0 || lengthPtr == 0) {
        return 0;
    }
    uint32_t room = 0;
    if (!Syscall::copyFromUser(&room, lengthPtr, sizeof(room))) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t unnamed[2] = {kAfUnix, 0};
    if (rawLength == 0) {
        raw = unnamed;
        rawLength = 2;
    }
    const uint64_t n = room < rawLength ? room : rawLength;
    const uint32_t full = static_cast<uint32_t>(rawLength);
    if ((n != 0 && !Syscall::copyToUser(address, raw, static_cast<size_t>(n))) ||
        !Syscall::copyToUser(lengthPtr, &full, sizeof(full))) {
        return syscall_error(SysErrInvalid);
    }
    return 0;
}

bool isRecordType(const UnixSocket* socket) {
    return socket->type == kSockSeqpacket || socket->type == kSockDgram;
}

// Takes the handles to pass from the caller's table: each is retained for
// the time in flight.
uint64_t collectRights(uint64_t handlesPtr, uint32_t count, HandleEntry** out) {
    *out = nullptr;
    if (count == 0) {
        return 0;
    }
    if (count > kSocketMaxRights || handlesPtr == 0) {
        return syscall_error(SysErrInvalid);
    }
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }
    auto* handles = new uint64_t[count];
    auto* rights = new HandleEntry[count];
    if (!handles || !rights) {
        delete[] handles;
        delete[] rights;
        return syscall_error(SysErrNoMemory);
    }
    if (!Syscall::copyFromUser(handles, handlesPtr, count * sizeof(uint64_t))) {
        delete[] handles;
        delete[] rights;
        return syscall_error(SysErrInvalid);
    }
    for (uint32_t i = 0; i < count; i++) {
        HandleEntry* entry = current->getHandle(handles[i]);
        if (!entry || (entry->type != HandleType::File && entry->type != HandleType::Pipe &&
                       entry->type != HandleType::Socket)) {
            for (uint32_t j = 0; j < i; j++) {
                if (rights[j].release) {
                    rights[j].release(rights[j].object);
                }
            }
            delete[] handles;
            delete[] rights;
            return syscall_error(SysErrBadFile);
        }
        rights[i] = *entry;
        rights[i].closeOnExec = false;
        if (rights[i].retain) {
            rights[i].retain(rights[i].object);
        }
    }
    delete[] handles;
    *out = rights;
    return 0;
}

// Room left in `target`'s receive queue.
uint64_t queueRoom(const UnixSocket* target) {
    return target->rxBytes >= kQueueCapacity ? 0 : kQueueCapacity - target->rxBytes;
}

// Sends `length` bytes (and the handles, which go with the first byte). A
// stream may send part when the peer's queue is short of room (at least one
// byte once it has any); a record goes whole or waits.
uint64_t sendCommon(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags,
                    HandleEntry* rights, uint32_t rightsCount) {
    auto fail = [&](uint64_t error) {
        if (rights) {
            releaseRights(rights, rightsCount);
        }
        return error;
    };
    if (!socket->connected) {
        return fail(syscall_error(SysErrNotConnected));
    }
    if (isRecordType(socket) && length > kQueueCapacity) {
        return fail(syscall_error(SysErrMessageSize));
    }
    const bool nonBlocking = socket->nonBlocking || (flags & kMsgDontWait) != 0;
    const uint64_t until = deadline(socket->sendTimeoutMs);
    for (;;) {
        if (socket->writeShut) {
            return fail(syscall_error(SysErrBrokenPipe));
        }
        UnixSocket* peer = socket->peer;
        if (!peer || peer->readShut) {
            return fail(syscall_error(SysErrBrokenPipe));
        }
        const uint64_t room = queueRoom(peer);
        const bool fits = isRecordType(socket) ? room >= length : (room > 0 || length == 0);
        if (fits) {
            const uint64_t n = isRecordType(socket) ? length : (length < room ? length : room);
            if (n == 0 && !isRecordType(socket) && !rights) {
                return 0;
            }
            auto* chunk = new Chunk {};
            uint8_t* data = new uint8_t[n ? n : 1];
            if (!chunk || !data) {
                delete chunk;
                delete[] data;
                return fail(syscall_error(SysErrNoMemory));
            }
            if (n != 0 && !Syscall::copyFromUser(data, buffer, static_cast<size_t>(n))) {
                delete chunk;
                delete[] data;
                return fail(syscall_error(SysErrInvalid));
            }
            chunk->data = data;
            chunk->length = n;
            chunk->rights = rights;
            chunk->rightsCount = rightsCount;
            if (peer->rxTail) {
                peer->rxTail->next = chunk;
            } else {
                peer->rxHead = chunk;
            }
            peer->rxTail = chunk;
            peer->rxBytes += n;
            wakeAll();
            return n;
        }
        if (nonBlocking) {
            return fail(syscall_error(SysErrAgain));
        }
        const uint64_t waited = waitForEvent(socket, until);
        if (waited != 0) {
            return fail(waited);
        }
    }
}

// Makes the handles of a chunk the receiver's, as many as it has room for.
uint32_t installRights(Chunk* chunk, uint64_t* handlesOut, uint32_t capacity, bool cloexec, bool* truncated) {
    Process* current = Scheduler::get().getCurrentProcess();
    uint32_t installed = 0;
    for (uint32_t i = 0; i < chunk->rightsCount; i++) {
        HandleEntry& right = chunk->rights[i];
        uint64_t handle = static_cast<uint64_t>(-1);
        if (current && installed < capacity) {
            handle = current->allocateHandle(right.type, right.rights, right.object, right.retain, right.release);
        }
        if (handle == static_cast<uint64_t>(-1)) {
            if (right.release) {
                right.release(right.object);   // dropped: nobody holds the in-flight reference now
            }
            *truncated = true;
            continue;
        }
        if (cloexec) {
            current->setHandleCloseOnExec(handle, true);
        }
        handlesOut[installed++] = handle;   // the in-flight reference is the handle's now
    }
    delete[] chunk->rights;
    chunk->rights = nullptr;
    chunk->rightsCount = 0;
    return installed;
}

struct RecvResult {
    uint64_t bytes;
    uint32_t handleCount;
    uint32_t flags;
};

uint64_t recvCommon(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags,
                    uint64_t* handlesOut, uint32_t handleCapacity, RecvResult* result) {
    *result = {};
    if (socket->listening) {
        return syscall_error(SysErrNotConnected);
    }
    if (!socket->connected) {
        return syscall_error(SysErrNotConnected);
    }
    const bool nonBlocking = socket->nonBlocking || (flags & kMsgDontWait) != 0;
    const bool peek = (flags & kMsgPeek) != 0;
    const bool waitAll = (flags & kMsgWaitAll) != 0 && !isRecordType(socket) && !peek;
    const bool cloexec = (flags & kMsgCmsgCloexec) != 0;
    const uint64_t until = deadline(socket->receiveTimeoutMs);
    uint64_t copied = 0;
    for (;;) {
        if (socket->rxHead && !socket->readShut) {
            if (isRecordType(socket)) {
                Chunk* chunk = socket->rxHead;
                const uint64_t n = chunk->length < length ? chunk->length : length;
                if (n != 0 && !Syscall::copyToUser(buffer, chunk->data, static_cast<size_t>(n))) {
                    return syscall_error(SysErrInvalid);
                }
                if (chunk->length > length) {
                    result->flags |= kSocketMsgTrunc;
                }
                if (peek) {
                    result->bytes = n;
                    return 0;
                }
                bool truncated = false;
                if (chunk->rights) {
                    result->handleCount = installRights(chunk, handlesOut, handlesOut ? handleCapacity : 0, cloexec, &truncated);
                }
                if (truncated) {
                    result->flags |= kSocketMsgCtrunc;
                }
                socket->rxHead = chunk->next;
                if (!socket->rxHead) {
                    socket->rxTail = nullptr;
                }
                socket->rxBytes -= chunk->length;
                freeChunk(chunk);
                wakeAll();
                result->bytes = n;
                return 0;
            }
            // Stream: read across chunks, but not into bytes that carry handles
            // once something was read, nor past the handles' own chunk.
            Chunk* chunk = socket->rxHead;
            bool tookRights = false;
            while (chunk && copied < length) {
                if (chunk->rights && (copied != 0 || tookRights)) {
                    break;
                }
                if (chunk->rights && !peek) {
                    bool truncated = false;
                    result->handleCount = installRights(chunk, handlesOut, handlesOut ? handleCapacity : 0, cloexec, &truncated);
                    if (truncated) {
                        result->flags |= kSocketMsgCtrunc;
                    }
                    tookRights = true;
                }
                const uint64_t available = chunk->length - chunk->offset;
                const uint64_t n = available < length - copied ? available : length - copied;
                if (n != 0 && !Syscall::copyToUser(buffer + copied, chunk->data + chunk->offset, static_cast<size_t>(n))) {
                    return syscall_error(SysErrInvalid);
                }
                copied += n;
                if (peek) {
                    chunk = chunk->next;
                    if (tookRights || (chunk && chunk->rights)) {
                        break;
                    }
                    continue;
                }
                chunk->offset += n;
                socket->rxBytes -= n;
                if (chunk->offset == chunk->length) {
                    Chunk* next = chunk->next;
                    socket->rxHead = next;
                    if (!next) {
                        socket->rxTail = nullptr;
                    }
                    freeChunk(chunk);
                    chunk = next;
                    if (tookRights) {
                        break;
                    }
                }
            }
            if (!peek) {
                wakeAll();
            }
            if (!waitAll || copied >= length || tookRights) {
                result->bytes = copied;
                return 0;
            }
        }
        if (socket->readShut || socket->peerWriteShut || (socket->connected && !socket->peer)) {
            result->bytes = copied;   // EOF (or what WAITALL got before it)
            return 0;
        }
        if (copied != 0 && !waitAll) {
            result->bytes = copied;
            return 0;
        }
        if (nonBlocking) {
            if (copied != 0) {
                result->bytes = copied;
                return 0;
            }
            return syscall_error(SysErrAgain);
        }
        const uint64_t waited = waitForEvent(socket, until);
        if (waited != 0) {
            if (copied != 0) {
                result->bytes = copied;
                return 0;
            }
            return waited;
        }
    }
}

int validType(int type) {
    const int base = type & ~(kSockNonblock | kSockCloexec);
    return base == kSockStream || base == kSockSeqpacket || base == kSockDgram ? base : -1;
}

} // namespace

bool isUnixEntry(const HandleEntry* entry) {
    return entry && entry->type == HandleType::Socket && entry->release == releaseSocket;
}

UnixSocket* fromHandle(uint64_t handle) {
    Process* current = Scheduler::get().getCurrentProcess();
    HandleEntry* entry = current ? current->getHandle(handle) : nullptr;
    return isUnixEntry(entry) ? static_cast<UnixSocket*>(entry->object) : nullptr;
}

uint64_t create(int type, int protocol) {
    const int base = validType(type);
    if (base < 0) {
        return syscall_error(SysErrInvalid);
    }
    if (protocol != 0) {
        return syscall_error(SysErrProtocolNotSupported);
    }
    if (base == kSockDgram) {
        // Unconnected datagrams (bind + sendto) are not there yet; socketpair has them.
        return syscall_error(SysErrProtocolNotSupported);
    }
    UnixSocket* socket = newSocket(base);
    if (!socket) {
        return syscall_error(SysErrNoMemory);
    }
    socket->nonBlocking = (type & kSockNonblock) != 0;
    const uint64_t handle = installSocket(socket, (type & kSockCloexec) != 0);
    if (static_cast<int64_t>(handle) < 0) {
        delete socket;
    }
    return handle;
}

uint64_t socketPair(int type, int protocol, uint64_t handlesOut) {
    const int base = validType(type);
    if (base < 0) {
        return syscall_error(SysErrInvalid);
    }
    if (protocol != 0) {
        return syscall_error(SysErrProtocolNotSupported);
    }
    if (!Syscall::isValidUserPointer(handlesOut, 2 * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }
    UnixSocket* a = newSocket(base);
    UnixSocket* b = newSocket(base);
    if (!a || !b) {
        delete a;
        delete b;
        return syscall_error(SysErrNoMemory);
    }
    Process* current = Scheduler::get().getCurrentProcess();
    a->peer = b;
    b->peer = a;
    a->connected = b->connected = true;
    a->peerCred = b->peerCred = credOf(current);
    a->nonBlocking = b->nonBlocking = (type & kSockNonblock) != 0;
    const bool cloexec = (type & kSockCloexec) != 0;
    uint64_t handles[2];
    handles[0] = installSocket(a, cloexec);
    if (static_cast<int64_t>(handles[0]) < 0) {
        delete a;   // nothing holds it yet
        b->peer = nullptr;
        delete b;
        return handles[0];
    }
    handles[1] = installSocket(b, cloexec);
    if (static_cast<int64_t>(handles[1]) < 0) {
        delete b;
        a->peer = nullptr;
        current->closeHandle(handles[0]);
        return handles[1];
    }
    if (!Syscall::copyToUser(handlesOut, handles, sizeof(handles))) {
        current->closeHandle(handles[0]);
        current->closeHandle(handles[1]);
        return syscall_error(SysErrInvalid);
    }
    return 0;
}

uint64_t bind(UnixSocket* socket, uint64_t address, uint64_t length) {
    if (socket->bound) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t raw[kAddressMax];
    uint64_t rawLength = 0;
    char key[kPathMax + 1];
    uint64_t keyLength = 0;
    bool abstract = false;
    if (uint64_t e = parseAddress(address, length, raw, &rawLength, key, &keyLength, &abstract); e != 0) {
        return e;
    }
    if (findBound(key, keyLength, abstract)) {
        return syscall_error(SysErrAddressInUse);
    }
    memcpy(socket->address, raw, rawLength);
    socket->addressLength = rawLength;
    memcpy(socket->key, key, keyLength);
    socket->key[keyLength] = '\0';
    socket->keyLength = keyLength;
    socket->abstract = abstract;
    socket->bound = true;
    socket->nextBound = gBound;
    gBound = socket;
    return 0;
}

uint64_t listen(UnixSocket* socket, uint64_t backlog) {
    if (socket->type == kSockDgram) {
        return syscall_error(SysErrOperationNotSupported);
    }
    if (!socket->bound || socket->connected) {
        return syscall_error(SysErrInvalid);
    }
    socket->listening = true;
    socket->peerCred = credOf(Scheduler::get().getCurrentProcess());   // what connecting clients see
    const int64_t wanted = static_cast<int64_t>(backlog);
    socket->backlog = wanted <= 0 ? 1 : (wanted > 4096 ? 4096 : static_cast<int>(wanted));
    wakeAll();
    return 0;
}

uint64_t connect(UnixSocket* socket, uint64_t address, uint64_t length) {
    if (socket->listening) {
        return syscall_error(SysErrInvalid);
    }
    if (socket->connected) {
        return syscall_error(SysErrIsConnected);
    }
    uint8_t raw[kAddressMax];
    uint64_t rawLength = 0;
    char key[kPathMax + 1];
    uint64_t keyLength = 0;
    bool abstract = false;
    if (uint64_t e = parseAddress(address, length, raw, &rawLength, key, &keyLength, &abstract); e != 0) {
        return e;
    }
    const uint64_t until = deadline(socket->sendTimeoutMs);
    for (;;) {
        UnixSocket* listener = findBound(key, keyLength, abstract);
        if (!listener) {
            return syscall_error(abstract ? SysErrConnectionRefused : SysErrNoEntry);
        }
        if (!listener->listening || listener->type != socket->type) {
            return syscall_error(SysErrConnectionRefused);
        }
        if (listener->acceptCount < static_cast<uint32_t>(listener->backlog)) {
            UnixSocket* server = newSocket(socket->type);
            if (!server) {
                return syscall_error(SysErrNoMemory);
            }
            Process* current = Scheduler::get().getCurrentProcess();
            server->peer = socket;
            socket->peer = server;
            server->connected = socket->connected = true;
            server->peerCred = credOf(current);
            socket->peerCred = listener->peerCred;   // the listener's owner, recorded at listen
            // The server end reports the listener's name (getsockname), the
            // client end it as its peer.
            memcpy(server->address, listener->address, listener->addressLength);
            server->addressLength = listener->addressLength;
            server->passCred = listener->passCred;
            if (listener->acceptTail) {
                listener->acceptTail->acceptNext = server;
            } else {
                listener->acceptHead = server;
            }
            listener->acceptTail = server;
            listener->acceptCount++;
            wakeAll();
            return 0;
        }
        if (socket->nonBlocking) {
            return syscall_error(SysErrAgain);
        }
        const uint64_t waited = waitForEvent(socket, until);
        if (waited != 0) {
            return waited;
        }
    }
}

uint64_t accept(UnixSocket* socket, uint64_t address, uint64_t lengthPtr) {
    if (!socket->listening) {
        return syscall_error(SysErrInvalid);
    }
    const uint64_t until = deadline(socket->receiveTimeoutMs);
    for (;;) {
        if (UnixSocket* server = socket->acceptHead) {
            socket->acceptHead = server->acceptNext;
            if (!socket->acceptHead) {
                socket->acceptTail = nullptr;
            }
            server->acceptNext = nullptr;
            socket->acceptCount--;
            // The queue's reference becomes the handle's.
            Process* current = Scheduler::get().getCurrentProcess();
            const uint64_t handle = current
                ? current->allocateHandle(HandleType::Socket, kSocketRights, server, retainSocket, releaseSocket)
                : static_cast<uint64_t>(-1);
            if (handle == static_cast<uint64_t>(-1)) {
                releaseSocket(server);
                return syscall_error(SysErrNoMemory);
            }
            // The client's name, if it bound one.
            UnixSocket* client = server->peer;
            if (uint64_t e = copyAddressOut(client ? client->address : nullptr, client ? client->addressLength : 0,
                                            address, lengthPtr); e != 0) {
                current->closeHandle(handle);
                return e;
            }
            wakeAll();   // a connect waiting for backlog room
            return handle;
        }
        if (socket->nonBlocking) {
            return syscall_error(SysErrAgain);
        }
        const uint64_t waited = waitForEvent(socket, until);
        if (waited != 0) {
            return waited;
        }
    }
}

uint64_t send(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags) {
    return sendCommon(socket, buffer, length, flags, nullptr, 0);
}

uint64_t recv(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags) {
    RecvResult result;
    const uint64_t e = recvCommon(socket, buffer, length, flags, nullptr, 0, &result);
    return e != 0 ? e : result.bytes;
}

uint64_t sendMsg(UnixSocket* socket, uint64_t messagePtr, uint64_t flags) {
    SocketMessage message {};
    if (!Syscall::copyFromUser(&message, messagePtr, sizeof(message))) {
        return syscall_error(SysErrInvalid);
    }
    HandleEntry* rights = nullptr;
    if (uint64_t e = collectRights(message.handles, message.handleCount, &rights); e != 0) {
        return e;
    }
    return sendCommon(socket, message.buffer, message.length, flags, rights, message.handleCount);
}

uint64_t recvMsg(UnixSocket* socket, uint64_t messagePtr, uint64_t flags) {
    SocketMessage message {};
    if (!Syscall::copyFromUser(&message, messagePtr, sizeof(message))) {
        return syscall_error(SysErrInvalid);
    }
    uint32_t capacity = message.handleCount;
    if (capacity > kSocketMaxRights) {
        capacity = kSocketMaxRights;
    }
    if (capacity != 0 && !Syscall::isValidUserPointer(message.handles, capacity * sizeof(uint64_t))) {
        return syscall_error(SysErrInvalid);
    }
    uint64_t* handles = capacity ? new uint64_t[capacity] : nullptr;
    if (capacity && !handles) {
        return syscall_error(SysErrNoMemory);
    }
    RecvResult result;
    const uint64_t e = recvCommon(socket, message.buffer, message.length, flags, handles, capacity, &result);
    if (e != 0) {
        delete[] handles;
        return e;
    }
    message.handleCount = result.handleCount;
    message.flags = result.flags;
    if ((result.handleCount != 0 &&
         !Syscall::copyToUser(message.handles, handles, result.handleCount * sizeof(uint64_t))) ||
        !Syscall::copyToUser(messagePtr, &message, sizeof(message))) {
        delete[] handles;
        return syscall_error(SysErrInvalid);   // the handles stay installed, as on Linux
    }
    delete[] handles;
    return result.bytes;
}

uint64_t shutdown(UnixSocket* socket, uint64_t how) {
    if (how > 2) {
        return syscall_error(SysErrInvalid);
    }
    if (!socket->connected) {
        return syscall_error(SysErrNotConnected);
    }
    if (how == 0 || how == 2) {
        socket->readShut = true;
    }
    if (how == 1 || how == 2) {
        socket->writeShut = true;
        if (socket->peer) {
            socket->peer->peerWriteShut = true;
        }
    }
    wakeAll();
    return 0;
}

uint64_t getsockopt(UnixSocket* socket, uint64_t level, uint64_t option, uint64_t value, uint64_t lengthPtr) {
    if (level != kSolSocket) {
        return syscall_error(SysErrProtoOpt);
    }
    uint32_t room = 0;
    if (!Syscall::copyFromUser(&room, lengthPtr, sizeof(room))) {
        return syscall_error(SysErrInvalid);
    }
    auto putInt = [&](int v) -> uint64_t {
        if (room < sizeof(int)) {
            return syscall_error(SysErrInvalid);
        }
        const uint32_t n = sizeof(int);
        if (!Syscall::copyToUser(value, &v, sizeof(v)) || !Syscall::copyToUser(lengthPtr, &n, sizeof(n))) {
            return syscall_error(SysErrInvalid);
        }
        return 0;
    };
    switch (static_cast<int>(option)) {
    case kSoType: return putInt(socket->type);
    case kSoError: return putInt(0);
    case kSoDomain: return putInt(kAfUnix);
    case kSoProtocol: return putInt(0);
    case kSoAcceptConn: return putInt(socket->listening ? 1 : 0);
    case kSoPassCred: return putInt(socket->passCred ? 1 : 0);
    case kSoSendBuffer:
    case kSoReceiveBuffer: return putInt(static_cast<int>(kQueueCapacity));
    case kSoPeerCred: {
        if (room < sizeof(Ucred)) {
            return syscall_error(SysErrInvalid);
        }
        Ucred cred = socket->connected ? socket->peerCred : Ucred {0, static_cast<uint32_t>(-1), static_cast<uint32_t>(-1)};
        const uint32_t n = sizeof(cred);
        if (!Syscall::copyToUser(value, &cred, sizeof(cred)) || !Syscall::copyToUser(lengthPtr, &n, sizeof(n))) {
            return syscall_error(SysErrInvalid);
        }
        return 0;
    }
    case kSoReceiveTimeout:
    case kSoSendTimeout: {
        if (room < sizeof(Timeval)) {
            return syscall_error(SysErrInvalid);
        }
        const uint64_t ms = option == kSoReceiveTimeout ? socket->receiveTimeoutMs : socket->sendTimeoutMs;
        Timeval tv {static_cast<int64_t>(ms / 1000), static_cast<int64_t>((ms % 1000) * 1000)};
        const uint32_t n = sizeof(tv);
        if (!Syscall::copyToUser(value, &tv, sizeof(tv)) || !Syscall::copyToUser(lengthPtr, &n, sizeof(n))) {
            return syscall_error(SysErrInvalid);
        }
        return 0;
    }
    default:
        return syscall_error(SysErrProtoOpt);
    }
}

uint64_t setsockopt(UnixSocket* socket, uint64_t level, uint64_t option, uint64_t value, uint64_t length) {
    if (level != kSolSocket) {
        return syscall_error(SysErrProtoOpt);
    }
    switch (static_cast<int>(option)) {
    case kSoReceiveTimeout:
    case kSoSendTimeout: {
        Timeval tv {};
        if (length < sizeof(tv) || !Syscall::copyFromUser(&tv, value, sizeof(tv)) || tv.seconds < 0 ||
            tv.microseconds < 0 || tv.microseconds >= 1000000) {
            return syscall_error(SysErrInvalid);
        }
        uint64_t ms = static_cast<uint64_t>(tv.seconds) * 1000 + static_cast<uint64_t>(tv.microseconds + 999) / 1000;
        (option == kSoReceiveTimeout ? socket->receiveTimeoutMs : socket->sendTimeoutMs) = ms;
        return 0;
    }
    case kSoPassCred: {
        int on = 0;
        if (length < sizeof(on) || !Syscall::copyFromUser(&on, value, sizeof(on))) {
            return syscall_error(SysErrInvalid);
        }
        socket->passCred = on != 0;   // recorded; SCM_CREDENTIALS are not sent yet
        return 0;
    }
    case kSoSendBuffer:
    case kSoReceiveBuffer:
        return 0;   // the queues keep their size
    default:
        return syscall_error(SysErrProtoOpt);
    }
}

uint64_t getsockname(UnixSocket* socket, uint64_t address, uint64_t lengthPtr) {
    return copyAddressOut(socket->address, socket->addressLength, address, lengthPtr);
}

uint64_t getpeername(UnixSocket* socket, uint64_t address, uint64_t lengthPtr) {
    if (!socket->connected || !socket->peer) {
        return syscall_error(SysErrNotConnected);
    }
    return copyAddressOut(socket->peer->address, socket->peer->addressLength, address, lengthPtr);
}

int16_t poll(void* object, int16_t events) {
    auto* socket = static_cast<UnixSocket*>(object);
    if (!socket) {
        return 0;
    }
    int16_t revents = 0;
    if (socket->listening) {
        if (socket->acceptHead) {
            revents |= kPollIn;
        }
        return revents & events;
    }
    const bool ended = socket->connected && (!socket->peer || socket->peerWriteShut);
    if (socket->rxHead || socket->readShut || ended) {
        revents |= kPollIn;
    }
    if (socket->peerWriteShut || socket->readShut) {
        revents |= kPollRdHup;
    }
    if (socket->connected && socket->peer && !socket->writeShut && queueRoom(socket->peer) > 0) {
        revents |= kPollOut;
    }
    if (socket->connected && !socket->peer) {
        revents |= kPollHup;   // POLLHUP is reported whether asked for or not
        return static_cast<int16_t>((revents & events) | kPollHup);
    }
    return revents & events;
}

bool isNonBlocking(void* object) {
    auto* socket = static_cast<UnixSocket*>(object);
    return socket && socket->nonBlocking;
}

void setNonBlocking(void* object, bool nonBlocking) {
    if (auto* socket = static_cast<UnixSocket*>(object)) {
        socket->nonBlocking = nonBlocking;
    }
}

} // namespace unixsock
