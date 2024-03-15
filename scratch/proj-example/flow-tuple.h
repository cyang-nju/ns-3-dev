#pragma once
#include <map>
#include <tuple>
#include <string_view>
#include "ns3/tcp-l4-protocol.h"
#include "ns3/udp-l4-protocol.h"
using namespace ns3;

using data_view = std::basic_string_view<uint8_t>;

struct FlowTuple {
    uint32_t srcAddr = 0;
    uint32_t dstAddr = 0;
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint8_t proto = 0;

    data_view GetTuple4Raw() const {
        return data_view{reinterpret_cast<const uint8_t*>(this), 12};
    }

    data_view GetTuple5Raw() const {
        return data_view{reinterpret_cast<const uint8_t*>(this), 13};
    }

    void SetSrc(const Ipv4Address &addr, uint16_t port) {
        srcAddr = addr.Get();
        srcPort = port;
    }

    void SetDst(const Ipv4Address &addr, uint16_t port) {
        dstAddr = addr.Get();
        dstPort = port;
    }

    void SwapDirection() {
        std::swap(srcAddr, dstAddr);
        std::swap(srcPort, dstPort);
    }

    bool operator == (const FlowTuple &another) const {
        return proto == another.proto
            && srcAddr == another.srcAddr && dstAddr == another.dstAddr
            && srcPort == another.srcPort && dstPort == another.dstPort;
    }

    bool operator != (const FlowTuple &another) const { return !(*this == another); }

    /// for potential use of std::map<FlowTuple>
    bool operator < (const FlowTuple &b) const {
        auto left = std::tuple{proto, srcAddr, dstAddr, srcPort, dstPort};
        auto right = std::tuple{b.proto, b.srcAddr, b.dstAddr, b.srcPort, b.dstPort};
        return left < right;
    }
};

inline std::ostream& operator<< (std::ostream &out, const FlowTuple &flow) {
    static std::map<uint8_t, const char*> protStrMap{
        {TcpL4Protocol::PROT_NUMBER, "TCP"},
        {UdpL4Protocol::PROT_NUMBER, "UDP"},
        {1, "ICMP"}
    };
    if (flow.proto != 0) {
        if (protStrMap.count(flow.proto) != 0) {
            out << '(' << protStrMap[flow.proto] << ')';
        } else {
            out << "(UnknownProto " << (int)flow.proto << ')';
        }
    }
    out << Ipv4Address{flow.srcAddr} << ':' << flow.srcPort
        << "->"
        << Ipv4Address{flow.dstAddr} << ':' << flow.dstPort
    ;
    return out;
}
