#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/tcp_internal.hpp>
#include <cpu/cereal/cereal.hpp>
#include <common/string.hpp>
#include <common/krandom.hpp>
#include <time/tsc_timer.hpp>

namespace {
constexpr int kAfInet = 2;
constexpr int kSockStream = 1;
constexpr int kSockDgram = 2;
constexpr int kSockRaw = 3;
constexpr int kIpProtoTcp = 6;
constexpr int kIpProtoUdp = 17;
constexpr int kIpProtoIcmp = 1;
constexpr uint8_t kIcmpEchoReplyType = 0;
constexpr uint8_t kIcmpEchoRequestType = 8;
constexpr int kSolSocket = 1;
constexpr int kSoReuseAddr = 2;
constexpr int kSoKeepAlive = 9;
constexpr int kSoBroadcast = 6;
constexpr int kSoLinger = 13;
constexpr int kSoReceiveBuffer = 8;
constexpr int kSoSendBuffer = 7;
constexpr int kSoError = 4;
constexpr int kSoType = 3;
constexpr int kSoReceiveTimeout = 20;
constexpr int kSoSendTimeout = 21;
constexpr uint64_t kMsgDontWait = 0x40;
constexpr int kSockNonblock = 04000;     // SOCK_NONBLOCK (Linux ABI)
constexpr int kSockCloexec = 02000000;   // SOCK_CLOEXEC  (Linux ABI)
constexpr int kShutdownRead = 0;
constexpr int kShutdownWrite = 1;
constexpr int kShutdownBoth = 2;
constexpr uint64_t kMaxSocketAddressSize = 128;
constexpr uint64_t kMaxDatagramSize = 65507;
constexpr uint16_t kEphemeralPortStart = 49152;
constexpr uint16_t kEphemeralPortEnd = 65535;
constexpr uint32_t kLoopbackAddressNetworkOrder = 0x0100007f;
constexpr int16_t kPollIn = 0x0001;
constexpr int16_t kPollOut = 0x0004;
constexpr int16_t kPollErr = 0x0008;
constexpr int16_t kPollHup = 0x0010;
constexpr uint32_t kSocketRights =
    HandleRightRead | HandleRightWrite | HandleRightDuplicate | HandleRightControl | HandleRightWait;

struct SocketLinger {
    int on;
    int seconds;
};

struct SocketTimeval {
    int64_t seconds;
    int64_t microseconds;
};

// TCP connection states (subset of RFC 793 sufficient for a client + simple
// server).  CLOSED is the implicit default for a fresh socket.
enum class TcpState : uint8_t {
    Closed = 0,
    Listen,
    SynSent,
    SynReceived,
    Established,
    FinWait1,    // we sent FIN, awaiting ACK of it
    FinWait2,    // our FIN ACKed, awaiting peer FIN
    CloseWait,   // peer sent FIN, app may still send
    LastAck,     // we sent FIN after CLOSE_WAIT, awaiting its ACK
    Closing,     // simultaneous close
    TimeWait,    // waiting out 2*MSL
};

// TCP tuning constants.
constexpr uint32_t kTcpMss = 1460;                 // our MSS: a 1500-byte MTU less headers
constexpr uint32_t kTcpDefaultSendMss = 536;       // RFC 1122, when the peer sends no MSS
constexpr uint32_t kTcpReceiveBuffer = 256 * 1024; // receive queue + out-of-order bytes
constexpr uint8_t kTcpWindowScale = 3;             // 65535 << 3 covers the buffer
constexpr uint32_t kTcpSendBufferCap = 256 * 1024; // queued, unacknowledged + unsent
constexpr uint32_t kTcpInitialCwndSegments = 10;   // RFC 6928
constexpr uint64_t kTcpInitialRtoMs = 1000;        // RFC 6298 before an RTT sample
constexpr uint64_t kTcpMinRtoMs = 200;             // Linux's floor (RFC says 1 s)
constexpr uint64_t kTcpMaxRtoMs = 30000;           // ceiling after backoff
constexpr uint32_t kTcpSynRetries = 6;             // ~2 minutes, like Linux
constexpr uint32_t kTcpMaxRetries = 12;            // timeouts in a row before giving up
constexpr uint64_t kTcpTimeWaitMs = 2000;          // simplified 2*MSL
constexpr uint64_t kTcpDelayedAckMs = 20;          // with the 10 ms timer: 20-30 ms
constexpr uint32_t kTcpQuickAckSegments = 16;      // acknowledged at once while the peer's cwnd opens
constexpr uint64_t kTcpOrphanTimeoutMs = 60000;    // a closed connection's last chance
constexpr int kTcpNoDelay = 1;                     // IPPROTO_TCP options
constexpr int kTcpMaxSeg = 2;
constexpr int kTcpKeepIdle = 4;
constexpr int kTcpKeepInterval = 5;
constexpr int kTcpKeepCount = 6;
constexpr int kTcpInfoOption = 11;

// A contiguous run of unacknowledged outbound bytes (or a SYN/FIN control
// segment).  Retained until the peer ACKs past it so it can be retransmitted.
struct TcpSendSegment {
    uint32_t seq;          // sequence number of first byte
    uint32_t length;       // payload bytes (0 for a pure FIN/SYN control seg)
    uint8_t flags;         // TCP flags to set when (re)transmitting
    uint64_t lastSentMs;   // time of last (re)transmission
    uint32_t transmissions; // times sent; more than 1 means no RTT sample (Karn)
    uint8_t* data;         // owned payload buffer (nullptr if length == 0)
    TcpSendSegment* next;
};

// An out-of-order received segment held until the gap before it fills.
struct TcpReassemblySegment {
    uint32_t seq;
    uint32_t length;
    uint8_t* data;
    TcpReassemblySegment* next;
};

struct SocketObject {
    uint32_t refs;
    int domain;
    int type;
    int protocol;
    int error;
    int receiveBufferSize;
    int sendBufferSize;
    bool reuseAddress;
    bool keepAlive;
    bool broadcast;
    bool lingerEnabled;
    int lingerSeconds;
    bool bound;
    bool connected;
    bool listening;
    bool remoteTcp;
    int listenBacklog;
    bool readClosed;
    bool writeClosed;
    bool nonBlocking;          // O_NONBLOCK: calls return EAGAIN instead of waiting
    uint64_t receiveTimeoutMs; // SO_RCVTIMEO, 0 = wait forever
    uint64_t sendTimeoutMs;    // SO_SNDTIMEO, 0 = wait forever
    uint8_t localAddress[kMaxSocketAddressSize];
    uint64_t localAddressLength;
    uint8_t peerAddress[kMaxSocketAddressSize];
    uint64_t peerAddressLength;
    uint32_t tcpSendSeq;       // end of the send queue: seq for the next new data
    uint32_t tcpRecvNext;      // RCV.NXT: next seq expected from peer
    // --- TCP state machine / reliability (remoteTcp sockets only) ---
    TcpState tcpState;
    uint32_t tcpSendUnacked;   // SND.UNA: oldest unacknowledged seq
    uint32_t tcpPeerWindow;    // peer's advertised receive window
    bool tcpFinSent;           // we have queued/sent our FIN
    bool tcpFinAcked;          // peer ACKed our FIN
    bool tcpPeerFinSeen;       // peer's FIN has been received & ACKed
    uint32_t tcpDupAckCount;   // consecutive duplicate ACKs
    uint16_t tcpLastWindow;    // receive window in the last segment we sent
    uint32_t tcpAnnouncedRight; // RCV.NXT + window as last announced ...
    bool tcpAnnounced;          // ... once a non-SYN segment went out
    int tcpSoftError;          // last ICMP error: reported if the connection times out
    uint32_t tcpSendNext;      // SND.NXT: next seq to (re)transmit
    uint32_t tcpSendMax;       // highest seq sent so far
    uint32_t tcpSendMss;       // payload per segment we send (the peer's MSS)
    uint8_t tcpSendScale;      // shift applied to the peer's window fields
    uint8_t tcpRecvScale;      // shift applied to the window fields we send
    bool tcpOfferedScale;      // our SYN carries a window scale option
    bool tcpInRecovery;        // fast recovery until tcpRecover is acknowledged
    uint32_t tcpRecover;
    uint32_t tcpCwnd;          // congestion window, bytes
    uint32_t tcpSsthresh;
    uint64_t tcpSrttUs;        // smoothed RTT, 0 before the first sample
    uint64_t tcpRttvarUs;
    uint64_t tcpRtoMs;
    uint32_t tcpBackoff;       // timeouts in a row
    uint64_t tcpRtxDeadline;   // retransmission / persist timer, 0 = stopped
    uint32_t tcpTotalRetrans;
    uint32_t tcpRecvQueued;    // bytes in the receive queue
    uint32_t tcpReasmQueued;   // bytes held out of order
    uint32_t tcpAckPending;    // in-order segments not acknowledged yet
    uint32_t tcpQuickAcks;     // segments still acknowledged at once (start of a connection)
    uint64_t tcpAckDeadline;   // delayed ACK, 0 = none due
    uint64_t tcpLastDataSentMs;
    uint64_t tcpLastDataRecvMs;
    uint64_t tcpLastAckRecvMs;
    bool tcpNoDelay;           // TCP_NODELAY: kept for getsockopt (no Nagle here)
    bool tcpOrphan;            // closed by the application, finishing alone
    uint64_t tcpOrphanDeadline;
    uint64_t tcpTimeWaitDeadline; // uptime ms at which TIME_WAIT expires
    TcpSendSegment* tcpSendHead;  // retransmission queue (in seq order)
    TcpSendSegment* tcpSendTail;
    TcpReassemblySegment* tcpReasm; // out-of-order hold queue (seq order)
    struct Datagram* datagramHead;
    struct Datagram* datagramTail;
    SocketObject* peer;
    SocketObject* acceptHead;
    SocketObject* acceptTail;
    SocketObject* acceptNext;
    SocketObject* next;
};

struct Datagram {
    uint8_t* data;
    uint64_t length;
    uint8_t sourceAddress[kMaxSocketAddressSize];
    uint64_t sourceAddressLength;
    Datagram* next;
};

SocketObject* gSockets = nullptr;

// Forward declarations (definitions appear later in this file).
uint16_t socketAddressPort(const uint8_t* address, uint64_t length);
uint32_t socketAddressIp(const uint8_t* address, uint64_t length);
void freeTcpQueues(SocketObject* socket);
uint16_t tcpLocalPort(const SocketObject* s);
void tcpInitConnection(SocketObject* socket, uint32_t isn);
void tcpSendWindowUpdate(SocketObject* socket);
void tcpSendControl(SocketObject* socket, uint8_t flags);
bool tcpQueueSegment(SocketObject* socket, uint8_t flags, const uint8_t* data, uint32_t length);
void tcpOutput(SocketObject* socket);
void tcpStartClose(SocketObject* socket);
bool tcpBeginOrphan(SocketObject* socket);
struct TcpInfo;
void fillTcpInfo(const SocketObject* socket, TcpInfo* info);

void unlinkSocket(SocketObject* socket) {
    SocketObject** link = &gSockets;
    while (*link) {
        if (*link == socket) {
            *link = socket->next;
            return;
        }
        link = &(*link)->next;
    }
}

void freeDatagrams(SocketObject* socket) {
    Datagram* datagram = socket ? socket->datagramHead : nullptr;
    while (datagram) {
        Datagram* next = datagram->next;
        delete[] datagram->data;
        delete datagram;
        datagram = next;
    }
    if (socket) {
        socket->datagramHead = nullptr;
        socket->datagramTail = nullptr;
    }
}

void freeTcpQueues(SocketObject* socket) {
    if (!socket) {
        return;
    }
    TcpSendSegment* seg = socket->tcpSendHead;
    while (seg) {
        TcpSendSegment* next = seg->next;
        delete[] seg->data;
        delete seg;
        seg = next;
    }
    socket->tcpSendHead = nullptr;
    socket->tcpSendTail = nullptr;

    TcpReassemblySegment* r = socket->tcpReasm;
    while (r) {
        TcpReassemblySegment* next = r->next;
        delete[] r->data;
        delete r;
        r = next;
    }
    socket->tcpReasm = nullptr;
}

void destroySocketObject(SocketObject* socket) {
    if (!socket) {
        return;
    }
    // A live connection closes gracefully: it stays in gSockets as an orphan
    // until what it queued and its FIN are acknowledged (socketTcpTick).
    if (socket->remoteTcp && !socket->tcpOrphan && tcpBeginOrphan(socket)) {
        Scheduler::get().wakeAllBlockedProcesses();
        return;
    }
    unlinkSocket(socket);
    if (socket->peer && socket->peer->peer == socket) {
        socket->peer->peer = nullptr;
    }
    SocketObject* accepted = socket->acceptHead;
    while (accepted) {
        SocketObject* next = accepted->acceptNext;
        destroySocketObject(accepted);
        accepted = next;
    }
    // If a remote TCP connection is still live when the last handle is dropped
    // without a clean shutdown, abort it with a RST so the peer is not left
    // with a half-open connection.
    if (socket->remoteTcp &&
        socket->tcpState != TcpState::Closed &&
        socket->tcpState != TcpState::TimeWait &&
        socket->tcpState != TcpState::Listen) {
        netTransmitTcpSegment(
            socketAddressPort(socket->localAddress, socket->localAddressLength),
            socket->peerAddress, socket->peerAddressLength,
            socket->tcpSendMax, socket->tcpRecvNext,
            kTcpRst | kTcpAck, 0, nullptr, 0);
    }
    freeTcpQueues(socket);
    freeDatagrams(socket);
    delete socket;
    // A local peer blocked in recv() now sees EOF.
    Scheduler::get().wakeAllBlockedProcesses();
}

void retainSocketHandle(void* object) {
    auto* socket = reinterpret_cast<SocketObject*>(object);
    if (socket) {
        socket->refs++;
    }
}

void releaseSocketHandle(void* object) {
    auto* socket = reinterpret_cast<SocketObject*>(object);
    if (!socket) {
        return;
    }
    if (socket->refs > 0) {
        socket->refs--;
    }
    if (socket->refs == 0) {
        destroySocketObject(socket);
    }
}

uint64_t validateSocketType(int type, int protocol) {
    if (type == kSockStream) {
        return (protocol == 0 || protocol == kIpProtoTcp)
            ? 0
            : syscall_error(SysErrProtocolNotSupported);
    }
    if (type == kSockDgram) {
        // IPPROTO_ICMP: Linux "ping sockets", echo requests without raw access.
        return (protocol == 0 || protocol == kIpProtoUdp || protocol == kIpProtoIcmp)
            ? 0
            : syscall_error(SysErrProtocolNotSupported);
    }
    if (type == kSockRaw) {
        return syscall_error(SysErrOperationNotSupported);
    }
    return syscall_error(SysErrInvalid);
}

SocketObject* resolveSocket(Process* process, uint64_t handle, uint32_t rights, uint64_t* error) {
    if (error) {
        *error = 0;
    }
    if (!process) {
        if (error) {
            *error = syscall_error(SysErrInvalid);
        }
        return nullptr;
    }

    HandleEntry* entry = process->getHandle(handle);
    if (!entry) {
        if (error) {
            *error = syscall_error(SysErrBadFile);
        }
        return nullptr;
    }
    if (entry->type != HandleType::Socket) {
        if (error) {
            *error = syscall_error(SysErrNotSocket);
        }
        return nullptr;
    }
    if ((entry->rights & rights) != rights) {
        if (error) {
            *error = syscall_error(SysErrBadFile);
        }
        return nullptr;
    }
    return reinterpret_cast<SocketObject*>(entry->object);
}

bool copySocketAddress(uint64_t address, uint64_t length, uint8_t* out, uint64_t* outLength) {
    if (!out || !outLength || address == 0 || length < sizeof(uint16_t) || length > kMaxSocketAddressSize) {
        return false;
    }
    if (!Syscall::copyFromUser(out, address, static_cast<size_t>(length))) {
        return false;
    }
    *outLength = length;
    return true;
}

uint16_t socketAddressFamily(const uint8_t* address) {
    uint16_t family = 0;
    memcpy(&family, address, sizeof(family));
    return family;
}

uint16_t socketAddressPort(const uint8_t* address, uint64_t length) {
    if (!address || length < 4) {
        return 0;
    }
    uint16_t port = 0;
    memcpy(&port, address + 2, sizeof(port));
    return port;
}

uint32_t socketAddressIp(const uint8_t* address, uint64_t length) {
    if (!address || length < 8) {
        return 0;
    }
    uint32_t ip = 0;
    memcpy(&ip, address + 4, sizeof(ip));
    return ip;
}

bool socketAddressIsLocalDestination(const uint8_t* address, uint64_t length) {
    const uint32_t ip = socketAddressIp(address, length);
    return ip == 0 || ip == kLoopbackAddressNetworkOrder || ip == netLocalAddressNetworkOrder();
}

void setSocketAddressPort(uint8_t* address, uint64_t length, uint16_t port) {
    if (address && length >= 4) {
        memcpy(address + 2, &port, sizeof(port));
    }
}

bool socketAddressConflicts(const SocketObject* socket, const uint8_t* address, uint64_t length) {
    const uint16_t port = socketAddressPort(address, length);
    if (port == 0) {
        return false;
    }
    for (SocketObject* other = gSockets; other; other = other->next) {
        if (other == socket || !other->bound || other->domain != socket->domain || other->type != socket->type) {
            continue;
        }
        if (socketAddressPort(other->localAddress, other->localAddressLength) != port) {
            continue;
        }
        if (!socket->reuseAddress || !other->reuseAddress) {
            return true;
        }
    }
    return false;
}

// Ports are handed out round-robin from a random start: restarting at the
// bottom of the range would give consecutive connections to one server the same
// address pair, and the old one may still be in TIME_WAIT at the peer.
uint32_t gNextEphemeralPort = 0;

bool allocateEphemeralPort(SocketObject* socket, uint8_t* address, uint64_t length) {
    constexpr uint32_t kRange = kEphemeralPortEnd - kEphemeralPortStart + 1;
    if (gNextEphemeralPort == 0) {
        uint32_t seed = 0;
        kernel_fill_entropy(&seed, sizeof(seed));
        gNextEphemeralPort = kEphemeralPortStart + seed % kRange;
    }
    for (uint32_t tried = 0; tried < kRange; ++tried) {
        const uint32_t port = gNextEphemeralPort;
        gNextEphemeralPort = port == kEphemeralPortEnd ? kEphemeralPortStart : port + 1;
        const uint16_t networkPort = static_cast<uint16_t>(((port & 0x00ffU) << 8) | ((port & 0xff00U) >> 8));
        setSocketAddressPort(address, length, networkPort);
        if (!socketAddressConflicts(socket, address, length)) {
            return true;
        }
    }
    return false;
}

bool addressesSameEndpoint(const uint8_t* a, uint64_t aLength, const uint8_t* b, uint64_t bLength) {
    if (socketAddressFamily(a) != socketAddressFamily(b) ||
        socketAddressPort(a, aLength) != socketAddressPort(b, bLength)) {
        return false;
    }
    const uint32_t aIp = socketAddressIp(a, aLength);
    const uint32_t bIp = socketAddressIp(b, bLength);
    return aIp == 0 || bIp == 0 || aIp == bIp;
}

SocketObject* findDatagramPeer(SocketObject* sender, const uint8_t* address, uint64_t length) {
    for (SocketObject* other = gSockets; other; other = other->next) {
        if (other->type != kSockDgram || !other->bound || other->readClosed) {
            continue;
        }
        if (other->domain == sender->domain && addressesSameEndpoint(other->localAddress, other->localAddressLength, address, length)) {
            return other;
        }
    }
    return nullptr;
}

SocketObject* findListeningStreamPeer(SocketObject* sender, const uint8_t* address, uint64_t length) {
    for (SocketObject* other = gSockets; other; other = other->next) {
        if (other == sender || other->type != kSockStream || !other->listening || !other->bound) {
            continue;
        }
        if (other->domain == sender->domain && addressesSameEndpoint(other->localAddress, other->localAddressLength, address, length)) {
            return other;
        }
    }
    return nullptr;
}

uint64_t acceptQueueLength(const SocketObject* socket) {
    uint64_t count = 0;
    for (SocketObject* child = socket ? socket->acceptHead : nullptr; child; child = child->acceptNext) {
        count++;
    }
    return count;
}

bool ensureDatagramBound(SocketObject* socket) {
    if (socket->bound) {
        return true;
    }
    uint8_t address[kMaxSocketAddressSize] {};
    uint16_t family = static_cast<uint16_t>(socket->domain);
    memcpy(address, &family, sizeof(family));
    uint64_t length = 8;
    if (!allocateEphemeralPort(socket, address, length)) {
        return false;
    }
    memcpy(socket->localAddress, address, length);
    socket->localAddressLength = length;
    socket->bound = true;
    return true;
}

bool ensureSocketBound(SocketObject* socket) {
    return ensureDatagramBound(socket);
}

uint64_t enqueueDatagram(SocketObject* target, SocketObject* source, uint64_t buffer, uint64_t length) {
    auto* datagram = new Datagram {};
    if (!datagram) {
        return syscall_error(SysErrNoMemory);
    }
    datagram->data = new uint8_t[length ? length : 1];
    if (!datagram->data) {
        delete datagram;
        return syscall_error(SysErrNoMemory);
    }
    if (length != 0 && !Syscall::copyFromUser(datagram->data, buffer, static_cast<size_t>(length))) {
        delete[] datagram->data;
        delete datagram;
        return syscall_error(SysErrInvalid);
    }
    datagram->length = length;
    memcpy(datagram->sourceAddress, source->localAddress, source->localAddressLength);
    datagram->sourceAddressLength = source->localAddressLength;
    if (target->datagramTail) {
        target->datagramTail->next = datagram;
    } else {
        target->datagramHead = datagram;
    }
    target->datagramTail = datagram;
    Scheduler::get().wakeAllBlockedProcesses();
    return length;
}

SocketObject* createSocketObject(int domain, int type, int protocol) {
    auto* socket = new SocketObject {};
    if (!socket) {
        return nullptr;
    }
    socket->refs = 1;
    socket->domain = domain;
    socket->type = type;
    socket->protocol = protocol;
    socket->receiveBufferSize = 65536;
    socket->sendBufferSize = 65536;
    socket->next = gSockets;
    gSockets = socket;
    return socket;
}

uint64_t copyIntOptionToUser(int value, uint64_t optionValue, uint64_t optionLength) {
    uint32_t length = 0;
    if (!Syscall::copyFromUser(&length, optionLength, sizeof(length))) {
        return syscall_error(SysErrInvalid);
    }
    if (length < sizeof(int)) {
        return syscall_error(SysErrInvalid);
    }
    length = sizeof(int);
    if (!Syscall::copyToUser(optionValue, &value, sizeof(value)) ||
        !Syscall::copyToUser(optionLength, &length, sizeof(length))) {
        return syscall_error(SysErrInvalid);
    }
    return 0;
}

uint64_t copyLingerOptionToUser(const SocketObject& socket, uint64_t optionValue, uint64_t optionLength) {
    uint32_t length = 0;
    if (!Syscall::copyFromUser(&length, optionLength, sizeof(length))) {
        return syscall_error(SysErrInvalid);
    }
    if (length < sizeof(SocketLinger)) {
        return syscall_error(SysErrInvalid);
    }

    SocketLinger linger {
        socket.lingerEnabled ? 1 : 0,
        socket.lingerSeconds
    };
    length = sizeof(SocketLinger);
    if (!Syscall::copyToUser(optionValue, &linger, sizeof(linger)) ||
        !Syscall::copyToUser(optionLength, &length, sizeof(length))) {
        return syscall_error(SysErrInvalid);
    }
    return 0;
}

bool copyIntOptionFromUser(uint64_t optionValue, uint64_t optionLength, int* value) {
    return value &&
           optionLength >= sizeof(int) &&
           Syscall::copyFromUser(value, optionValue, sizeof(int));
}

uint64_t socketDeadline(uint64_t timeoutMs) {
    return timeoutMs == 0 ? 0 : time_get_uptime_ms() + timeoutMs;
}

// Sleep until the network stack reports a change (every socket state change
// calls wakeAllBlockedProcesses), `deadlineMs` passes (0 = none) or a signal
// arrives. Returns 0 when the caller should look again, else -errno. A
// reference keeps the socket alive if another thread closes it meanwhile.
uint64_t waitForSocketEvent(SocketObject* socket, uint64_t deadlineMs) {
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
    retainSocketHandle(socket);
    current->setState(ProcessState::Blocked);
    Scheduler::get().scheduleFromSyscall();
    current->clearSleep();
    const bool closed = socket->refs == 1;
    releaseSocketHandle(socket);
    if (closed) {
        return syscall_error(SysErrBadFile);
    }
    if (current->hasDeliverableSignal()) {
        return syscall_error(SysErrInterrupted);
    }
    return 0;
}

uint64_t takeSocketError(SocketObject* socket) {
    const int e = socket->error;
    socket->error = 0;
    return syscall_error(static_cast<SyscallErrno>(e));
}
}

