#include <cpu/syscall/syscall.hpp>
#include <drivers/net/net_device.hpp>
#include <cpu/syscall/tcp_internal.hpp>
#include <common/string.hpp>
#include <cpu/cereal/cereal.hpp>
#include <time/tsc_timer.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/idt/interrupt.hpp>
#include <common/krandom.hpp>


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
constexpr uint8_t kIcmpDestUnreachable = 3;
constexpr uint8_t kIcmpTimeExceeded = 11;
constexpr uint8_t kIcmpCodePortUnreachable = 3;
constexpr uint32_t kBroadcastIpv4 = 0xFFFFFFFFU;
constexpr uint8_t kBroadcastMac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// The interface address, all host order. Zero until DHCP (or NetSetConfig)
// configures it; until then only DHCP traffic goes out.
struct InterfaceConfig {
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns[NET_CONFIG_MAX_DNS];
    uint32_t dnsCount;
    uint32_t ntp;
    char domain[NET_CONFIG_DOMAIN_SIZE];
    bool fromDhcp;
};

InterfaceConfig gIface {};
uint64_t gIfaceGeneration = 1;

// Resolve the next-hop IP for a destination: the destination itself if it is on
// our local subnet, otherwise the default gateway.  Without this, off-subnet
// destinations would ARP for an address no one on the link answers, so the
// frame is never sent (TCP to any public host, off-link UDP, etc.).
inline uint32_t nextHopIp(uint32_t destIp) {
    if (gIface.gateway == 0 || (destIp & gIface.netmask) == (gIface.address & gIface.netmask)) {
        return destIp;
    }
    return gIface.gateway;
}

bool isBroadcastIpv4(uint32_t ip) {
    return ip == kBroadcastIpv4 ||
           (gIface.address != 0 && gIface.netmask != 0xFFFFFFFFU && ip == (gIface.address | ~gIface.netmask));
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

struct ArpCacheEntry {
    bool valid;
    uint32_t ip;
    uint8_t mac[6];
};

// A frame waiting for the next hop's MAC. Sends never wait for ARP: the frame
// is parked here, the request goes out, and the reply releases it.
struct ArpPendingFrame {
    bool used;
    uint32_t nextHop;
    uint64_t queuedMs;
    uint16_t length;
    uint8_t data[NET_DEVICE_MTU];
};

// An address being resolved: when to repeat the request, when to give up.
struct ArpResolution {
    bool active;
    uint32_t ip;
    uint64_t startedMs;
    uint64_t lastRequestMs;
};

constexpr size_t kArpCacheSize = 16;
constexpr size_t kArpPendingFrames = 32;
constexpr size_t kArpResolutions = 8;
constexpr uint64_t kArpRetryMs = 250;
constexpr uint64_t kArpGiveUpMs = 3000;

ArpCacheEntry gArpCache[kArpCacheSize] {};
ArpPendingFrame* gArpPending = nullptr;  // kArpPendingFrames entries
ArpResolution gArpResolutions[kArpResolutions] {};
uint32_t gArpEvictNext = 0;

constexpr uint64_t kNetTimerPeriodMs = 10;  // ARP retries and TCP timers
NetDevice* gNetTickDevice = nullptr;
uint64_t gNetLastTimersMs = 0;
bool gLinkUp = false;   // as last seen by linkTick
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
    ArpCacheEntry& victim = gArpCache[gArpEvictNext++ % kArpCacheSize];
    victim.valid = true;
    victim.ip = ip;
    memcpy(victim.mac, mac, 6);
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
    arp->senderIp = toNetwork32(gIface.address);
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
    arp->senderIp = toNetwork32(gIface.address);
    memcpy(arp->targetMac, request.senderMac, 6);
    arp->targetIp = request.senderIp;
    return net.sendPacket(frame, sizeof(frame));
}

ArpResolution* findResolution(uint32_t ip) {
    for (auto& r : gArpResolutions) {
        if (r.active && r.ip == ip) {
            return &r;
        }
    }
    return nullptr;
}

void dropPendingFor(uint32_t ip) {
    if (!gArpPending) {
        return;
    }
    for (size_t i = 0; i < kArpPendingFrames; ++i) {
        if (gArpPending[i].used && gArpPending[i].nextHop == ip) {
            gArpPending[i].used = false;
        }
    }
}

// Record a mapping and release every frame that was waiting for it.
void learnArp(NetDevice& net, uint32_t ip, const uint8_t* mac) {
    rememberArp(ip, mac);
    if (ArpResolution* r = findResolution(ip)) {
        r->active = false;
    }
    if (!gArpPending) {
        return;
    }
    // Oldest first, so a burst leaves in the order it was sent.
    for (;;) {
        ArpPendingFrame* oldest = nullptr;
        for (size_t i = 0; i < kArpPendingFrames; ++i) {
            ArpPendingFrame& f = gArpPending[i];
            if (f.used && f.nextHop == ip && (!oldest || f.queuedMs < oldest->queuedMs)) {
                oldest = &f;
            }
        }
        if (!oldest) {
            return;
        }
        memcpy(oldest->data, mac, 6);  // EthernetHeader::dst
        net.sendPacket(oldest->data, oldest->length);
        oldest->used = false;
    }
}

// Send an Ethernet frame whose destination MAC is still to be filled in for
// `nextHop`. Returns false only if the frame was dropped.
bool sendToNextHop(NetDevice& net, uint32_t nextHop, uint8_t* frame, size_t length) {
    if (isBroadcastIpv4(nextHop)) {
        memcpy(frame, kBroadcastMac, 6);
        return net.sendPacket(frame, length);
    }
    uint8_t mac[6] {};
    if (lookupArp(nextHop, mac)) {
        memcpy(frame, mac, 6);
        return net.sendPacket(frame, length);
    }

    if (!gArpPending) {
        gArpPending = new ArpPendingFrame[kArpPendingFrames] {};
        if (!gArpPending) {
            return false;
        }
    }
    ArpPendingFrame* slot = nullptr;
    for (size_t i = 0; i < kArpPendingFrames && !slot; ++i) {
        if (!gArpPending[i].used) {
            slot = &gArpPending[i];
        }
    }
    if (!slot) {
        return false;
    }

    const uint64_t now = time_get_uptime_ms();
    ArpResolution* r = findResolution(nextHop);
    if (!r) {
        for (auto& candidate : gArpResolutions) {
            if (!candidate.active) {
                r = &candidate;
                break;
            }
        }
        if (!r) {
            return false;
        }
        r->active = true;
        r->ip = nextHop;
        r->startedMs = now;
        r->lastRequestMs = now;
        sendArpRequest(net, nextHop);
    }

    slot->used = true;
    slot->nextHop = nextHop;
    slot->queuedMs = now;
    slot->length = static_cast<uint16_t>(length);
    memcpy(slot->data, frame, length);
    return true;
}

// Repeat outstanding ARP requests; give up on (and drop the frames for)
// addresses that never answer.
void arpTick(NetDevice& net, uint64_t now) {
    for (auto& r : gArpResolutions) {
        if (!r.active) {
            continue;
        }
        if (now - r.startedMs >= kArpGiveUpMs) {
            r.active = false;
            dropPendingFor(r.ip);
        } else if (now - r.lastRequestMs >= kArpRetryMs) {
            r.lastRequestMs = now;
            sendArpRequest(net, r.ip);
        }
    }
}

