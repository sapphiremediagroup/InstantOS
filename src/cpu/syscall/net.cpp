#include <cpu/syscall/syscall.hpp>
#include <drivers/net/net_device.hpp>
#include <cpu/syscall/tcp_internal.hpp>
#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>

bool socketDeliverUdpDatagram(uint16_t srcPort, uint32_t srcIpNetworkOrder, uint16_t dstPort, const uint8_t* payload, uint64_t length);

namespace {
constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint8_t kIpProtocolIcmp = 1;
constexpr uint8_t kIpProtocolUdp = 17;
constexpr uint8_t kIpProtocolTcp = 6;
constexpr uint16_t kArpHardwareEthernet = 1;
constexpr uint16_t kArpOperationRequest = 1;
constexpr uint16_t kArpOperationReply = 2;
constexpr uint8_t kIcmpEchoReply = 0;
constexpr uint8_t kIcmpEchoRequest = 8;
constexpr uint8_t kTcpFlagFin = 0x01;
constexpr uint8_t kTcpFlagSyn = 0x02;
constexpr uint8_t kTcpFlagPsh = 0x08;
constexpr uint8_t kTcpFlagAck = 0x10;
constexpr uint32_t kLocalIpv4 = (10U << 24) | (0U << 16) | (2U << 8) | 15U;
// QEMU user-mode networking: guest 10.0.2.15/24, gateway 10.0.2.2.
constexpr uint32_t kGatewayIpv4 = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
constexpr uint32_t kSubnetMask = 0xFFFFFF00U;  // /24
constexpr uint8_t kBroadcastMac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// Resolve the next-hop IP for a destination: the destination itself if it is on
// our local subnet, otherwise the default gateway.  Without this, off-subnet
// destinations would ARP for an address no one on the link answers, so the
// frame is never sent (TCP to any public host, off-link UDP, etc.).
inline uint32_t nextHopIp(uint32_t destIp) {
    if ((destIp & kSubnetMask) == (kLocalIpv4 & kSubnetMask)) {
        return destIp;
    }
    return kGatewayIpv4;
}

struct EthernetHeader {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t etherType;
} __attribute__((packed));

struct ArpPacket {
    uint16_t hardwareType;
    uint16_t protocolType;
    uint8_t hardwareLength;
    uint8_t protocolLength;
    uint16_t operation;
    uint8_t senderMac[6];
    uint32_t senderIp;
    uint8_t targetMac[6];
    uint32_t targetIp;
} __attribute__((packed));

struct Ipv4Header {
    uint8_t versionIhl;
    uint8_t dscpEcn;
    uint16_t totalLength;
    uint16_t identification;
    uint16_t flagsFragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t headerChecksum;
    uint32_t srcIp;
    uint32_t dstIp;
} __attribute__((packed));

struct IcmpEchoHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct UdpHeader {
    uint16_t srcPort;
    uint16_t dstPort;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct TcpHeader {
    uint16_t srcPort;
    uint16_t dstPort;
    uint32_t seq;
    uint32_t ack;
    uint8_t dataOffsetReserved;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct TcpPseudoHeader {
    uint32_t srcIp;
    uint32_t dstIp;
    uint8_t zero;
    uint8_t protocol;
    uint16_t length;
} __attribute__((packed));

struct NetPingReply {
    uint32_t srcIp;
    uint16_t id;
    uint16_t seq;
    uint16_t payloadSize;
    uint16_t reserved;
} __attribute__((packed));

struct PendingPing {
    bool active;
    uint32_t destIp;
    uint16_t id;
    uint16_t seq;
};

struct ArpCacheEntry {
    bool valid;
    uint32_t ip;
    uint8_t mac[6];
};

PendingPing gPendingPing {};
ArpCacheEntry gArpCache[8] {};
NetPingReply gLastPingReply {};
bool gHasPingReply = false;
uint16_t gIpv4Identification = 1;

uint16_t bswap16(uint16_t value) {
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

uint32_t bswap32(uint32_t value) {
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

uint16_t toNetwork16(uint16_t value) {
    return bswap16(value);
}

uint32_t toNetwork32(uint32_t value) {
    return bswap32(value);
}

uint16_t fromNetwork16(uint16_t value) {
    return bswap16(value);
}

uint32_t fromNetwork32(uint32_t value) {
    return bswap32(value);
}

uint16_t internetChecksum(const void* data, size_t length) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    uint32_t sum = 0;
    while (length > 1) {
        sum += static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        length -= 2;
    }
    if (length != 0) {
        sum += static_cast<uint16_t>(bytes[0] << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

bool sameMac(const uint8_t* a, const uint8_t* b) {
    for (size_t i = 0; i < 6; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

void rememberArp(uint32_t ip, const uint8_t* mac) {
    for (auto& entry : gArpCache) {
        if (entry.valid && entry.ip == ip) {
            memcpy(entry.mac, mac, 6);
            return;
        }
    }
    for (auto& entry : gArpCache) {
        if (!entry.valid) {
            entry.valid = true;
            entry.ip = ip;
            memcpy(entry.mac, mac, 6);
            return;
        }
    }
    gArpCache[0].valid = true;
    gArpCache[0].ip = ip;
    memcpy(gArpCache[0].mac, mac, 6);
}

bool lookupArp(uint32_t ip, uint8_t* mac) {
    for (const auto& entry : gArpCache) {
        if (entry.valid && entry.ip == ip) {
            memcpy(mac, entry.mac, 6);
            return true;
        }
    }
    return false;
}

bool sendArpRequest(NetDevice& net, uint32_t targetIp) {
    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[sizeof(EthernetHeader) + sizeof(ArpPacket)] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthernetHeader));
    memcpy(eth->dst, kBroadcastMac, 6);
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeArp);
    arp->hardwareType = toNetwork16(kArpHardwareEthernet);
    arp->protocolType = toNetwork16(kEtherTypeIpv4);
    arp->hardwareLength = 6;
    arp->protocolLength = 4;
    arp->operation = toNetwork16(kArpOperationRequest);
    memcpy(arp->senderMac, localMac, 6);
    arp->senderIp = toNetwork32(kLocalIpv4);
    arp->targetIp = toNetwork32(targetIp);
    return net.sendPacket(frame, sizeof(frame));
}

bool sendArpReply(NetDevice& net, const ArpPacket& request) {
    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[sizeof(EthernetHeader) + sizeof(ArpPacket)] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* arp = reinterpret_cast<ArpPacket*>(frame + sizeof(EthernetHeader));
    memcpy(eth->dst, request.senderMac, 6);
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeArp);
    arp->hardwareType = toNetwork16(kArpHardwareEthernet);
    arp->protocolType = toNetwork16(kEtherTypeIpv4);
    arp->hardwareLength = 6;
    arp->protocolLength = 4;
    arp->operation = toNetwork16(kArpOperationReply);
    memcpy(arp->senderMac, localMac, 6);
    arp->senderIp = toNetwork32(kLocalIpv4);
    memcpy(arp->targetMac, request.senderMac, 6);
    arp->targetIp = request.senderIp;
    return net.sendPacket(frame, sizeof(frame));
}

bool sendIcmpEcho(NetDevice& net, uint32_t destIp, const uint8_t* destMac, uint16_t id, uint16_t seq) {
    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    constexpr size_t payloadSize = 16;
    uint8_t frame[sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payloadSize] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* icmp = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    auto* payload = reinterpret_cast<uint8_t*>(icmp + 1);

    memcpy(eth->dst, destMac, 6);
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payloadSize);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolIcmp;
    ip->srcIp = toNetwork32(kLocalIpv4);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    icmp->type = kIcmpEchoRequest;
    icmp->id = toNetwork16(id);
    icmp->seq = toNetwork16(seq);
    for (size_t i = 0; i < payloadSize; ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    icmp->checksum = toNetwork16(internetChecksum(icmp, sizeof(IcmpEchoHeader) + payloadSize));
    return net.sendPacket(frame, sizeof(frame));
}

bool sendUdpFrame(
    NetDevice& net,
    uint32_t destIp,
    const uint8_t* destMac,
    uint16_t srcPort,
    uint16_t dstPort,
    const uint8_t* payload,
    size_t payloadLength
) {
    if ((!payload && payloadLength != 0) || payloadLength > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - sizeof(UdpHeader)) {
        return false;
    }

    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[NET_DEVICE_MTU] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* udp = reinterpret_cast<UdpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));

    memcpy(eth->dst, destMac, 6);
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    const size_t udpLength = sizeof(UdpHeader) + payloadLength;
    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + udpLength);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolUdp;
    ip->srcIp = toNetwork32(kLocalIpv4);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    udp->srcPort = srcPort;
    udp->dstPort = dstPort;
    udp->length = toNetwork16(static_cast<uint16_t>(udpLength));
    udp->checksum = 0;
    if (payloadLength != 0) {
        memcpy(reinterpret_cast<uint8_t*>(udp + 1), payload, payloadLength);
    }

    return net.sendPacket(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + udpLength);
}

uint16_t tcpChecksum(uint32_t srcIpNetworkOrder, uint32_t dstIpNetworkOrder, const TcpHeader* tcp, size_t tcpLength) {
    uint8_t buffer[sizeof(TcpPseudoHeader) + NET_DEVICE_MTU] {};
    auto* pseudo = reinterpret_cast<TcpPseudoHeader*>(buffer);
    pseudo->srcIp = srcIpNetworkOrder;
    pseudo->dstIp = dstIpNetworkOrder;
    pseudo->protocol = kIpProtocolTcp;
    pseudo->length = toNetwork16(static_cast<uint16_t>(tcpLength));
    memcpy(buffer + sizeof(TcpPseudoHeader), tcp, tcpLength);
    return internetChecksum(buffer, sizeof(TcpPseudoHeader) + tcpLength);
}

bool sendTcpFrame(
    NetDevice& net,
    uint32_t destIp,
    const uint8_t* destMac,
    uint16_t srcPort,
    uint16_t dstPort,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    uint16_t window,
    const uint8_t* payload,
    size_t payloadLength
) {
    if ((!payload && payloadLength != 0) || payloadLength > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - sizeof(TcpHeader)) {
        return false;
    }

    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[NET_DEVICE_MTU] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* tcp = reinterpret_cast<TcpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));

