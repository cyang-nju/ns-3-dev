#pragma once
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/net-device.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/tcp-header.h"
#include "flow-tuple.h"
#include "utils.h"

struct ParsedPkt {
    unique_ptr<Ipv4Header> ipv4Hdr;
    unique_ptr<TcpHeader> tcpHdr;
    unique_ptr<UdpHeader> udpHdr;
    Ptr<Packet> payload;

    static ParsedPkt FromIpv4Pkt(Ptr<const Packet> packet);
    FlowTuple GetFlowTuple() const;
};

/**
 * This class implements per-flow ECMP and provides virtual methods for subclasses to process packet.
 * Note: all associated NetDevice MUST be PointToPointNetDevice
 */
class SwitchNode : public Node
{
public:
    static TypeId GetTypeId();
    SwitchNode() = default;

protected:
    void DoInitialize() override;
    int GetEgressDevIndex(const ParsedPkt &parsedPkt); // returns ECMP calculated egress port
    Ptr<NetDevice> GetEgressDev(const ParsedPkt &parsedPkt); // returns ECMP calculated egress port
    void SendIpv4Packet(Ptr<NetDevice> egressNetDev, ParsedPkt parsedPkt);
    void SendIpv4Packet(ParsedPkt parsedPkt);
    virtual void ReceiveIpv4Packet(Ptr<NetDevice> inDev, ParsedPkt parsedPkt);

private:
    bool ReceiveFromDevice(Ptr<NetDevice> device, Ptr<const Packet> packet, uint16_t protocol, const Address &from);

private:
    std::map<uint32_t, std::vector<int>> m_routeTable;
};