bool sendIcmpEcho(NetDevice& net, uint32_t destIp, uint16_t id, uint16_t seq) {
    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    constexpr size_t payloadSize = 16;
    uint8_t frame[sizeof(EthernetHeader) + sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payloadSize] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* icmp = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    auto* payload = reinterpret_cast<uint8_t*>(icmp + 1);

    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + sizeof(IcmpEchoHeader) + payloadSize);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolIcmp;
    ip->srcIp = toNetwork32(gIface.address);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    icmp->type = kIcmpEchoRequest;
    icmp->id = toNetwork16(id);
    icmp->seq = toNetwork16(seq);
    for (size_t i = 0; i < payloadSize; ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    icmp->checksum = toNetwork16(internetChecksum(icmp, sizeof(IcmpEchoHeader) + payloadSize));
    return sendToNextHop(net, nextHopIp(destIp), frame, sizeof(frame));
}

constexpr size_t kMaxUdpPayload = 65535 - sizeof(Ipv4Header) - sizeof(UdpHeader);
constexpr uint16_t kIpMoreFragments = 0x2000;
constexpr uint16_t kIpFragmentOffsetMask = 0x1FFF;

// An IPv4 datagram carrying `l4` (the transport header and data), as one
// frame or, when too big for one, as fragments: every fragment but the last
// carries a multiple of 8 bytes.
bool sendIpv4Payload(NetDevice& net, uint32_t srcIp, uint32_t destIp, uint8_t protocol,
                     const uint8_t* l4, size_t l4Length) {
    const size_t udpLength = l4Length;
    const uint8_t* datagram = l4;
    uint8_t localMac[6] {};
    net.getMacAddress(localMac);
    constexpr size_t kPerFragment = (NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header)) & ~size_t(7);
    const uint16_t id = gIpv4Identification++;
    const uint32_t nextHop = nextHopIp(destIp);
    for (size_t offset = 0; offset < udpLength; offset += kPerFragment) {
        const size_t chunk = udpLength - offset < kPerFragment ? udpLength - offset : kPerFragment;
        const bool more = offset + chunk < udpLength;
        uint8_t frame[NET_DEVICE_MTU] {};
        auto* eth = reinterpret_cast<EthernetHeader*>(frame);
        auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
        memcpy(eth->src, localMac, 6);
        eth->etherType = toNetwork16(kEtherTypeIpv4);
        ip->versionIhl = 0x45;
        ip->totalLength = toNetwork16(static_cast<uint16_t>(sizeof(Ipv4Header) + chunk));
        ip->identification = toNetwork16(id);
        ip->flagsFragment = toNetwork16(static_cast<uint16_t>((more ? kIpMoreFragments : 0) | (offset / 8)));
        ip->ttl = 64;
        ip->protocol = protocol;
        ip->srcIp = toNetwork32(srcIp);
        ip->dstIp = toNetwork32(destIp);
        ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));
        memcpy(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header), datagram + offset, chunk);
        if (!sendToNextHop(net, nextHop, frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + chunk)) {
            return false;
        }
    }
    return true;
}

bool sendUdpFrame(
    NetDevice& net,
    uint32_t srcIp,
    uint32_t destIp,
    uint16_t srcPort,
    uint16_t dstPort,
    const uint8_t* payload,
    size_t payloadLength
) {
    if ((!payload && payloadLength != 0) || payloadLength > kMaxUdpPayload) {
        return false;
    }
    if (payloadLength > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - sizeof(UdpHeader)) {
        static uint8_t datagram[sizeof(UdpHeader) + kMaxUdpPayload];
        auto* udp = reinterpret_cast<UdpHeader*>(datagram);
        udp->srcPort = srcPort;
        udp->dstPort = dstPort;
        udp->length = toNetwork16(static_cast<uint16_t>(sizeof(UdpHeader) + payloadLength));
        udp->checksum = 0;
        memcpy(datagram + sizeof(UdpHeader), payload, payloadLength);
        return sendIpv4Payload(net, srcIp, destIp, kIpProtocolUdp, datagram, sizeof(UdpHeader) + payloadLength);
    }

    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[NET_DEVICE_MTU] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* udp = reinterpret_cast<UdpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));

    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    const size_t udpLength = sizeof(UdpHeader) + payloadLength;
    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + udpLength);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolUdp;
    ip->srcIp = toNetwork32(srcIp);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    udp->srcPort = srcPort;
    udp->dstPort = dstPort;
    udp->length = toNetwork16(static_cast<uint16_t>(udpLength));
    udp->checksum = 0;
    if (payloadLength != 0) {
        memcpy(reinterpret_cast<uint8_t*>(udp + 1), payload, payloadLength);
    }

    return sendToNextHop(net, nextHopIp(destIp), frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + udpLength);
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
    uint16_t srcPort,
    uint16_t dstPort,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    uint16_t window,
    const uint8_t* payload,
    size_t payloadLength,
    const TcpOptions* options
) {
    // SYN options: MSS (4 bytes), then NOP + window scale (3) when offered.
    uint8_t optionBytes[8] {};
    size_t optionLength = 0;
    if (options && options->mss != 0) {
        optionBytes[0] = 2;
        optionBytes[1] = 4;
        optionBytes[2] = static_cast<uint8_t>(options->mss >> 8);
        optionBytes[3] = static_cast<uint8_t>(options->mss);
        optionLength = 4;
        if (options->hasWindowScale) {
            optionBytes[4] = 1;
            optionBytes[5] = 3;
            optionBytes[6] = 3;
            optionBytes[7] = options->windowScale;
            optionLength = 8;
        }
    }
    const size_t headerLength = sizeof(TcpHeader) + optionLength;
    if ((!payload && payloadLength != 0) ||
        payloadLength > NET_DEVICE_MTU - sizeof(EthernetHeader) - sizeof(Ipv4Header) - headerLength) {
        return false;
    }

    uint8_t localMac[6] {};
    net.getMacAddress(localMac);

    uint8_t frame[NET_DEVICE_MTU] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* tcp = reinterpret_cast<TcpHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));

    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);

    const size_t tcpLength = headerLength + payloadLength;
    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(sizeof(Ipv4Header) + tcpLength);
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolTcp;
    ip->srcIp = toNetwork32(gIface.address);
    ip->dstIp = toNetwork32(destIp);
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));

    tcp->srcPort = srcPort;
    tcp->dstPort = dstPort;
    tcp->seq = toNetwork32(seq);
    tcp->ack = toNetwork32(ack);
    tcp->dataOffsetReserved = static_cast<uint8_t>((headerLength / 4) << 4);
    tcp->flags = flags;
    tcp->window = toNetwork16(window);
    memcpy(reinterpret_cast<uint8_t*>(tcp + 1), optionBytes, optionLength);
    if (payloadLength != 0) {
        memcpy(reinterpret_cast<uint8_t*>(tcp + 1) + optionLength, payload, payloadLength);
    }
    tcp->checksum = toNetwork16(tcpChecksum(ip->srcIp, ip->dstIp, tcp, tcpLength));
    return sendToNextHop(net, nextHopIp(destIp), frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + tcpLength);
}

// "a.b.c.d" for a host-order address; `out` holds at least 16 bytes.
const char* formatIpv4(uint32_t ip, char* out) {
    char* p = out;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const uint32_t octet = (ip >> shift) & 0xFF;
        if (octet >= 100) *p++ = static_cast<char>('0' + octet / 100);
        if (octet >= 10) *p++ = static_cast<char>('0' + (octet / 10) % 10);
        *p++ = static_cast<char>('0' + octet % 10);
        if (shift != 0) *p++ = '.';
    }
    *p = '\0';
    return out;
}

