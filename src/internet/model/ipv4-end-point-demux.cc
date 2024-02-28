/*
 * Copyright (c) 2005 INRIA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 */

#include "ipv4-end-point-demux.h"

#include "ipv4-end-point.h"
#include "ipv4-interface-address.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("Ipv4EndPointDemux");

static uint64_t
MakeTuple4Key(uint32_t localAddr, uint16_t localPort, uint32_t peerAddr, uint16_t peerPort) {
    return ((uint64_t)((localPort << 16) ^ localAddr) << 32) | ((peerPort << 16) ^ peerAddr);
}

static uint64_t
MakeTuple4Key(Ipv4EndPoint* endPoint) {
    uint32_t localAddr = endPoint->GetLocalAddress().Get();
    uint16_t localPort = endPoint->GetLocalPort();
    uint32_t peerAddr = endPoint->GetPeerAddress().Get();
    uint16_t peerPort = endPoint->GetPeerPort();
    return MakeTuple4Key(localAddr, localPort, peerAddr, peerPort);
}

Ipv4EndPointDemux::Ipv4EndPointDemux()
    : m_ephemeral(49152),
      m_portLast(65535),
      m_portFirst(49152)
{
    NS_LOG_FUNCTION(this);
}

Ipv4EndPointDemux::~Ipv4EndPointDemux()
{
    NS_LOG_FUNCTION(this);
    for (auto &[lport, slot] : m_lportEndPointsMap) {
        for (auto endP : slot) {
            delete endP;
        }
    }
    m_lportEndPointsMap.clear();
    m_tuple4EndPointsMap.clear();
}

bool
Ipv4EndPointDemux::LookupPortLocal(uint16_t port)
{
    NS_LOG_FUNCTION(this << port);
    return !m_lportEndPointsMap[port].empty();
}

bool
Ipv4EndPointDemux::LookupLocal(Ptr<NetDevice> boundNetDevice, Ipv4Address addr, uint16_t port)
{
    NS_LOG_FUNCTION(this << addr << port);
    for (Ipv4EndPoint* endP : m_lportEndPointsMap[port]) {
        if (endP->GetLocalAddress() == addr && endP->GetBoundNetDevice() == boundNetDevice) {
            return true;
        }
    }
    return false;
}

Ipv4EndPoint*
Ipv4EndPointDemux::Allocate()
{
    NS_LOG_FUNCTION(this);
    uint16_t port = AllocateEphemeralPort();
    if (port == 0)
    {
        NS_LOG_WARN("Ephemeral port allocation failed.");
        return nullptr;
    }
    auto endPoint = new Ipv4EndPoint(Ipv4Address::GetAny(), port);
    m_lportEndPointsMap[port].push_back(endPoint);
    return endPoint;
}

Ipv4EndPoint*
Ipv4EndPointDemux::Allocate(Ipv4Address address)
{
    NS_LOG_FUNCTION(this << address);
    uint16_t port = AllocateEphemeralPort();
    if (port == 0)
    {
        NS_LOG_WARN("Ephemeral port allocation failed.");
        return nullptr;
    }
    auto endPoint = new Ipv4EndPoint(address, port);
    m_lportEndPointsMap[port].push_back(endPoint);
    return endPoint;
}

Ipv4EndPoint*
Ipv4EndPointDemux::Allocate(Ptr<NetDevice> boundNetDevice, uint16_t port)
{
    NS_LOG_FUNCTION(this << port << boundNetDevice);

    return Allocate(boundNetDevice, Ipv4Address::GetAny(), port);
}

Ipv4EndPoint*
Ipv4EndPointDemux::Allocate(Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port)
{
    NS_LOG_FUNCTION(this << address << port << boundNetDevice);
    if (LookupLocal(boundNetDevice, address, port) || LookupLocal(nullptr, address, port))
    {
        NS_LOG_WARN("Duplicated endpoint.");
        return nullptr;
    }
    auto endPoint = new Ipv4EndPoint(address, port);
    m_lportEndPointsMap[port].push_back(endPoint);
    return endPoint;
}

Ipv4EndPoint*
Ipv4EndPointDemux::Allocate(Ptr<NetDevice> boundNetDevice,
                            Ipv4Address localAddress,
                            uint16_t localPort,
                            Ipv4Address peerAddress,
                            uint16_t peerPort)
{
    NS_LOG_FUNCTION(this << localAddress << localPort << peerAddress << peerPort << boundNetDevice);
    for (Ipv4EndPoint* endP : m_lportEndPointsMap[localPort]) {
        if (endP->GetLocalPort() == localPort && endP->GetLocalAddress() == localAddress
            && endP->GetPeerPort() == peerPort && endP->GetPeerAddress() == peerAddress
            && (endP->GetBoundNetDevice() == boundNetDevice || !endP->GetBoundNetDevice()))
        {
            NS_LOG_WARN("Duplicated endpoint.");
            return nullptr;
        }
    }
    auto endPoint = new Ipv4EndPoint(localAddress, localPort);
    endPoint->SetPeer(peerAddress, peerPort);
    m_lportEndPointsMap[localPort].push_back(endPoint);
    m_tuple4EndPointsMap[MakeTuple4Key(endPoint)].push_back(endPoint);
    return endPoint;
}

