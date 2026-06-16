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
constexpr int kSolSocket = 1;
constexpr int kSoReuseAddr = 2;
constexpr int kSoKeepAlive = 9;
constexpr int kSoBroadcast = 6;
constexpr int kSoLinger = 13;
constexpr int kSoReceiveBuffer = 8;
constexpr int kSoSendBuffer = 7;
constexpr int kSoError = 4;
constexpr int kSoType = 3;
constexpr int kShutdownRead = 0;
constexpr int kShutdownWrite = 1;
constexpr int kShutdownBoth = 2;
constexpr uint64_t kMaxSocketAddressSize = 128;
constexpr uint64_t kMaxDatagramSize = 65507;
constexpr uint16_t kEphemeralPortStart = 49152;
constexpr uint16_t kEphemeralPortEnd = 65535;
constexpr uint32_t kLoopbackAddressNetworkOrder = 0x0100007f;
constexpr uint32_t kLocalAddressNetworkOrder = 0x0f02000a;
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
constexpr uint32_t kTcpMss = 1460;            // payload bytes per segment
constexpr uint32_t kTcpReceiveWindow = 65535; // advertised window cap
constexpr uint64_t kTcpRtoMs = 500;           // initial retransmission timeout
constexpr uint64_t kTcpRtoMaxMs = 8000;       // RTO ceiling after backoff
constexpr int kTcpMaxRetries = 8;             // give up after this many resends
constexpr uint64_t kTcpTimeWaitMs = 2000;     // simplified 2*MSL
constexpr uint32_t kTcpSendBufferCap = 65536; // max unacked+pending bytes

// A contiguous run of unacknowledged outbound bytes (or a SYN/FIN control
// segment).  Retained until the peer ACKs past it so it can be retransmitted.
struct TcpSendSegment {
    uint32_t seq;          // sequence number of first byte
    uint32_t length;       // payload bytes (0 for a pure FIN/SYN control seg)
    uint8_t flags;         // TCP flags to set when (re)transmitting
    uint64_t lastSentMs;   // time of last (re)transmission
    int retries;           // retransmission count
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
    uint8_t localAddress[kMaxSocketAddressSize];
    uint64_t localAddressLength;
    uint8_t peerAddress[kMaxSocketAddressSize];
    uint64_t peerAddressLength;
    uint32_t tcpSendSeq;       // SND.NXT: next seq to assign to new data
    uint32_t tcpRecvNext;      // RCV.NXT: next seq expected from peer
    // --- TCP state machine / reliability (remoteTcp sockets only) ---
    TcpState tcpState;
    uint32_t tcpSendUnacked;   // SND.UNA: oldest unacknowledged seq
    uint32_t tcpPeerWindow;    // peer's advertised receive window
    bool tcpFinSent;           // we have queued/sent our FIN
    bool tcpFinAcked;          // peer ACKed our FIN
    bool tcpPeerFinSeen;       // peer's FIN has been received & ACKed
    uint32_t tcpDupAckCount;   // consecutive duplicate ACKs
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
uint16_t tcpAdvertisedWindow(const SocketObject* socket);
void tcpSendControl(SocketObject* socket, uint8_t flags);
bool tcpQueueSegment(SocketObject* socket, uint8_t flags, const uint8_t* data, uint32_t length);

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
            socket->tcpSendSeq, socket->tcpRecvNext,
            kTcpRst | kTcpAck, 0, nullptr, 0);
    }
    freeTcpQueues(socket);
    freeDatagrams(socket);
    delete socket;
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
        return (protocol == 0 || protocol == kIpProtoUdp)
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
    return ip == 0 || ip == kLoopbackAddressNetworkOrder || ip == kLocalAddressNetworkOrder;
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