bool socketIsNonBlocking(void* object) {
    auto* socket = reinterpret_cast<SocketObject*>(object);
    return socket && socket->nonBlocking;
}

void socketSetNonBlocking(void* object, bool nonBlocking) {
    if (auto* socket = reinterpret_cast<SocketObject*>(object)) {
        socket->nonBlocking = nonBlocking;
    }
}

uint64_t netSendUdpDatagram(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength, uint64_t buffer, uint64_t length);
bool netSendIcmpMessage(uint32_t dstIpNetworkOrder, const uint8_t* message, uint64_t length);

namespace {
constexpr uint64_t kSockaddrInSize = 16;
constexpr uint64_t kMaxIcmpMessage = 65507;

// Queue a datagram that is already in kernel memory, from `source` (a
// sockaddr_in), and wake readers.
bool queueKernelDatagram(SocketObject* socket, const uint8_t* data, uint64_t length, const uint8_t* source) {
    auto* datagram = new Datagram {};
    if (!datagram) {
        return false;
    }
    datagram->data = new uint8_t[length ? length : 1];
    if (!datagram->data) {
        delete datagram;
        return false;
    }
    if (length != 0) {
        memcpy(datagram->data, data, static_cast<size_t>(length));
    }
    datagram->length = length;
    memcpy(datagram->sourceAddress, source, kSockaddrInSize);
    datagram->sourceAddressLength = kSockaddrInSize;
    if (socket->datagramTail) {
        socket->datagramTail->next = datagram;
    } else {
        socket->datagramHead = datagram;
    }
    socket->datagramTail = datagram;
    // A blocked reader is invisible to the scheduler until re-readied.
    Scheduler::get().wakeAllBlockedProcesses();
    return true;
}

void makeSockaddrIn(uint8_t* out, uint32_t ipNetworkOrder, uint16_t portNetworkOrder) {
    memset(out, 0, kSockaddrInSize);
    uint16_t family = kAfInet;
    memcpy(out, &family, sizeof(family));
    memcpy(out + 2, &portNetworkOrder, sizeof(portNetworkOrder));
    memcpy(out + 4, &ipNetworkOrder, sizeof(ipNetworkOrder));
}

uint16_t onesComplementChecksum(const uint8_t* data, uint64_t length) {
    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < length; i += 2) {
        sum += static_cast<uint32_t>((data[i] << 8) | data[i + 1]);
    }
    if (length & 1) {
        sum += static_cast<uint32_t>(data[length - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

// An ICMP socket sends echo requests only. The identifier is the socket's
// local "port" (as on Linux, so replies find their way back) and the kernel
// fills in the checksum. A request to ourselves is answered on the spot.
uint64_t sendIcmpEcho(SocketObject* socket, const uint8_t* destination, uint64_t destinationLength,
                      uint64_t buffer, uint64_t length) {
    static uint8_t message[kMaxIcmpMessage];
    if (length < 8 || length > kMaxIcmpMessage) {
        return syscall_error(SysErrInvalid);
    }
    if (!Syscall::copyFromUser(message, buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }
    if (message[0] != kIcmpEchoRequestType || message[1] != 0) {
        return syscall_error(SysErrInvalid);
    }
    const uint16_t id = socketAddressPort(socket->localAddress, socket->localAddressLength);
    memcpy(message + 4, &id, sizeof(id));
    message[2] = message[3] = 0;
    uint16_t checksum = onesComplementChecksum(message, length);
    message[2] = static_cast<uint8_t>(checksum >> 8);
    message[3] = static_cast<uint8_t>(checksum);

    const uint32_t dst = socketAddressIp(destination, destinationLength);
    if (socketAddressIsLocalDestination(destination, destinationLength)) {
        message[0] = kIcmpEchoReplyType;
        message[2] = message[3] = 0;
        checksum = onesComplementChecksum(message, length);
        message[2] = static_cast<uint8_t>(checksum >> 8);
        message[3] = static_cast<uint8_t>(checksum);
        uint8_t source[kSockaddrInSize];
        makeSockaddrIn(source, dst != 0 ? dst : kLoopbackAddressNetworkOrder, 0);
        return queueKernelDatagram(socket, message, length, source) ? length : syscall_error(SysErrNoMemory);
    }
    return netSendIcmpMessage(dst, message, length) ? length : syscall_error(SysErrAgain);
}

// One datagram to `destination` (a sockaddr_in): UDP, or an ICMP echo
// request on a ping socket.
uint64_t sendDatagram(SocketObject* socket, const uint8_t* destination, uint64_t destinationLength,
                      uint64_t buffer, uint64_t length) {
    if (socket->error != 0) {
        return takeSocketError(socket);   // an ICMP error about an earlier datagram
    }
    if (length > kMaxDatagramSize) {
        return syscall_error(SysErrMessageSize);
    }
    if (length > static_cast<uint64_t>(socket->sendBufferSize)) {
        return syscall_error(SysErrInvalid);
    }
    if (!ensureDatagramBound(socket)) {
        return syscall_error(SysErrAddressInUse);
    }
    if (socket->protocol == kIpProtoIcmp) {
        return sendIcmpEcho(socket, destination, destinationLength, buffer, length);
    }
    SocketObject* target = socketAddressIsLocalDestination(destination, destinationLength)
        ? findDatagramPeer(socket, destination, destinationLength)
        : nullptr;
    if (!target) {
        return netSendUdpDatagram(
            socketAddressPort(socket->localAddress, socket->localAddressLength),
            destination,
            destinationLength,
            buffer,
            length
        );
    }
    return enqueueDatagram(target, socket, buffer, length);
}
}  // namespace

uint64_t netStartTcpConnect(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength);
uint64_t netSendTcpPayload(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength, uint32_t seq, uint32_t ack, uint64_t buffer, uint64_t length);

int16_t pollSocketHandle(void* object, uint32_t rights, int16_t events) {
    auto* socket = reinterpret_cast<SocketObject*>(object);
    if (!socket) {
        return kPollErr;
    }

    int16_t revents = 0;
    if (socket->error && (events & (kPollIn | kPollOut))) {
        revents |= kPollErr;
    }
    if ((events & kPollOut) && (rights & HandleRightWrite) && !socket->writeClosed && !socket->listening) {
        // For remote TCP, writable once established and the send queue has
        // room.
        if (socket->remoteTcp) {
            const uint32_t queued = socket->tcpSendSeq - socket->tcpSendUnacked;
            if ((socket->tcpState == TcpState::Established ||
                 socket->tcpState == TcpState::CloseWait) && queued < kTcpSendBufferCap) {
                revents |= kPollOut;
            }
        } else {
            revents |= kPollOut;
        }
    }
    if ((events & kPollIn) && (rights & HandleRightRead)) {
        if (socket->remoteTcp) {
            if (socket->acceptHead || socket->datagramHead ||
                socket->tcpPeerFinSeen || socket->readClosed || socket->error) {
                revents |= kPollIn;
            }
            if (socket->tcpPeerFinSeen && !socket->datagramHead) {
                revents |= kPollHup;
            }
        } else {
            const bool streamPeerGone = socket->type == kSockStream && socket->connected && !socket->peer;
            if (socket->acceptHead || socket->datagramHead || socket->readClosed || streamPeerGone || (socket->peer && socket->peer->writeClosed)) {
                revents |= kPollIn;
            }
            if (socket->readClosed || streamPeerGone || (socket->peer && socket->peer->writeClosed)) {
                revents |= kPollHup;
            }
        }
    }
    if ((events & kPollHup) && socket->readClosed && socket->writeClosed) {
        revents |= kPollHup;
    }
    return revents;
}

uint64_t Syscall::sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    if (domain != kAfInet) {
        return syscall_error(SysErrAddressFamilyNotSupported);
    }

    // The Linux socket type argument carries optional SOCK_NONBLOCK (04000) and
    // SOCK_CLOEXEC (02000000) flag bits OR'd into the base type; libcurl in
    // particular requests SOCK_STREAM|SOCK_NONBLOCK. SOCK_NONBLOCK sets the
    // socket's O_NONBLOCK (also reachable through fcntl); close-on-exec is
    // handled at the handle level.
    const int socketType = static_cast<int>(type) & ~(kSockNonblock | kSockCloexec);
    const int socketProtocol = static_cast<int>(protocol);
    const uint64_t typeError = validateSocketType(socketType, socketProtocol);
    if (typeError != 0) {
        return typeError;
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }

    auto* socket = createSocketObject(static_cast<int>(domain), socketType, socketProtocol != 0
        ? socketProtocol
        : (socketType == kSockStream ? kIpProtoTcp : kIpProtoUdp));
    if (!socket) {
        return syscall_error(SysErrNoMemory);
    }
    socket->nonBlocking = (static_cast<int>(type) & kSockNonblock) != 0;

    const uint64_t handle = current->allocateHandle(
        HandleType::Socket,
        kSocketRights,
        socket,
        retainSocketHandle,
        releaseSocketHandle
    );
    if (handle == static_cast<uint64_t>(-1)) {
        gSockets = socket->next;
        delete socket;
        return syscall_error(SysErrNoMemory);
    }
    return handle;
}

uint64_t Syscall::sys_bind(uint64_t socketHandle, uint64_t address, uint64_t addressLength) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    if (socket->connected || socket->listening) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t copiedAddress[kMaxSocketAddressSize];
    uint64_t copiedLength = 0;
    if (!copySocketAddress(address, addressLength, copiedAddress, &copiedLength)) {
        return syscall_error(SysErrInvalid);
    }
    if (socketAddressFamily(copiedAddress) != socket->domain) {
        return syscall_error(SysErrAddressFamilyNotSupported);
    }
    if (socketAddressPort(copiedAddress, copiedLength) == 0 &&
        !allocateEphemeralPort(socket, copiedAddress, copiedLength)) {
        return syscall_error(SysErrAddressInUse);
    }
    if (socketAddressConflicts(socket, copiedAddress, copiedLength)) {
        return syscall_error(SysErrAddressInUse);
    }
    memcpy(socket->localAddress, copiedAddress, copiedLength);
    socket->localAddressLength = copiedLength;
    socket->bound = true;
    return 0;
}

uint64_t Syscall::sys_connect(uint64_t socketHandle, uint64_t address, uint64_t addressLength) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    if (socket->listening) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t copiedAddress[kMaxSocketAddressSize];
    uint64_t copiedLength = 0;
    if (!copySocketAddress(address, addressLength, copiedAddress, &copiedLength)) {
        return syscall_error(SysErrInvalid);
    }
    if (socketAddressFamily(copiedAddress) != socket->domain) {
        return syscall_error(SysErrAddressFamilyNotSupported);
    }

    if (socket->type == kSockStream && socket->remoteTcp) {
        // connect() again on a connection already under way: report where it
        // stands. Established answers 0 rather than EISCONN, since callers
        // poll with connect() until it succeeds.
        if (socket->error != 0) {
            return takeSocketError(socket);
        }
        if (socket->tcpState == TcpState::Established) {
            return 0;
        }
        if (socket->tcpState == TcpState::SynSent ||
            socket->tcpState == TcpState::SynReceived) {
            return syscall_error(SysErrAlready);
        }
    }
    memcpy(socket->peerAddress, copiedAddress, copiedLength);
    socket->peerAddressLength = copiedLength;

    if (socket->type == kSockStream) {
        if (!ensureSocketBound(socket)) {
            return syscall_error(SysErrAddressInUse);
        }
        SocketObject* listener = socketAddressIsLocalDestination(copiedAddress, copiedLength)
            ? findListeningStreamPeer(socket, copiedAddress, copiedLength)
            : nullptr;
        if (!listener) {
            if (netLocalAddressNetworkOrder() == 0) {
                return syscall_error(SysErrNetUnreachable);   // no address yet
            }
            // Active open to a remote host: initialize the TCP state machine
            // and transmit a SYN with a randomized ISN.  The SYN is retained on
            // the send queue and retransmitted by socketTcpTick() until ACKed.
            socket->remoteTcp = true;
            uint32_t isn = 0;
            kernel_fill_entropy(&isn, sizeof(isn));
            tcpInitConnection(socket, isn);
            socket->tcpOfferedScale = true;
            socket->tcpState = TcpState::SynSent;
            socket->connected = false;
            if (!tcpQueueSegment(socket, kTcpSyn, nullptr, 0)) {
                socket->tcpState = TcpState::Closed;
                return syscall_error(SysErrNoMemory);
            }
            if (socket->nonBlocking) {
                return syscall_error(SysErrInProgress);
            }
            // Blocking: wait for the SYN-ACK (or RST / retransmit give-up).
            for (;;) {
                if (socket->error != 0) {
                    return takeSocketError(socket);
                }
                if (socket->tcpState == TcpState::Established) {
                    return 0;
                }
                if (socket->tcpState != TcpState::SynSent &&
                    socket->tcpState != TcpState::SynReceived) {
                    return syscall_error(SysErrConnectionRefused);
                }
                const uint64_t waited = waitForSocketEvent(socket, socketDeadline(socket->sendTimeoutMs));
                if (waited != 0) {
                    // Timed out or interrupted: the handshake carries on, and
                    // a later connect() reports how it ended (Linux EINPROGRESS).
                    return waited == syscall_error(SysErrAgain)
                        ? syscall_error(SysErrInProgress)
                        : waited;
                }
            }
        }
        if (acceptQueueLength(listener) >= static_cast<uint64_t>(listener->listenBacklog)) {
            return syscall_error(SysErrAgain);
        }

        SocketObject* accepted = createSocketObject(socket->domain, socket->type, socket->protocol);
        if (!accepted) {
            return syscall_error(SysErrNoMemory);
        }
        accepted->bound = true;
        accepted->connected = true;
        memcpy(accepted->localAddress, listener->localAddress, listener->localAddressLength);
        accepted->localAddressLength = listener->localAddressLength;
        memcpy(accepted->peerAddress, socket->localAddress, socket->localAddressLength);
        accepted->peerAddressLength = socket->localAddressLength;
        accepted->peer = socket;

        socket->connected = true;
        socket->peer = accepted;
        if (listener->acceptTail) {
            listener->acceptTail->acceptNext = accepted;
        } else {
            listener->acceptHead = accepted;
        }
        listener->acceptTail = accepted;
        Scheduler::get().wakeAllBlockedProcesses();
        return 0;
    }

    if (!socketAddressIsLocalDestination(copiedAddress, copiedLength) && netLocalAddressNetworkOrder() == 0) {
        return syscall_error(SysErrNetUnreachable);
    }
    socket->error = 0;
    socket->connected = true;
    return 0;
}

uint64_t Syscall::sys_listen(uint64_t socketHandle, uint64_t backlog) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    if (socket->type != kSockStream || socket->connected || backlog > 4096) {
        return syscall_error(SysErrOperationNotSupported);
    }
    socket->listenBacklog = backlog == 0 ? 1 : static_cast<int>(backlog);
    socket->listening = true;
    return 0;
}

