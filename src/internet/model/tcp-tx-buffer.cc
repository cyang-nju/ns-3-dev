/*
 * Copyright (c) 2010-2015 Adrian Sai-wah Tam
 * Copyright (c) 2016 Natale Patriciello <natale.patriciello@gmail.com>
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
 * Original author: Adrian Sai-wah Tam <adrian.sw.tam@gmail.com>
 */

#include "tcp-tx-buffer.h"

#include "ns3/abort.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <iostream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpTxBuffer");
NS_OBJECT_ENSURE_REGISTERED(TcpTxBuffer);

Callback<void, TcpTxItem*> TcpTxBuffer::m_nullCb = MakeNullCallback<void, TcpTxItem*>();

TypeId
TcpTxBuffer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TcpTxBuffer")
                            .SetParent<Object>()
                            .SetGroupName("Internet")
                            .AddConstructor<TcpTxBuffer>()
                            .AddTraceSource("UnackSequence",
                                            "First unacknowledged sequence number (SND.UNA)",
                                            MakeTraceSourceAccessor(&TcpTxBuffer::m_firstByteSeq),
                                            "ns3::SequenceNumber32TracedValueCallback");
    return tid;
}

/* A user is supposed to create a TcpSocket through a factory. In TcpSocket,
 * there are attributes SndBufSize and RcvBufSize to control the default Tx and
 * Rx window sizes respectively, with default of 128 KiByte. The attribute
 * SndBufSize is passed to TcpTxBuffer by TcpSocketBase::SetSndBufSize() and in
 * turn, TcpTxBuffer:SetMaxBufferSize(). Therefore, the m_maxBuffer value
 * initialized below is insignificant.
 */
TcpTxBuffer::TcpTxBuffer(uint32_t n)
    : m_maxBuffer(32768),
      m_size(0),
      m_sentSize(0),
      m_firstByteSeq(n)
{
    m_rWndCallback = MakeNullCallback<uint32_t>();
}

TcpTxBuffer::~TcpTxBuffer()
{
    for (auto it = m_sentBuf.begin(); it != m_sentBuf.end(); ++it)
    {
        TcpTxItem* item = it->second;
        m_sentSize -= item->m_packet->GetSize();
        delete item;
    }

    for (auto it = m_appList.begin(); it != m_appList.end(); ++it)
    {
        TcpTxItem* item = *it;
        m_size -= item->m_packet->GetSize();
        delete item;
    }
}

SequenceNumber32
TcpTxBuffer::HeadSequence() const
{
    return m_firstByteSeq;
}

SequenceNumber32
TcpTxBuffer::TailSequence() const
{
    return m_firstByteSeq + SequenceNumber32(m_size);
}

uint32_t
TcpTxBuffer::Size() const
{
    return m_size;
}

uint32_t
TcpTxBuffer::MaxBufferSize() const
{
    return m_maxBuffer;
}

void
TcpTxBuffer::SetMaxBufferSize(uint32_t n)
{
    m_maxBuffer = n;
}

bool
TcpTxBuffer::IsSackEnabled() const
{
    return m_sackEnabled;
}

void
TcpTxBuffer::SetSackEnabled(bool enabled)
{
    m_sackEnabled = enabled;
}

uint32_t
TcpTxBuffer::Available() const
{
    return m_maxBuffer - m_size;
}

void
TcpTxBuffer::SetDupAckThresh(uint32_t dupAckThresh)
{
    m_dupAckThresh = dupAckThresh;
}

void
TcpTxBuffer::SetSegmentSize(uint32_t segmentSize)
{
    m_segmentSize = segmentSize;
}

uint32_t
TcpTxBuffer::GetRetransmitsCount() const
{
    return m_retrans;
}

uint64_t
TcpTxBuffer::GetTotalLost() const
{
    return m_totalLost;
}

uint64_t
TcpTxBuffer::GetTotalRetrans() const
{
    return m_totalRetrans;
}


uint32_t
TcpTxBuffer::GetLost() const
{
    return m_lostOut;
}

uint32_t
TcpTxBuffer::GetSacked() const
{
    return m_sackedOut;
}

void
TcpTxBuffer::SetHeadSequence(const SequenceNumber32& seq)
{
    NS_LOG_FUNCTION(this << seq);
    m_firstByteSeq = seq;

    // if you change the head with data already sent, something bad will happen
    NS_ASSERT(m_sentBuf.empty());
    m_highestSackIter = m_sentBuf.end();
    m_nextSegLostHint = m_sentBuf.end();
}

uint32_t
TcpTxBuffer::GetSentSize() const
{
    return m_sentSize;
}

const TcpTxItem*
TcpTxBuffer::GetHeadItem()
{
    if (m_sentBuf.empty())
    {
        return nullptr;
    }
    else
    {
        return m_sentBuf.begin()->second;
    }
}

bool
TcpTxBuffer::Add(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    NS_LOG_LOGIC("Try to append " << p->GetSize() << " bytes to window starting at "
                                  << m_firstByteSeq << ", availSize=" << Available());
    if (p->GetSize() <= Available())
    {
        if (p->GetSize() > 0)
        {
            auto item = new TcpTxItem();
            item->m_packet = p->Copy();
            m_appList.insert(m_appList.end(), item);
            m_size += p->GetSize();

            NS_LOG_LOGIC("Updated size=" << m_size << ", lastSeq="
                                         << m_firstByteSeq + SequenceNumber32(m_size));
        }
        return true;
    }
    NS_LOG_LOGIC("Rejected. Not enough room to buffer packet.");
    return false;
}

uint32_t
TcpTxBuffer::SizeFromSequence(const SequenceNumber32& seq) const
{
    NS_LOG_FUNCTION(this << seq);
    // Sequence of last byte in buffer
    SequenceNumber32 lastSeq = TailSequence();

    if (lastSeq >= seq)
    {
        return static_cast<uint32_t>(lastSeq - seq);
    }

    NS_LOG_ERROR("Requested a sequence beyond our space (" << seq << " > " << lastSeq
                                                           << "). Returning 0 for convenience.");
    return 0;
}