    memcpy(eth->dst, destMac, 6);
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    const size_t tcpLength = sizeof(TcpHeader) + payloadLength;
    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + tcpLength);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolTcp;
    ip->srcIp = toNetwork32(kLocalIpv4);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    tcp->srcPort = srcPort;
    tcp->dstPort = dstPort;
    tcp->seq = toNetwork32(seq);
    tcp->ack = toNetwork32(ack);
    tcp->dataOffsetReserved = static_cast<uint8_t>((sizeof(TcpHeader) / 4) << 4);
    tcp->flags = flags;
    tcp->window = toNetwork16(window);
    if (payloadLength != 0) {
        memcpy(reinterpret_cast<uint8_t*>(tcp + 1), payload, payloadLength);
    }
    tcp->checksum = toNetwork16(tcpChecksum(ip->srcIp, ip->dstIp, tcp, tcpLength));
    return net.sendPacket(frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + tcpLength);
}

void handleArp(NetDevice& net, const uint8_t* packet, size_t length) {
    if (length < sizeof(EthernetHeader) + sizeof(ArpPacket)) {
        return;
    }
    const auto* arp = reinterpret_cast<const ArpPacket*>(packet + sizeof(EthernetHeader));
    if (fromNetwork16(arp->hardwareType) != kArpHardwareEthernet ||
        fromNetwork16(arp->protocolType) != kEtherTypeIpv4 ||
        arp->hardwareLength != 6 || arp->protocolLength != 4) {
        return;
    }

    const uint32_t senderIp = fromNetwork32(arp->senderIp);
    const uint32_t targetIp = fromNetwork32(arp->targetIp);
    rememberArp(senderIp, arp->senderMac);

    const uint16_t operation = fromNetwork16(arp->operation);
    if (operation == kArpOperationRequest && targetIp == kLocalIpv4) {
        sendArpReply(net, *arp);
    } else if (operation == kArpOperationReply && gPendingPing.active && senderIp == gPendingPing.destIp) {
        sendIcmpEcho(net, gPendingPing.destIp, arp->senderMac, gPendingPing.id, gPendingPing.seq);
        gPendingPing.active = false;
    }
}