uint64_t Syscall::sys_accept(uint64_t socketHandle, uint64_t address, uint64_t addressLength) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    if (socket->type != kSockStream || !socket->listening) {
        return syscall_error(SysErrInvalid);
    }
    const uint64_t deadline = socketDeadline(socket->receiveTimeoutMs);
    while (!socket->acceptHead) {
        if (socket->nonBlocking) {
            return syscall_error(SysErrAgain);
        }
        const uint64_t waited = waitForSocketEvent(socket, deadline);
        if (waited != 0) {
            return waited;
        }
        if (!socket->listening) {
            return syscall_error(SysErrInvalid);
        }
    }
    SocketObject* accepted = socket->acceptHead;
    socket->acceptHead = accepted->acceptNext;
    if (!socket->acceptHead) {
        socket->acceptTail = nullptr;
    }
    accepted->acceptNext = nullptr;

    if (address != 0 && addressLength != 0) {
        uint32_t capacity = 0;
        if (!copyFromUser(&capacity, addressLength, sizeof(capacity))) {
            return syscall_error(SysErrInvalid);
        }
        const uint32_t actual = static_cast<uint32_t>(accepted->peerAddressLength);
        const uint32_t copied = capacity < actual ? capacity : actual;
        if (copied != 0 && !copyToUser(address, accepted->peerAddress, copied)) {
            return syscall_error(SysErrInvalid);
        }
        if (!copyToUser(addressLength, &actual, sizeof(actual))) {
            return syscall_error(SysErrInvalid);
        }
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) {
        return syscall_error(SysErrInvalid);
    }
    const uint64_t handle = current->allocateHandle(
        HandleType::Socket,
        kSocketRights,
        accepted,
        retainSocketHandle,
        releaseSocketHandle
    );
    if (handle == static_cast<uint64_t>(-1)) {
        destroySocketObject(accepted);
        return syscall_error(SysErrNoMemory);
    }
    return handle;
}