TcpTxItem*
TcpTxBuffer::CopyFromSequence(uint32_t numBytes, const SequenceNumber32& seq)
{
    NS_LOG_FUNCTION(this << numBytes << seq);

    NS_ABORT_MSG_IF(m_firstByteSeq > seq,
                    "Requested a sequence number which is not in the buffer anymore");
    ConsistencyCheck();

    // Real size to extract. Insure not beyond end of data
    uint32_t s = std::min(numBytes, SizeFromSequence(seq));

    if (s == 0)
    {
        return nullptr;
    }

    TcpTxItem* outItem = nullptr;

    if (m_firstByteSeq + m_sentSize >= seq + s)
    {
        // already sent this block completely
        outItem = GetTransmittedSegment(s, seq);
        NS_ASSERT(outItem != nullptr);
        NS_ASSERT(!outItem->m_sacked);

        NS_LOG_DEBUG("Returning already sent item " << *outItem << " from " << *this);
    }
    else if (m_firstByteSeq + m_sentSize <= seq)
    {
        NS_ABORT_MSG_UNLESS(m_firstByteSeq + m_sentSize == seq,
                            "Requesting a piece of new data with an hole");

        // this is the first time we transmit this block
        outItem = GetNewSegment(s);
        NS_ASSERT(outItem != nullptr);
        NS_ASSERT(outItem->m_retrans == false);

        NS_LOG_DEBUG("Returning new item " << *outItem << " from " << *this);
    }
    else if (m_firstByteSeq.Get().GetValue() + m_sentSize > seq.GetValue() &&
             m_firstByteSeq.Get().GetValue() + m_sentSize < seq.GetValue() + s)
    {
        // Partial: a part is retransmission, the remaining data is new
        // Just return the old segment, without taking new data. Hopefully
        // TcpSocketBase will request new data

        uint32_t amount = (m_firstByteSeq.Get().GetValue() + m_sentSize) - seq.GetValue();

        return CopyFromSequence(amount, seq);
    }

    outItem->m_lastSent = Simulator::Now();
    if (outItem->m_tsortedAnchor.has_value()) {
        m_tsortedItemList.erase(outItem->m_tsortedAnchor.value());
    }
    m_tsortedItemList.push_back(outItem);
    outItem->m_tsortedAnchor = std::prev(m_tsortedItemList.end());
    NS_ASSERT_MSG(outItem->m_startSeq >= m_firstByteSeq,
                  "Returning an item " << *outItem << " with SND.UNA as " << m_firstByteSeq);
    ConsistencyCheck();
    return outItem;
}

TcpTxItem*
TcpTxBuffer::GetNewSegment(uint32_t numBytes)
{
    NS_LOG_FUNCTION(this << numBytes);

    SequenceNumber32 startOfAppList = m_firstByteSeq + m_sentSize;

    NS_LOG_INFO("AppList start at " << startOfAppList << ", sentSize = " << m_sentSize
                                    << " firstByte: " << m_firstByteSeq);

    NS_ASSERT(!m_appList.empty());

    TcpTxItem* item = *(m_appList.begin());
    while (item->m_packet->GetSize() < numBytes)
    {
        auto it = std::next(m_appList.begin());
        if (it == m_appList.end())
        {
            break;
        }
        TcpTxItem* nextItem = *it;
        MergeItems(item, nextItem);
        m_appList.erase(it);
        delete nextItem;
    }
    if (numBytes < item->m_packet->GetSize())
    {
        auto* firstPart = new TcpTxItem();
        SplitItems(firstPart, item, numBytes);
        item = firstPart;
    }
    else
    {
        m_appList.pop_front();
    }

    item->m_startSeq = startOfAppList;
    m_sentBuf.emplace_hint(m_sentBuf.end(), startOfAppList, item);
    m_sentSize += item->m_packet->GetSize();
    return item;
}

TcpTxItem*
TcpTxBuffer::GetTransmittedSegment(uint32_t numBytes, const SequenceNumber32& seq)
{
    NS_LOG_FUNCTION(this << numBytes << seq);
    NS_ASSERT(seq >= m_firstByteSeq);
    NS_ASSERT(numBytes <= m_sentSize);
    NS_ASSERT(!m_sentBuf.empty());

    uint32_t s = numBytes;
    auto it = m_sentBuf.find(seq);
    if (it != m_sentBuf.end())
    {
        TcpTxItem* currItem = it->second;
        auto next = std::next(it);
        if (next != m_sentBuf.end())
        {
            // Next is not sacked and have the same value for m_lost ... there is the
            // possibility to merge
            TcpTxItem* nextItem = next->second;
            if ((!nextItem->m_sacked) && (currItem->m_lost == nextItem->m_lost))
            {
                s = std::min(s, currItem->m_packet->GetSize() + nextItem->m_packet->GetSize());
            }
            else
            {
                // Next is sacked... better to retransmit only the first segment
                s = std::min(s, currItem->m_packet->GetSize());
            }
        }
        else
        {
            s = std::min(s, currItem->m_packet->GetSize());
        }
    }

    TcpTxItem* item = GetPacketFromSentBuf(s, seq);

    if (!item->m_retrans)
    {
        m_retrans += item->m_packet->GetSize();
        item->m_retrans = true;
    }

    m_totalRetrans += item->m_packet->GetSize();

    return item;
}