void handleIpv4(NetDevice& net, const uint8_t* packet, size_t length) {
    if (length < sizeof(EthernetHeader) + sizeof(Ipv4Header)) {
        return;
    }
    const auto* eth = reinterpret_cast<const EthernetHeader*>(packet);
    const auto* ip = reinterpret_cast<const Ipv4Header*>(packet + sizeof(EthernetHeader));
    const size_t ihl = (ip->versionIhl & 0x0f) * 4;
    if ((ip->versionIhl >> 4) != 4 || ihl < sizeof(Ipv4Header) ||
        length < sizeof(EthernetHeader) + ihl ||
        fromNetwork32(ip->dstIp) != kLocalIpv4) {
        return;
    }
    rememberArp(fromNetwork32(ip->srcIp), eth->src);

    const size_t totalLength = fromNetwork16(ip->totalLength);
    if (totalLength < ihl || length < sizeof(EthernetHeader) + totalLength) {
        return;
    }

    if (ip->protocol == kIpProtocolUdp) {
        if (totalLength < ihl + sizeof(UdpHeader)) {
            return;
        }
        const auto* udp = reinterpret_cast<const UdpHeader*>(packet + sizeof(EthernetHeader) + ihl);
        const size_t udpLength = fromNetwork16(udp->length);
        if (udpLength < sizeof(UdpHeader) || ihl + udpLength > totalLength) {
            return;
        }
        socketDeliverUdpDatagram(
            udp->srcPort,
            ip->srcIp,
            udp->dstPort,
            reinterpret_cast<const uint8_t*>(udp + 1),
            udpLength - sizeof(UdpHeader)
        );
        return;
    }

    if (ip->protocol == kIpProtocolTcp) {
        if (totalLength < ihl + sizeof(TcpHeader)) {
            return;
        }
        const auto* tcp = reinterpret_cast<const TcpHeader*>(packet + sizeof(EthernetHeader) + ihl);
        const size_t tcpHeaderLength = ((tcp->dataOffsetReserved >> 4) & 0x0f) * 4;
        if (tcpHeaderLength < sizeof(TcpHeader) || totalLength < ihl + tcpHeaderLength) {
            return;
        }

        TcpSegmentInfo segment {};
        segment.srcPort = tcp->srcPort;
        segment.dstPort = tcp->dstPort;
        segment.srcIpNetworkOrder = ip->srcIp;
        segment.seq = fromNetwork32(tcp->seq);
        segment.ack = fromNetwork32(tcp->ack);
        segment.window = fromNetwork16(tcp->window);
        segment.flags = tcp->flags;
        segment.payload = packet + sizeof(EthernetHeader) + ihl + tcpHeaderLength;
        segment.payloadLength = totalLength - ihl - tcpHeaderLength;
        socketProcessTcpSegment(segment);
        return;
    }

    if (ip->protocol != kIpProtocolIcmp || totalLength < ihl + sizeof(IcmpEchoHeader)) {
        return;
    }
    const auto* icmp = reinterpret_cast<const IcmpEchoHeader*>(packet + sizeof(EthernetHeader) + ihl);
    if (icmp->type != kIcmpEchoReply || icmp->code != 0) {
        return;
    }

    gLastPingReply.srcIp = fromNetwork32(ip->srcIp);
    gLastPingReply.id = fromNetwork16(icmp->id);
    gLastPingReply.seq = fromNetwork16(icmp->seq);
    gLastPingReply.payloadSize = static_cast<uint16_t>(totalLength - ihl - sizeof(IcmpEchoHeader));
    gLastPingReply.reserved = 0;
    gHasPingReply = true;
}

