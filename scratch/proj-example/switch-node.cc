#include "flow-tuple.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-net-device.h"
#include "switch-node.h"
#include <unordered_set>

NS_LOG_COMPONENT_DEFINE("SwitchNode");

NS_OBJECT_ENSURE_REGISTERED(SwitchNode);

TypeId SwitchNode::GetTypeId() {
    static TypeId tid = TypeId("SwitchNode")
        .SetParent<Node>()
        .AddConstructor<SwitchNode>()
    ;
    return tid;
}

ParsedPkt ParsedPkt::FromIpv4Pkt(Ptr<const Packet> p) {
    ParsedPkt parsed;
    Ptr<Packet> pkt = p->Copy();
    parsed.ipv4Hdr = std::make_unique<Ipv4Header>();
    pkt->RemoveHeader(*parsed.ipv4Hdr);
    auto L4Proto = parsed.ipv4Hdr->GetProtocol();
    if (L4Proto == TcpL4Protocol::PROT_NUMBER) {
        parsed.tcpHdr = std::make_unique<TcpHeader>();
        pkt->RemoveHeader(*parsed.tcpHdr);
    } else if (L4Proto == UdpL4Protocol::PROT_NUMBER) {
        parsed.udpHdr = std::make_unique<UdpHeader>();
        pkt->RemoveHeader(*parsed.udpHdr);
    }
    parsed.payload = pkt;
    return parsed;
}

FlowTuple ParsedPkt::GetFlowTuple() const {
    FlowTuple flowTuple;
    if (ipv4Hdr == nullptr) {
        return flowTuple;
    }
    flowTuple.srcAddr = ipv4Hdr->GetSource().Get();
    flowTuple.dstAddr = ipv4Hdr->GetDestination().Get();
    flowTuple.proto = ipv4Hdr->GetProtocol();
    if (tcpHdr != nullptr) {
        flowTuple.srcPort = tcpHdr->GetSourcePort();
        flowTuple.dstPort = tcpHdr->GetDestinationPort();
    } else if (udpHdr != nullptr) {
        flowTuple.srcPort = udpHdr->GetSourcePort();
        flowTuple.dstPort = udpHdr->GetDestinationPort();
    }
    return flowTuple;
}

void SwitchNode::DoInitialize() {
    for (uint32_t i = 0; i < GetNDevices(); i++) {
        Ptr<NetDevice> dev = GetDevice(i);
        dev->SetReceiveCallback(MakeCallback(&SwitchNode::ReceiveFromDevice, this));
    }

    // setup route table
    std::unordered_map<uint32_t, std::set<int>> routeTable;
    auto globalRouting = GetObject<GlobalRouter>()->GetRoutingProtocol();
    auto ipv4L3Proto = GetObject<Ipv4L3Protocol>();
    std::vector<int> iface2DevIdxMap;
    iface2DevIdxMap.resize(ipv4L3Proto->GetNInterfaces());
    for (uint32_t i = 0; i < GetNDevices(); i++) {
        auto dev = GetDevice(i);
        int ifaceNum = ipv4L3Proto->GetInterfaceForDevice(dev);
        iface2DevIdxMap[ifaceNum] = i;
    }
    for (uint32_t i = 0; i < globalRouting->GetNRoutes(); i++) {
        auto routeEntry = globalRouting->GetRoute(i);
        if (!routeEntry->IsHost()) {
            continue;
        }
        uint32_t dstIp = routeEntry->GetDest().Get();
        int ifaceNum = routeEntry->GetInterface();
        auto devIdx = iface2DevIdxMap[ifaceNum];
        NS_ASSERT_MSG(DynamicCast<PointToPointNetDevice>(GetDevice(devIdx)) != nullptr,
                        "NetDevice must be PointToPointNetDevice or its subclass");
        routeTable[dstIp].insert(devIdx);
    }
    for (const auto &[dstIp, egressSet] : routeTable) {
        m_routeTable[dstIp] = std::vector<int>{egressSet.begin(), egressSet.end()};
        NS_LOG_DEBUG(
            "[Switch " << GetId() << "] ns3::GlobalRouting for " << Ipv4Address{dstIp}
            << " = " << m_routeTable[dstIp]
        );
    }

    Node::DoInitialize();
}

static uint32_t EcmpHash(std::basic_string_view<uint8_t> keyData, uint32_t seed) {
    auto key = keyData.data();
    auto len = keyData.size();

/// https://github.com/alibaba-edu/High-Precision-Congestion-Control/blob/master/simulation/src/point-to-point/model/switch-node.cc#L138
    uint32_t h = seed;
    if (len > 3) {
        auto* key_x4 = (const uint32_t*) key;
        size_t i = len >> 2;
        do {
            uint32_t k = *key_x4++;
            k *= 0xcc9e2d51;
            k = (k << 15) | (k >> 17);
            k *= 0x1b873593;
            h ^= k;
            h = (h << 13) | (h >> 19);
            h += (h << 2) + 0xe6546b64;
        } while (--i);
        key = (const uint8_t*) key_x4;
    }
    if (len & 3) {
        size_t i = len & 3;
        uint32_t k = 0;
        key = &key[i - 1];
        do {
            k <<= 8;
            k |= *key--;
        } while (--i);
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
    }
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

int SwitchNode::GetEgressDevIndex(const ParsedPkt &parsedPkt) {
    FlowTuple tuple = parsedPkt.GetFlowTuple();
    auto &egressNetDevs = m_routeTable[tuple.dstAddr];
    if (egressNetDevs.size() == 1) {
        return egressNetDevs[0];
    }
    uint32_t idx = EcmpHash(tuple.GetTuple4Raw(), GetId()) % egressNetDevs.size();
    return egressNetDevs[idx];
}

Ptr<NetDevice> SwitchNode::GetEgressDev(const ParsedPkt &parsedPkt) {
    return GetDevice(GetEgressDevIndex(parsedPkt));
}

void SwitchNode::SendIpv4Packet(Ptr<NetDevice> dev, ParsedPkt parsedPkt) {
    Ptr<Packet> p = parsedPkt.payload;
    if (parsedPkt.tcpHdr != nullptr) {
        p->AddHeader(*parsedPkt.tcpHdr);
    } else if (parsedPkt.udpHdr != nullptr) {
        p->AddHeader(*parsedPkt.udpHdr);
    }
    parsedPkt.ipv4Hdr->SetPayloadSize(p->GetSize());
    auto tc = GetObject<TrafficControlLayer>();
    tc->Send(dev, Create<Ipv4QueueDiscItem>(p, dev->GetBroadcast(), Ipv4L3Protocol::PROT_NUMBER, *parsedPkt.ipv4Hdr));
}

void SwitchNode::SendIpv4Packet(ParsedPkt parsedPkt) {
    auto dev = GetEgressDev(parsedPkt);
    SendIpv4Packet(dev, std::move(parsedPkt));
}

bool SwitchNode::ReceiveFromDevice(Ptr<NetDevice> device, Ptr<const Packet> p,
                                    uint16_t protocol, const Address &from)
{
    if (protocol != Ipv4L3Protocol::PROT_NUMBER) {
        NS_LOG_ERROR("SwitchNode Recv Packet with non-ipv4 protol 0x" << std::hex << protocol << std::dec);
        return false;
    }
    auto parsedPkt = ParsedPkt::FromIpv4Pkt(p);
    ReceiveIpv4Packet(device, std::move(parsedPkt));
    return true;
}

void SwitchNode::ReceiveIpv4Packet(Ptr<NetDevice> inDev, ParsedPkt parsedPkt) {
    SendIpv4Packet(std::move(parsedPkt));
}