void
TcpTxBuffer::SplitItems(TcpTxItem* t1, TcpTxItem* t2, uint32_t size)
{
    NS_ASSERT(t1 != nullptr && t2 != nullptr);
    NS_LOG_FUNCTION(this << *t2 << size);

    t1->m_packet = t2->m_packet->CreateFragment(0, size);
    t2->m_packet->RemoveAtStart(size);

    t1->m_startSeq = t2->m_startSeq;
    t1->m_sacked = t2->m_sacked;
    t1->m_lastSent = t2->m_lastSent;
    t1->m_retrans = t2->m_retrans;
    t1->m_lost = t2->m_lost;
    t1->m_rttNotReliable = t2->m_rttNotReliable;
    t2->m_startSeq += size;

    if (t1->m_sacked) {
        m_sackedPkts++;
    }

    if (m_nextSegLostHint != m_sentBuf.end() && m_nextSegLostHint->second == t2)
    {
        m_nextSegLostHint = m_sentBuf.begin();
    }

    if (t2->m_tsortedAnchor.has_value()) {
        m_tsortedItemList.erase(t2->m_tsortedAnchor.value());
        t2->m_tsortedAnchor.reset();
    }

    NS_LOG_INFO("Split of size " << size << " result: t1 " << *t1 << " t2 " << *t2);
}

TcpTxItem*
TcpTxBuffer::GetPacketFromSentBuf(uint32_t numBytes, const SequenceNumber32& seq)
{
    NS_LOG_FUNCTION(this << numBytes << seq);

    /*
     * Our possibilities are sketched out in the following:
     *
     *                    |------|     |----|     |----|
     * GetList (m_data) = |      | --> |    | --> |    |
     *                    |------|     |----|     |----|
     *
     *                    ^ ^ ^  ^
     *                    | | |  |         (1)
     *                  seq | |  numBytes
     *                      | |
     *                      | |
     *                    seq numBytes     (2)
     *
     * (1) seq and numBytes are the boundary of some packet
     * (2) seq and numBytes are not the boundary of some packet
     *
     * We can have mixed case (e.g. seq over the boundary while numBytes not).
     *
     * If we discover that we are in (2) or in a mixed case, we split
     * packets accordingly to the requested bounds and re-run the function.
     *
     * In (1), things are pretty easy, it's just a matter of walking the list and
     * defragment packets, if needed (e.g. seq is the beginning of the first packet
     * while maxBytes is the end of some packet next in the list).
     */

    auto it = m_sentBuf.upper_bound(seq); // the first that startSeq > seq
    NS_ASSERT_MSG(it != m_sentBuf.begin(), "There is no packet containing" << seq);
    it--;
    TcpTxItem* item = it->second; // seq >= item->m_startSeq
    Ptr<Packet> packet = item->m_packet;
    SequenceNumber32 beginOfPacket = it->first;
    if (seq == beginOfPacket)
    {
        // seq is the beginning of the current packet. Hurray!
        NS_LOG_INFO("Current packet starts at seq " << seq << " ends at "
                                                    << seq + packet->GetSize());
    }
    else
    {
        // seq is inside the current packet but seq is not the beginning,
        // it's somewhere in the middle. Just fragment the beginning and
        // start again.
        NS_LOG_INFO("we are at " << beginOfPacket << " searching for " << seq
                                    << " and now we recurse because packet ends at "
                                    << beginOfPacket + packet->GetSize());
        auto* firstPart = new TcpTxItem();
        SplitItems(firstPart, item, seq - beginOfPacket);
        it->second = firstPart;
        it = m_sentBuf.emplace_hint(std::next(it), seq, item);
        beginOfPacket = seq;
    }

    // The objective of this snippet is to find (or to create) the packet
    // that ends after numBytes bytes. We are sure that outPacket starts
    // at seq.
    if (numBytes <= packet->GetSize())
    {
        // the end boundary is inside the current packet
        if (numBytes == packet->GetSize())
        {
            return item;
        }
        else
        {
            // the end is inside the current packet, but it isn't exactly
            // the packet end. Just fragment, fix the list, and return.
            auto* firstPart = new TcpTxItem();
            SplitItems(firstPart, item, numBytes);
            it->second = firstPart;
            it = m_sentBuf.emplace_hint(std::next(it), item->m_startSeq, item);
            return firstPart;
        }
    }
    else
    {
        // The end isn't inside current packet, but there is an exception for
        // the merge and recurse strategy...
        it++;
        if (it == m_sentBuf.end())
        {
            // ...current is the last packet we sent. We have not more data;
            // Go for this one.
            NS_LOG_WARN("Cannot reach the end, but this case is covered "
                        "with conditional statements inside CopyFromSequence."
                        "Something has gone wrong, report a bug");
            return item;
        }

        // The current packet does not contain the requested end. Merge current
        // with the packet that follows, and recurse
        TcpTxItem* next = it->second; // Please remember we have incremented it

        MergeItems(item, next);
        m_sentBuf.erase(it);

        delete next;

        return GetPacketFromSentBuf(numBytes, seq);
    }
}

