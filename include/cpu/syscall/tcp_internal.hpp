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
};

// Implemented in socket.cpp: drive the TCP state machine for one inbound
// segment.  Handles handshakes, data, ACKs, FIN and RST.
void socketProcessTcpSegment(const TcpSegmentInfo& segment);

// Implemented in socket.cpp: periodic timer tick (called from the RX pump) that
// drives retransmission of unacknowledged segments and TIME_WAIT expiry.
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
    size_t payloadLength
);

// Implemented in net.cpp: process all currently-available received frames and
// run the TCP timer tick.  Used by the socket layer to make connect() block
// until the handshake completes.  Returns the number of frames processed.
uint64_t netPumpOnce();
