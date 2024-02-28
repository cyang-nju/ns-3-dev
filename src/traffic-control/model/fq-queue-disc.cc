/*
 * Copyright (c) 2017 Universita' degli Studi di Napoli Federico II
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
 * Authors:  Stefano Avallone <stavallo@unina.it>
 */

#include "fq-queue-disc.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/tcp-socket-base.h"
#include "queue-disc.h"
#include <array>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FqQueueDisc");

NS_OBJECT_ENSURE_REGISTERED(FqQueueDisc);

class FqFlow
{
public:
    std::multimap<Time, Ptr<QueueDiscItem>> m_itemsMap;
    std::list<Ptr<QueueDiscItem>> m_itemsList;

    bool m_detached;
    Time m_age;
    uintptr_t sk;
    int m_qlen{0};
    int m_credit;
    Time timeNextPacket{0};
    

    void Add(Ptr<QueueDiscItem> item);
    Ptr<QueueDiscItem> Peek();
    void EraseHead(Ptr<QueueDiscItem> item); // item must be nullptr
};

void FqFlow::Add(Ptr<QueueDiscItem> item) {
    Time txTime = item->GetPacket()->GetTxTime();
    if (m_itemsList.empty() || txTime >= m_itemsList.back()->GetPacket()->GetTxTime()) {
        m_itemsList.push_back(item);
        return;
    }

    m_itemsMap.insert({txTime, item});
}

Ptr<QueueDiscItem> FqFlow::Peek() {
    if (m_itemsMap.empty()) {
        return m_itemsList.empty() ? nullptr : m_itemsList.front();
    }
    if (m_itemsList.empty()) {
        return m_itemsMap.begin()->second;
    }

    Ptr<QueueDiscItem> item1 = m_itemsList.front();
    Ptr<QueueDiscItem> item2 = m_itemsMap.begin()->second;
    if (item1->GetPacket()->GetTxTime() < item2->GetPacket()->GetTxTime()) {
        return item1;
    } else {
        return item2;
    }
}

void FqFlow::EraseHead(Ptr<QueueDiscItem> item) {
    if (!m_itemsList.empty() && item == m_itemsList.front()) {
        m_itemsList.pop_front();
    } else {
        m_itemsMap.erase(m_itemsMap.begin());
    }
}



constexpr uint64_t RATE_BPS_MAX = (1ULL << 63) - 1;

