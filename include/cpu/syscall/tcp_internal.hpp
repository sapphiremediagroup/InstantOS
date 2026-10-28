#pragma once

#include <stdint.h>
#include <stddef.h>

// Internal interface between the TCP wire layer (src/cpu/syscall/net.cpp) and
// the socket/TCP state machine (src/cpu/syscall/socket.cpp).  Not a public
// syscall surface.

// TCP control flag bits (host order, as they appear in TcpHeader::flags).
constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpPsh = 0x08;
constexpr uint8_t kTcpAck = 0x10;

// The options this stack sends and understands, from SYN segments only.
struct TcpOptions {
    uint16_t mss;          // maximum segment size, 0 = not present
    bool hasWindowScale;   // window scale offered (RFC 7323) ...
    uint8_t windowScale;   // ... with this shift count
};

// A fully-parsed inbound TCP segment handed from net.cpp to the socket layer.
// All multi-byte fields are in host byte order except the addresses, which are
// kept in network byte order to match how SocketObject stores them.
struct TcpSegmentInfo {
    uint16_t srcPort;            // network order (matches sockaddr layout)
    uint16_t dstPort;            // network order
    uint32_t srcIpNetworkOrder;  // network order
    uint32_t seq;                // host order
    uint32_t ack;                // host order
    uint16_t window;             // host order (peer's advertised receive window)
    uint8_t flags;               // host order TCP flags
    const uint8_t* payload;
    uint64_t payloadLength;
    TcpOptions options;          // as parsed from the header
};

// Implemented in socket.cpp: drive the TCP state machine for one inbound
// segment.  Handles handshakes, data, ACKs, FIN and RST.
void socketProcessTcpSegment(const TcpSegmentInfo& segment);

// Implemented in socket.cpp: queue an inbound UDP datagram on the socket bound
// to dstPort (network order). False when no socket has that port, which the
// caller answers with an ICMP port unreachable.
bool socketDeliverUdpDatagram(uint16_t srcPort, uint32_t srcIpNetworkOrder, uint16_t dstPort,
                              const uint8_t* payload, uint64_t length);

// Implemented in socket.cpp: an ICMP echo reply for the ping socket whose
// identifier (its local port) is `id` (network order). `message` is the ICMP
// header and data.
bool socketDeliverIcmpEcho(uint32_t srcIpNetworkOrder, uint16_t id, const uint8_t* message, uint64_t length);

// An ICMP destination unreachable or time exceeded about a datagram we sent,
// identified by the header it quotes.
struct IcmpErrorInfo {
    enum Protocol : uint8_t { Tcp, Udp };
    Protocol protocol;
    uint16_t localPort;            // network order
    uint16_t remotePort;           // network order
    uint32_t remoteIpNetworkOrder;
    uint32_t tcpSeq;               // host order, the quoted sequence number
    int error;                     // errno to report
    bool hard;                     // port/protocol unreachable and the like
};

// Implemented in socket.cpp: a connecting TCP socket fails with info.error, an
// established one keeps it as the reason should it time out later; a connected
// UDP socket reports a hard error on its next call (Linux semantics).
void socketProcessIcmpError(const IcmpErrorInfo& info);

// Implemented in net.cpp: the interface address in network order, 0 while the
// interface is unconfigured (no DHCP lease yet).
uint32_t netLocalAddressNetworkOrder();

// Implemented in socket.cpp: periodic timer tick (every 10ms from the kernel's
// network pump, netTimerTick) that drives retransmission of unacknowledged
// segments and TIME_WAIT expiry.
void socketTcpTick();

// Implemented in net.cpp: transmit a single TCP segment to a remote peer,
// resolving ARP as needed.  `peerAddress` is a sockaddr_in-style buffer (family,
// port, ipv4 all in network order).  Returns true if the frame was sent.
bool netTransmitTcpSegment(
    uint16_t srcPort,
    const uint8_t* peerAddress,
    uint64_t peerAddressLength,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    uint16_t window,
    const uint8_t* payload,
    size_t payloadLength,
    const TcpOptions* options = nullptr   // MSS/window scale, for SYNs
);

// Implemented in net.cpp: process all currently-available received frames and
// run the ARP and TCP timers once (the NetProcessPackets syscall).  Returns the
// number of frames processed.
uint64_t netPumpOnce();