void
Ipv4EndPointDemux::DeAllocate(Ipv4EndPoint* endPoint)
{
    NS_LOG_FUNCTION(this << endPoint);
    EndPoints& lportSlot = m_lportEndPointsMap[endPoint->GetLocalPort()];
    for (auto i = lportSlot.begin(); i != lportSlot.end(); i++) {
        if (*i == endPoint) {
            lportSlot.erase(i);
            break;
        }
    }

    auto tuple4SlotIt = m_tuple4EndPointsMap.find(MakeTuple4Key(endPoint));
    if (tuple4SlotIt != m_tuple4EndPointsMap.end()) {
        EndPoints& slot = tuple4SlotIt->second;
        for (auto i = slot.begin(); i != slot.end(); i++) {
            if (*i == endPoint) {
                slot.erase(i);
                break;
            }
        }
        if (slot.empty()) {
            m_tuple4EndPointsMap.erase(tuple4SlotIt);
        }
    }

    delete endPoint;
}

/*
 * If we have an exact match, we return it.
 * Otherwise, if we find a generic match, we return it.
 * Otherwise, we return 0.
 */
Ipv4EndPointDemux::EndPoints
Ipv4EndPointDemux::Lookup(Ipv4Address daddr,
                          uint16_t dport,
                          Ipv4Address saddr,
                          uint16_t sport,
                          Ptr<Ipv4Interface> incomingInterface)
{
    NS_LOG_FUNCTION(this << daddr << dport << saddr << sport << incomingInterface);

    EndPoints retval1; // Matches exact on local port, wildcards on others
    EndPoints retval2; // Matches exact on local port/adder, wildcards on others
    EndPoints retval3; // Matches all but local address
    EndPoints retval4; // Exact match on all 4

    auto tuple4Key = MakeTuple4Key(daddr.Get(), dport, saddr.Get(), sport);
    if (auto slotIt = m_tuple4EndPointsMap.find(tuple4Key); slotIt != m_tuple4EndPointsMap.end()) {
        for (Ipv4EndPoint* endP : slotIt->second) {
            if (!endP->IsRxEnabled()) {
                continue;
            }
            Ptr<NetDevice> dev = endP->GetBoundNetDevice();
            if (dev && dev != incomingInterface->GetDevice()) {
                continue;
            }

            EndPoints ret;
            ret.push_back(endP);
            return ret;
        }
    }

    for (Ipv4EndPoint* endP : m_lportEndPointsMap[dport]) {
        if (!endP->IsRxEnabled()) {
            continue;
        }
        Ptr<NetDevice> dev = endP->GetBoundNetDevice();
        if (dev && dev != incomingInterface->GetDevice()) {
            continue;
        }

        bool localAddressMatchesExact = false;
        bool localAddressIsAny = false;
        bool localAddressIsSubnetAny = false;
        
        // We have 3 cases:
        // 1) Exact local / destination address match
        // 2) Local endpoint bound to Any -> matches anything
        // 3) Local endpoint bound to x.y.z.0 -> matches Subnet-directed broadcast packet (e.g.,
        // x.y.z.255 in a /24 net) and direct destination match.

        if (endP->GetLocalAddress() == daddr)
        {
            // Case 1:
            localAddressMatchesExact = true;
        }
        else if (endP->GetLocalAddress() == Ipv4Address::GetAny())
        {
            // Case 2:
            localAddressIsAny = true;
        }
        else
        {
            // Case 3:
            for (uint32_t i = 0; i < incomingInterface->GetNAddresses(); i++)
            {
                Ipv4InterfaceAddress addr = incomingInterface->GetAddress(i);

                Ipv4Address addrNetpart = addr.GetLocal().CombineMask(addr.GetMask());
                if (endP->GetLocalAddress() == addrNetpart)
                {
                    NS_LOG_LOGIC("Endpoint is SubnetDirectedAny "
                                 << endP->GetLocalAddress() << "/"
                                 << addr.GetMask().GetPrefixLength());

                    Ipv4Address daddrNetPart = daddr.CombineMask(addr.GetMask());
                    if (addrNetpart == daddrNetPart)
                    {
                        localAddressIsSubnetAny = true;
                    }
                }
            }

            // if no match here, keep looking
            if (!localAddressIsSubnetAny)
            {
                continue;
            }
        }

        bool remotePortMatchesExact = endP->GetPeerPort() == sport;
        bool remotePortMatchesWildCard = endP->GetPeerPort() == 0;
        bool remoteAddressMatchesExact = endP->GetPeerAddress() == saddr;
        bool remoteAddressMatchesWildCard = endP->GetPeerAddress() == Ipv4Address::GetAny();
        // If remote does not match either with exact or wildcard,
        // skip this one
        if (!(remotePortMatchesExact || remotePortMatchesWildCard))
        {
            continue;
        }
        if (!(remoteAddressMatchesExact || remoteAddressMatchesWildCard))
        {
            continue;
        }

        bool localAddressMatchesWildCard = localAddressIsAny || localAddressIsSubnetAny;

        if (localAddressMatchesExact && remoteAddressMatchesExact && remotePortMatchesExact)
        { // All 4 match - this is the case of an open TCP connection, for example.
            NS_LOG_LOGIC("Found an endpoint for case 4, adding " << endP->GetLocalAddress() << ":"
                                                                 << endP->GetLocalPort());
            retval4.push_back(endP);
        }
        if (localAddressMatchesWildCard && remoteAddressMatchesExact && remotePortMatchesExact)
        { // All but local address - no idea what this case could be.
            NS_LOG_LOGIC("Found an endpoint for case 3, adding " << endP->GetLocalAddress() << ":"
                                                                 << endP->GetLocalPort());
            retval3.push_back(endP);
        }
        if (localAddressMatchesExact && remoteAddressMatchesWildCard && remotePortMatchesWildCard)
        { // Only local port and local address matches exactly - Not yet opened connection
            NS_LOG_LOGIC("Found an endpoint for case 2, adding " << endP->GetLocalAddress() << ":"
                                                                 << endP->GetLocalPort());
            retval2.push_back(endP);
        }
        if (localAddressMatchesWildCard && remoteAddressMatchesWildCard &&
            remotePortMatchesWildCard)
        { // Only local port matches exactly - Endpoint open to "any" connection
            NS_LOG_LOGIC("Found an endpoint for case 1, adding " << endP->GetLocalAddress() << ":"
                                                                 << endP->GetLocalPort());
            retval1.push_back(endP);
        }
    }

    // Here we find the most exact match
    EndPoints retval;
    if (!retval4.empty())
    {
        retval = retval4;
        Ipv4EndPoint* endP = retval.front();
        m_tuple4EndPointsMap[MakeTuple4Key(endP)].push_back(endP);
    }
    else if (!retval3.empty())
    {
        retval = retval3;
    }
    else if (!retval2.empty())
    {
        retval = retval2;
    }
    else
    {
        retval = retval1;
    }

    NS_ABORT_MSG_IF(retval.size() > 1,
                    "Too many endpoints - perhaps you created too many sockets without binding "
                    "them to different NetDevices.");
    return retval; // might be empty if no matches
}