// Drain and dispatch all currently-available received frames.  Returns the
// number of frames processed.  Shared by the NetProcessPackets syscall and the
// internal synchronous ARP-resolution wait below.
uint64_t pumpPackets(NetDevice& net) {
    uint64_t processed = 0;
    for (;;) {
        uint8_t packet[NET_DEVICE_MTU];
        const int length = net.receivePacket(packet, sizeof(packet));
        if (length <= 0) {
            break;
        }
        processed++;
        if (static_cast<size_t>(length) < sizeof(EthernetHeader)) {
            continue;
        }
        const auto* eth = reinterpret_cast<const EthernetHeader*>(packet);
        const uint16_t etherType = fromNetwork16(eth->etherType);
        if (etherType == kEtherTypeArp) {
            handleArp(net, packet, static_cast<size_t>(length));
        } else if (etherType == kEtherTypeIpv4) {
            handleIpv4(net, packet, static_cast<size_t>(length));
        }
    }
    return processed;
}

// Resolve the destination MAC for `dstIp`, sending an ARP request and pumping
// the receive path until the reply arrives.  Returns true if `dstMac` was
// filled.  This makes outbound sends synchronous with respect to ARP so a
// single send() does not spuriously fail with EAGAIN on a cold ARP cache
// (which the mlibc resolver and most blocking callers do not retry).
bool resolveArpBlocking(NetDevice& net, uint32_t dstIp, uint8_t* dstMac) {
    if (lookupArp(dstIp, dstMac)) {
        return true;
    }
    constexpr int kArpAttempts = 50;  // ~500ms worst case at 10ms/attempt
    for (int attempt = 0; attempt < kArpAttempts; ++attempt) {
        sendArpRequest(net, dstIp);
        // Spin briefly pumping RX so the ARP reply gets processed.
        for (int spin = 0; spin < 20000; ++spin) {
            pumpPackets(net);
            if (lookupArp(dstIp, dstMac)) {
                return true;
            }
            asm volatile("pause");
        }
    }
    return lookupArp(dstIp, dstMac);
}
}