TypeId
FqQueueDisc::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::FqQueueDisc")
            .SetParent<QueueDisc>()
            .SetGroupName("TrafficControl")
            .AddConstructor<FqQueueDisc>()
            .AddAttribute("MaxSize",
                          "The max queue size",
                          QueueSizeValue(QueueSize("10000p")),
                          MakeQueueSizeAccessor(&QueueDisc::SetMaxSize, &QueueDisc::GetMaxSize),
                          MakeQueueSizeChecker())
            .AddAttribute("Quantum",
                          "Quantum",
                          UintegerValue(2 * 1500),
                          MakeUintegerAccessor(&FqQueueDisc::m_quantum),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("InitialQuantum",
                          "Initial quantum of flow",
                          UintegerValue(10 * 1500),
                          MakeUintegerAccessor(&FqQueueDisc::m_initialQuantum),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("FlowRefillDelay",
                          "Flow refill delay",
                          TimeValue(MilliSeconds(40)),
                          MakeTimeAccessor(&FqQueueDisc::m_flowRefillDelay),
                          MakeTimeChecker())
            .AddAttribute("FlowPktLimit",
                          "Packet Limit of a flow",
                          UintegerValue(100),
                          MakeUintegerAccessor(&FqQueueDisc::m_flowPktLimit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("FlowMaxRate",
                          "Optional max rate per flow",
                          DataRateValue(DataRate{RATE_BPS_MAX}),
                          MakeDataRateAccessor(&FqQueueDisc::m_flowMaxRate),
                          MakeDataRateChecker())
            .AddAttribute("CeThreshold",
                          "Threshold to mark packets",
                          TimeValue(Time::Max()),
                          MakeTimeAccessor(&FqQueueDisc::m_ceThreshold),
                          MakeTimeChecker())
            .AddAttribute("Horizon",
                          "horizon",
                          TimeValue(Seconds(10)),
                          MakeTimeAccessor(&FqQueueDisc::m_horizon),
                          MakeTimeChecker())
            .AddAttribute("HorizonDrop",
                          "Enable (true) or disable (false) drop of packet beyond horizon",
                          BooleanValue(true),
                          MakeBooleanAccessor(&FqQueueDisc::m_horizonDrop),
                          MakeBooleanChecker())
            .AddAttribute("RateEnable",
                          "Enable (true) or disable (false) drop of packet beyond horizon",
                          BooleanValue(true),
                          MakeBooleanAccessor(&FqQueueDisc::m_rateEnable),
                          MakeBooleanChecker())
    ;
    return tid;
}

FqQueueDisc::FqQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{
}

FqQueueDisc::~FqQueueDisc()
{
    m_newFlows.clear();
    m_oldFlows.clear();
    m_delayedFlows.clear();
    for (auto& root : m_fqRoot) {
        for (auto [key, flow] : root) {
            delete flow;
        }
        root.clear();
    }
}


static uint32_t HashPtr(uintptr_t val, uint32_t bits) {
    if constexpr (std::is_same_v<uintptr_t, uint64_t>) {
        return static_cast<uint32_t>((val * 0x61C8864680B583EBull) >> (64 - bits));
    } else {
        return (val * 0x61C88647) >> (32 - bits);
    }
}

FqFlow*
FqQueueDisc::Classify(Ptr<QueueDiscItem> item)
{
    Ptr<Packet> pkt =  item->GetPacket();
    Ptr<Socket> sock = pkt->GetSocket();
    auto sk = reinterpret_cast<uintptr_t>(PeekPointer(sock));
    if (sk == 0) {
        sk = ((uintptr_t)item->Hash() << 1) | 1;
    }

    auto& root = m_fqRoot[HashPtr(sk, 10)];
    if (m_flows >= 2 * (int)m_fqRoot.size() && m_inactiveFlows > m_flows/2) {
        GarbageCollection(root, sk);
    }
    auto it = root.find(sk);
    if (it != root.end()) {
        return it->second;
    }

    auto flow = new FqFlow{};
    flow->m_detached = true;
    flow->m_age = Simulator::Now();
    flow->sk = sk;
    if (auto tcpSock = DynamicCast<TcpSocketBase>(sock); tcpSock != nullptr) {
        tcpSock->SetFqPacing();
    }

    flow->m_credit = m_initialQuantum;
    root[sk] = flow;
    m_flows++;
    m_inactiveFlows++;
    return flow;
}

bool
FqQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item)
{
    if (GetCurrentSize() + item > GetMaxSize())
    {
        DropBeforeEnqueue(item, LIMIT_EXCEEDED_DROP);
        std::cout << "Fq enqueue unexpected drop: limit\n";
        return false;
    }

    Time now = Simulator::Now();
    Ptr<Packet> pkt =  item->GetPacket();
    Time txTime = pkt->GetTxTime();
    if (txTime.IsZero()) {
        pkt->SetTxTime(now);
    } else {
        if (txTime > now + m_horizon) {
            if (m_horizonDrop) {
                DropBeforeEnqueue(item, "Packet beyond horizon");
                std::cout << "Fq enqueue unexpected drop: horizon\n";
                return false;
            }
            pkt->SetTxTime(now + m_horizon);
        }
    }

    FqFlow* flow = Classify(item);
    if (flow->m_qlen >= (int)m_flowPktLimit) {
        DropBeforeEnqueue(item, LIMIT_EXCEEDED_DROP);
        return false;
    }

    flow->m_qlen++;
    PacketEnqueued(item);
    if (flow->m_detached) {
        m_newFlows.push_back(flow);
        flow->m_detached = false;
        if (now > flow->m_age + m_flowRefillDelay) {
            flow->m_credit = std::max(flow->m_credit, (int)m_quantum);
        }
        m_inactiveFlows--;
        
    }
    flow->Add(item);
    return true;
}

void
FqQueueDisc::CheckThrottled()
{
    Time now = Simulator::Now();
    if (m_timeNextDelayedFlow > now) {
        return;
    }

    Time sample = now - m_timeNextDelayedFlow;
    m_unthrottleLatency += (sample - m_unthrottleLatency) / 8;
    m_timeNextDelayedFlow = Time::Max();
    while (!m_delayedFlows.empty()) {
        FqFlow* flow = m_delayedFlows.begin()->second;
        if (flow->timeNextPacket > now) {
            m_timeNextDelayedFlow = flow->timeNextPacket;
            break;
        }
        m_delayedFlows.erase(m_delayedFlows.begin());
        m_throttledFlows--;
        m_oldFlows.push_back(flow);
    }
}

Ptr<QueueDiscItem>
FqQueueDisc::DoDequeue()
{
    if (GetCurrentSize().GetValue() == 0) {
        return nullptr;
    }

    // TODO: internal queue

    Time now = Simulator::Now();
    CheckThrottled();

    Ptr<QueueDiscItem> item;
    FqFlow* flow;
    while (true) {
        auto flowList = &m_newFlows;
        if (m_newFlows.empty()) {
            flowList = &m_oldFlows;
            if (m_oldFlows.empty()) {
                if (m_timeNextDelayedFlow != Time::Max()) {
                    m_sheduleEvent = Simulator::Schedule(m_timeNextDelayedFlow,
                                                         &QueueDisc::Run, this);
                }
                if (GetCurrentSize().GetValue() > 0) {
                    std::cout << "Fq unexpected null dequeue\n";
                }
                return nullptr;
            }
        }
        
        flow = flowList->front();
        if (flow->m_credit <= 0) {
            flow->m_credit += m_quantum;
            flowList->pop_front();
            m_oldFlows.push_back(flow);
            continue;
        }

        item = flow->Peek();
        if (item != nullptr) {
            Time timeNextPacket = std::max(item->GetPacket()->GetTxTime(), flow->timeNextPacket);
            if (now < timeNextPacket) {
                flowList->pop_front();
                flow->timeNextPacket = timeNextPacket;
                m_delayedFlows.insert({timeNextPacket, flow});
                m_throttledFlows++;
                m_timeNextDelayedFlow = std::min(m_timeNextDelayedFlow, timeNextPacket);
                continue;
            }
            if (now - timeNextPacket > m_ceThreshold) {
                std::cout << "Fq Mark\n";
                Mark(item, "Queuing time beyond threshold mark");
            }
            flow->EraseHead(item);
            flow->m_qlen--;
            PacketDequeued(item);
            break;
        } else {
            flowList->pop_front();
            if (flowList == (&m_newFlows) && !m_oldFlows.empty()) {
                m_oldFlows.push_back(flow);
            } else {
                flow->m_detached = true;
                m_inactiveFlows++;
            }
            continue;
        }
    }

    item->GetPacket()->TakeTxTime();
    flow->m_credit -= item->GetSize();

    if (!m_rateEnable) {    
        return item;
    }

    DataRate rate = m_flowMaxRate;

	/* TODO: (currently it is fine because TcpSocketBase send packet with ts)
     * If EDT time was provided for this skb, we need to
	 * update f->time_next_packet only if this qdisc enforces
	 * a flow max rate.
	 */

    if (rate.GetBitRate() != RATE_BPS_MAX) {
        Time t = rate.CalculateBytesTxTime(item->GetSize());
        // Since socket rate can change later, clamp the delay to 1 second.
        if (t > Seconds(1)) {
            t = Seconds(1);
        }
        if (!flow->timeNextPacket.IsZero()) {
            t -= std::min(t/2, now - flow->timeNextPacket);
        }
        flow->timeNextPacket = now + t;
    }

    return item;
}

void
FqQueueDisc::GarbageCollection(FlowTree_t &root, uintptr_t sk)
{
    constexpr int FQ_GC_MAX = 8;
    const Time FQ_GC_AGE = MilliSeconds(12);

    std::vector<FlowTree_t::iterator> toFree;
    for (auto it = root.begin(); it != root.end(); it++) {
        FqFlow *flow = it->second;
        if (flow->sk == sk) {
            break;
        }

        if (flow->m_detached && Simulator::Now() > flow->m_age + FQ_GC_AGE) {
            toFree.push_back(it);
            if (toFree.size() >= FQ_GC_MAX) {
                break;
            }
        }
    }

    for (const auto &it : toFree) {
        delete it->second;
        root.erase(it);
    }

    int cnt = toFree.size();
    m_flows -= cnt;
    m_inactiveFlows -= cnt;
}

bool
FqQueueDisc::CheckConfig()
{
    if (GetNQueueDiscClasses() > 0)
    {
        NS_LOG_ERROR("FqQueueDisc cannot have classes");
        return false;
    }

    if (GetNPacketFilters() > 0)
    {
        NS_LOG_ERROR("FqQueueDisc needs no packet filter");
        return false;
    }

    if (GetNInternalQueues() == 0)
    {
        // add a DropTail queue
        AddInternalQueue(
            CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>>("MaxSize",
                                                                     QueueSizeValue(GetMaxSize())));
    }

    if (GetNInternalQueues() != 1)
    {
        NS_LOG_ERROR("FqQueueDisc needs 1 internal queue");
        return false;
    }

    return true;
}

void
FqQueueDisc::InitializeParams()
{
}

} // namespace ns3