void
TcpTxBuffer::MergeItems(TcpTxItem* t1, TcpTxItem* t2)
{
    NS_ASSERT(t1 != nullptr && t2 != nullptr);
    NS_LOG_FUNCTION(this << *t1 << *t2);
    NS_LOG_INFO("Merging " << *t2 << " into " << *t1);

    NS_ASSERT_MSG(t1->m_sacked == t2->m_sacked,
                  "Merging one sacked and another not sacked. Impossible");
    NS_ASSERT_MSG(t1->m_lost == t2->m_lost, "Merging one lost and another not lost. Impossible");

    // If one is retrans and the other is not, cancel the retransmitted flag.
    // We are merging this segment for the retransmit, so the count will
    // be updated in MarkTransmittedSegment.
    if (t1->m_retrans != t2->m_retrans)
    {
        if (t1->m_retrans)
        {
            auto self = const_cast<TcpTxBuffer*>(this);
            self->m_retrans -= t1->m_packet->GetSize();
            t1->m_retrans = false;
        }
        else
        {
            NS_ASSERT(t2->m_retrans);
            auto self = const_cast<TcpTxBuffer*>(this);
            self->m_retrans -= t2->m_packet->GetSize();
            t2->m_retrans = false;
        }
    }

    if (t1->m_lastSent < t2->m_lastSent)
    {
        t1->m_lastSent = t2->m_lastSent;
    }

    t1->m_packet->AddAtEnd(t2->m_packet);

    if (m_nextSegLostHint != m_sentBuf.end() && m_nextSegLostHint->second == t2)
    {
        m_nextSegLostHint = m_sentBuf.begin();
    }

    if (t2->m_tsortedAnchor.has_value())
    {
        m_tsortedItemList.erase(t2->m_tsortedAnchor.value());
        t2->m_tsortedAnchor.reset();
    }

    if (t2->m_sacked) {
        m_sackedPkts--;
    }

    if (t1->m_rttNotReliable || t2->m_rttNotReliable) {
        t1->m_rttNotReliable = true;
        t2->m_rttNotReliable = true;
    }

    NS_LOG_INFO("Situation after the merge: " << *t1);
}

void
TcpTxBuffer::RemoveFromCounts(TcpTxItem* item, uint32_t size)
{
    NS_LOG_FUNCTION(this << *item << size);
    if (item->m_sacked)
    {
        NS_ASSERT(m_sackedOut >= size);
        m_sackedPkts--;
        m_sackedOut -= size;
    }
    if (item->m_retrans)
    {
        NS_ASSERT(m_retrans >= size);
        m_retrans -= size;
    }
    if (item->m_lost)
    {
        NS_ASSERT_MSG(m_lostOut >= size,
                      "Trying to remove " << size << " bytes from " << m_lostOut);
        m_lostOut -= size;
    }
}

bool
TcpTxBuffer::IsRetransmittedDataAcked(const SequenceNumber32& ack) const
{
    NS_LOG_FUNCTION(this);
    auto it = m_sentBuf.find(ack);
    if (it == m_sentBuf.begin()) {
        return false;
    }

    it--;
    TcpTxItem* item = it->second;
    Ptr<Packet> p = item->m_packet;
    return (item->m_startSeq + p->GetSize() == ack && !item->m_sacked && item->m_retrans);
}

void
TcpTxBuffer::DiscardUpTo(Ptr<TcpSocketState> tcb, const SequenceNumber32& seq, const Callback<void, TcpTxItem*>& beforeDelCb)
{
    NS_LOG_FUNCTION(this << seq);

    // Cases do not need to scan the buffer
    if (m_firstByteSeq >= seq)
    {
        NS_LOG_DEBUG("Seq " << seq << " already discarded.");
        return;
    }
    NS_LOG_DEBUG("Remove up to " << seq << " lost: " << m_lostOut << " retrans: " << m_retrans
                                 << " sacked: " << m_sackedOut);

    SequenceNumber32 highestSackSeq{0};
    if (m_highestSackIter != m_sentBuf.end())
    {
        highestSackSeq = m_highestSackIter->first;
    }

    SequenceNumber32 nextSegLostHintSeq;
    if (m_nextSegLostHint != m_sentBuf.end())
    {
        nextSegLostHintSeq = m_nextSegLostHint->first;
    }
    else
    {
        nextSegLostHintSeq = m_firstByteSeq + m_sentSize;
    }

    // Scan the buffer and discard packets
    uint32_t offset = seq - m_firstByteSeq.Get(); // Number of bytes to remove
    auto i = m_sentBuf.begin();
    while (m_size > 0 && offset > 0)
    {
        if (i == m_sentBuf.end())
        {
            // Move data from app list to sent list, so we can delete the item
            Ptr<Packet> p [[maybe_unused]] =
                CopyFromSequence(offset, m_firstByteSeq)->GetPacketCopy();
            NS_ASSERT(p);
            i = m_sentBuf.begin();
            NS_ASSERT(i != m_sentBuf.end());
        }
        TcpTxItem* item = i->second;
        Ptr<Packet> p = item->m_packet;
        uint32_t pktSize = p->GetSize();
        NS_ASSERT_MSG(item->m_startSeq == m_firstByteSeq,
                      "Item starts at " << item->m_startSeq << " while SND.UNA is "
                                        << m_firstByteSeq << " from " << *this);

        if (offset >= pktSize)
        { // This packet is behind the seqnum. Remove this packet from the buffer
            m_size -= pktSize;
            m_sentSize -= pktSize;
            offset -= pktSize;
            m_firstByteSeq += pktSize;

            if (!beforeDelCb.IsNull())
            {
                // Inform Rate algorithms only when a full packet is ACKed
                beforeDelCb(item);
            }

            rackUpdateMostRecent(tcb, item);
            RemoveFromCounts(item, pktSize);
            if (item->m_tsortedAnchor.has_value())
            {
                m_tsortedItemList.erase(item->m_tsortedAnchor.value());
            }

            i = m_sentBuf.erase(i);
            NS_LOG_INFO("Removed " << *item << " lost: " << m_lostOut << " retrans: " << m_retrans
                                   << " sacked: " << m_sackedOut << ". Remaining data " << m_size);

            delete item;
        }
        else if (offset > 0)
        { // Part of the packet is behind the seqnum. Fragment
            pktSize -= offset;
            NS_LOG_INFO(*item);
            // PacketTags are preserved when fragmenting
            item->m_packet = item->m_packet->CreateFragment(offset, pktSize);
            item->m_startSeq += offset;
            m_sentBuf.emplace_hint(std::next(i), item->m_startSeq, item);
            i = m_sentBuf.erase(i);
            m_size -= offset;
            m_sentSize -= offset;
            m_firstByteSeq += offset;

            RemoveFromCounts(item, offset);

            NS_LOG_INFO("Fragmented one packet by size " << offset << ", new size=" << pktSize
                                                         << " resulting item is " << *item
                                                         << " status: " << *this);
            break;
        }
    }
    // Catching the case of ACKing a FIN
    if (m_size == 0)
    {
        m_firstByteSeq = seq;
    }

    if (!m_sentBuf.empty())
    {
        TcpTxItem* head = m_sentBuf.begin()->second;
        if (head->m_sacked)
        {
            NS_ASSERT(!head->m_lost);
            // It is not possible to have the UNA sacked; otherwise, it would
            // have been ACKed. This is, most likely, our wrong guessing
            // when adding Reno dupacks in the count.
            head->m_sacked = false;
            m_sackedPkts--;
            m_sackedOut -= head->m_packet->GetSize();
            NS_LOG_INFO("Moving the SACK flag from the HEAD to another segment");
            AddRenoSack();
            MarkHeadAsLost();
        }

        NS_ASSERT_MSG(head->m_startSeq == seq,
                      "While removing up to " << seq << " we get SND.UNA to " << m_firstByteSeq
                                              << " this is the result: " << *this);
    }

    if (highestSackSeq <= m_firstByteSeq)
    {
        m_highestSackIter = m_sentBuf.end();
    }
    if (nextSegLostHintSeq < m_firstByteSeq)
    {
        m_nextSegLostHint = m_sentBuf.begin();
    }

    NS_LOG_DEBUG("Discarded up to " << seq << " lost: " << m_lostOut << " retrans: " << m_retrans
                                    << " sacked: " << m_sackedOut);
    NS_LOG_LOGIC("Buffer status after discarding data " << *this);
    NS_ASSERT(m_firstByteSeq >= seq);
    NS_ASSERT(m_sentSize >= m_sackedOut + m_lostOut);
    ConsistencyCheck();
}