void writeDecimal(char* out, uint64_t value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0) *out++ = digits[--n];
    *out = '\0';
}

// Every change to gIface goes through here: waiters in NetGetConfig see the
// new generation.
void interfaceChanged() {
    gIfaceGeneration++;
    char a[16], b[16], c[16], d[16];
    if (gIface.address == 0) {
        Cereal::get().write("[net] interface down\n");
    } else {
        Cereal::get().write(gIface.fromDhcp ? "[net] dhcp: " : "[net] static: ");
        Cereal::get().write(formatIpv4(gIface.address, a));
        Cereal::get().write(" mask ");
        Cereal::get().write(formatIpv4(gIface.netmask, b));
        Cereal::get().write(" gw ");
        Cereal::get().write(formatIpv4(gIface.gateway, c));
        Cereal::get().write(" dns ");
        Cereal::get().write(gIface.dnsCount ? formatIpv4(gIface.dns[0], d) : "none");
        Cereal::get().write("\n");
    }
    Scheduler::get().wakeAllBlockedProcesses();
}

// ---------------------------------------------------------------------------
// DHCP client (RFC 2131). Runs from the network timer and the receive path;
// sends never block. The lease is renewed at T1 by unicast, rebound at T2 by
// broadcast and dropped when it runs out.
// ---------------------------------------------------------------------------
constexpr uint16_t kDhcpServerPort = 67;
constexpr uint16_t kDhcpClientPort = 68;
constexpr uint32_t kDhcpMagicCookie = 0x63825363;
constexpr uint8_t kBootRequest = 1;
constexpr uint8_t kBootReply = 2;
constexpr uint8_t kDhcpDiscover = 1;
constexpr uint8_t kDhcpOffer = 2;
constexpr uint8_t kDhcpRequest = 3;
constexpr uint8_t kDhcpAck = 5;
constexpr uint8_t kDhcpNak = 6;
constexpr uint8_t kOptPad = 0;
constexpr uint8_t kOptSubnetMask = 1;
constexpr uint8_t kOptRouter = 3;
constexpr uint8_t kOptDns = 6;
constexpr uint8_t kOptHostName = 12;
constexpr uint8_t kOptDomainName = 15;
constexpr uint8_t kOptNtpServers = 42;
constexpr uint8_t kOptRequestedIp = 50;
constexpr uint8_t kOptLeaseTime = 51;
constexpr uint8_t kOptMessageType = 53;
constexpr uint8_t kOptServerId = 54;
constexpr uint8_t kOptParameterList = 55;
constexpr uint8_t kOptMaxMessageSize = 57;
constexpr uint8_t kOptRenewalTime = 58;
constexpr uint8_t kOptRebindingTime = 59;
constexpr uint8_t kOptClientId = 61;
constexpr uint8_t kOptEnd = 255;
constexpr uint64_t kDhcpMaxBackoffMs = 32000;
constexpr uint64_t kDhcpMinRenewRetryMs = 60000;
constexpr uint32_t kDhcpRequestAttempts = 4;

struct BootpHeader {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
} __attribute__((packed));

enum class DhcpState : uint8_t { Off, Selecting, Requesting, Bound, Renewing, Rebinding };

struct DhcpClient {
    DhcpState state;
    uint32_t xid;
    uint32_t offeredIp;      // host order
    uint32_t serverId;       // host order
    uint32_t attempt;
    uint64_t nextSendMs;
    uint64_t leaseStartMs;
    uint32_t leaseSeconds;   // 0xFFFFFFFF = infinite
    uint32_t t1Seconds;
    uint32_t t2Seconds;
};

DhcpClient gDhcp {};

struct DhcpOptions {
    uint8_t messageType;
    bool hasMask;
    uint32_t mask;
    uint32_t router;
    uint32_t dns[NET_CONFIG_MAX_DNS];
    uint32_t dnsCount;
    uint32_t serverId;
    uint32_t leaseSeconds;
    uint32_t t1Seconds;
    uint32_t t2Seconds;
    uint32_t ntp;
    char domain[NET_CONFIG_DOMAIN_SIZE];
};

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void dhcpNewTransaction(uint64_t now) {
    kernel_fill_entropy(&gDhcp.xid, sizeof(gDhcp.xid));
    gDhcp.attempt = 0;
    gDhcp.nextSendMs = now;
}

void dhcpStartSelecting(uint64_t now) {
    gDhcp.state = DhcpState::Selecting;
    gDhcp.offeredIp = 0;
    gDhcp.serverId = 0;
    dhcpNewTransaction(now);
}

// Forget the address (the lease ran out, a NAK, or NetSetConfig).
void clearInterface() {
    if (gIface.address == 0 && gIface.dnsCount == 0) {
        return;
    }
    memset(&gIface, 0, sizeof(gIface));
    interfaceChanged();
}

uint64_t dhcpBackoffMs(uint32_t attempt) {
    const uint64_t ms = 1000ULL << (attempt < 6 ? attempt : 6);
    return ms < kDhcpMaxBackoffMs ? ms : kDhcpMaxBackoffMs;
}

bool dhcpSend(NetDevice& net, uint8_t messageType) {
    uint8_t mac[6] {};
    net.getMacAddress(mac);

    uint8_t message[sizeof(BootpHeader) + 80] {};
    auto* bootp = reinterpret_cast<BootpHeader*>(message);
    bootp->op = kBootRequest;
    bootp->htype = 1;
    bootp->hlen = 6;
    bootp->xid = gDhcp.xid;
    memcpy(bootp->chaddr, mac, 6);
    bootp->magic = toNetwork32(kDhcpMagicCookie);

    // Renewing and rebinding clients own their address and say so in ciaddr;
    // before that the server is asked to broadcast its answer.
    const bool haveAddress = gDhcp.state == DhcpState::Renewing || gDhcp.state == DhcpState::Rebinding;
    if (haveAddress) {
        bootp->ciaddr = toNetwork32(gIface.address);
    } else {
        bootp->flags = toNetwork16(0x8000);
    }

    uint8_t* opt = message + sizeof(BootpHeader);
    auto put32 = [&](uint8_t code, uint32_t value) {
        *opt++ = code;
        *opt++ = 4;
        *opt++ = static_cast<uint8_t>(value >> 24);
        *opt++ = static_cast<uint8_t>(value >> 16);
        *opt++ = static_cast<uint8_t>(value >> 8);
        *opt++ = static_cast<uint8_t>(value);
    };
    *opt++ = kOptMessageType;
    *opt++ = 1;
    *opt++ = messageType;
    *opt++ = kOptClientId;
    *opt++ = 7;
    *opt++ = 1;
    memcpy(opt, mac, 6);
    opt += 6;
    if (messageType == kDhcpRequest && gDhcp.state == DhcpState::Requesting) {
        put32(kOptRequestedIp, gDhcp.offeredIp);
        put32(kOptServerId, gDhcp.serverId);
    }
    *opt++ = kOptMaxMessageSize;
    *opt++ = 2;
    *opt++ = static_cast<uint8_t>(576 >> 8);
    *opt++ = static_cast<uint8_t>(576 & 0xFF);
    static const char kHostName[] = "instantos";
    *opt++ = kOptHostName;
    *opt++ = sizeof(kHostName) - 1;
    memcpy(opt, kHostName, sizeof(kHostName) - 1);
    opt += sizeof(kHostName) - 1;
    static const uint8_t kWanted[] = {
        kOptSubnetMask, kOptRouter, kOptDns, kOptDomainName, kOptNtpServers,
        kOptLeaseTime, kOptServerId, kOptRenewalTime, kOptRebindingTime,
    };
    *opt++ = kOptParameterList;
    *opt++ = sizeof(kWanted);
    memcpy(opt, kWanted, sizeof(kWanted));
    opt += sizeof(kWanted);
    *opt++ = kOptEnd;

    // BOOTP relays expect at least 300 bytes; the zero padding is legal.
    size_t length = static_cast<size_t>(opt - message);
    if (length < 300) {
        length = 300;
    }
    const uint32_t srcIp = haveAddress ? gIface.address : 0;
    const uint32_t dstIp = gDhcp.state == DhcpState::Renewing ? gDhcp.serverId : kBroadcastIpv4;
    return sendUdpFrame(net, srcIp, dstIp, toNetwork16(kDhcpClientPort), toNetwork16(kDhcpServerPort),
                        message, length);
}