uint64_t netSendUdpDatagram(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength, uint64_t buffer, uint64_t length) {
    if (!dstAddress || dstAddressLength < 8 || length > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - sizeof(UdpHeader)) {
        return syscall_error(SysErrInvalid);
    }

    uint32_t dstIpNetworkOrder = 0;
    memcpy(&dstIpNetworkOrder, dstAddress + 4, sizeof(dstIpNetworkOrder));
    const uint32_t dstIp = fromNetwork32(dstIpNetworkOrder);
    uint16_t dstPort = 0;
    memcpy(&dstPort, dstAddress + 2, sizeof(dstPort));
    if (dstIp == 0 || srcPort == 0 || dstPort == 0) {
        return syscall_error(SysErrInvalid);
    }

    uint8_t payload[NET_DEVICE_MTU] {};
    if (length != 0 && !Syscall::copyFromUser(payload, buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    NetDevice& net = *netDev;

    uint8_t dstMac[6] {};
    if (!resolveArpBlocking(net, nextHopIp(dstIp), dstMac)) {
        return syscall_error(SysErrAgain);
    }

    return sendUdpFrame(net, dstIp, dstMac, srcPort, dstPort, payload, static_cast<size_t>(length))
        ? length
        : syscall_error(SysErrAgain);
}

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
) {
    if (!peerAddress || peerAddressLength < 8 || srcPort == 0 ||
        payloadLength > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - sizeof(TcpHeader)) {
        return false;
    }

    uint32_t dstIpNetworkOrder = 0;
    uint16_t dstPort = 0;
    memcpy(&dstPort, peerAddress + 2, sizeof(dstPort));
    memcpy(&dstIpNetworkOrder, peerAddress + 4, sizeof(dstIpNetworkOrder));
    const uint32_t dstIp = fromNetwork32(dstIpNetworkOrder);
    if (dstIp == 0 || dstPort == 0) {
        return false;
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return false;
    }
    NetDevice& net = *netDev;

    // Resolve ARP for the next hop (gateway for off-subnet destinations)
    // synchronously so a cold cache does not silently drop the segment.
    uint8_t dstMac[6] {};
    if (!resolveArpBlocking(net, nextHopIp(dstIp), dstMac)) {
        return false;
    }

    return sendTcpFrame(net, dstIp, dstMac, srcPort, dstPort, seq, ack, flags, window,
                        payload, payloadLength);
}

uint64_t Syscall::sys_net_get_mac(uint64_t macPtr) {
    uint8_t mac[6] {};
    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    NetDevice& net = *netDev;
    net.getMacAddress(mac);
    return copyToUser(macPtr, mac, sizeof(mac)) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_net_send(uint64_t data, uint64_t len) {
    if (len == 0 || len > NET_DEVICE_MTU) {
        return syscall_error(SysErrInvalid);
    }
    uint8_t packet[NET_DEVICE_MTU];
    if (!copyFromUser(packet, data, static_cast<size_t>(len))) {
        return syscall_error(SysErrInvalid);
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    NetDevice& net = *netDev;
    return net.sendPacket(packet, static_cast<size_t>(len)) ? len : syscall_error(SysErrAgain);
}

uint64_t Syscall::sys_net_recv(uint64_t buffer, uint64_t maxlen) {
    if (maxlen == 0) {
        return syscall_error(SysErrInvalid);
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    NetDevice& net = *netDev;

    uint8_t packet[NET_DEVICE_MTU];
    const size_t capacity = maxlen < NET_DEVICE_MTU ? static_cast<size_t>(maxlen) : NET_DEVICE_MTU;
    const int received = net.receivePacket(packet, capacity);
    if (received < 0) {
        return syscall_error(SysErrAgain);
    }
    return copyToUser(buffer, packet, static_cast<size_t>(received))
        ? static_cast<uint64_t>(received)
        : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_net_link_status() {
    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return 0;
    }
    NetDevice& net = *netDev;
    return net.isLinkUp() ? 1 : 0;
}

uint64_t Syscall::sys_net_ping(uint64_t destIp, uint64_t id, uint64_t seq) {
    const uint32_t targetIp = static_cast<uint32_t>(destIp);
    if (targetIp == 0) {
        return syscall_error(SysErrInvalid);
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    NetDevice& net = *netDev;

    uint8_t targetMac[6] {};
    if (lookupArp(targetIp, targetMac)) {
        return sendIcmpEcho(net, targetIp, targetMac, static_cast<uint16_t>(id), static_cast<uint16_t>(seq))
            ? 0
            : syscall_error(SysErrAgain);
    }

    gPendingPing.active = true;
    gPendingPing.destIp = targetIp;
    gPendingPing.id = static_cast<uint16_t>(id);
    gPendingPing.seq = static_cast<uint16_t>(seq);
    return sendArpRequest(net, targetIp) ? syscall_error(SysErrAgain) : syscall_error(SysErrNoEntry);
}

uint64_t Syscall::sys_net_process_packets() {
    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    const uint64_t processed = pumpPackets(*netDev);
    // Drive TCP retransmission/timers once per pump (top-level only, so we do
    // not recurse through resolveArpBlocking -> pumpPackets).
    socketTcpTick();
    return processed;
}

uint64_t netPumpOnce() {
    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return 0;
    }
    const uint64_t processed = pumpPackets(*netDev);
    socketTcpTick();
    return processed;
}

uint64_t Syscall::sys_net_get_ping_reply(uint64_t replyPtr) {
    if (!gHasPingReply) {
        return syscall_error(SysErrAgain);
    }
    if (!copyToUser(replyPtr, &gLastPingReply, sizeof(gLastPingReply))) {
        return syscall_error(SysErrInvalid);
    }
    gHasPingReply = false;
    return 0;
}