uint32_t
TcpTxBuffer::SackBlockUpdate(Ptr<TcpSocketState> tcb, TcpOptionSack::SackBlock block, const Callback<void, TcpTxItem*>& sackedCb)
{
    uint32_t bytesSacked = 0;
    auto [startSeq, endSeq] = block;
    auto itemIt = m_sentBuf.lower_bound(startSeq);
    for (; itemIt != m_sentBuf.end(); itemIt++)
    {
        TcpTxItem* item = itemIt->second;
        uint32_t pktSize = item->m_packet->GetSize();
        SequenceNumber32 endOfCurrentPacket = itemIt->first + pktSize;

        // only mark as sacked if it is precisely mapped over the option.
        // It means that if the receiver is reporting as sacked single range bytes
        // that are not mapped 1:1 in what we have, the option is discarded.
        // There's room for improvement here.
        if (endOfCurrentPacket > endSeq)
        {
            break;
        }

        if (item->m_sacked)
        {
            NS_LOG_INFO("Received block " << block << ", checking sentList for block "
                                            << item
                                            << ", found in the sackboard already sacked");
            continue;
        }

        if (!sackedCb.IsNull())
        {
            sackedCb(item);
        }

        rackUpdateMostRecent(tcb, item);

        if (item->m_retrans) {
            item->m_retrans = false;
            m_retrans -= item->m_packet->GetSize();
        }

        if (item->m_lost)
        {
            item->m_lost = false;
            m_lostOut -= item->m_packet->GetSize();
        }

        item->m_sacked = true;
        if (item->m_tsortedAnchor.has_value())
        {
            m_tsortedItemList.erase(item->m_tsortedAnchor.value());
            item->m_tsortedAnchor.reset();
        }
        m_sackedPkts++;
        m_sackedOut += item->m_packet->GetSize();
        bytesSacked += item->m_packet->GetSize();

        if (m_highestSackIter == m_sentBuf.end() || m_highestSackIter->first <= endOfCurrentPacket)
        {
            m_highestSackIter = itemIt;
        }

        NS_LOG_INFO("Received block "
                    << block << ", checking sentList for block " << item
                    << ", found in the sackboard, sacking, current highSack: "
                    << m_highestSackIter->first);
    }
    return bytesSacked;
}

uint32_t
TcpTxBuffer::Update(Ptr<TcpSocketState> tcb, TcpOptionSack::SackList list, const Callback<void, TcpTxItem*>& sackedCb)
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("Updating scoreboard, got " << list.size() << " blocks to analyze");

    uint32_t bytesSacked = 0;

    std::sort(list.begin(), list.end());
    auto cacheIt = m_recvSackCache.begin();
    for (auto sackIt = list.begin(); sackIt != list.end(); ++sackIt)
    {
        auto [startSeq, endSeq] = *sackIt;

        while (cacheIt != m_recvSackCache.end()) {
            while (cacheIt != m_recvSackCache.end() && startSeq >= cacheIt->second) {
                cacheIt++;
            }
            if (cacheIt != m_recvSackCache.end() && endSeq > cacheIt->first) {
                if (startSeq < cacheIt->first) {
                    bytesSacked += SackBlockUpdate(tcb, {startSeq, cacheIt->first}, sackedCb);
                    startSeq = cacheIt->second; // skip cached (already processed) SACK block
                }
                if (endSeq <= cacheIt->second) {
                    break; // There is no further cache that overlaps with current block
                }
                cacheIt++;
            } else {
                break;
            }
        }

        if (startSeq >= endSeq) {
            continue;
        }

        bytesSacked += SackBlockUpdate(tcb, {startSeq, endSeq}, sackedCb);
    }

    m_recvSackCache.swap(list);

    if (bytesSacked > 0)
    {
        NS_ASSERT_MSG(m_highestSackIter != m_sentBuf.end(), "Buffer status: " << *this);
        UpdateLostCount(tcb);
    }

    NS_ASSERT(m_sentBuf.begin()->second->m_sacked == false);
    NS_ASSERT_MSG(m_sentSize >= m_sackedOut + m_lostOut, *this);
    // NS_ASSERT (list.size () == 0 || modified);   // Assert for duplicated SACK or
    //  impossiblity to map the option into the sent blocks
    ConsistencyCheck();
    return bytesSacked;
}