bool parseDhcpOptions(const uint8_t* p, size_t length, DhcpOptions* out) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    while (i < length) {
        const uint8_t code = p[i++];
        if (code == kOptPad) {
            continue;
        }
        if (code == kOptEnd || i >= length) {
            break;
        }
        const uint8_t len = p[i++];
        if (i + len > length) {
            return false;
        }
        const uint8_t* v = p + i;
        switch (code) {
        case kOptMessageType:
            if (len >= 1) out->messageType = v[0];
            break;
        case kOptSubnetMask:
            if (len >= 4) { out->hasMask = true; out->mask = readBe32(v); }
            break;
        case kOptRouter:
            if (len >= 4) out->router = readBe32(v);
            break;
        case kOptDns:
            for (uint8_t k = 0; k + 4 <= len && out->dnsCount < NET_CONFIG_MAX_DNS; k += 4) {
                out->dns[out->dnsCount++] = readBe32(v + k);
            }
            break;
        case kOptServerId:
            if (len >= 4) out->serverId = readBe32(v);
            break;
        case kOptNtpServers:
            if (len >= 4) out->ntp = readBe32(v);
            break;
        case kOptLeaseTime:
            if (len >= 4) out->leaseSeconds = readBe32(v);
            break;
        case kOptRenewalTime:
            if (len >= 4) out->t1Seconds = readBe32(v);
            break;
        case kOptRebindingTime:
            if (len >= 4) out->t2Seconds = readBe32(v);
            break;
        case kOptDomainName: {
            size_t n = len < NET_CONFIG_DOMAIN_SIZE - 1 ? len : NET_CONFIG_DOMAIN_SIZE - 1;
            memcpy(out->domain, v, n);
            out->domain[n] = '\0';
            break;
        }
        default:
            break;
        }
        i += len;
    }
    return out->messageType != 0;
}

void dhcpBind(uint32_t address, const DhcpOptions& o, uint64_t now) {
    gDhcp.state = DhcpState::Bound;
    gDhcp.leaseStartMs = now;
    gDhcp.leaseSeconds = o.leaseSeconds ? o.leaseSeconds : 3600;
    if (gDhcp.leaseSeconds == 0xFFFFFFFFU) {
        gDhcp.t1Seconds = gDhcp.t2Seconds = 0xFFFFFFFFU;
    } else {
        gDhcp.t1Seconds = o.t1Seconds ? o.t1Seconds : gDhcp.leaseSeconds / 2;
        gDhcp.t2Seconds = o.t2Seconds ? o.t2Seconds
                                      : static_cast<uint32_t>(static_cast<uint64_t>(gDhcp.leaseSeconds) * 7 / 8);
    }
    if (o.serverId) {
        gDhcp.serverId = o.serverId;
    }

    gIface.address = address;
    gIface.netmask = o.hasMask ? o.mask : 0xFFFFFF00U;
    gIface.gateway = o.router;
    gIface.dnsCount = o.dnsCount;
    memcpy(gIface.dns, o.dns, sizeof(gIface.dns));
    gIface.ntp = o.ntp;
    memcpy(gIface.domain, o.domain, sizeof(gIface.domain));
    gIface.fromDhcp = true;
    interfaceChanged();

    char lease[24];
    writeDecimal(lease, gDhcp.leaseSeconds);
    Cereal::get().write("[net] dhcp: lease ");
    Cereal::get().write(lease);
    Cereal::get().write(" s\n");
}

void handleDhcp(NetDevice& net, uint32_t srcIp, const uint8_t* data, size_t length) {
    if (gDhcp.state == DhcpState::Off || gDhcp.state == DhcpState::Bound ||
        length < sizeof(BootpHeader)) {
        return;
    }
    const auto* bootp = reinterpret_cast<const BootpHeader*>(data);
    uint8_t mac[6] {};
    net.getMacAddress(mac);
    if (bootp->op != kBootReply || bootp->xid != gDhcp.xid ||
        fromNetwork32(bootp->magic) != kDhcpMagicCookie || !sameMac(bootp->chaddr, mac)) {
        return;
    }
    DhcpOptions o {};
    if (!parseDhcpOptions(data + sizeof(BootpHeader), length - sizeof(BootpHeader), &o)) {
        return;
    }
    const uint32_t yiaddr = fromNetwork32(bootp->yiaddr);
    const uint64_t now = time_get_uptime_ms();

    if (gDhcp.state == DhcpState::Selecting) {
        if (o.messageType != kDhcpOffer || yiaddr == 0) {
            return;
        }
        gDhcp.offeredIp = yiaddr;
        gDhcp.serverId = o.serverId ? o.serverId : srcIp;
        gDhcp.state = DhcpState::Requesting;
        gDhcp.attempt = 0;
        gDhcp.nextSendMs = now;   // the REQUEST goes out on this tick
        dhcpSend(net, kDhcpRequest);
        gDhcp.attempt = 1;
        gDhcp.nextSendMs = now + dhcpBackoffMs(0);
        return;
    }

    // Requesting, Renewing or Rebinding: waiting for ACK or NAK.
    if (o.serverId && gDhcp.state == DhcpState::Requesting && o.serverId != gDhcp.serverId) {
        return;
    }
    if (o.messageType == kDhcpNak) {
        Cereal::get().write("[net] dhcp: NAK, starting over\n");
        if (gDhcp.state != DhcpState::Requesting) {
            clearInterface();
        }
        dhcpStartSelecting(now);
        return;
    }
    if (o.messageType != kDhcpAck || yiaddr == 0) {
        return;
    }
    dhcpBind(yiaddr, o, now);
}