uint64_t Syscall::sys_send(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightWrite,
        &error
    );
    if (!socket) {
        return error;
    }
    if (length != 0 && !isValidUserPointer(buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }
    if (socket->writeClosed) {
        return syscall_error(SysErrBrokenPipe);
    }
    if (length == 0) {
        return 0;
    }
    if (socket->type == kSockStream) {
        if (!socket->connected) {
            return syscall_error(SysErrNotConnected);
        }
        if (socket->remoteTcp) {
            const bool mayWait = !socket->nonBlocking && (flags & kMsgDontWait) == 0;
            const uint64_t deadline = socketDeadline(socket->sendTimeoutMs);
            // Copy from user space into a kernel staging buffer, then segment
            // into MSS-sized PSH/ACK segments on the send queue; tcpOutput
            // sends them as the windows allow. A blocking send waits for
            // queue space until it has queued it all.
            uint8_t staging[kTcpMss];
            uint64_t offset = 0;
            while (offset < length) {
                if (socket->error != 0) {
                    return offset != 0 ? offset : takeSocketError(socket);
                }
                if (socket->writeClosed) {
                    return offset != 0 ? offset : syscall_error(SysErrBrokenPipe);
                }
                if (socket->tcpState != TcpState::Established &&
                    socket->tcpState != TcpState::CloseWait) {
                    return offset != 0 ? offset : syscall_error(SysErrNotConnected);
                }

                // Backpressure: the send queue (in flight + not yet sent)
                // holds at most kTcpSendBufferCap bytes.
                const uint32_t queued = socket->tcpSendSeq - socket->tcpSendUnacked;
                if (queued >= kTcpSendBufferCap) {
                    if (!mayWait) {
                        return offset != 0 ? offset : syscall_error(SysErrAgain);
                    }
                    const uint64_t waited = waitForSocketEvent(socket, deadline);
                    if (waited != 0) {
                        return offset != 0 ? offset : waited;
                    }
                    continue;
                }

                uint64_t chunk = length - offset;
                if (chunk > kTcpSendBufferCap - queued) {
                    chunk = kTcpSendBufferCap - queued;
                }
                if (chunk > socket->tcpSendMss) {
                    chunk = socket->tcpSendMss;
                }
                if (!copyFromUser(staging, buffer + offset, static_cast<size_t>(chunk))) {
                    return offset != 0 ? offset : syscall_error(SysErrInvalid);
                }
                if (!tcpQueueSegment(socket, kTcpPsh | kTcpAck, staging, static_cast<uint32_t>(chunk))) {
                    return offset != 0 ? offset : syscall_error(SysErrNoMemory);
                }
                offset += chunk;
            }
            return offset;
        }
        if (!socket->peer) {
            return syscall_error(SysErrBrokenPipe);
        }
        if (socket->peer->readClosed) {
            return syscall_error(SysErrBrokenPipe);
        }
        if (length > static_cast<uint64_t>(socket->sendBufferSize)) {
            return syscall_error(SysErrInvalid);
        }
        return enqueueDatagram(socket->peer, socket, buffer, length);
    }
    if (socket->type != kSockDgram) {
        return syscall_error(SysErrNoSys);
    }
    if (!socket->connected) {
        return syscall_error(SysErrNotConnected);
    }
    return sendDatagram(socket, socket->peerAddress, socket->peerAddressLength, buffer, length);
}

uint64_t Syscall::sys_sendto(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags,
                             uint64_t address) {
    if (address == 0) {
        return sys_send(socketHandle, buffer, length, flags);
    }
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightWrite,
        &error
    );
    if (!socket) {
        return error;
    }
    // A stream has its peer already; Linux ignores the address there too.
    if (socket->type != kSockDgram) {
        return sys_send(socketHandle, buffer, length, flags);
    }
    if (socket->writeClosed) {
        return syscall_error(SysErrBrokenPipe);
    }
    if (length != 0 && !isValidUserPointer(buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t destination[kMaxSocketAddressSize] {};
    uint64_t destinationLength = 0;
    if (!copySocketAddress(address, kSockaddrInSize, destination, &destinationLength)) {
        return syscall_error(SysErrInvalid);
    }
    if (socketAddressFamily(destination) != socket->domain) {
        return syscall_error(SysErrAddressFamilyNotSupported);
    }
    if (!socketAddressIsLocalDestination(destination, destinationLength) && netLocalAddressNetworkOrder() == 0) {
        return syscall_error(SysErrNetUnreachable);
    }
    return sendDatagram(socket, destination, destinationLength, buffer, length);
}

bool socketDeliverUdpDatagram(
    uint16_t srcPort,
    uint32_t srcIpNetworkOrder,
    uint16_t dstPort,
    const uint8_t* payload,
    uint64_t length
) {
    if (!payload && length != 0) {
        return false;
    }

    uint8_t sourceAddress[kSockaddrInSize];
    makeSockaddrIn(sourceAddress, srcIpNetworkOrder, srcPort);

    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockDgram || socket->protocol != kIpProtoUdp || !socket->bound ||
            socket->readClosed ||
            socketAddressPort(socket->localAddress, socket->localAddressLength) != dstPort) {
            continue;
        }
        // A connected socket hears only from its peer (as on Linux).
        if (socket->connected && socket->peerAddressLength >= 8 &&
            !socketAddressIsLocalDestination(socket->peerAddress, socket->peerAddressLength) &&
            (socketAddressIp(socket->peerAddress, socket->peerAddressLength) != srcIpNetworkOrder ||
             socketAddressPort(socket->peerAddress, socket->peerAddressLength) != srcPort)) {
            continue;
        }
        // Out of memory drops the datagram, but the port is open: true.
        queueKernelDatagram(socket, payload, length, sourceAddress);
        return true;
    }
    return false;
}