void
TcpTxBuffer::rackUpdateMostRecent(Ptr<TcpSocketState> tcb, TcpTxItem* item)
{
    if (item->m_rttNotReliable) {
        return;
    }

    SequenceNumber32 endSeq = item->m_startSeq + item->m_packet->GetSize();
    Time xmitTs = item->m_lastSent;
    Time rtt = Simulator::Now() - xmitTs;
    if (rtt < tcb->m_minRtt && item->m_retrans) {
        return;
    }

    if (std::pair{xmitTs, endSeq} > std::pair{m_rackXmitTs, m_rackEndSeq}) {
        m_rackXmitTs = xmitTs;
        m_rackEndSeq = endSeq;
        m_rackRtt = rtt;
    }
}

void
TcpTxBuffer::UpdateLostCount(Ptr<TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this);

    Time reorderWindow;
    if (tcb->m_congState.Get() == TcpSocketState::CA_RECOVERY || tcb->m_congState.Get() == TcpSocketState::CA_LOSS) {
        reorderWindow = Time{0};
    } else if (m_sackedPkts >= 3) {
        reorderWindow = Time{0};
    } else {
        reorderWindow = std::min(tcb->m_minRtt / 4, tcb->m_sRtt.Get() / 8);
    }

    TcpTxItem* lowestLostItem = nullptr;

    auto it = m_tsortedItemList.begin();
    while (it != m_tsortedItemList.end())
    {
        TcpTxItem* item = *it;
        if (item->m_lost && !item->m_retrans)
        {
            it++;
            continue;
        }
        SequenceNumber32 itemEndSeq = item->m_startSeq + item->m_packet->GetSize();
        if (std::pair{item->m_lastSent, itemEndSeq} >= std::pair{m_rackXmitTs, m_rackEndSeq})
        {
            break;
        }

        Time remaining = m_rackRtt + reorderWindow - Simulator::Now() + item->m_lastSent;
        if (remaining.IsNegative())
        {
            if (item->m_lost)
            {
                if (item->m_retrans)
                {
                    item->m_retrans = false;
                    m_retrans -= item->m_packet->GetSize();
                    m_totalLost += item->m_packet->GetSize();
                }
            }
            else
            {
                NS_ASSERT(!item->m_sacked);
                item->m_lost = true;
                m_lostOut += item->m_packet->GetSize();
                m_totalLost += item->m_packet->GetSize();
            }

            it = m_tsortedItemList.erase(it);
            item->m_tsortedAnchor.reset();

            if (lowestLostItem == nullptr || item->m_startSeq < lowestLostItem->m_startSeq)
            {
                lowestLostItem = item;
            }
        }
        else
        {
            it++;
        }
    }

    if (lowestLostItem != nullptr)
    {
        if (m_nextSegLostHint == m_sentBuf.end() ||
            lowestLostItem->m_startSeq < m_nextSegLostHint->first)
        {
            m_nextSegLostHint = m_sentBuf.find(lowestLostItem->m_startSeq);
            NS_ASSERT(m_nextSegLostHint != m_sentBuf.end());
        }
    }

    NS_LOG_INFO("Status after the update: " << *this);
    ConsistencyCheck();
}

bool
TcpTxBuffer::IsLost(const SequenceNumber32& seq) const
{
    NS_LOG_FUNCTION(this << seq);

    if (seq >= m_highestSackIter->first)
    {
        return false;
    }

    auto it = m_sentBuf.upper_bound(seq);
    if (it == m_sentBuf.begin())
    {
        // no item satisfies item->m_startSeq <= seq
        return false;
    }
    it--;

    TcpTxItem* item = it->second;
    if (seq < it->first + item->m_packet->GetSize())
    {
        if (item->m_lost)
        {
            NS_LOG_INFO("seq=" << seq << " is lost because of lost flag");
            return true;
        }

        if (item->m_sacked)
        {
            NS_LOG_INFO("seq=" << seq << " is not lost because of sacked flag");
            return false;
        }
    }

    return false;
}