void dhcpTick(NetDevice& net, uint64_t now) {
    if (gDhcp.state == DhcpState::Off) {
        return;
    }
    const uint64_t elapsedMs = now - gDhcp.leaseStartMs;
    const uint64_t t1Ms = static_cast<uint64_t>(gDhcp.t1Seconds) * 1000;
    const uint64_t t2Ms = static_cast<uint64_t>(gDhcp.t2Seconds) * 1000;
    const uint64_t leaseMs = static_cast<uint64_t>(gDhcp.leaseSeconds) * 1000;
    const bool infinite = gDhcp.leaseSeconds == 0xFFFFFFFFU;

    switch (gDhcp.state) {
    case DhcpState::Selecting:
        if (now >= gDhcp.nextSendMs) {
            dhcpSend(net, kDhcpDiscover);
            gDhcp.nextSendMs = now + dhcpBackoffMs(gDhcp.attempt++);
        }
        return;
    case DhcpState::Requesting:
        if (now >= gDhcp.nextSendMs) {
            if (gDhcp.attempt >= kDhcpRequestAttempts) {
                dhcpStartSelecting(now);
                return;
            }
            dhcpSend(net, kDhcpRequest);
            gDhcp.nextSendMs = now + dhcpBackoffMs(gDhcp.attempt++);
        }
        return;
    case DhcpState::Bound:
        if (!infinite && elapsedMs >= t1Ms) {
            gDhcp.state = DhcpState::Renewing;
            dhcpNewTransaction(now);
        }
        return;
    case DhcpState::Renewing:
    case DhcpState::Rebinding: {
        if (!infinite && elapsedMs >= leaseMs) {
            Cereal::get().write("[net] dhcp: lease expired\n");
            clearInterface();
            dhcpStartSelecting(now);
            return;
        }
        if (gDhcp.state == DhcpState::Renewing && !infinite && elapsedMs >= t2Ms) {
            gDhcp.state = DhcpState::Rebinding;
            gDhcp.nextSendMs = now;
        }
        if (now >= gDhcp.nextSendMs) {
            dhcpSend(net, kDhcpRequest);
            // RFC 2131 4.4.5: half the time left to T2 (or to the end of the
            // lease), but not more often than once a minute.
            const uint64_t until = gDhcp.state == DhcpState::Renewing ? t2Ms : leaseMs;
            uint64_t wait = until > elapsedMs ? (until - elapsedMs) / 2 : 0;
            if (wait < kDhcpMinRenewRetryMs) {
                wait = kDhcpMinRenewRetryMs;
            }
            gDhcp.nextSendMs = now + wait;
        }
        return;
    }
    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// ICMP
// ---------------------------------------------------------------------------
uint64_t gIcmpErrorLastMs = 0;
uint32_t gIcmpErrorBurst = 0;
constexpr uint32_t kIcmpErrorBurst = 8;   // errors per 100 ms, like a token bucket

// Send an ICMP error about `packet` (Ethernet frame, IPv4 at +14) back to its
// sender. Never about a broadcast, an ICMP error or an unconfigured source.
void sendIcmpError(NetDevice& net, const uint8_t* packet, size_t ihl, size_t totalLength,
                   uint8_t type, uint8_t code) {
    const auto* orig = reinterpret_cast<const Ipv4Header*>(packet + sizeof(EthernetHeader));
    const uint32_t dest = fromNetwork32(orig->srcIp);
    if (gIface.address == 0 || dest == 0 || isBroadcastIpv4(dest) || (dest >> 28) == 0xE) {
        return;
    }
    const uint64_t now = time_get_uptime_ms();
    if (now - gIcmpErrorLastMs >= 100) {
        gIcmpErrorLastMs = now;
        gIcmpErrorBurst = 0;
    }
    if (gIcmpErrorBurst++ >= kIcmpErrorBurst) {
        return;
    }

    // The original header plus as much of its payload as keeps us within the
    // 576 bytes every host must accept.
    size_t quoted = totalLength;
    const size_t maxQuoted = 576 - sizeof(Ipv4Header) - sizeof(IcmpEchoHeader);
    if (quoted > maxQuoted) {
        quoted = maxQuoted;
    }
    if (quoted < ihl) {
        return;
    }

    uint8_t localMac[6] {};
    net.getMacAddress(localMac);
    uint8_t frame[sizeof(EthernetHeader) + 576] {};
    auto* eth = reinterpret_cast<EthernetHeader*>(frame);
    auto* ip = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
    auto* icmp = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
    memcpy(eth->src, localMac, 6);
    eth->etherType = toNetwork16(kEtherTypeIpv4);
    const size_t icmpLength = sizeof(IcmpEchoHeader) + quoted;
    ip->versionIhl = 0x45;
    ip->totalLength = toNetwork16(static_cast<uint16_t>(sizeof(Ipv4Header) + icmpLength));
    ip->identification = toNetwork16(gIpv4Identification++);
    ip->ttl = 64;
    ip->protocol = kIpProtocolIcmp;
    ip->srcIp = toNetwork32(gIface.address);
    ip->dstIp = orig->srcIp;
    ip->headerChecksum = toNetwork16(internetChecksum(ip, sizeof(Ipv4Header)));
    icmp->type = type;
    icmp->code = code;
    memcpy(icmp + 1, orig, quoted);
    icmp->checksum = toNetwork16(internetChecksum(icmp, icmpLength));
    sendToNextHop(net, nextHopIp(dest), frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + icmpLength);
}

// Errno for a destination-unreachable code, the way Linux maps them. `hard`
// errors end a connection attempt; soft ones are only reported if it fails.
int icmpUnreachableErrno(uint8_t code, bool* hard) {
    *hard = true;
    switch (code) {
    case 0:  *hard = false; return SysErrNetUnreachable;     // net unreachable
    case 1:  *hard = false; return SysErrHostUnreachable;    // host unreachable
    case 2:  return SysErrProtoOpt;                          // protocol unreachable
    case 3:  return SysErrConnectionRefused;                 // port unreachable
    case 4:  *hard = false; return SysErrMessageSize;        // fragmentation needed
    case 5:  return SysErrOperationNotSupported;             // source route failed
    case 6:  return SysErrNetUnreachable;                    // network unknown
    case 7:  return SysErrHostDown;                          // host unknown
    case 9:  return SysErrNetUnreachable;                    // network prohibited
    case 11: *hard = false; return SysErrNetUnreachable;     // net unreachable for TOS
    case 12: *hard = false; return SysErrHostUnreachable;    // host unreachable for TOS
    default: return SysErrHostUnreachable;                   // prohibited, filtered, ...
    }
}

void handleIcmp(NetDevice& net, const uint8_t* packet, size_t ihl, size_t totalLength, bool unicast) {
    const auto* ip = reinterpret_cast<const Ipv4Header*>(packet + sizeof(EthernetHeader));
    const auto* icmp = reinterpret_cast<const IcmpEchoHeader*>(packet + sizeof(EthernetHeader) + ihl);
    const size_t icmpLength = totalLength - ihl;
    if (icmpLength < sizeof(IcmpEchoHeader) || internetChecksum(icmp, icmpLength) != 0) {
        return;
    }

    if (icmp->type == kIcmpEchoReply && icmp->code == 0) {
        socketDeliverIcmpEcho(ip->srcIp, icmp->id, reinterpret_cast<const uint8_t*>(icmp), icmpLength);
        gLastPingReply.srcIp = fromNetwork32(ip->srcIp);
        gLastPingReply.id = fromNetwork16(icmp->id);
        gLastPingReply.seq = fromNetwork16(icmp->seq);
        gLastPingReply.payloadSize = static_cast<uint16_t>(icmpLength - sizeof(IcmpEchoHeader));
        gLastPingReply.reserved = 0;
        gHasPingReply = true;
        return;
    }

    if (icmp->type == kIcmpEchoRequest && unicast) {
        // Same identifier, sequence and payload back to the sender.
        uint8_t localMac[6] {};
        net.getMacAddress(localMac);
        uint8_t frame[NET_DEVICE_MTU] {};
        auto* eth = reinterpret_cast<EthernetHeader*>(frame);
        auto* reply = reinterpret_cast<Ipv4Header*>(frame + sizeof(EthernetHeader));
        auto* echo = reinterpret_cast<IcmpEchoHeader*>(frame + sizeof(EthernetHeader) + sizeof(Ipv4Header));
        if (sizeof(EthernetHeader) + sizeof(Ipv4Header) + icmpLength > sizeof(frame)) {
            return;
        }
        memcpy(eth->src, localMac, 6);
        eth->etherType = toNetwork16(kEtherTypeIpv4);
        reply->versionIhl = 0x45;
        reply->totalLength = toNetwork16(static_cast<uint16_t>(sizeof(Ipv4Header) + icmpLength));
        reply->identification = toNetwork16(gIpv4Identification++);
        reply->ttl = 64;
        reply->protocol = kIpProtocolIcmp;
        reply->srcIp = toNetwork32(gIface.address);
        reply->dstIp = ip->srcIp;
        reply->headerChecksum = toNetwork16(internetChecksum(reply, sizeof(Ipv4Header)));
        memcpy(echo, icmp, icmpLength);
        echo->type = kIcmpEchoReply;
        echo->checksum = 0;
        echo->checksum = toNetwork16(internetChecksum(echo, icmpLength));
        const uint32_t dest = fromNetwork32(ip->srcIp);
        sendToNextHop(net, nextHopIp(dest), frame, sizeof(EthernetHeader) + sizeof(Ipv4Header) + icmpLength);
        return;
    }

    if (icmp->type != kIcmpDestUnreachable && icmp->type != kIcmpTimeExceeded) {
        return;
    }
    // The error quotes the header of the datagram we sent plus at least its
    // first 8 bytes: the ports, and for TCP the sequence number.
    const uint8_t* quoted = reinterpret_cast<const uint8_t*>(icmp + 1);
    const size_t quotedLength = icmpLength - sizeof(IcmpEchoHeader);
    if (quotedLength < sizeof(Ipv4Header)) {
        return;
    }
    const auto* inner = reinterpret_cast<const Ipv4Header*>(quoted);
    const size_t innerIhl = (inner->versionIhl & 0x0f) * 4;
    if ((inner->versionIhl >> 4) != 4 || innerIhl < sizeof(Ipv4Header) ||
        quotedLength < innerIhl + 8 || fromNetwork32(inner->srcIp) != gIface.address ||
        (inner->protocol != kIpProtocolTcp && inner->protocol != kIpProtocolUdp)) {
        return;
    }
    const uint8_t* ports = quoted + innerIhl;
    IcmpErrorInfo info {};
    info.protocol = inner->protocol == kIpProtocolTcp ? IcmpErrorInfo::Tcp : IcmpErrorInfo::Udp;
    memcpy(&info.localPort, ports, 2);
    memcpy(&info.remotePort, ports + 2, 2);
    info.remoteIpNetworkOrder = inner->dstIp;
    if (inner->protocol == kIpProtocolTcp) {
        info.tcpSeq = readBe32(ports + 4);
    }
    if (icmp->type == kIcmpTimeExceeded) {
        info.error = SysErrHostUnreachable;
        info.hard = false;
    } else {
        info.error = icmpUnreachableErrno(icmp->code, &info.hard);
    }
    socketProcessIcmpError(info);
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
    if (senderIp != 0) {   // 0 is a probe (RFC 5227), not a mapping
        learnArp(net, senderIp, arp->senderMac);
    }

    const uint16_t operation = fromNetwork16(arp->operation);
    if (operation == kArpOperationRequest && gIface.address != 0 && targetIp == gIface.address) {
        sendArpReply(net, *arp);
    }
}

// ---------------------------------------------------------------------------
// IPv4 reassembly (RFC 815 style): a few datagrams at a time, tracked in
// 8-byte blocks so overlapping or repeated fragments are harmless.
// ---------------------------------------------------------------------------
constexpr size_t kReassemblySlots = 4;
constexpr uint64_t kReassemblyTimeoutMs = 15000;
constexpr size_t kReassemblyHeadroom = sizeof(EthernetHeader) + sizeof(Ipv4Header);
constexpr size_t kReassemblyBlocks = 65536 / 8;

struct Reassembly {
    bool used;
    uint32_t src;
    uint32_t dst;
    uint16_t id;
    uint8_t protocol;
    uint64_t startedMs;
    uint32_t payloadLength;   // known once the last fragment arrived, else 0
    uint32_t blocksHave;
    uint8_t* buffer;          // headroom for Ethernet + IPv4 headers, then data
    uint8_t blocks[kReassemblyBlocks / 8];
};

Reassembly gReassembly[kReassemblySlots] {};

void handleIpv4(NetDevice& net, const uint8_t* packet, size_t length);

// Takes one fragment; dispatches the whole datagram once every byte is in.
void reassembleFragment(NetDevice& net, const uint8_t* packet, size_t ihl, size_t totalLength) {
    const auto* ip = reinterpret_cast<const Ipv4Header*>(packet + sizeof(EthernetHeader));
    const uint16_t fragment = fromNetwork16(ip->flagsFragment);
    const size_t offset = static_cast<size_t>(fragment & kIpFragmentOffsetMask) * 8;
    const bool more = (fragment & kIpMoreFragments) != 0;
    const size_t dataLength = totalLength - ihl;
    if ((more && (dataLength % 8) != 0) || dataLength == 0 || offset + dataLength > 65535 - sizeof(Ipv4Header)) {
        return;
    }
    const uint64_t now = time_get_uptime_ms();
    Reassembly* slot = nullptr;
    Reassembly* oldest = nullptr;
    for (auto& r : gReassembly) {
        if (r.used && now - r.startedMs >= kReassemblyTimeoutMs) {
            r.used = false;
        }
        if (r.used && r.src == ip->srcIp && r.dst == ip->dstIp && r.id == ip->identification &&
            r.protocol == ip->protocol) {
            slot = &r;
        }
        if (!oldest || !r.used || (oldest->used && r.startedMs < oldest->startedMs)) {
            oldest = &r;
        }
    }
    if (!slot) {
        slot = oldest;   // a free slot, or else the oldest datagram is given up
        if (!slot->buffer) {
            slot->buffer = new uint8_t[kReassemblyHeadroom + 65535];
            if (!slot->buffer) {
                return;
            }
        }
        slot->used = true;
        slot->src = ip->srcIp;
        slot->dst = ip->dstIp;
        slot->id = ip->identification;
        slot->protocol = ip->protocol;
        slot->startedMs = now;
        slot->payloadLength = 0;
        slot->blocksHave = 0;
        memset(slot->blocks, 0, sizeof(slot->blocks));
    }

    memcpy(slot->buffer + kReassemblyHeadroom + offset, packet + sizeof(EthernetHeader) + ihl, dataLength);
    const size_t firstBlock = offset / 8;
    const size_t endBlock = (offset + dataLength + 7) / 8;
    for (size_t b = firstBlock; b < endBlock; ++b) {
        if ((slot->blocks[b / 8] & (1u << (b % 8))) == 0) {
            slot->blocks[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
            slot->blocksHave++;
        }
    }
    if (!more) {
        slot->payloadLength = static_cast<uint32_t>(offset + dataLength);
    }
    if (offset == 0) {
        // The first fragment's Ethernet header and IP header (without options)
        // become the reassembled datagram's.
        memcpy(slot->buffer, packet, sizeof(EthernetHeader) + sizeof(Ipv4Header));
    }
    if (slot->payloadLength == 0 || slot->blocksHave != (slot->payloadLength + 7) / 8) {
        return;
    }
    // A first fragment has always been seen by now: block 0 is in.
    auto* whole = reinterpret_cast<Ipv4Header*>(slot->buffer + sizeof(EthernetHeader));
    whole->versionIhl = 0x45;
    whole->flagsFragment = 0;
    whole->totalLength = toNetwork16(static_cast<uint16_t>(sizeof(Ipv4Header) + slot->payloadLength));
    slot->used = false;
    handleIpv4(net, slot->buffer, kReassemblyHeadroom + slot->payloadLength);
}

void handleIpv4(NetDevice& net, const uint8_t* packet, size_t length) {
    if (length < sizeof(EthernetHeader) + sizeof(Ipv4Header)) {
        return;
    }
    const auto* eth = reinterpret_cast<const EthernetHeader*>(packet);
    const auto* ip = reinterpret_cast<const Ipv4Header*>(packet + sizeof(EthernetHeader));
    const size_t ihl = (ip->versionIhl & 0x0f) * 4;
    if ((ip->versionIhl >> 4) != 4 || ihl < sizeof(Ipv4Header) ||
        length < sizeof(EthernetHeader) + ihl) {
        return;
    }
    // Ours, a broadcast, or - while DHCP has no address yet - anything UDP,
    // since a server may send the offer to the address it is offering.
    const uint32_t dstIp = fromNetwork32(ip->dstIp);
    const bool unicast = gIface.address != 0 && dstIp == gIface.address;
    const bool broadcast = isBroadcastIpv4(dstIp);
    const bool unconfiguredUdp = gIface.address == 0 && ip->protocol == kIpProtocolUdp;
    if (!unicast && !broadcast && !unconfiguredUdp) {
        return;
    }
    // Only an on-link sender's address maps to the frame's source MAC; an
    // off-link one arrived through the gateway.
    const uint32_t srcIp = fromNetwork32(ip->srcIp);
    if (gIface.address != 0 && srcIp != 0 && nextHopIp(srcIp) == srcIp) {
        learnArp(net, srcIp, eth->src);
    }

    const size_t totalLength = fromNetwork16(ip->totalLength);
    if (totalLength < ihl || length < sizeof(EthernetHeader) + totalLength) {
        return;
    }
    if ((fromNetwork16(ip->flagsFragment) & (kIpMoreFragments | kIpFragmentOffsetMask)) != 0) {
        if (unicast || broadcast) {
            reassembleFragment(net, packet, ihl, totalLength);
        }
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
        const auto* data = reinterpret_cast<const uint8_t*>(udp + 1);
        const size_t dataLength = udpLength - sizeof(UdpHeader);
        if (udp->dstPort == toNetwork16(kDhcpClientPort) && udp->srcPort == toNetwork16(kDhcpServerPort)) {
            handleDhcp(net, srcIp, data, dataLength);
            return;
        }
        if (!unicast && !broadcast) {
            return;
        }
        const bool delivered = socketDeliverUdpDatagram(udp->srcPort, ip->srcIp, udp->dstPort, data, dataLength);
        if (!delivered && unicast) {
            sendIcmpError(net, packet, ihl, totalLength, kIcmpDestUnreachable, kIcmpCodePortUnreachable);
        }
        return;
    }

    if (!unicast) {
        return;   // no TCP or ICMP to broadcast addresses
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
        const uint8_t* opt = reinterpret_cast<const uint8_t*>(tcp + 1);
        const uint8_t* optEnd = packet + sizeof(EthernetHeader) + ihl + tcpHeaderLength;
        while (opt < optEnd && *opt != 0) {
            if (*opt == 1) {
                opt++;
                continue;
            }
            if (opt + 1 >= optEnd || opt[1] < 2 || opt + opt[1] > optEnd) {
                break;
            }
            if (opt[0] == 2 && opt[1] == 4) {
                segment.options.mss = static_cast<uint16_t>((opt[2] << 8) | opt[3]);
            } else if (opt[0] == 3 && opt[1] == 3) {
                segment.options.hasWindowScale = true;
                segment.options.windowScale = opt[2] > 14 ? 14 : opt[2];   // RFC 7323 cap
            }
            opt += opt[1];
        }
        socketProcessTcpSegment(segment);
        return;
    }

    if (ip->protocol == kIpProtocolIcmp) {
        handleIcmp(net, packet, ihl, totalLength, unicast);
    }
}

// Drain and dispatch all currently-available received frames.  Returns the
// number of frames processed.
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

}

uint64_t netSendUdpDatagram(uint16_t srcPort, const uint8_t* dstAddress, uint64_t dstAddressLength, uint64_t buffer, uint64_t length) {
    if (!dstAddress || dstAddressLength < 8) {
        return syscall_error(SysErrInvalid);
    }
    if (length > kMaxUdpPayload) {
        return syscall_error(SysErrMessageSize);
    }

    uint32_t dstIpNetworkOrder = 0;
    memcpy(&dstIpNetworkOrder, dstAddress + 4, sizeof(dstIpNetworkOrder));
    const uint32_t dstIp = fromNetwork32(dstIpNetworkOrder);
    uint16_t dstPort = 0;
    memcpy(&dstPort, dstAddress + 2, sizeof(dstPort));
    if (dstIp == 0 || srcPort == 0 || dstPort == 0) {
        return syscall_error(SysErrInvalid);
    }

    static uint8_t payload[kMaxUdpPayload];
    if (length != 0 && !Syscall::copyFromUser(payload, buffer, static_cast<size_t>(length))) {
        return syscall_error(SysErrInvalid);
    }

    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return syscall_error(SysErrNoEntry);
    }
    if (gIface.address == 0) {
        return syscall_error(SysErrNetUnreachable);
    }
    NetDevice& net = *netDev;

    return sendUdpFrame(net, gIface.address, dstIp, srcPort, dstPort, payload, static_cast<size_t>(length))
        ? length
        : syscall_error(SysErrAgain);
}

// An ICMP message from a ping socket (checksum already filled in).
bool netSendIcmpMessage(uint32_t dstIpNetworkOrder, const uint8_t* message, uint64_t length) {
    NetDevice* netDev = NetDeviceRegistry::active();
    const uint32_t dstIp = fromNetwork32(dstIpNetworkOrder);
    if (!netDev || gIface.address == 0 || dstIp == 0 || length > kMaxUdpPayload + sizeof(UdpHeader)) {
        return false;
    }
    return sendIpv4Payload(*netDev, gIface.address, dstIp, kIpProtocolIcmp, message, static_cast<size_t>(length));
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
    size_t payloadLength,
    const TcpOptions* options
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
    if (!netDev || gIface.address == 0) {
        return false;
    }
    NetDevice& net = *netDev;

    return sendTcpFrame(net, dstIp, srcPort, dstPort, seq, ack, flags, window,
                        payload, payloadLength, options);
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
    if (gIface.address == 0) {
        return syscall_error(SysErrNetUnreachable);
    }
    NetDevice& net = *netDev;

    return sendIcmpEcho(net, targetIp, static_cast<uint16_t>(id), static_cast<uint16_t>(seq))
        ? 0
        : syscall_error(SysErrAgain);
}

// The cable. A link that comes up (a real PHY needs a few seconds after
// reset, or the cable went in later) gets a DHCP message straight away rather
// than at the end of the current back-off; after an outage the lease is
// confirmed by broadcast, since this may be another network. NetGetConfig
// waiters hear of every change.
void linkTick(NetDevice& net, uint64_t now) {
    const bool up = net.isLinkUp();
    if (up == gLinkUp) {
        return;
    }
    gLinkUp = up;
    Cereal::get().write(up ? "[net] link up\n" : "[net] link down\n");
    if (up) {
        switch (gDhcp.state) {
        case DhcpState::Selecting:
        case DhcpState::Requesting:
            gDhcp.attempt = 0;
            gDhcp.nextSendMs = now;
            break;
        case DhcpState::Bound:
        case DhcpState::Renewing:
            gDhcp.state = DhcpState::Rebinding;
            dhcpNewTransaction(now);
            break;
        case DhcpState::Rebinding:
            gDhcp.nextSendMs = now;
            break;
        case DhcpState::Off:
            break;
        }
    }
    gIfaceGeneration++;
    Scheduler::get().wakeAllBlockedProcesses();
}

uint64_t netPumpOnce() {
    NetDevice* netDev = NetDeviceRegistry::active();
    if (!netDev) {
        return 0;
    }
    const uint64_t processed = pumpPackets(*netDev);
    const uint64_t now = time_get_uptime_ms();
    linkTick(*netDev, now);
    arpTick(*netDev, now);
    dhcpTick(*netDev, now);
    socketTcpTick();
    return processed;
}

// The kernel pumps the NIC itself (netTimerTick); this stays for programs that
// still call it.
uint64_t Syscall::sys_net_process_packets() {
    if (!NetDeviceRegistry::active()) {
        return syscall_error(SysErrNoEntry);
    }
    return netPumpOnce();
}

void netTimerTick() {
    // Set by netEnableTimerPump once a NIC is up: probing for one from the
    // tick would stall it.
    NetDevice* netDev = gNetTickDevice;
    if (!netDev) {
        return;
    }
    // A NIC with a receive interrupt is still drained with the timers, in
    // case an interrupt went missing.
    const uint64_t now = time_get_uptime_ms();
    const bool timersDue = now - gNetLastTimersMs >= kNetTimerPeriodMs;
    if (!netDev->interruptDriven() || timersDue) {
        pumpPackets(*netDev);
    }
    if (timersDue) {
        gNetLastTimersMs = now;
        linkTick(*netDev, now);
        arpTick(*netDev, now);
        dhcpTick(*netDev, now);
        socketTcpTick();
    }
}

void netDeviceInterrupt(InterruptFrame* frame, bool mayReschedule) {
    NetDevice* netDev = gNetTickDevice;
    if (!netDev) {
        return;   // before netEnableTimerPump: the first tick drains the ring
    }
    if (pumpPackets(*netDev) == 0 || !mayReschedule || !frame) {
        return;
    }
    // Interrupts only arrive in user mode or idle (kernel code runs with them
    // off), so the interrupted context can give way to the woken process.
    Process* current = Scheduler::get().getCurrentProcess();
    const bool interruptedUser = frame->cs == 0x23;
    const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;
    if (interruptedUser || runningIdle) {
        Scheduler::get().schedule(frame);
    }
}

void netEnableTimerPump() {
    gNetTickDevice = NetDeviceRegistry::active();
    // The interface comes up by DHCP; the first DISCOVER leaves on the next
    // network tick.
    if (gNetTickDevice) {
        dhcpStartSelecting(time_get_uptime_ms());
    }
}

uint32_t netLocalAddressNetworkOrder() {
    return toNetwork32(gIface.address);
}

uint64_t Syscall::sys_net_get_config(uint64_t configPtr, uint64_t knownGeneration, uint64_t timeoutMs) {
    // With a timeout, wait for the configuration to move past the generation
    // the caller already has (UINT64_MAX waits forever).
    if (timeoutMs != 0 && knownGeneration == gIfaceGeneration) {
        Process* current = Scheduler::get().getCurrentProcess();
        if (!current) {
            return syscall_error(SysErrInvalid);
        }
        const uint64_t deadline = timeoutMs == UINT64_MAX ? 0 : time_get_uptime_ms() + timeoutMs;
        while (knownGeneration == gIfaceGeneration) {
            if (current->hasDeliverableSignal()) {
                return syscall_error(SysErrInterrupted);
            }
            if (deadline != 0) {
                if (time_get_uptime_ms() >= deadline) {
                    break;
                }
                current->sleepUntil(deadline, true);
            }
            current->setState(ProcessState::Blocked);
            Scheduler::get().scheduleFromSyscall();
            current->clearSleep();
        }
    }

    NetConfig config {};
    if (gIface.address != 0) {
        config.flags = NetConfigUp | (gIface.fromDhcp ? NetConfigDhcp : 0);
        config.address = toNetwork32(gIface.address);
        config.netmask = toNetwork32(gIface.netmask);
        config.gateway = toNetwork32(gIface.gateway);
        config.dnsCount = gIface.dnsCount;
        for (uint32_t i = 0; i < gIface.dnsCount; ++i) {
            config.dns[i] = toNetwork32(gIface.dns[i]);
        }
        config.ntpServer = toNetwork32(gIface.ntp);
        memcpy(config.domain, gIface.domain, sizeof(config.domain));
        if (gIface.fromDhcp) {
            config.dhcpServer = toNetwork32(gDhcp.serverId);
            config.leaseSeconds = gDhcp.leaseSeconds;
            if (gDhcp.leaseSeconds == 0xFFFFFFFFU) {
                config.leaseRemaining = 0xFFFFFFFFU;
            } else {
                const uint64_t elapsed = (time_get_uptime_ms() - gDhcp.leaseStartMs) / 1000;
                config.leaseRemaining = elapsed < gDhcp.leaseSeconds
                    ? static_cast<uint32_t>(gDhcp.leaseSeconds - elapsed) : 0;
            }
        }
    }
    config.generation = gIfaceGeneration;
    config.linkUp = gLinkUp ? 1 : 0;
    if (gNetTickDevice) {
        strncpy(config.device, gNetTickDevice->name(), sizeof(config.device) - 1);
    }
    return copyToUser(configPtr, &config, sizeof(config)) ? 0 : syscall_error(SysErrInvalid);
}

uint64_t Syscall::sys_net_set_config(uint64_t configPtr, uint64_t mode) {
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current || !current->isPrivileged()) {
        return syscall_error(SysErrAccess);
    }
    if (!NetDeviceRegistry::active()) {
        return syscall_error(SysErrNoEntry);
    }
    const uint64_t now = time_get_uptime_ms();
    switch (mode) {
    case NetConfigStatic: {
        NetConfig config {};
        if (!copyFromUser(&config, configPtr, sizeof(config)) ||
            config.address == 0 || config.dnsCount > NET_CONFIG_MAX_DNS) {
            return syscall_error(SysErrInvalid);
        }
        gDhcp.state = DhcpState::Off;
        memset(&gIface, 0, sizeof(gIface));
        gIface.address = fromNetwork32(config.address);
        gIface.netmask = config.netmask ? fromNetwork32(config.netmask) : 0xFFFFFF00U;
        gIface.gateway = fromNetwork32(config.gateway);
        gIface.dnsCount = config.dnsCount;
        for (uint32_t i = 0; i < config.dnsCount; ++i) {
            gIface.dns[i] = fromNetwork32(config.dns[i]);
        }
        gIface.ntp = fromNetwork32(config.ntpServer);
        memcpy(gIface.domain, config.domain, sizeof(gIface.domain));
        gIface.domain[sizeof(gIface.domain) - 1] = '\0';
        interfaceChanged();
        return 0;
    }
    case NetConfigDhcpStart:
        clearInterface();
        dhcpStartSelecting(now);
        return 0;
    case NetConfigDhcpRenew:
        if (gDhcp.state != DhcpState::Bound && gDhcp.state != DhcpState::Renewing &&
            gDhcp.state != DhcpState::Rebinding) {
            return syscall_error(SysErrInvalid);
        }
        gDhcp.state = DhcpState::Renewing;
        dhcpNewTransaction(now);
        return 0;
    case NetConfigDown:
        gDhcp.state = DhcpState::Off;
        clearInterface();
        return 0;
    default:
        return syscall_error(SysErrInvalid);
    }
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
