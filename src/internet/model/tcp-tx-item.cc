/*
 * Copyright (c) 2018 Natale Patriciello <natale.patriciello@gmail.com>
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
 */
#include "tcp-tx-item.h"

namespace ns3
{

void
TcpTxItem::Print(std::ostream& os, Time::Unit unit /* = Time::S */) const
{
    bool comma = false;
    os << "[" << m_startSeq << ";" << m_startSeq + GetSeqSize() << "|" << GetSeqSize() << "]";

    if (m_lost)
    {
        os << "[lost]";
        comma = true;
    }
    if (m_retrans)
    {
        if (comma)
        {
            os << ",";
        }

        os << "[retrans]";
        comma = true;
    }
    if (m_sacked)
    {
        if (comma)
        {
            os << ",";
        }
        os << "[sacked]";
        comma = true;
    }
    if (comma)
    {
        os << ",";
    }
    os << "[" << m_lastSent.As(unit) << "]";
}

uint32_t
TcpTxItem::GetSeqSize() const
{
    return m_packet && m_packet->GetSize() > 0 ? m_packet->GetSize() : 1;
}

SequenceNumber32
TcpTxItem::GetStartSeq() const
{
    return m_startSeq;
}

SequenceNumber32
TcpTxItem::GetEndSeq() const
{
    return m_startSeq + GetSeqSize();
}

bool
TcpTxItem::IsSacked() const
{
    return m_sacked;
}

bool
TcpTxItem::IsRetrans() const
{
    return m_retrans;
}

bool
TcpTxItem::IsLost() const
{
    return m_lost;
}

Ptr<Packet>
TcpTxItem::GetPacketCopy() const
{
    return m_packet->Copy();
}

Ptr<const Packet>
TcpTxItem::GetPacket() const
{
    return m_packet;
}

const Time&
TcpTxItem::GetLastSent() const
{
    return m_lastSent;
}

TcpTxItem::RateInformation&
TcpTxItem::GetRateInformation()
{
    return m_rateInfo;
}

} // namespace ns3