bool socketDeliverIcmpEcho(uint32_t srcIpNetworkOrder, uint16_t id, const uint8_t* message, uint64_t length) {
    uint8_t source[kSockaddrInSize];
    makeSockaddrIn(source, srcIpNetworkOrder, 0);
    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type == kSockDgram && socket->protocol == kIpProtoIcmp && socket->bound &&
            !socket->readClosed && socketAddressPort(socket->localAddress, socket->localAddressLength) == id) {
            queueKernelDatagram(socket, message, length, source);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// TCP state machine (remoteTcp sockets)
//
// Sending: send() appends segments to the queue at tcpSendSeq and tcpOutput()
// transmits from tcpSendNext as far as min(congestion window, peer window)
// allows. Congestion control is Reno with NewReno partial ACKs (RFC 5681,
// 6582) and an initial window of 10 segments (RFC 6928); the retransmission
// timer follows RFC 6298 with Karn's rule and goes back to SND.UNA when it
// fires. The same timer probes a zero window.
// Receiving: in-order data goes to the receive queue, data beyond it is held
// for reassembly, both bounded by the advertised window; ACKs are delayed
// for at most kTcpDelayedAckMs or every second segment.
// ===========================================================================
namespace {

bool seqLt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
bool seqLe(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }
bool seqGt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
bool seqGe(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

uint16_t tcpLocalPort(const SocketObject* s) {
    return socketAddressPort(s->localAddress, s->localAddressLength);
}

// Sequence space a queued segment occupies: its payload, or 1 for SYN/FIN.
uint32_t tcpSegmentSpan(const TcpSendSegment* seg) {
    return seg->length != 0 ? seg->length : ((seg->flags & (kTcpSyn | kTcpFin)) ? 1u : 0u);
}

uint32_t tcpSegmentEnd(const TcpSendSegment* seg) {
    return seg->seq + tcpSegmentSpan(seg);
}

// A fresh connection: ISN, timers and congestion state.
void tcpInitConnection(SocketObject* socket, uint32_t isn) {
    socket->tcpSendSeq = isn;
    socket->tcpSendUnacked = isn;
    socket->tcpSendNext = isn;
    socket->tcpSendMax = isn;
    socket->tcpRecvNext = 0;
    socket->tcpPeerWindow = 0;
    socket->tcpSendMss = kTcpDefaultSendMss;
    socket->tcpSendScale = 0;
    socket->tcpRecvScale = 0;
    socket->tcpOfferedScale = false;
    socket->tcpCwnd = kTcpInitialCwndSegments * kTcpDefaultSendMss;
    socket->tcpSsthresh = 0xFFFFFFFFU;
    socket->tcpInRecovery = false;
    socket->tcpRecover = isn;
    socket->tcpDupAckCount = 0;
    socket->tcpSrttUs = 0;
    socket->tcpRttvarUs = 0;
    socket->tcpRtoMs = kTcpInitialRtoMs;
    socket->tcpBackoff = 0;
    socket->tcpRtxDeadline = 0;
    socket->tcpTotalRetrans = 0;
    socket->tcpRecvQueued = 0;
    socket->tcpReasmQueued = 0;
    socket->tcpAckPending = 0;
    socket->tcpAckDeadline = 0;
    socket->tcpAnnounced = false;
    socket->tcpSoftError = 0;
    socket->tcpFinSent = false;
    socket->tcpFinAcked = false;
    socket->tcpPeerFinSeen = false;
}

// Handshake done: the MSS and window scale are settled, so is the initial
// congestion window.
void tcpEstablish(SocketObject* socket, const TcpOptions& peer) {
    socket->tcpSendMss = peer.mss != 0 ? (peer.mss < kTcpMss ? peer.mss : kTcpMss) : kTcpDefaultSendMss;
    if (socket->tcpSendMss < 64) {
        socket->tcpSendMss = 64;
    }
    if (socket->tcpOfferedScale && peer.hasWindowScale) {
        socket->tcpSendScale = peer.windowScale;
        socket->tcpRecvScale = kTcpWindowScale;
    } else {
        socket->tcpSendScale = 0;
        socket->tcpRecvScale = 0;
    }
    socket->tcpCwnd = kTcpInitialCwndSegments * socket->tcpSendMss;
    socket->tcpQuickAcks = kTcpQuickAckSegments;
}

uint64_t tcpCurrentRto(const SocketObject* socket) {
    const uint32_t shift = socket->tcpBackoff < 8 ? socket->tcpBackoff : 8;
    const uint64_t rto = socket->tcpRtoMs << shift;
    return rto < kTcpMaxRtoMs ? rto : kTcpMaxRtoMs;
}

// RFC 6298: SRTT/RTTVAR from one sample (ms; the clock ticks in ms, so the
// clock granularity G is the 10 ms network timer).
void tcpRttSample(SocketObject* socket, uint64_t sampleMs) {
    const uint64_t r = (sampleMs != 0 ? sampleMs : 1) * 1000;
    if (socket->tcpSrttUs == 0) {
        socket->tcpSrttUs = r;
        socket->tcpRttvarUs = r / 2;
    } else {
        const uint64_t diff = socket->tcpSrttUs > r ? socket->tcpSrttUs - r : r - socket->tcpSrttUs;
        socket->tcpRttvarUs = (3 * socket->tcpRttvarUs + diff) / 4;
        socket->tcpSrttUs = (7 * socket->tcpSrttUs + r) / 8;
    }
    const uint64_t variance = 4 * socket->tcpRttvarUs > 10000 ? 4 * socket->tcpRttvarUs : 10000;
    uint64_t rto = (socket->tcpSrttUs + variance + 999) / 1000;
    if (rto < kTcpMinRtoMs) {
        rto = kTcpMinRtoMs;
    }
    socket->tcpRtoMs = rto < kTcpMaxRtoMs ? rto : kTcpMaxRtoMs;
}

// Room past RCV.NXT. Only the in-order queue counts: bytes held out of order
// already sit inside the window we advertised, and shrinking the window for
// them would make every duplicate ACK look like a window update to the
// sender, which then never fast-retransmits (a BSD-derived stack such as
// slirp counts an ACK as duplicate only if the window is unchanged).
uint32_t tcpReceiveSpace(const SocketObject* socket) {
    return socket->tcpRecvQueued >= kTcpReceiveBuffer ? 0 : kTcpReceiveBuffer - socket->tcpRecvQueued;
}

// The window field for an outgoing segment. SYNs are never scaled. The right
// edge never moves left (RFC 793/7323), and it moves right only by a segment
// or more at a time, which also keeps a silly small window from being offered
// (RFC 1122 4.2.3.3).
uint16_t tcpAdvertisedWindow(const SocketObject* socket, bool syn) {
    const uint32_t space = tcpReceiveSpace(socket);
    if (syn) {
        return static_cast<uint16_t>(space > 0xFFFF ? 0xFFFF : space);
    }
    const uint32_t largest = 0xFFFFU << socket->tcpRecvScale;
    uint32_t want = space < largest ? space : largest;
    if (socket->tcpAnnounced) {
        const uint32_t current = seqGt(socket->tcpAnnouncedRight, socket->tcpRecvNext)
            ? socket->tcpAnnouncedRight - socket->tcpRecvNext : 0;
        if (want < current + kTcpMss) {
            want = current;
        }
    }
    want >>= socket->tcpRecvScale;
    return static_cast<uint16_t>(want > 0xFFFF ? 0xFFFF : want);
}

// Put one segment on the wire for `socket`, with the current ACK and window.
// `repeatWindow` sends the window last announced, unchanged: a duplicate ACK
// only counts as one at the sender if its window has not moved.
void tcpTransmit(SocketObject* socket, uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t length,
                 bool repeatWindow = false) {
    TcpOptions options {};
    const bool syn = (flags & kTcpSyn) != 0;
    if (syn) {
        options.mss = kTcpMss;
        options.hasWindowScale = socket->tcpOfferedScale;
        options.windowScale = kTcpWindowScale;
    }
    const uint16_t window = repeatWindow && socket->tcpAnnounced && !syn
        ? socket->tcpLastWindow : tcpAdvertisedWindow(socket, syn);
    if (!syn) {
        socket->tcpLastWindow = window;
        socket->tcpAnnouncedRight = socket->tcpRecvNext + (static_cast<uint32_t>(window) << socket->tcpRecvScale);
        socket->tcpAnnounced = true;
    }
    netTransmitTcpSegment(
        tcpLocalPort(socket), socket->peerAddress, socket->peerAddressLength,
        seq, socket->tcpRecvNext, flags, window, data, length, syn ? &options : nullptr);
    if ((flags & kTcpAck) != 0) {
        socket->tcpAckPending = 0;
        socket->tcpAckDeadline = 0;
    }
    if (length != 0) {
        socket->tcpLastDataSentMs = time_get_uptime_ms();
    }
}

// A control segment (ACK, RST) that is not retained.
void tcpSendControl(SocketObject* socket, uint8_t flags) {
    tcpTransmit(socket, socket->tcpSendNext, flags, nullptr, 0);
}

void tcpSendSegment(SocketObject* socket, TcpSendSegment* seg, uint64_t now) {
    seg->transmissions++;
    if (seg->transmissions > 1) {
        socket->tcpTotalRetrans++;
    }
    seg->lastSentMs = now;
    tcpTransmit(socket, seg->seq, seg->flags, seg->data, seg->length);
}

// Transmit whatever the windows allow, starting at SND.NXT.
void tcpOutput(SocketObject* socket) {
    const uint64_t now = time_get_uptime_ms();
    TcpSendSegment* seg = socket->tcpSendHead;

    // During the handshake only the SYN (or SYN-ACK) goes out.
    if (socket->tcpState == TcpState::SynSent || socket->tcpState == TcpState::SynReceived) {
        if (seg && (seg->flags & kTcpSyn) != 0 && socket->tcpSendNext == socket->tcpSendUnacked) {
            tcpSendSegment(socket, seg, now);
            socket->tcpSendNext = tcpSegmentEnd(seg);
            if (seqGt(socket->tcpSendNext, socket->tcpSendMax)) {
                socket->tcpSendMax = socket->tcpSendNext;
            }
            if (socket->tcpRtxDeadline == 0) {
                socket->tcpRtxDeadline = now + tcpCurrentRto(socket);
            }
        }
        return;
    }
    if (socket->tcpState == TcpState::Closed || socket->tcpState == TcpState::Listen ||
        socket->tcpState == TcpState::TimeWait) {
        return;
    }

    while (seg && seqLe(tcpSegmentEnd(seg), socket->tcpSendNext)) {
        seg = seg->next;
    }
    const uint32_t window = socket->tcpCwnd < socket->tcpPeerWindow ? socket->tcpCwnd : socket->tcpPeerWindow;
    while (seg) {
        const uint32_t end = tcpSegmentEnd(seg);
        const bool bareFin = seg->length == 0 && (seg->flags & kTcpFin) != 0;
        if (!bareFin && end - socket->tcpSendUnacked > window) {
            // Past the window. With nothing in flight one segment may still go
            // into a small but open window (the peer keeps what fits).
            const bool idle = socket->tcpSendNext == socket->tcpSendUnacked;
            if (!idle || socket->tcpPeerWindow == 0) {
                break;
            }
        }
        tcpSendSegment(socket, seg, now);
        socket->tcpSendNext = end;
        if (seqGt(end, socket->tcpSendMax)) {
            socket->tcpSendMax = end;
        }
        if (socket->tcpRtxDeadline == 0) {
            socket->tcpRtxDeadline = now + tcpCurrentRto(socket);
        }
        seg = seg->next;
    }
    // Data waits behind a zero window with nothing in flight: the timer will
    // probe it (persist).
    if (seg && socket->tcpRtxDeadline == 0) {
        socket->tcpRtxDeadline = now + tcpCurrentRto(socket);
    }
}

// Append a retained segment (data, SYN or FIN) at the end of the send queue
// and transmit what may go.
bool tcpQueueSegment(SocketObject* socket, uint8_t flags, const uint8_t* data, uint32_t length) {
    auto* seg = new TcpSendSegment {};
    if (!seg) {
        return false;
    }
    seg->seq = socket->tcpSendSeq;
    seg->length = length;
    seg->flags = flags;
    if (length != 0) {
        seg->data = new uint8_t[length];
        if (!seg->data) {
            delete seg;
            return false;
        }
        memcpy(seg->data, data, length);
    }
    if (socket->tcpSendTail) {
        socket->tcpSendTail->next = seg;
    } else {
        socket->tcpSendHead = seg;
    }
    socket->tcpSendTail = seg;
    socket->tcpSendSeq += tcpSegmentSpan(seg);
    tcpOutput(socket);
    return true;
}

// Free the segments `ackNum` covers. Returns the RTT sample in ms from the
// newest of them that was sent only once (Karn), or UINT64_MAX for none.
uint64_t tcpAckSendQueue(SocketObject* socket, uint32_t ackNum, uint64_t now) {
    uint64_t sample = UINT64_MAX;
    while (socket->tcpSendHead) {
        TcpSendSegment* seg = socket->tcpSendHead;
        if (seqLt(ackNum, tcpSegmentEnd(seg))) {
            break;
        }
        if ((seg->flags & kTcpFin) != 0) {
            socket->tcpFinAcked = true;
        }
        if (seg->transmissions == 1) {
            sample = now - seg->lastSentMs;
        }
        socket->tcpSendHead = seg->next;
        if (!socket->tcpSendHead) {
            socket->tcpSendTail = nullptr;
        }
        delete[] seg->data;
        delete seg;
    }
    if (seqGt(ackNum, socket->tcpSendUnacked)) {
        socket->tcpSendUnacked = ackNum;
    }
    if (seqLt(socket->tcpSendNext, socket->tcpSendUnacked)) {
        socket->tcpSendNext = socket->tcpSendUnacked;
    }
    return sample;
}

// The ACK field of an incoming segment (any synchronized state).
void tcpProcessAck(SocketObject* socket, const TcpSegmentInfo& seg, uint64_t now) {
    const uint32_t ack = seg.ack;
    if (seqGt(ack, socket->tcpSendMax)) {
        tcpSendControl(socket, kTcpAck);   // acknowledges what we never sent
        return;
    }
    if (seqLt(ack, socket->tcpSendUnacked)) {
        return;   // old
    }
    const uint32_t oldWindow = socket->tcpPeerWindow;
    socket->tcpPeerWindow = static_cast<uint32_t>(seg.window) << socket->tcpSendScale;
    socket->tcpLastAckRecvMs = now;
    const uint32_t mss = socket->tcpSendMss;

    if (seqGt(ack, socket->tcpSendUnacked)) {
        const uint32_t acked = ack - socket->tcpSendUnacked;
        const uint64_t sample = tcpAckSendQueue(socket, ack, now);
        if (sample != UINT64_MAX) {
            tcpRttSample(socket, sample);
        }
        socket->tcpBackoff = 0;
        socket->tcpDupAckCount = 0;
        if (socket->tcpInRecovery) {
            if (seqLt(ack, socket->tcpRecover)) {
                // NewReno partial ACK: the next hole is lost too.
                if (TcpSendSegment* head = socket->tcpSendHead) {
                    tcpSendSegment(socket, head, now);
                }
                socket->tcpCwnd = socket->tcpCwnd > acked ? socket->tcpCwnd - acked + mss : mss;
            } else {
                socket->tcpInRecovery = false;
                socket->tcpCwnd = socket->tcpSsthresh;
            }
        } else if (socket->tcpCwnd < socket->tcpSsthresh) {
            socket->tcpCwnd += acked < mss ? acked : mss;                     // slow start
        } else {
            const uint32_t step = mss * mss / socket->tcpCwnd;
            socket->tcpCwnd += step != 0 ? step : 1;                           // congestion avoidance
        }
        if (socket->tcpCwnd > (1U << 30)) {
            socket->tcpCwnd = 1U << 30;
        }
        socket->tcpRtxDeadline = socket->tcpSendUnacked == socket->tcpSendMax ? 0 : now + tcpCurrentRto(socket);
        return;
    }

    // ack == SND.UNA.
    if (socket->tcpPeerWindow == 0) {
        socket->tcpBackoff = 0;   // the peer answers our probes: it is alive
    }
    const bool duplicate = seg.payloadLength == 0 && (seg.flags & (kTcpSyn | kTcpFin)) == 0 &&
                           socket->tcpPeerWindow == oldWindow && socket->tcpSendUnacked != socket->tcpSendMax;
    if (!duplicate) {
        return;
    }
    socket->tcpDupAckCount++;
    if (socket->tcpInRecovery) {
        socket->tcpCwnd += mss;   // each duplicate means a segment has left the network
    } else if (socket->tcpDupAckCount == 3 && socket->tcpSendHead) {
        // Fast retransmit, then fast recovery (RFC 5681 3.2).
        const uint32_t flight = socket->tcpSendMax - socket->tcpSendUnacked;
        socket->tcpSsthresh = flight / 2 > 2 * mss ? flight / 2 : 2 * mss;
        socket->tcpRecover = socket->tcpSendMax;
        socket->tcpInRecovery = true;
        tcpSendSegment(socket, socket->tcpSendHead, now);
        socket->tcpCwnd = socket->tcpSsthresh + 3 * mss;
        socket->tcpRtxDeadline = now + tcpCurrentRto(socket);
    }
}

// After the application has read data: tell the peer once the window has
// grown by two segments. Without this a peer that filled the window waits for
// its persist timer (seconds) before sending again.
void tcpSendWindowUpdate(SocketObject* socket) {
    if (socket->tcpState != TcpState::Established &&
        socket->tcpState != TcpState::FinWait1 &&
        socket->tcpState != TcpState::FinWait2) {
        return;
    }
    const uint32_t now = tcpAdvertisedWindow(socket, false);
    if (now > socket->tcpLastWindow &&
        (static_cast<uint32_t>(now - socket->tcpLastWindow) << socket->tcpRecvScale) >= 2 * kTcpMss) {
        tcpSendControl(socket, kTcpAck);
    }
}

bool tcpAddressMatches(const SocketObject* socket, uint16_t srcPort, uint32_t srcIp) {
    if (socketAddressPort(socket->peerAddress, socket->peerAddressLength) != srcPort) {
        return false;
    }
    uint32_t peerIp = 0;
    if (socket->peerAddressLength >= 8) {
        memcpy(&peerIp, socket->peerAddress + 4, sizeof(peerIp));
    }
    return peerIp == srcIp;
}

// Append in-order bytes to the socket receive (datagram) queue.
bool tcpAppendReceived(SocketObject* socket, const uint8_t* data, uint32_t length) {
    if (length == 0) {
        return true;
    }
    auto* datagram = new Datagram {};
    if (!datagram) {
        return false;
    }
    datagram->data = new uint8_t[length];
    if (!datagram->data) {
        delete datagram;
        return false;
    }
    memcpy(datagram->data, data, length);
    datagram->length = length;
    if (socket->datagramTail) {
        socket->datagramTail->next = datagram;
    } else {
        socket->datagramHead = datagram;
    }
    socket->datagramTail = datagram;
    socket->tcpRecvNext += length;
    socket->tcpRecvQueued += length;
    return true;
}

void tcpFreeReassembly(SocketObject* socket, TcpReassemblySegment* r) {
    socket->tcpReasmQueued -= r->length < socket->tcpReasmQueued ? r->length : socket->tcpReasmQueued;
    delete[] r->data;
    delete r;
}

// After advancing tcpRecvNext, pull any held segments that now continue the
// stream (trimming what overlaps) into the receive queue. Returns true if
// anything moved.
bool tcpDrainReassembly(SocketObject* socket) {
    bool moved = false;
    bool progress = true;
    while (progress) {
        progress = false;
        TcpReassemblySegment** link = &socket->tcpReasm;
        while (*link) {
            TcpReassemblySegment* r = *link;
            const uint32_t end = r->seq + r->length;
            if (seqLe(end, socket->tcpRecvNext)) {
                *link = r->next;   // already have all of it
                tcpFreeReassembly(socket, r);
                progress = true;
                continue;
            }
            if (seqLe(r->seq, socket->tcpRecvNext)) {
                const uint32_t skip = socket->tcpRecvNext - r->seq;
                tcpAppendReceived(socket, r->data + skip, r->length - skip);
                *link = r->next;
                tcpFreeReassembly(socket, r);
                progress = true;
                moved = true;
                continue;
            }
            link = &r->next;
        }
    }
    return moved;
}

// Hold an out-of-order segment until the gap before it fills.
void tcpBufferOutOfOrder(SocketObject* socket, uint32_t seq, const uint8_t* data, uint32_t length) {
    for (TcpReassemblySegment* r = socket->tcpReasm; r; r = r->next) {
        if (r->seq == seq && r->length >= length) {
            return;   // already held
        }
    }
    auto* r = new TcpReassemblySegment {};
    if (!r) {
        return;
    }
    r->data = new uint8_t[length];
    if (!r->data) {
        delete r;
        return;
    }
    memcpy(r->data, data, length);
    r->seq = seq;
    r->length = length;
    r->next = socket->tcpReasm;
    socket->tcpReasm = r;
    socket->tcpReasmQueued += length;
}

// Incoming payload in a state that accepts data. Trims what we already have
// and what does not fit the window, then queues or holds the rest.
void tcpReceiveData(SocketObject* socket, const TcpSegmentInfo& seg, uint64_t now) {
    uint32_t seq = seg.seq;
    const uint8_t* data = seg.payload;
    uint32_t length = static_cast<uint32_t>(seg.payloadLength);
    if (seqLt(seq, socket->tcpRecvNext)) {
        const uint32_t skip = socket->tcpRecvNext - seq;
        if (skip >= length) {
            tcpTransmit(socket, socket->tcpSendNext, kTcpAck, nullptr, 0, true);   // a duplicate: say where we are
            return;
        }
        seq += skip;
        data += skip;
        length -= skip;
    }
    const uint32_t space = tcpReceiveSpace(socket);
    const uint32_t offset = seq - socket->tcpRecvNext;
    if (offset >= space) {
        tcpSendControl(socket, kTcpAck);       // outside the window
        return;
    }
    if (offset + length > space) {
        length = space - offset;
    }
    socket->tcpLastDataRecvMs = now;
    if (offset != 0) {
        // A hole before this: hold it and send a duplicate ACK at once, which
        // is what drives the peer's fast retransmit.
        tcpBufferOutOfOrder(socket, seq, data, length);
        tcpTransmit(socket, socket->tcpSendNext, kTcpAck, nullptr, 0, true);
        return;
    }
    tcpAppendReceived(socket, data, length);
    const bool filledHole = tcpDrainReassembly(socket);
    socket->tcpAckPending++;
    // Delay the ACK only in the middle of a stream: not at its start (the
    // sender's window is still opening), not after a write ends (PSH: a
    // sender holding a short segment back for Nagle waits for this ACK).
    const bool quick = socket->tcpQuickAcks != 0;
    if (quick) {
        socket->tcpQuickAcks--;
    }
    if (filledHole || socket->tcpReasm || socket->tcpAckPending >= 2 || quick || (seg.flags & kTcpPsh) != 0) {
        tcpSendControl(socket, kTcpAck);
    } else if (socket->tcpAckDeadline == 0) {
        socket->tcpAckDeadline = now + kTcpDelayedAckMs;
    }
}

// Begin our side of the close: queue a FIN behind any data still to send.
void tcpStartClose(SocketObject* socket) {
    if (socket->tcpFinSent ||
        (socket->tcpState != TcpState::Established &&
         socket->tcpState != TcpState::CloseWait &&
         socket->tcpState != TcpState::SynReceived)) {
        return;
    }
    socket->tcpFinSent = true;
    socket->tcpState = socket->tcpState == TcpState::CloseWait ? TcpState::LastAck : TcpState::FinWait1;
    tcpQueueSegment(socket, kTcpFin | kTcpAck, nullptr, 0);
}

void tcpAbort(SocketObject* socket, int error) {
    socket->error = error;
    socket->readClosed = true;
    socket->writeClosed = true;
    socket->connected = false;
    socket->tcpState = TcpState::Closed;
    socket->tcpRtxDeadline = 0;
    socket->tcpAckDeadline = 0;
    freeTcpQueues(socket);
}

SocketObject* findTcpListener(uint16_t dstPort) {
    for (SocketObject* s = gSockets; s; s = s->next) {
        if (s->type == kSockStream && s->listening && s->bound &&
            tcpLocalPort(s) == dstPort &&
            acceptQueueLength(s) < static_cast<uint64_t>(s->listenBacklog)) {
            return s;
        }
    }
    return nullptr;
}

// Find an established/connecting connection matching the 4-tuple.
SocketObject* findTcpConnection(uint16_t dstPort, uint16_t srcPort, uint32_t srcIp) {
    for (SocketObject* s = gSockets; s; s = s->next) {
        if (s->type != kSockStream || !s->remoteTcp || !s->bound || s->listening) {
            continue;
        }
        if (tcpLocalPort(s) == dstPort && tcpAddressMatches(s, srcPort, srcIp)) {
            return s;
        }
    }
    return nullptr;
}

void processTcpSegment(const TcpSegmentInfo& seg);

// Answer a segment that belongs to no connection with a RST (RFC 793, "reset
// generation"): it takes its sequence number from the segment's ACK, or ACKs
// everything the segment occupied.
void tcpRefuseSegment(const TcpSegmentInfo& seg) {
    if ((seg.flags & kTcpRst) != 0) {
        return;
    }
    uint8_t peer[16] {};
    uint16_t family = kAfInet;
    memcpy(peer, &family, sizeof(family));
    memcpy(peer + 2, &seg.srcPort, sizeof(seg.srcPort));
    memcpy(peer + 4, &seg.srcIpNetworkOrder, sizeof(seg.srcIpNetworkOrder));
    uint32_t seq = 0;
    uint32_t ack = 0;
    uint8_t flags = kTcpRst;
    if ((seg.flags & kTcpAck) != 0) {
        seq = seg.ack;
    } else {
        ack = seg.seq + static_cast<uint32_t>(seg.payloadLength) +
              ((seg.flags & kTcpSyn) ? 1 : 0) + ((seg.flags & kTcpFin) ? 1 : 0);
        flags |= kTcpAck;
    }
    netTransmitTcpSegment(seg.dstPort, peer, sizeof(peer), seq, ack, flags, 0, nullptr, 0);
}

}  // namespace

void socketProcessTcpSegment(const TcpSegmentInfo& seg) {
    processTcpSegment(seg);
    // A connection state change, new data, FIN (EOF), reset or error may make
    // a process blocked in connect()/recv()/accept()/poll() runnable. Wake them
    // so they re-scan (a Blocked process is invisible to the scheduler until
    // explicitly re-readied). Every path through processTcpSegment needs this,
    // including the early returns for SYN-ACK and RST.
    Scheduler::get().wakeAllBlockedProcesses();
}

void socketProcessIcmpError(const IcmpErrorInfo& info) {
    if (info.protocol == IcmpErrorInfo::Tcp) {
        SocketObject* socket = findTcpConnection(info.localPort, info.remotePort, info.remoteIpNetworkOrder);
        // Only an error about a sequence number still in flight counts
        // (RFC 5927), so a stray or forged one cannot kill a connection.
        if (!socket || info.tcpSeq - socket->tcpSendUnacked > socket->tcpSendMax - socket->tcpSendUnacked) {
            return;
        }
        // "Fragmentation needed" is path MTU business, not a failure.
        if (info.error == SysErrMessageSize) {
            return;
        }
        if (socket->tcpState == TcpState::SynSent || socket->tcpState == TcpState::SynReceived) {
            tcpAbort(socket, info.error);
        } else {
            socket->tcpSoftError = info.error;
        }
        Scheduler::get().wakeAllBlockedProcesses();
        return;
    }

    // UDP: only a connected socket hears about errors, and only hard ones.
    if (!info.hard) {
        return;
    }
    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockDgram || !socket->bound || !socket->connected ||
            socketAddressPort(socket->localAddress, socket->localAddressLength) != info.localPort ||
            socketAddressPort(socket->peerAddress, socket->peerAddressLength) != info.remotePort ||
            socketAddressIp(socket->peerAddress, socket->peerAddressLength) != info.remoteIpNetworkOrder) {
            continue;
        }
        socket->error = info.error;
        Scheduler::get().wakeAllBlockedProcesses();
        return;
    }
}

