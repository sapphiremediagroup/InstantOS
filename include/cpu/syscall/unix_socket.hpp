#pragma once

#include <stdint.h>
#include <cpu/process/handles.hpp>

// AF_UNIX sockets (src/cpu/syscall/unix_socket.cpp): stream and seqpacket
// sockets on path or abstract names, socketpair (also SOCK_DGRAM, with
// seqpacket's record boundaries), SCM_RIGHTS (file-like handles passed in
// SocketSendMsg/SocketRecvMsg) and SO_PEERCRED. They are Socket handles like
// the IPv4 ones; what tells them apart is the handle's release function, so
// the socket syscalls in socket.cpp forward to these first.
namespace unixsock {

struct UnixSocket;

constexpr int kAfUnix = 1;

// The handle is an AF_UNIX socket.
bool isUnixEntry(const HandleEntry* entry);
// The current process's handle as an AF_UNIX socket, or nullptr (also when it
// is another kind of handle: the caller then goes on as before).
UnixSocket* fromHandle(uint64_t handle);

uint64_t create(int type, int protocol);                 // socket(AF_UNIX, ...)
uint64_t socketPair(int type, int protocol, uint64_t handlesOut);
uint64_t bind(UnixSocket* socket, uint64_t address, uint64_t length);
uint64_t connect(UnixSocket* socket, uint64_t address, uint64_t length);
uint64_t listen(UnixSocket* socket, uint64_t backlog);
uint64_t accept(UnixSocket* socket, uint64_t address, uint64_t lengthPtr);
uint64_t send(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags);
uint64_t recv(UnixSocket* socket, uint64_t buffer, uint64_t length, uint64_t flags);
uint64_t shutdown(UnixSocket* socket, uint64_t how);
uint64_t getsockopt(UnixSocket* socket, uint64_t level, uint64_t option, uint64_t value, uint64_t lengthPtr);
uint64_t setsockopt(UnixSocket* socket, uint64_t level, uint64_t option, uint64_t value, uint64_t length);
uint64_t getsockname(UnixSocket* socket, uint64_t address, uint64_t lengthPtr);
uint64_t getpeername(UnixSocket* socket, uint64_t address, uint64_t lengthPtr);
uint64_t sendMsg(UnixSocket* socket, uint64_t messagePtr, uint64_t flags);
uint64_t recvMsg(UnixSocket* socket, uint64_t messagePtr, uint64_t flags);

int16_t poll(void* object, int16_t events);
bool isNonBlocking(void* object);
void setNonBlocking(void* object, bool nonBlocking);

} // namespace unixsock