bool
TcpTxBuffer::NextSeg(SequenceNumber32* seq, SequenceNumber32* seqHigh, bool isRecovery)
{
    NS_LOG_FUNCTION(this << isRecovery);
    /* RFC 6675, NextSeg definition.
     *
     * (1) If there exists a smallest unSACKed sequence number 'S2' that
     *     meets the following three criteria for determining loss, the
     *     sequence range of one segment of up to SMSS octets starting
     *     with S2 MUST be returned.
     *
     *     (1.a) S2 is greater than HighRxt.
     *
     *     (1.b) S2 is less than the highest octet covered by any
     *           received SACK.
     *
     *     (1.c) IsLost (S2) returns true.
     */
    for (; m_nextSegLostHint != m_sentBuf.end(); m_nextSegLostHint++)
    {
        TcpTxItem* item = m_nextSegLostHint->second;
        // Condition 1.a , 1.b , and 1.c
        if (item->m_lost && !item->m_retrans && !item->m_sacked)
        {
            NS_LOG_INFO("IsLost, returning" << item->m_startSeq);
            *seq = item->m_startSeq;
            *seqHigh = *seq + m_segmentSize;
            return true;
        }
    }

    /* (2) If no sequence number 'S2' per rule (1) exists but there
     *     exists available unsent data and the receiver's advertised
     *     window allows, the sequence range of one segment of up to SMSS
     *     octets of previously unsent data starting with sequence number
     *     HighData+1 MUST be returned.
     */
    if (SizeFromSequence(m_firstByteSeq + m_sentSize) > 0)
    {
        if (m_sentSize < m_rWndCallback())
        {
            NS_LOG_INFO("There is unsent data. Send it");
            *seq = m_firstByteSeq + m_sentSize;
            *seqHigh = *seq + std::min<uint32_t>(m_segmentSize, (m_rWndCallback() - m_sentSize));
            return true;
        }
        else
        {
            NS_LOG_INFO("There is no available receiver window to send");
            return false;
        }
    }
    else
    {
        NS_LOG_INFO("There isn't unsent data.");
    }

    /* (3) If the conditions for rules (1) and (2) fail, but there exists
     *     an unSACKed sequence number 'S3' that meets the criteria for
     *     detecting loss given in steps (1.a) and (1.b) above
     *     (specifically excluding step (1.c)), then one segment of up to
     *     SMSS octets starting with S3 SHOULD be returned.
     */
    if (isRecovery)
    {
        for (auto it = m_sentBuf.begin(); it != m_sentBuf.end(); ++it)
        {
            TcpTxItem* item = it->second;
            SequenceNumber32 beginOfCurrentPkt = it->first;
            if (!item->m_retrans && !item->m_sacked)
            {
                NS_LOG_INFO("Rule3 valid. " << beginOfCurrentPkt);
                item->m_rttNotReliable = true;
                *seq = beginOfCurrentPkt;
                *seqHigh = *seq + m_segmentSize;
                return true;
            }
        }
    }

    /* (4) If the conditions for (1), (2), and (3) fail, but there exists
     *     outstanding unSACKed data, we provide the opportunity for a
     *     single "rescue" retransmission per entry into loss recovery.
     *     If HighACK is greater than RescueRxt (or RescueRxt is
     *     undefined), then one segment of up to SMSS octets that MUST
     *     include the highest outstanding unSACKed sequence number
     *     SHOULD be returned, and RescueRxt set to RecoveryPoint.
     *     HighRxt MUST NOT be updated.
     *
     * This point require too much interaction between us and TcpSocketBase.
     * We choose to not respect the SHOULD (allowed from RFC MUST/SHOULD definition)
     */
    NS_LOG_INFO("Can't return anything");
    return false;
}

uint32_t
TcpTxBuffer::BytesInFlight() const
{
    NS_ASSERT_MSG(m_sackedOut + m_lostOut <= m_sentSize,
                  "Count of sacked " << m_sackedOut << " and lost " << m_lostOut
                                     << " is out of sync with sent list size " << m_sentSize << " "
                                     << *this);
    uint32_t leftOut = m_sackedOut + m_lostOut;
    uint32_t retrans = m_retrans;

    NS_LOG_INFO("Sent size: " << m_sentSize << " leftOut: " << leftOut << " retrans: " << retrans);
    uint32_t in_flight = m_sentSize - leftOut + retrans;

    // uint32_t rfc_in_flight = BytesInFlightRFC (3, segmentSize);
    // NS_ASSERT_MSG(in_flight == rfc_in_flight,
    //               "Calculated: " << in_flight << " RFC: " << rfc_in_flight <<
    //               "Sent size: " << m_sentSize << " leftOut: " << leftOut <<
    //                              " retrans: " << retrans);
    return in_flight;
}

void
TcpTxBuffer::ResetRenoSack()
{
    NS_LOG_FUNCTION(this);

    m_sackedPkts = 0;
    m_sackedOut = 0;
    for (auto it = m_sentBuf.begin(); it != m_sentBuf.end(); ++it)
    {
        it->second->m_sacked = false;
    }

    m_highestSackIter = m_sentBuf.end();
}

void
TcpTxBuffer::SetRWndCallback(Callback<uint32_t> rWndCallback)
{
    NS_LOG_FUNCTION(this);
    m_rWndCallback = rWndCallback;
}

void
TcpTxBuffer::ResetLastSegmentSent()
{
    std::clog << "unexpected TcpTxBuffer::ResetLastSegmentSent()\n";
    NS_LOG_FUNCTION(this);
    if (!m_sentBuf.empty())
    {
        auto lastIt = std::prev(m_sentBuf.end());
        TcpTxItem* item = lastIt->second;

        m_sentBuf.erase(lastIt);
        m_sentSize -= item->m_packet->GetSize();
        if (item->m_retrans)
        {
            m_retrans -= item->m_packet->GetSize();
        }
        item->m_rttNotReliable = true;
        m_appList.insert(m_appList.begin(), item);
    }
    ConsistencyCheck();
}

void
TcpTxBuffer::SetSentListLost(bool resetSack)
{
    NS_LOG_FUNCTION(this);
    m_retrans = 0;

    if (resetSack)
    {
        m_sackedPkts = 0;
        m_sackedOut = 0;
        m_lostOut = m_sentSize;
        m_highestSackIter = m_sentBuf.end();
    }
    else
    {
        m_lostOut = 0;
    }

    for (auto it = m_sentBuf.begin(); it != m_sentBuf.end(); ++it)
    {
        TcpTxItem* item = it->second;
        if (resetSack)
        {
            item->m_sacked = false;
            item->m_lost = true;
        }
        else
        {
            if (item->m_lost)
            {
                // Have to increment it because we set m_lostOut to 0 in the begining
                m_lostOut += item->m_packet->GetSize();
            }
            else if (!item->m_sacked)
            {
                // Packet is not marked lost, nor is sacked. Then it becomes lost.
                item->m_lost = true;
                m_lostOut += item->m_packet->GetSize();
                m_totalLost += item->m_packet->GetSize();
            }
        }

        if (item->m_tsortedAnchor.has_value())
        {
            m_tsortedItemList.erase(item->m_tsortedAnchor.value());
            item->m_tsortedAnchor.reset();
        }
        item->m_retrans = false;
        item->m_rttNotReliable = true;
    }

    m_nextSegLostHint = m_sentBuf.begin();

    NS_LOG_INFO("Set sent list lost, status: " << *this);
    NS_ASSERT_MSG(m_sentSize >= m_sackedOut + m_lostOut, *this);
    ConsistencyCheck();
}