namespace {
void processTcpSegment(const TcpSegmentInfo& seg) {
    const uint16_t dstPort = seg.dstPort;
    const uint16_t srcPort = seg.srcPort;
    const uint32_t srcIp = seg.srcIpNetworkOrder;
    const uint64_t now = time_get_uptime_ms();

    SocketObject* socket = findTcpConnection(dstPort, srcPort, srcIp);

    // No existing connection: only a bare SYN to a listener is interesting;
    // anything else is refused with a RST, as RFC 793 asks for a closed port.
    if (!socket) {
        if ((seg.flags & (kTcpSyn | kTcpAck | kTcpRst)) != kTcpSyn) {
            tcpRefuseSegment(seg);
            return;
        }
        SocketObject* listener = findTcpListener(dstPort);
        if (!listener) {
            tcpRefuseSegment(seg);
            return;
        }
        SocketObject* accepted = createSocketObject(listener->domain, listener->type, listener->protocol);
        if (!accepted) {
            return;
        }
        accepted->bound = true;
        accepted->remoteTcp = true;
        memcpy(accepted->localAddress, listener->localAddress, listener->localAddressLength);
        accepted->localAddressLength = listener->localAddressLength;
        uint16_t family = kAfInet;
        memcpy(accepted->peerAddress, &family, sizeof(family));
        memcpy(accepted->peerAddress + 2, &srcPort, sizeof(srcPort));
        memcpy(accepted->peerAddress + 4, &srcIp, sizeof(srcIp));
        accepted->peerAddressLength = 16;
        uint32_t isn = 0;
        kernel_fill_entropy(&isn, sizeof(isn));
        tcpInitConnection(accepted, isn);
        accepted->tcpState = TcpState::SynReceived;
        accepted->tcpRecvNext = seg.seq + 1;   // consume the peer's SYN
        accepted->tcpPeerWindow = seg.window;  // never scaled in a SYN
        accepted->tcpOfferedScale = seg.options.hasWindowScale;   // answer in kind
        tcpEstablish(accepted, seg.options);
        // Link into the listener's accept queue now; promoted to the app on
        // the final ACK (it is already reachable for matching).
        if (listener->acceptTail) {
            listener->acceptTail->acceptNext = accepted;
        } else {
            listener->acceptHead = accepted;
        }
        listener->acceptTail = accepted;
        tcpQueueSegment(accepted, kTcpSyn | kTcpAck, nullptr, 0);
        return;
    }

    // RST: abort, but only one that names our SYN or falls in the window
    // (RFC 5961 lite), so a blind reset cannot guess its way in.
    if ((seg.flags & kTcpRst) != 0) {
        if (socket->tcpState == TcpState::SynSent) {
            if ((seg.flags & kTcpAck) == 0 || seg.ack != socket->tcpSendMax) {
                return;
            }
            tcpAbort(socket, SysErrConnectionRefused);
            return;
        }
        const uint32_t window = tcpReceiveSpace(socket) ? tcpReceiveSpace(socket) : 1;
        if (seqLt(seg.seq, socket->tcpRecvNext) || seqGe(seg.seq, socket->tcpRecvNext + window)) {
            return;
        }
        tcpAbort(socket, SysErrConnectionReset);
        return;
    }

    // SYN-ACK completing our active open.
    if (socket->tcpState == TcpState::SynSent) {
        if ((seg.flags & (kTcpSyn | kTcpAck)) == (kTcpSyn | kTcpAck)) {
            if (seg.ack != socket->tcpSendMax) {
                tcpRefuseSegment(seg);   // not an answer to our SYN
                return;
            }
            const uint64_t sample = tcpAckSendQueue(socket, seg.ack, now);
            if (sample != UINT64_MAX) {
                tcpRttSample(socket, sample);
            }
            socket->tcpBackoff = 0;
            socket->tcpRtxDeadline = 0;
            socket->tcpRecvNext = seg.seq + 1;   // consume the peer's SYN
            socket->tcpPeerWindow = seg.window;  // never scaled in a SYN
            tcpEstablish(socket, seg.options);
            socket->tcpState = TcpState::Established;
            socket->connected = true;
            socket->tcpLastAckRecvMs = now;
            tcpSendControl(socket, kTcpAck);
        } else if ((seg.flags & kTcpSyn) != 0) {
            // Simultaneous open: the peer's SYN crossed ours.
            socket->tcpRecvNext = seg.seq + 1;
            socket->tcpPeerWindow = seg.window;
            tcpEstablish(socket, seg.options);
            socket->tcpState = TcpState::SynReceived;
            if (socket->tcpSendHead) {
                socket->tcpSendHead->flags = kTcpSyn | kTcpAck;
                socket->tcpSendNext = socket->tcpSendUnacked;
                tcpOutput(socket);
            }
        }
        return;
    }

    if ((seg.flags & kTcpSyn) != 0) {
        // The peer repeats its SYN: our SYN-ACK (or the ACK of theirs) was lost.
        if (socket->tcpState == TcpState::SynReceived && socket->tcpSendHead) {
            tcpSendSegment(socket, socket->tcpSendHead, now);
        } else {
            tcpSendControl(socket, kTcpAck);
        }
        return;
    }
    if ((seg.flags & kTcpAck) == 0) {
        return;   // every segment after the handshake carries an ACK
    }

    tcpProcessAck(socket, seg, now);
    if (socket->tcpState == TcpState::Closed) {
        return;
    }
    // Our SYN-ACK acknowledged: a passive open is complete.
    if (socket->tcpState == TcpState::SynReceived && socket->tcpSendUnacked == socket->tcpSendMax) {
        socket->tcpState = TcpState::Established;
        socket->connected = true;
    }
    // Our FIN acknowledged: advance the close.
    if (socket->tcpFinAcked) {
        if (socket->tcpState == TcpState::FinWait1) {
            socket->tcpState = TcpState::FinWait2;
        } else if (socket->tcpState == TcpState::Closing) {
            socket->tcpState = TcpState::TimeWait;
            socket->tcpTimeWaitDeadline = now + kTcpTimeWaitMs;
        } else if (socket->tcpState == TcpState::LastAck) {
            socket->tcpState = TcpState::Closed;
            socket->connected = false;
            socket->tcpRtxDeadline = 0;
            return;
        }
    }

    const bool acceptsData = socket->tcpState == TcpState::Established ||
                             socket->tcpState == TcpState::FinWait1 ||
                             socket->tcpState == TcpState::FinWait2;
    if (seg.payloadLength != 0 && acceptsData) {
        if (socket->tcpOrphan) {
            // Nobody is left to read it (RFC 1122 4.2.2.13): reset.
            tcpTransmit(socket, socket->tcpSendMax, kTcpRst | kTcpAck, nullptr, 0);
            tcpAbort(socket, SysErrConnectionReset);
            return;
        }
        tcpReceiveData(socket, seg, now);
    }

    // FIN: the peer is done sending. It counts once everything before it is in.
    if ((seg.flags & kTcpFin) != 0) {
        const uint32_t finSeq = seg.seq + static_cast<uint32_t>(seg.payloadLength);
        if (finSeq == socket->tcpRecvNext && !socket->tcpPeerFinSeen) {
            socket->tcpRecvNext += 1;
            socket->tcpPeerFinSeen = true;
            socket->readClosed = true;
            tcpSendControl(socket, kTcpAck);
            switch (socket->tcpState) {
                case TcpState::Established:
                case TcpState::SynReceived:
                    socket->tcpState = TcpState::CloseWait;
                    break;
                case TcpState::FinWait1:
                    if (socket->tcpFinAcked) {
                        socket->tcpState = TcpState::TimeWait;
                        socket->tcpTimeWaitDeadline = now + kTcpTimeWaitMs;
                    } else {
                        socket->tcpState = TcpState::Closing;
                    }
                    break;
                case TcpState::FinWait2:
                    socket->tcpState = TcpState::TimeWait;
                    socket->tcpTimeWaitDeadline = now + kTcpTimeWaitMs;
                    break;
                default:
                    break;
            }
        } else if (socket->tcpPeerFinSeen && finSeq + 1 == socket->tcpRecvNext) {
            tcpSendControl(socket, kTcpAck);   // a repeated FIN: our ACK was lost
        }
    }

    tcpOutput(socket);   // the window may have opened, cwnd grown
}

// Per-connection timers: delayed ACK, TIME_WAIT, retransmission and persist.
void tcpTimers(SocketObject* socket, uint64_t now) {
    if (socket->tcpAckDeadline != 0 && now >= socket->tcpAckDeadline) {
        tcpSendControl(socket, kTcpAck);
    }
    if (socket->tcpState == TcpState::TimeWait) {
        if (now >= socket->tcpTimeWaitDeadline) {
            socket->tcpState = TcpState::Closed;
            socket->connected = false;
            freeTcpQueues(socket);
        }
        return;
    }
    if (socket->tcpRtxDeadline == 0 || now < socket->tcpRtxDeadline) {
        return;
    }
    socket->tcpRtxDeadline = 0;
    if (!socket->tcpSendHead) {
        return;
    }
    const uint32_t limit = socket->tcpState == TcpState::SynSent || socket->tcpState == TcpState::SynReceived
        ? kTcpSynRetries : kTcpMaxRetries;
    if (socket->tcpBackoff >= limit) {
        // Give up: the connection is dead. An ICMP error seen meanwhile says
        // why better than a bare timeout (Linux reports it the same way).
        tcpAbort(socket, socket->tcpSoftError ? socket->tcpSoftError : SysErrTimedOut);
        Scheduler::get().wakeAllBlockedProcesses();
        return;
    }
    socket->tcpBackoff++;
    if (socket->tcpSendUnacked == socket->tcpSendMax) {
        // Nothing in flight, data held back by a zero window: probe it with
        // the next segment, which also restarts the timer.
        TcpSendSegment* seg = socket->tcpSendHead;
        tcpSendSegment(socket, seg, now);
        socket->tcpSendNext = tcpSegmentEnd(seg);
        socket->tcpSendMax = socket->tcpSendNext;
        socket->tcpRtxDeadline = now + tcpCurrentRto(socket);
        return;
    }
    // Timeout: collapse the window and go back to the oldest unacknowledged
    // byte (RFC 5681 3.1, RFC 6298 5.4-5.6).
    const uint32_t flight = socket->tcpSendMax - socket->tcpSendUnacked;
    const uint32_t mss = socket->tcpSendMss;
    socket->tcpSsthresh = flight / 2 > 2 * mss ? flight / 2 : 2 * mss;
    socket->tcpCwnd = mss;
    socket->tcpInRecovery = false;
    socket->tcpDupAckCount = 0;
    socket->tcpSendNext = socket->tcpSendUnacked;
    tcpOutput(socket);
}
}  // namespace

