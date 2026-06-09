#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <common/string.hpp>

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
    uint32_t tcpSendSeq;
    uint32_t tcpRecvNext;
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
        revents |= kPollOut;
    }
    if ((events & kPollIn) && (rights & HandleRightRead)) {
        const bool streamPeerGone = socket->type == kSockStream && socket->connected && !socket->peer;
        if (socket->acceptHead || socket->datagramHead || socket->readClosed || streamPeerGone || (socket->peer && socket->peer->writeClosed)) {
            revents |= kPollIn;
        }
        if (socket->readClosed || streamPeerGone || (socket->peer && socket->peer->writeClosed)) {
            revents |= kPollHup;
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

    const int socketType = static_cast<int>(type);
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
        if (!ensureSocketBound(socket)) {
            return syscall_error(SysErrAddressInUse);
        }
        SocketObject* listener = socketAddressIsLocalDestination(copiedAddress, copiedLength)
            ? findListeningStreamPeer(socket, copiedAddress, copiedLength)
            : nullptr;
        if (!listener) {
            const uint64_t result = netStartTcpConnect(
                socketAddressPort(socket->localAddress, socket->localAddressLength),
                copiedAddress,
                copiedLength
            );
            if (result == 0 || result == syscall_error(SysErrAgain)) {
                socket->remoteTcp = true;
                socket->tcpSendSeq = 1;
            }
            return result;
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
            const uint64_t sent = netSendTcpPayload(
                socketAddressPort(socket->localAddress, socket->localAddressLength),
                socket->peerAddress,
                socket->peerAddressLength,
                socket->tcpSendSeq,
                socket->tcpRecvNext,
                buffer,
                length
            );
            if (sent == length) {
                socket->tcpSendSeq += static_cast<uint32_t>(length);
            }
            return sent;
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
        return true;
    }
    return false;
}

bool socketAcceptTcpConnection(
    uint16_t srcPort,
    uint32_t srcIpNetworkOrder,
    uint16_t dstPort,
    uint32_t remoteSeq,
    uint32_t* outSeq,
    uint32_t* outAck
) {
    for (SocketObject* listener = gSockets; listener; listener = listener->next) {
        if (listener->type != kSockStream || !listener->listening || !listener->bound ||
            socketAddressPort(listener->localAddress, listener->localAddressLength) != dstPort ||
            acceptQueueLength(listener) >= static_cast<uint64_t>(listener->listenBacklog)) {
            continue;
        }

        SocketObject* accepted = createSocketObject(listener->domain, listener->type, listener->protocol);
        if (!accepted) {
            return false;
        }

        accepted->bound = true;
        accepted->connected = true;
        accepted->remoteTcp = true;
        accepted->tcpSendSeq = 2;
        accepted->tcpRecvNext = remoteSeq + 1;
        memcpy(accepted->localAddress, listener->localAddress, listener->localAddressLength);
        accepted->localAddressLength = listener->localAddressLength;
        uint16_t family = kAfInet;
        memcpy(accepted->peerAddress, &family, sizeof(family));
        memcpy(accepted->peerAddress + 2, &srcPort, sizeof(srcPort));
        memcpy(accepted->peerAddress + 4, &srcIpNetworkOrder, sizeof(srcIpNetworkOrder));
        accepted->peerAddressLength = 16;

        if (listener->acceptTail) {
            listener->acceptTail->acceptNext = accepted;
        } else {
            listener->acceptHead = accepted;
        }
        listener->acceptTail = accepted;
        if (outSeq) {
            *outSeq = 1;
        }
        if (outAck) {
            *outAck = accepted->tcpRecvNext;
        }
        return true;
    }
    return false;
}

bool socketCompleteTcpConnect(uint16_t srcPort, uint32_t srcIpNetworkOrder, uint16_t dstPort, uint32_t remoteSeq) {
    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockStream || !socket->remoteTcp || socket->connected || !socket->bound ||
            socketAddressPort(socket->localAddress, socket->localAddressLength) != dstPort ||
            socketAddressPort(socket->peerAddress, socket->peerAddressLength) != srcPort) {
            continue;
        }
        uint32_t peerIp = 0;
        if (socket->peerAddressLength >= 8) {
            memcpy(&peerIp, socket->peerAddress + 4, sizeof(peerIp));
        }
        if (peerIp != srcIpNetworkOrder) {
            continue;
        }
        socket->connected = true;
        socket->tcpSendSeq = 2;
        socket->tcpRecvNext = remoteSeq + 1;
        return true;
    }
    return false;
}

bool socketDeliverTcpPayload(
    uint16_t srcPort,
    uint32_t srcIpNetworkOrder,
    uint16_t dstPort,
    uint32_t seq,
    const uint8_t* payload,
    uint64_t length,
    uint32_t* outSeq,
    uint32_t* outAck
) {
    for (SocketObject* socket = gSockets; socket; socket = socket->next) {
        if (socket->type != kSockStream || !socket->remoteTcp || !socket->connected || !socket->bound || socket->readClosed ||
            socketAddressPort(socket->localAddress, socket->localAddressLength) != dstPort ||
            socketAddressPort(socket->peerAddress, socket->peerAddressLength) != srcPort) {
            continue;
        }
        uint32_t peerIp = 0;
        if (socket->peerAddressLength >= 8) {
            memcpy(&peerIp, socket->peerAddress + 4, sizeof(peerIp));
        }
        if (peerIp != srcIpNetworkOrder) {
            continue;
        }

        if (seq != socket->tcpRecvNext) {
            if (outSeq) {
                *outSeq = socket->tcpSendSeq;
            }
            if (outAck) {
                *outAck = socket->tcpRecvNext;
            }
            return true;
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
        if (socket->datagramTail) {
            socket->datagramTail->next = datagram;
        } else {
            socket->datagramHead = datagram;
        }
        socket->datagramTail = datagram;
        socket->tcpRecvNext += static_cast<uint32_t>(length);
        if (outSeq) {
            *outSeq = socket->tcpSendSeq;
        }
        if (outAck) {
            *outAck = socket->tcpRecvNext;
        }
        return true;
    }
    return false;
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
    if (socket->readClosed) {
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
        if (socket->type == kSockStream && (!socket->peer || socket->peer->writeClosed)) {
            return 0;
        }
        return syscall_error(SysErrAgain);
    }
    const uint64_t copied = datagram->length < length ? datagram->length : length;
    if (copied != 0 && !copyToUser(buffer, datagram->data, static_cast<size_t>(copied))) {
        return syscall_error(SysErrInvalid);
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