Ipv4EndPoint*
Ipv4EndPointDemux::SimpleLookup(Ipv4Address daddr,
                                uint16_t dport,
                                Ipv4Address saddr,
                                uint16_t sport)
{
    NS_LOG_FUNCTION(this << daddr << dport << saddr << sport);

    // this code is a copy/paste version of an old BSD ip stack lookup
    // function.
    uint32_t genericity = 3;
    Ipv4EndPoint* generic = nullptr;
    for (Ipv4EndPoint* endP : m_lportEndPointsMap[dport]) {
        if (endP->GetLocalAddress() == daddr
            && endP->GetPeerPort() == sport
            && endP->GetPeerAddress() == saddr)
        {
            /* this is an exact match. */
            return endP;
        }

        uint32_t tmp = 0;
        if (endP->GetLocalAddress() == Ipv4Address::GetAny()) {
            tmp++;
        }
        if (endP->GetPeerAddress() == Ipv4Address::GetAny()) {
            tmp++;
        }
        if (tmp < genericity) {
            generic = endP;
            genericity = tmp;
        }
    }
    return generic;
}

uint16_t
Ipv4EndPointDemux::AllocateEphemeralPort()
{
    // Similar to counting up logic in netinet/in_pcb.c
    NS_LOG_FUNCTION(this);
    uint16_t port = m_ephemeral;
    int count = m_portLast - m_portFirst;
    do
    {
        if (count-- < 0)
        {
            return 0;
        }
        ++port;
        if (port < m_portFirst || port > m_portLast)
        {
            port = m_portFirst;
        }
    } while (LookupPortLocal(port));
    m_ephemeral = port;
    return port;
}

} // namespace ns3