void socketTcpTick() {
    const uint64_t now = time_get_uptime_ms();
    SocketObject* next = nullptr;
    for (SocketObject* socket = gSockets; socket; socket = next) {
        next = socket->next;
        if (socket->type != kSockStream || !socket->remoteTcp) {
            continue;
        }
        tcpTimers(socket, now);
        // A closed socket finishing its close alone goes once it is done, or
        // once it has taken too long (the peer never closes its side).
        if (socket->tcpOrphan &&
            (socket->tcpState == TcpState::Closed || now >= socket->tcpOrphanDeadline)) {
            destroySocketObject(socket);
        }
    }
}

namespace {

// The last handle to a live connection was closed. Returns true when it stays
// as an orphan that sends what is queued and a FIN; false means abort it
// (unread data was thrown away, or SO_LINGER with a zero timeout, as Linux).
bool tcpBeginOrphan(SocketObject* socket) {
    switch (socket->tcpState) {
        case TcpState::Established:
        case TcpState::CloseWait:
        case TcpState::SynReceived:
        case TcpState::FinWait1:
        case TcpState::FinWait2:
        case TcpState::Closing:
        case TcpState::LastAck:
            break;
        default:
            return false;
    }
    if (socket->datagramHead || (socket->lingerEnabled && socket->lingerSeconds == 0)) {
        return false;
    }
    tcpStartClose(socket);
    socket->tcpOrphan = true;
    socket->readClosed = true;
    socket->tcpOrphanDeadline = time_get_uptime_ms() + kTcpOrphanTimeoutMs;
    return true;
}

// TCP_INFO (struct tcp_info, the fields up to tcpi_total_retrans).
struct TcpInfo {
    uint8_t state;
    uint8_t caState;
    uint8_t retransmits;
    uint8_t probes;
    uint8_t backoff;
    uint8_t options;
    uint8_t windowScales;   // snd_wscale in the low nibble, rcv_wscale high
    uint8_t flags;
    uint32_t rto;           // microseconds
    uint32_t ato;
    uint32_t sndMss;
    uint32_t rcvMss;
    uint32_t unacked;       // segments
    uint32_t sacked;
    uint32_t lost;
    uint32_t retrans;
    uint32_t fackets;
    uint32_t lastDataSent;  // milliseconds ago
    uint32_t lastAckSent;
    uint32_t lastDataRecv;
    uint32_t lastAckRecv;
    uint32_t pmtu;
    uint32_t rcvSsthresh;
    uint32_t rtt;           // microseconds
    uint32_t rttvar;
    uint32_t sndSsthresh;   // segments
    uint32_t sndCwnd;       // segments
    uint32_t advmss;
    uint32_t reordering;
    uint32_t rcvRtt;
    uint32_t rcvSpace;
    uint32_t totalRetrans;
};

void fillTcpInfo(const SocketObject* socket, TcpInfo* info) {
    memset(info, 0, sizeof(*info));
    static const uint8_t kLinuxState[] = {
        7,   // Closed
        10,  // Listen
        2,   // SynSent
        3,   // SynReceived
        1,   // Established
        4,   // FinWait1
        5,   // FinWait2
        8,   // CloseWait
        9,   // LastAck
        11,  // Closing
        6,   // TimeWait
    };
    const uint64_t now = time_get_uptime_ms();
    const uint32_t mss = socket->tcpSendMss ? socket->tcpSendMss : kTcpMss;
    info->state = socket->listening ? 10 : kLinuxState[static_cast<uint8_t>(socket->tcpState)];
    info->caState = socket->tcpInRecovery ? 3 : (socket->tcpBackoff ? 4 : 0);   // Recovery / Loss / Open
    info->retransmits = static_cast<uint8_t>(socket->tcpBackoff);
    info->backoff = static_cast<uint8_t>(socket->tcpBackoff);
    if (socket->tcpRecvScale != 0 || socket->tcpSendScale != 0) {
        info->options = 4;   // TCPI_OPT_WSCALE
    }
    info->windowScales = static_cast<uint8_t>((socket->tcpSendScale & 0xF) | (socket->tcpRecvScale << 4));
    info->rto = static_cast<uint32_t>(tcpCurrentRto(socket) * 1000);
    info->ato = static_cast<uint32_t>(kTcpDelayedAckMs * 1000);
    info->sndMss = mss;
    info->rcvMss = kTcpMss;
    info->unacked = (socket->tcpSendMax - socket->tcpSendUnacked + mss - 1) / mss;
    info->lastDataSent = static_cast<uint32_t>(now - socket->tcpLastDataSentMs);
    info->lastDataRecv = static_cast<uint32_t>(now - socket->tcpLastDataRecvMs);
    info->lastAckRecv = static_cast<uint32_t>(now - socket->tcpLastAckRecvMs);
    info->pmtu = 1500;
    info->rcvSsthresh = kTcpReceiveBuffer;
    info->rtt = static_cast<uint32_t>(socket->tcpSrttUs);
    info->rttvar = static_cast<uint32_t>(socket->tcpRttvarUs);
    info->sndSsthresh = socket->tcpSsthresh == 0xFFFFFFFFU ? 0x7FFFFFFF : socket->tcpSsthresh / mss;
    info->sndCwnd = socket->tcpCwnd / mss;
    info->advmss = kTcpMss;
    info->reordering = 3;
    info->rcvSpace = tcpReceiveSpace(socket);
    info->totalRetrans = socket->tcpTotalRetrans;
}

}  // namespace


uint64_t Syscall::sys_recv(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags) {
    return sys_recvfrom(socketHandle, buffer, length, flags, 0);
}