bool
TcpTxBuffer::IsHeadRetransmitted() const
{
    NS_LOG_FUNCTION(this);

    if (m_sentSize == 0)
    {
        return false;
    }

    return m_sentBuf.begin()->second->m_retrans;
}

void
TcpTxBuffer::MarkHeadAsLost()
{
    if (m_sentBuf.empty())
    {
        return;
    }

    TcpTxItem* headItem = m_sentBuf.begin()->second;
    // If the head is sacked (reneging by the receiver the previously sent
    // information) we revert the sacked flag.
    // A sacked head means that we should advance SND.UNA.. so it's an error.
    if (headItem->m_sacked)
    {
        headItem->m_sacked = false;
        m_sackedPkts--;
        m_sackedOut -= headItem->m_packet->GetSize();
    }

    if (headItem->m_retrans)
    {
        headItem->m_retrans = false;
        m_retrans -= headItem->m_packet->GetSize();
    }

    if (!headItem->m_lost)
    {
        headItem->m_lost = true;
        m_lostOut += headItem->m_packet->GetSize();
        m_totalLost += headItem->m_packet->GetSize();
    }

    if (headItem->m_tsortedAnchor.has_value())
    {
        m_tsortedItemList.erase(headItem->m_tsortedAnchor.value());
        headItem->m_tsortedAnchor.reset();
    }
    headItem->m_rttNotReliable = true;

    m_nextSegLostHint = m_sentBuf.begin();

    ConsistencyCheck();
}

void
TcpTxBuffer::AddRenoSack()
{
    NS_LOG_FUNCTION(this);
    std::clog << "unexpected TcpTxBuffer::AddRenoSack()\n";

    if (m_sackEnabled)
    {
        NS_ASSERT(m_sentBuf.size() > 1);
    }
    else
    {
        NS_ASSERT(!m_sentBuf.empty());
    }

    m_renoSack = true;

    // We can _never_ SACK the head, so start from the second segment sent
    auto it = std::next(m_sentBuf.begin());

    // Find the "highest sacked" point, that is SND.UNA + m_sackedOut
    while (it != m_sentBuf.end() && it->second->m_sacked)
    {
        ++it;
    }

    // Add to the sacked size the size of the first "not sacked" segment
    if (it != m_sentBuf.end())
    {
        TcpTxItem* item = it->second;
        item->m_sacked = true;
        m_sackedPkts++;
        m_sackedOut += item->m_packet->GetSize();
        m_highestSackIter = it;
        NS_LOG_INFO("Added a Reno SACK, status: " << *this);
    }
    else
    {
        NS_LOG_INFO("Can't add a Reno SACK because we miss segments. This dupack"
                    " should be arrived from spurious retransmissions");
    }

    ConsistencyCheck();
}

void
TcpTxBuffer::ConsistencyCheck() const
{
    static const bool enable = false;

    if (!enable)
    {
        return;
    }

    uint32_t sacked = 0;
    uint32_t lost = 0;
    uint32_t retrans = 0;

    for (auto it = m_sentBuf.begin(); it != m_sentBuf.end(); ++it)
    {
        TcpTxItem* item = it->second;
        if (item->m_sacked)
        {
            sacked += item->m_packet->GetSize();
        }
        if (item->m_lost)
        {
            lost += item->m_packet->GetSize();
        }
        if (item->m_retrans)
        {
            retrans += item->m_packet->GetSize();
        }
    }

    NS_ASSERT_MSG(sacked == m_sackedOut,
                  "Counted SACK: " << sacked << " stored SACK: " << m_sackedOut);
    NS_ASSERT_MSG(lost == m_lostOut, " Counted lost: " << lost << " stored lost: " << m_lostOut);
    NS_ASSERT_MSG(retrans == m_retrans,
                  " Counted retrans: " << retrans << " stored retrans: " << m_retrans);
}

std::ostream&
operator<<(std::ostream& os, const TcpTxItem& item)
{
    item.Print(os);
    return os;
}

std::ostream&
operator<<(std::ostream& os, const TcpTxBuffer& tcpTxBuf)
{
    std::stringstream ss;
    SequenceNumber32 beginOfCurrentPacket = tcpTxBuf.m_firstByteSeq;
    uint32_t sentSize = 0;
    uint32_t appSize = 0;

    Ptr<const Packet> p;
    for (auto it = tcpTxBuf.m_sentBuf.begin(); it != tcpTxBuf.m_sentBuf.end(); ++it)
    {
        TcpTxItem* item = it->second;
        p = item->GetPacket();
        ss << "{";
        item->Print(ss);
        ss << "}";
        sentSize += p->GetSize();
        beginOfCurrentPacket += p->GetSize();
    }

    for (auto it = tcpTxBuf.m_appList.begin(); it != tcpTxBuf.m_appList.end(); ++it)
    {
        appSize += (*it)->GetPacket()->GetSize();
    }

    os << "Sent list: " << ss.str() << ", size = " << tcpTxBuf.m_sentBuf.size()
       << " Total size: " << tcpTxBuf.m_size << " m_firstByteSeq = " << tcpTxBuf.m_firstByteSeq
       << " m_sentSize = " << tcpTxBuf.m_sentSize << " m_retransOut = " << tcpTxBuf.m_retrans
       << " m_lostOut = " << tcpTxBuf.m_lostOut << " m_sackedOut = " << tcpTxBuf.m_sackedOut;

    NS_ASSERT(sentSize == tcpTxBuf.m_sentSize);
    NS_ASSERT(tcpTxBuf.m_size - tcpTxBuf.m_sentSize == appSize);
    return os;
}

} // namespace ns3