bool allocateEphemeralPort(SocketObject* socket, uint8_t* address, uint64_t length) {
    for (uint32_t port = kEphemeralPortStart; port <= kEphemeralPortEnd; ++port) {
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
}

uint64_t netSendUdpDatagram(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength, uint64_t buffer, uint64_t length);
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
        // For remote TCP, writable only once established and the send window
        // has room.
        if (socket->remoteTcp) {
            uint32_t inFlight = socket->tcpSendSeq - socket->tcpSendUnacked;
            uint32_t window = socket->tcpPeerWindow ? socket->tcpPeerWindow : kTcpMss;
            if ((socket->tcpState == TcpState::Established ||
                 socket->tcpState == TcpState::CloseWait) && inFlight < window) {
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
    // SOCK_CLOEXEC (02000000) flag bits OR'd into the base type. Strip them
    // before validating/creating the socket; libcurl in particular requests
    // SOCK_STREAM|SOCK_NONBLOCK. The flags are honoured at the handle level
    // (fcntl O_NONBLOCK / close-on-exec), so dropping them here is safe.
    constexpr int kSockNonblock = 04000;     // SOCK_NONBLOCK (Linux ABI)
    constexpr int kSockCloexec = 02000000;   // SOCK_CLOEXEC  (Linux ABI)
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
    memcpy(socket->peerAddress, copiedAddress, copiedLength);
    socket->peerAddressLength = copiedLength;

    if (socket->type == kSockStream) {
        // Re-entrant connect() on an in-progress/established remote connection
        // (mlibc retries connect() while EAGAIN): report current status.
        if (socket->remoteTcp) {
            if (socket->error != 0) {
                const int e = socket->error;
                socket->error = 0;
                return syscall_error(static_cast<SyscallErrno>(e));
            }
            if (socket->tcpState == TcpState::Established) {
                return 0;
            }
            if (socket->tcpState == TcpState::SynSent ||
                socket->tcpState == TcpState::SynReceived) {
                return syscall_error(SysErrAgain);
            }
        }
        if (!ensureSocketBound(socket)) {
            return syscall_error(SysErrAddressInUse);
        }
        SocketObject* listener = socketAddressIsLocalDestination(copiedAddress, copiedLength)
            ? findListeningStreamPeer(socket, copiedAddress, copiedLength)
            : nullptr;
        if (!listener) {
            // Active open to a remote host: initialize the TCP state machine
            // and transmit a SYN with a randomized ISN.  The SYN is retained on
            // the send queue and retransmitted by socketTcpTick() until ACKed.
            socket->remoteTcp = true;
            uint32_t isn = 0;
            kernel_fill_entropy(&isn, sizeof(isn));
            socket->tcpSendSeq = isn;
            socket->tcpSendUnacked = isn;
            socket->tcpRecvNext = 0;
            socket->tcpState = TcpState::SynSent;
            socket->connected = false;
            if (!tcpQueueSegment(socket, kTcpSyn, nullptr, 0)) {
                socket->tcpState = TcpState::Closed;
                return syscall_error(SysErrNoMemory);
            }
            // Non-blocking: the handshake completes as the userspace packet pump
            // delivers the SYN-ACK; the caller retries connect() (EAGAIN) until
            // the re-entrant check above reports Established.
            return syscall_error(SysErrAgain);
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
    SocketObject* accepted = socket->acceptHead;
    if (!accepted) {
        return syscall_error(SysErrAgain);
    }
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
    (void)flags;
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
            if (socket->error != 0) {
                const int e = socket->error;
                socket->error = 0;
                return syscall_error(static_cast<SyscallErrno>(e));
            }
            if (socket->tcpState != TcpState::Established &&
                socket->tcpState != TcpState::CloseWait) {
                return syscall_error(SysErrNotConnected);
            }

            // Backpressure: limit total unacknowledged bytes in flight to the
            // smaller of our send buffer and the peer's advertised window.
            uint32_t inFlight = socket->tcpSendSeq - socket->tcpSendUnacked;
            uint32_t window = socket->tcpPeerWindow ? socket->tcpPeerWindow : kTcpMss;
            uint32_t cap = window < kTcpSendBufferCap ? window : kTcpSendBufferCap;
            if (inFlight >= cap) {
                return syscall_error(SysErrAgain);  // window full, try later
            }
            uint64_t allowed = cap - inFlight;
            uint64_t toSend = length < allowed ? length : allowed;
            if (toSend == 0) {
                return syscall_error(SysErrAgain);
            }

            // Copy from user space into a kernel staging buffer, then segment
            // into MSS-sized PSH/ACK segments retained for retransmission.
            uint8_t staging[kTcpMss];
            uint64_t offset = 0;
            while (offset < toSend) {
                uint32_t chunk = static_cast<uint32_t>(toSend - offset);
                if (chunk > kTcpMss) {
                    chunk = kTcpMss;
                }
                if (!copyFromUser(staging, buffer + offset, chunk)) {
                    return offset != 0 ? offset : syscall_error(SysErrInvalid);
                }
                if (!tcpQueueSegment(socket, kTcpPsh | kTcpAck, staging, chunk)) {
                    return offset != 0 ? offset : syscall_error(SysErrAgain);
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
    if (length > kMaxDatagramSize || length > static_cast<uint64_t>(socket->sendBufferSize)) {
        return syscall_error(SysErrInvalid);
    }
    if (!ensureDatagramBound(socket)) {
        return syscall_error(SysErrAddressInUse);
    }
    SocketObject* target = socketAddressIsLocalDestination(socket->peerAddress, socket->peerAddressLength)
        ? findDatagramPeer(socket, socket->peerAddress, socket->peerAddressLength)
        : nullptr;
    if (!target) {
        return netSendUdpDatagram(
            socketAddressPort(socket->localAddress, socket->localAddressLength),
            socket->peerAddress,
            socket->peerAddressLength,
            buffer,
            length
        );
    }
    return enqueueDatagram(target, socket, buffer, length);
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

    uint8_t sourceAddress[kMaxSocketAddressSize] {};
    uint16_t family = kAfInet;
    memcpy(sourceAddress, &family, sizeof(family));
    memcpy(sourceAddress + 2, &srcPort, sizeof(srcPort));
    memcpy(sourceAddress + 4, &srcIpNetworkOrder, sizeof(srcIpNetworkOrder));
    const uint64_t sourceAddressLength = 16;

    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockDgram || !socket->bound || socket->readClosed ||
            socketAddressPort(socket->localAddress, socket->localAddressLength) != dstPort) {
            continue;
        }

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
            memcpy(datagram->data, payload, static_cast<size_t>(length));
        }
        datagram->length = length;
        memcpy(datagram->sourceAddress, sourceAddress, sourceAddressLength);
        datagram->sourceAddressLength = sourceAddressLength;
        if (socket->datagramTail) {
            socket->datagramTail->next = datagram;
        } else {
            socket->datagramHead = datagram;
        }
        socket->datagramTail = datagram;
        // Wake any process blocked in poll()/recv() on this socket. A blocked
        // process is invisible to the scheduler until re-readied, so without
        // this the datagram would sit unnoticed (lost-wakeup race).
        Scheduler::get().wakeAllBlockedProcesses();
        return true;
    }
    return false;
}

// ===========================================================================
// TCP state machine (remoteTcp sockets)
// ===========================================================================
namespace {

uint16_t tcpLocalPort(const SocketObject* s) {
    return socketAddressPort(s->localAddress, s->localAddressLength);
}

// Compute the advertised receive window from free receive-buffer space.
uint16_t tcpAdvertisedWindow(const SocketObject* socket) {
    uint64_t queued = 0;
    for (const Datagram* d = socket->datagramHead; d; d = d->next) {
        queued += d->length;
    }
    uint64_t free = queued >= kTcpReceiveWindow ? 0 : kTcpReceiveWindow - queued;
    if (free > 0xFFFF) {
        free = 0xFFFF;
    }
    return static_cast<uint16_t>(free);
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

// Transmit a control/ACK segment (no retained payload) for `socket`.
void tcpSendControl(SocketObject* socket, uint8_t flags) {
    netTransmitTcpSegment(
        tcpLocalPort(socket), socket->peerAddress, socket->peerAddressLength,
        socket->tcpSendSeq, socket->tcpRecvNext, flags,
        tcpAdvertisedWindow(socket), nullptr, 0);
}

// Enqueue a retained outbound segment (data, or a SYN/FIN control segment that
// consumes one sequence number) and transmit it immediately.  `seqLen` is the
// sequence space the segment consumes (payload length, or 1 for SYN/FIN).
bool tcpQueueSegment(SocketObject* socket, uint8_t flags, const uint8_t* data, uint32_t length) {
    auto* seg = new TcpSendSegment {};
    if (!seg) {
        return false;
    }
    seg->seq = socket->tcpSendSeq;
    seg->length = length;
    seg->flags = flags;
    seg->retries = 0;
    seg->lastSentMs = time_get_uptime_ms();
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

    // Sequence space consumed: payload length, or 1 for a bare SYN/FIN.
    const uint32_t seqLen = length != 0 ? length
        : ((flags & (kTcpSyn | kTcpFin)) ? 1u : 0u);
    socket->tcpSendSeq += seqLen;

    netTransmitTcpSegment(
        tcpLocalPort(socket), socket->peerAddress, socket->peerAddressLength,
        seg->seq, socket->tcpRecvNext, flags,
        tcpAdvertisedWindow(socket), seg->data, seg->length);
    return true;
}

// Drop send segments fully acknowledged by `ackNum` (cumulative).  Returns the
// number of segments freed.
int tcpAckSendQueue(SocketObject* socket, uint32_t ackNum) {
    int freed = 0;
    while (socket->tcpSendHead) {
        TcpSendSegment* seg = socket->tcpSendHead;
        const uint32_t seqLen = seg->length != 0 ? seg->length
            : ((seg->flags & (kTcpSyn | kTcpFin)) ? 1u : 0u);
        const uint32_t segEnd = seg->seq + seqLen;
        // Acked if ackNum has advanced to or past the segment's end (handle
        // wraparound with signed difference).
        if (static_cast<int32_t>(ackNum - segEnd) < 0) {
            break;  // not yet fully acknowledged
        }
        if ((seg->flags & kTcpFin) != 0) {
            socket->tcpFinAcked = true;
        }
        socket->tcpSendHead = seg->next;
        if (!socket->tcpSendHead) {
            socket->tcpSendTail = nullptr;
        }
        delete[] seg->data;
        delete seg;
        freed++;
    }
    if (static_cast<int32_t>(ackNum - socket->tcpSendUnacked) > 0) {
        socket->tcpSendUnacked = ackNum;
    }
    return freed;
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
    return true;
}

// After advancing tcpRecvNext, pull any buffered out-of-order segments that are
// now contiguous into the receive queue.
void tcpDrainReassembly(SocketObject* socket) {
    bool progress = true;
    while (progress) {
        progress = false;
        TcpReassemblySegment** link = &socket->tcpReasm;
        while (*link) {
            TcpReassemblySegment* r = *link;
            const uint32_t end = r->seq + r->length;
            if (static_cast<int32_t>(end - socket->tcpRecvNext) <= 0) {
                // Entirely old/duplicate — drop.
                *link = r->next;
                delete[] r->data;
                delete r;
                progress = true;
                continue;
            }
            if (r->seq == socket->tcpRecvNext) {
                tcpAppendReceived(socket, r->data, r->length);
                *link = r->next;
                delete[] r->data;
                delete r;
                progress = true;
                continue;
            }
            link = &r->next;
        }
    }
}

// Buffer an out-of-order segment for later reassembly (deduplicated by seq).
void tcpBufferOutOfOrder(SocketObject* socket, uint32_t seq, const uint8_t* data, uint32_t length) {
    if (length == 0) {
        return;
    }
    for (TcpReassemblySegment* r = socket->tcpReasm; r; r = r->next) {
        if (r->seq == seq) {
            return;  // already held
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

}  // namespace

void socketProcessTcpSegment(const TcpSegmentInfo& seg) {
    const uint16_t dstPort = seg.dstPort;
    const uint16_t srcPort = seg.srcPort;
    const uint32_t srcIp = seg.srcIpNetworkOrder;

    SocketObject* socket = findTcpConnection(dstPort, srcPort, srcIp);

    // No existing connection: only a bare SYN to a listener is interesting.
    if (!socket) {
        if ((seg.flags & kTcpSyn) != 0 && (seg.flags & kTcpAck) == 0) {
            SocketObject* listener = findTcpListener(dstPort);
            if (!listener) {
                return;
            }
            SocketObject* accepted = createSocketObject(listener->domain, listener->type, listener->protocol);
            if (!accepted) {
                return;
            }
            accepted->bound = true;
            accepted->remoteTcp = true;
            accepted->tcpState = TcpState::SynReceived;
            memcpy(accepted->localAddress, listener->localAddress, listener->localAddressLength);
            accepted->localAddressLength = listener->localAddressLength;
            uint16_t family = kAfInet;
            memcpy(accepted->peerAddress, &family, sizeof(family));
            memcpy(accepted->peerAddress + 2, &srcPort, sizeof(srcPort));
            memcpy(accepted->peerAddress + 4, &srcIp, sizeof(srcIp));
            accepted->peerAddressLength = 16;
            accepted->tcpRecvNext = seg.seq + 1;  // consume peer SYN
            accepted->tcpPeerWindow = seg.window;
            // Randomized ISN; SYN-ACK consumes one sequence number.
            uint32_t isn = 0;
            kernel_fill_entropy(&isn, sizeof(isn));
            accepted->tcpSendSeq = isn;
            accepted->tcpSendUnacked = isn;
            // Link into the listener's accept queue now; promoted to the app on
            // the final ACK (it is already reachable for matching).
            if (listener->acceptTail) {
                listener->acceptTail->acceptNext = accepted;
            } else {
                listener->acceptHead = accepted;
            }
            listener->acceptTail = accepted;
            tcpQueueSegment(accepted, kTcpSyn | kTcpAck, nullptr, 0);
        }
        return;
    }

    socket->tcpPeerWindow = seg.window;

    // RST: abort the connection.
    if ((seg.flags & kTcpRst) != 0) {
        socket->error = SysErrConnectionReset;
        socket->readClosed = true;
        socket->writeClosed = true;
        socket->connected = false;
        socket->tcpState = TcpState::Closed;
        freeTcpQueues(socket);
        return;
    }

    // SYN-ACK completing our active open.
    if (socket->tcpState == TcpState::SynSent) {
        if ((seg.flags & (kTcpSyn | kTcpAck)) == (kTcpSyn | kTcpAck)) {
            tcpAckSendQueue(socket, seg.ack);   // ACKs our SYN
            socket->tcpRecvNext = seg.seq + 1;  // consume peer SYN
            socket->tcpState = TcpState::Established;
            socket->connected = true;
            tcpSendControl(socket, kTcpAck);
        } else if ((seg.flags & kTcpSyn) != 0) {
            // Simultaneous open: peer SYN without ACK.
            socket->tcpRecvNext = seg.seq + 1;
            socket->tcpState = TcpState::SynReceived;
            tcpSendControl(socket, kTcpSyn | kTcpAck);
        }
        return;
    }

    // Final ACK of our SYN-ACK promotes a passive open to ESTABLISHED.
    if (socket->tcpState == TcpState::SynReceived) {
        if ((seg.flags & kTcpAck) != 0) {
            tcpAckSendQueue(socket, seg.ack);
            socket->tcpState = TcpState::Established;
            socket->connected = true;
        }
        // fall through to process any piggybacked data/FIN
    }

    // Process the ACK field for any state that can carry data.
    if ((seg.flags & kTcpAck) != 0) {
        const int32_t adv = static_cast<int32_t>(seg.ack - socket->tcpSendUnacked);
        if (adv > 0) {
            tcpAckSendQueue(socket, seg.ack);
            socket->tcpDupAckCount = 0;
            // Our FIN may now be acknowledged -> advance close states.
            if (socket->tcpFinAcked) {
                if (socket->tcpState == TcpState::FinWait1) {
                    socket->tcpState = TcpState::FinWait2;
                } else if (socket->tcpState == TcpState::Closing) {
                    socket->tcpState = TcpState::TimeWait;
                    socket->tcpTimeWaitDeadline = time_get_uptime_ms() + kTcpTimeWaitMs;
                } else if (socket->tcpState == TcpState::LastAck) {
                    socket->tcpState = TcpState::Closed;
                    socket->connected = false;
                }
            }
        } else if (adv == 0 && socket->tcpSendHead) {
            // Duplicate ACK: count for fast retransmit.
            if (++socket->tcpDupAckCount == 3) {
                TcpSendSegment* seg0 = socket->tcpSendHead;
                seg0->lastSentMs = time_get_uptime_ms();
                netTransmitTcpSegment(
                    tcpLocalPort(socket), socket->peerAddress, socket->peerAddressLength,
                    seg0->seq, socket->tcpRecvNext, seg0->flags,
                    tcpAdvertisedWindow(socket), seg0->data, seg0->length);
            }
        }
    }

    // Incoming data payload.
    if (seg.payloadLength != 0 &&
        (socket->tcpState == TcpState::Established ||
         socket->tcpState == TcpState::FinWait1 ||
         socket->tcpState == TcpState::FinWait2)) {
        const int32_t delta = static_cast<int32_t>(seg.seq - socket->tcpRecvNext);
        if (delta == 0) {
            tcpAppendReceived(socket, seg.payload, static_cast<uint32_t>(seg.payloadLength));
            tcpDrainReassembly(socket);
        } else if (delta > 0) {
            // Out-of-order: buffer for later, ACK current cumulative point.
            tcpBufferOutOfOrder(socket, seg.seq, seg.payload, static_cast<uint32_t>(seg.payloadLength));
        }
        // delta < 0 -> already-received duplicate, just re-ACK.
        tcpSendControl(socket, kTcpAck);
    }

    // Incoming FIN: peer closing its send direction.
    if ((seg.flags & kTcpFin) != 0) {
        // Only accept the FIN if it is in-order (its seq equals what we expect
        // next, accounting for any payload we just consumed).
        if (seg.seq + static_cast<uint32_t>(seg.payloadLength) == socket->tcpRecvNext) {
            socket->tcpRecvNext += 1;  // FIN consumes one sequence number
            socket->tcpPeerFinSeen = true;
            socket->readClosed = true;
            tcpSendControl(socket, kTcpAck);
            switch (socket->tcpState) {
                case TcpState::Established:
                case TcpState::SynReceived:
                    socket->tcpState = TcpState::CloseWait;
                    break;
                case TcpState::FinWait1:
                    socket->tcpState = socket->tcpFinAcked ? TcpState::TimeWait : TcpState::Closing;
                    if (socket->tcpState == TcpState::TimeWait) {
                        socket->tcpTimeWaitDeadline = time_get_uptime_ms() + kTcpTimeWaitMs;
                    }
                    break;
                case TcpState::FinWait2:
                    socket->tcpState = TcpState::TimeWait;
                    socket->tcpTimeWaitDeadline = time_get_uptime_ms() + kTcpTimeWaitMs;
                    break;
                default:
                    break;
            }
        }
    }

    // A connection state change, new data, FIN (EOF) or error here may make a
    // process blocked in connect()/recv()/poll() runnable. Wake them so they
    // re-scan (lost-wakeup race otherwise — a Blocked process is invisible to
    // the scheduler until explicitly re-readied).
    Scheduler::get().wakeAllBlockedProcesses();
}

void socketTcpTick() {
    const uint64_t now = time_get_uptime_ms();
    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockStream || !socket->remoteTcp) {
            continue;
        }

        // TIME_WAIT expiry.
        if (socket->tcpState == TcpState::TimeWait &&
            now >= socket->tcpTimeWaitDeadline) {
            socket->tcpState = TcpState::Closed;
            socket->connected = false;
            freeTcpQueues(socket);
            continue;
        }

        // Retransmit the oldest unacknowledged segment past its RTO.
        TcpSendSegment* seg = socket->tcpSendHead;
        if (!seg) {
            continue;
        }
        // Exponential backoff: RTO doubles per retry, capped.
        uint64_t rto = kTcpRtoMs << (seg->retries < 5 ? seg->retries : 5);
        if (rto > kTcpRtoMaxMs) {
            rto = kTcpRtoMaxMs;
        }
        if (now - seg->lastSentMs < rto) {
            continue;
        }
        if (seg->retries >= kTcpMaxRetries) {
            // Give up: connection is dead.
            socket->error = SysErrTimedOut;
            socket->readClosed = true;
            socket->writeClosed = true;
            socket->connected = false;
            socket->tcpState = TcpState::Closed;
            freeTcpQueues(socket);
            Scheduler::get().wakeAllBlockedProcesses();
            continue;
        }
        seg->retries++;
        seg->lastSentMs = now;
        netTransmitTcpSegment(
            tcpLocalPort(socket), socket->peerAddress, socket->peerAddressLength,
            seg->seq, socket->tcpRecvNext, seg->flags,
            tcpAdvertisedWindow(socket), seg->data, seg->length);
    }
}


uint64_t Syscall::sys_recv(uint64_t socketHandle, uint64_t buffer, uint64_t length, uint64_t flags) {
    (void)flags;
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
    // Surface a pending connection error (RST / retransmission timeout) once
    // any buffered data has been consumed.
    if (socket->remoteTcp && socket->error != 0 && !socket->datagramHead) {
        const int e = socket->error;
        socket->error = 0;
        return syscall_error(static_cast<SyscallErrno>(e));
    }
    // For remote TCP, only report EOF (readClosed) once the receive queue is
    // drained — buffered bytes received before the FIN must still be returned.
    if (socket->readClosed && (!socket->remoteTcp || !socket->datagramHead)) {
        return 0;
    }
    if (length == 0) {
        return 0;
    }
    if (socket->type != kSockDgram && socket->type != kSockStream) {
        return syscall_error(SysErrNoSys);
    }
    Datagram* datagram = socket->datagramHead;
    if (!datagram) {
        // Remote TCP: EOF once the peer's FIN has been seen.
        if (socket->remoteTcp && socket->tcpPeerFinSeen) {
            return 0;
        }
        if (socket->type == kSockStream && !socket->remoteTcp &&
            (!socket->peer || socket->peer->writeClosed)) {
            return 0;
        }
        return syscall_error(SysErrAgain);
    }
    const uint64_t copied = datagram->length < length ? datagram->length : length;
    if (copied != 0 && !copyToUser(buffer, datagram->data, static_cast<size_t>(copied))) {
        return syscall_error(SysErrInvalid);
    }
    if (socket->type == kSockDgram) {
    }
    if (socket->type == kSockStream && copied < datagram->length) {
        const uint64_t remaining = datagram->length - copied;
        memmove(datagram->data, datagram->data + copied, static_cast<size_t>(remaining));
        datagram->length = remaining;
        return copied;
    }
    socket->datagramHead = datagram->next;
    if (!socket->datagramHead) {
        socket->datagramTail = nullptr;
    }
    delete[] datagram->data;
    delete datagram;
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
        // Initiate an active close on a remote TCP connection: queue a FIN
        // (retransmitted by socketTcpTick) and advance the close state.
        if (socket->remoteTcp && !socket->tcpFinSent &&
            (socket->tcpState == TcpState::Established ||
             socket->tcpState == TcpState::CloseWait ||
             socket->tcpState == TcpState::SynReceived)) {
            socket->tcpFinSent = true;
            tcpQueueSegment(socket, kTcpFin | kTcpAck, nullptr, 0);
            if (socket->tcpState == TcpState::CloseWait) {
                socket->tcpState = TcpState::LastAck;
            } else {
                socket->tcpState = TcpState::FinWait1;
            }
        }
    }
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
    if (level != kSolSocket) {
        return syscall_error(SysErrProtoOpt);
    }

    int value = 0;
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
    return copySocketNameToUser(
        socket->localAddress, socket->localAddressLength, addrPtr, addrLenPtr);
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