// recv() that also reports where the data came from: a sockaddr_in (16 bytes)
// at `addressOut` unless it is 0.
uint64_t Syscall::sys_recvfrom(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags,
                               uint64_t addressOut) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightRead,
        &error
    );
    if (!socket) {
        return error;
    }
    if (length != 0 && !isValidUserPointer(buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }
    if (socket->type != kSockDgram && socket->type != kSockStream) {
        return syscall_error(SysErrNoSys);
    }
    const bool mayWait = !socket->nonBlocking && (flags & kMsgDontWait) == 0;
    const uint64_t deadline = socketDeadline(socket->receiveTimeoutMs);
    Datagram* datagram = nullptr;
    for (;;) {
        // Surface a pending connection error (RST / retransmission timeout)
        // once any buffered data has been consumed.
        if (socket->remoteTcp && socket->error != 0 && !socket->datagramHead) {
            return takeSocketError(socket);
        }
        // UDP: an ICMP error about what we sent is reported before data.
        if (socket->type == kSockDgram && socket->error != 0) {
            return takeSocketError(socket);
        }
        // For remote TCP, only report EOF (readClosed) once the receive queue
        // is drained: bytes received before the FIN must still be returned.
        if (socket->readClosed && (!socket->remoteTcp || !socket->datagramHead)) {
            return 0;
        }
        if (length == 0) {
            return 0;
        }
        datagram = socket->datagramHead;
        if (datagram) {
            break;
        }
        // Remote TCP: EOF once the peer's FIN has been seen.
        if (socket->remoteTcp && socket->tcpPeerFinSeen) {
            return 0;
        }
        if (socket->type == kSockStream && !socket->remoteTcp &&
            (!socket->peer || socket->peer->writeClosed)) {
            return 0;
        }
        // A stream that never connected has nothing to wait for.
        if (socket->type == kSockStream && !socket->connected && !socket->remoteTcp) {
            return syscall_error(SysErrNotConnected);
        }
        if (!mayWait) {
            return syscall_error(SysErrAgain);
        }
        const uint64_t waited = waitForSocketEvent(socket, deadline);
        if (waited != 0) {
            return waited;
        }
    }
    if (addressOut != 0) {
        uint8_t source[kSockaddrInSize] {};
        const uint8_t* from = socket->type == kSockDgram ? datagram->sourceAddress : socket->peerAddress;
        const uint64_t fromLength = socket->type == kSockDgram ? datagram->sourceAddressLength
                                                               : socket->peerAddressLength;
        memcpy(source, from, fromLength < kSockaddrInSize ? fromLength : kSockaddrInSize);
        // A local sender bound to any address sent from loopback.
        uint16_t family = kAfInet;
        memcpy(source, &family, sizeof(family));
        if (socketAddressIp(source, kSockaddrInSize) == 0) {
            const uint32_t loopback = kLoopbackAddressNetworkOrder;
            memcpy(source + 4, &loopback, sizeof(loopback));
        }
        if (!copyToUser(addressOut, source, sizeof(source))) {
            return syscall_error(SysErrInvalid);
        }
    }
    // A datagram socket returns one datagram (truncated to the buffer); a
    // stream fills the buffer from as many queued chunks as it takes.
    uint64_t copied = 0;
    while (datagram && copied < length) {
        const uint64_t chunk = datagram->length < length - copied ? datagram->length : length - copied;
        if (chunk != 0 && !copyToUser(buffer + copied, datagram->data, static_cast<size_t>(chunk))) {
            return copied != 0 ? copied : syscall_error(SysErrInvalid);
        }
        copied += chunk;
        if (socket->type == kSockStream && chunk < datagram->length) {
            const uint64_t remaining = datagram->length - chunk;
            memmove(datagram->data, datagram->data + chunk, static_cast<size_t>(remaining));
            datagram->length = remaining;
            break;
        }
        socket->datagramHead = datagram->next;
        if (!socket->datagramHead) {
            socket->datagramTail = nullptr;
        }
        delete[] datagram->data;
        delete datagram;
        if (socket->type != kSockStream) {
            break;
        }
        datagram = socket->datagramHead;
    }
    if (socket->remoteTcp) {
        socket->tcpRecvQueued -= copied < socket->tcpRecvQueued ? static_cast<uint32_t>(copied) : socket->tcpRecvQueued;
        tcpSendWindowUpdate(socket);
    }
    return copied;
}

uint64_t Syscall::sys_shutdown(uint64_t socketHandle, uint64_t how) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }

    if (how == kShutdownRead || how == kShutdownBoth) {
        socket->readClosed = true;
    } else if (how != kShutdownWrite) {
        return syscall_error(SysErrInvalid);
    }
    if (how == kShutdownWrite || how == kShutdownBoth) {
        socket->writeClosed = true;
        // Initiate an active close on a remote TCP connection: a FIN goes
        // behind whatever is still queued.
        if (socket->remoteTcp) {
            tcpStartClose(socket);
        }
    }
    // Readers of this socket, or of its local peer, may be waiting for EOF.
    Scheduler::get().wakeAllBlockedProcesses();
    return 0;
}

uint64_t Syscall::sys_getsockopt(
    uint64_t socketHandle,
    uint64_t level,
    uint64_t optionName,
    uint64_t optionValue,
    uint64_t optionLength
) {
    if (optionValue == 0 || optionLength == 0) {
        return syscall_error(SysErrInvalid);
    }

    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    if (level == kIpProtoTcp) {
        if (socket->type != kSockStream) {
            return syscall_error(SysErrOperationNotSupported);
        }
        switch (optionName) {
            case kTcpNoDelay:
                return copyIntOptionToUser(socket->tcpNoDelay ? 1 : 0, optionValue, optionLength);
            case kTcpMaxSeg:
                return copyIntOptionToUser(static_cast<int>(socket->remoteTcp ? socket->tcpSendMss : kTcpMss),
                                           optionValue, optionLength);
            case kTcpInfoOption: {
                uint32_t length = 0;
                if (!copyFromUser(&length, optionLength, sizeof(length))) {
                    return syscall_error(SysErrInvalid);
                }
                TcpInfo info {};
                fillTcpInfo(socket, &info);
                if (length > sizeof(info)) {
                    length = sizeof(info);
                }
                if (!copyToUser(optionValue, &info, length) ||
                    !copyToUser(optionLength, &length, sizeof(length))) {
                    return syscall_error(SysErrInvalid);
                }
                return 0;
            }
            default:
                return syscall_error(SysErrProtoOpt);
        }
    }
    if (level != kSolSocket) {
        return syscall_error(SysErrProtoOpt);
    }

    switch (optionName) {
        case kSoReuseAddr:
            return copyIntOptionToUser(socket->reuseAddress ? 1 : 0, optionValue, optionLength);
        case kSoKeepAlive:
            return copyIntOptionToUser(socket->keepAlive ? 1 : 0, optionValue, optionLength);
        case kSoBroadcast:
            return copyIntOptionToUser(socket->broadcast ? 1 : 0, optionValue, optionLength);
        case kSoReceiveBuffer:
            return copyIntOptionToUser(socket->receiveBufferSize, optionValue, optionLength);
        case kSoSendBuffer:
            return copyIntOptionToUser(socket->sendBufferSize, optionValue, optionLength);
        case kSoError: {
            const int value = socket->error;
            socket->error = 0;
            return copyIntOptionToUser(value, optionValue, optionLength);
        }
        case kSoType:
            return copyIntOptionToUser(socket->type, optionValue, optionLength);
        case kSoLinger:
            return copyLingerOptionToUser(*socket, optionValue, optionLength);
        case kSoReceiveTimeout:
        case kSoSendTimeout: {
            uint32_t length = 0;
            if (!copyFromUser(&length, optionLength, sizeof(length)) || length < sizeof(SocketTimeval)) {
                return syscall_error(SysErrInvalid);
            }
            const uint64_t ms = optionName == kSoReceiveTimeout
                ? socket->receiveTimeoutMs
                : socket->sendTimeoutMs;
            SocketTimeval tv { static_cast<int64_t>(ms / 1000), static_cast<int64_t>((ms % 1000) * 1000) };
            length = sizeof(SocketTimeval);
            if (!copyToUser(optionValue, &tv, sizeof(tv)) ||
                !copyToUser(optionLength, &length, sizeof(length))) {
                return syscall_error(SysErrInvalid);
            }
            return 0;
        }
        default:
            return syscall_error(SysErrProtoOpt);
    }
}

uint64_t Syscall::sys_setsockopt(
    uint64_t socketHandle,
    uint64_t level,
    uint64_t optionName,
    uint64_t optionValue,
    uint64_t optionLength
) {
    if (optionValue == 0) {
        return syscall_error(SysErrInvalid);
    }

    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightControl,
        &error
    );
    if (!socket) {
        return error;
    }
    int value = 0;
    if (level == kIpProtoTcp) {
        if (socket->type != kSockStream) {
            return syscall_error(SysErrOperationNotSupported);
        }
        switch (optionName) {
            case kTcpNoDelay:
                if (!copyIntOptionFromUser(optionValue, optionLength, &value)) {
                    return syscall_error(SysErrInvalid);
                }
                socket->tcpNoDelay = value != 0;
                return 0;
            // Accepted and not acted on: segments always go out at once, and
            // there are no keepalive probes yet.
            case kTcpMaxSeg:
            case kTcpKeepIdle:
            case kTcpKeepInterval:
            case kTcpKeepCount:
                if (!copyIntOptionFromUser(optionValue, optionLength, &value) || value <= 0) {
                    return syscall_error(SysErrInvalid);
                }
                return 0;
            default:
                return syscall_error(SysErrProtoOpt);
        }
    }
    if (level != kSolSocket) {
        return syscall_error(SysErrProtoOpt);
    }

    switch (optionName) {
        case kSoReuseAddr:
            if (!copyIntOptionFromUser(optionValue, optionLength, &value)) {
                return syscall_error(SysErrInvalid);
            }
            socket->reuseAddress = value != 0;
            return 0;
        case kSoKeepAlive:
            if (!copyIntOptionFromUser(optionValue, optionLength, &value)) {
                return syscall_error(SysErrInvalid);
            }
            socket->keepAlive = value != 0;
            return 0;
        case kSoBroadcast:
            if (!copyIntOptionFromUser(optionValue, optionLength, &value)) {
                return syscall_error(SysErrInvalid);
            }
            socket->broadcast = value != 0;
            return 0;
        case kSoReceiveBuffer:
            if (!copyIntOptionFromUser(optionValue, optionLength, &value) || value <= 0) {
                return syscall_error(SysErrInvalid);
            }
            socket->receiveBufferSize = value;
            return 0;
        case kSoSendBuffer:
            if (!copyIntOptionFromUser(optionValue, optionLength, &value) || value <= 0) {
                return syscall_error(SysErrInvalid);
            }
            socket->sendBufferSize = value;
            return 0;
        case kSoLinger: {
            if (optionLength < sizeof(SocketLinger)) {
                return syscall_error(SysErrInvalid);
            }
            SocketLinger linger {};
            if (!copyFromUser(&linger, optionValue, sizeof(linger))) {
                return syscall_error(SysErrInvalid);
            }
            socket->lingerEnabled = linger.on != 0;
            socket->lingerSeconds = linger.seconds < 0 ? 0 : linger.seconds;
            return 0;
        }
        case kSoReceiveTimeout:
        case kSoSendTimeout: {
            if (optionLength < sizeof(SocketTimeval)) {
                return syscall_error(SysErrInvalid);
            }
            SocketTimeval tv {};
            if (!copyFromUser(&tv, optionValue, sizeof(tv)) ||
                tv.seconds < 0 || tv.microseconds < 0 || tv.microseconds >= 1000000) {
                return syscall_error(SysErrInvalid);
            }
            uint64_t ms = static_cast<uint64_t>(tv.seconds) * 1000 +
                static_cast<uint64_t>(tv.microseconds + 999) / 1000;
            (optionName == kSoReceiveTimeout ? socket->receiveTimeoutMs : socket->sendTimeoutMs) = ms;
            return 0;
        }
        case kSoError:
        case kSoType:
            return syscall_error(SysErrInvalid);
        default:
            return syscall_error(SysErrProtoOpt);
    }
}

// Copy a stored socket address (local or peer) out to user space using the
// standard capacity-in / actual-length-out protocol shared with accept().
static uint64_t copySocketNameToUser(
    const uint8_t* storedAddress,
    uint64_t storedLength,
    uint64_t addrPtr,
    uint64_t addrLenPtr
) {
    if (addrLenPtr == 0) {
        return syscall_error(SysErrInvalid);
    }
    uint32_t capacity = 0;
    if (!Syscall::copyFromUser(&capacity, addrLenPtr, sizeof(capacity))) {
        return syscall_error(SysErrInvalid);
    }
    const uint32_t actual = static_cast<uint32_t>(storedLength);
    const uint32_t copied = capacity < actual ? capacity : actual;
    if (addrPtr != 0 && copied != 0 && !Syscall::copyToUser(addrPtr, storedAddress, copied)) {
        return syscall_error(SysErrInvalid);
    }
    if (!Syscall::copyToUser(addrLenPtr, &actual, sizeof(actual))) {
        return syscall_error(SysErrInvalid);
    }
    return 0;
}

uint64_t Syscall::sys_getsockname(uint64_t socketHandle, uint64_t addrPtr, uint64_t addrLenPtr) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightRead,
        &error
    );
    if (!socket) {
        return error;
    }
    // Bound to any address but talking to a remote host: the interface
    // address is the one in use, as Linux reports it.
    uint8_t name[kMaxSocketAddressSize];
    memcpy(name, socket->localAddress, sizeof(name));
    const bool remote = socket->remoteTcp ||
        (socket->type == kSockDgram && socket->connected &&
         !socketAddressIsLocalDestination(socket->peerAddress, socket->peerAddressLength));
    if (remote && socket->localAddressLength >= 8 && socketAddressIp(name, socket->localAddressLength) == 0) {
        const uint32_t local = netLocalAddressNetworkOrder();
        memcpy(name + 4, &local, sizeof(local));
    }
    return copySocketNameToUser(name, socket->localAddressLength, addrPtr, addrLenPtr);
}

uint64_t Syscall::sys_getpeername(uint64_t socketHandle, uint64_t addrPtr, uint64_t addrLenPtr) {
    uint64_t error = 0;
    SocketObject* socket = resolveSocket(
        Scheduler::get().getCurrentProcess(),
        socketHandle,
        HandleRightRead,
        &error
    );
    if (!socket) {
        return error;
    }
    // Only a connected socket has a peer address recorded.
    if (socket->peerAddressLength == 0) {
        return syscall_error(SysErrNotConnected);
    }
    return copySocketNameToUser(
        socket->peerAddress, socket->peerAddressLength, addrPtr, addrLenPtr);
}
