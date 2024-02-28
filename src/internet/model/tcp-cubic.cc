/*
 * Copyright (c) 2014 Natale Patriciello <natale.patriciello@gmail.com>
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

#define NS_LOG_APPEND_CONTEXT                                                                      \
    {                                                                                              \
        std::clog << Simulator::Now().GetSeconds() << " ";                                         \
    }

#include "tcp-cubic.h"

#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE("TcpCubic");

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(TcpCubic);

TypeId
TcpCubic::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpCubic")
            .SetParent<TcpSocketBase>()
            .AddConstructor<TcpCubic>()
            .SetGroupName("Internet")
            .AddAttribute("FastConvergence",
                          "Enable (true) or disable (false) fast convergence",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCubic::m_fastConvergence),
                          MakeBooleanChecker())
            .AddAttribute("TcpFriendliness",
                          "Enable (true) or disable (false) TCP friendliness",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCubic::m_tcpFriendliness),
                          MakeBooleanChecker())
            .AddAttribute("Beta",
                          "Beta for multiplicative decrease",
                          DoubleValue(0.7),
                          MakeDoubleAccessor(&TcpCubic::m_beta),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("HyStart",
                          "Enable (true) or disable (false) hybrid slow start algorithm",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpCubic::m_hystart),
                          MakeBooleanChecker())
            .AddAttribute("HyStartLowWindow",
                          "Lower bound cWnd for hybrid slow start (segments)",
                          UintegerValue(16),
                          MakeUintegerAccessor(&TcpCubic::m_hystartLowWindow),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("HyStartDetect",
                          "Hybrid Slow Start detection mechanisms:"
                          "1: packet train, 2: delay, 3: both",
                          IntegerValue(3),
                          MakeIntegerAccessor(&TcpCubic::m_hystartDetect),
                          MakeIntegerChecker<int>(1, 3))
            .AddAttribute("HyStartMinSamples",
                          "Number of delay samples for detecting the increase of delay",
                          UintegerValue(8),
                          MakeUintegerAccessor(&TcpCubic::m_hystartMinSamples),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("HyStartAckDelta",
                          "Spacing between ack's indicating train",
                          TimeValue(MilliSeconds(2)),
                          MakeTimeAccessor(&TcpCubic::m_hystartAckDelta),
                          MakeTimeChecker())
            .AddAttribute("HyStartDelayMin",
                          "Minimum time for hystart algorithm",
                          TimeValue(MilliSeconds(4)),
                          MakeTimeAccessor(&TcpCubic::m_hystartDelayMin),
                          MakeTimeChecker())
            .AddAttribute("HyStartDelayMax",
                          "Maximum time for hystart algorithm",
                          TimeValue(MilliSeconds(16)),
                          MakeTimeAccessor(&TcpCubic::m_hystartDelayMax),
                          MakeTimeChecker())
            .AddAttribute("CubicDelta",
                          "Delta Time to wait after fast recovery before adjusting param",
                          TimeValue(MilliSeconds(4)),
                          MakeTimeAccessor(&TcpCubic::m_cubicDelta),
                          MakeTimeChecker())
            .AddAttribute("CntClamp",
                          "Counter value when no losses are detected (counter is used"
                          " when incrementing cWnd in congestion avoidance, to avoid"
                          " floating point arithmetic). It is the modulo of the (avoided)"
                          " division",
                          UintegerValue(20),
                          MakeUintegerAccessor(&TcpCubic::m_cntClamp),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("C",
                          "Cubic Scaling factor",
                          DoubleValue(0.4),
                          MakeDoubleAccessor(&TcpCubic::m_c),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

TcpCubic::TcpCubic()
    : TcpCongestionOps(),
      m_cubicDelta(Time::Min()),
      m_cWndCnt(0),
      m_lastMaxCwnd(0),
      m_bicOriginPoint(0),
      m_bicK(0.0),
      m_delayMin(0),
      m_epochStart(Time::Min()),
      m_found(false),
      m_roundStart(Time::Min()),
      m_endSeq(0),
      m_lastAck(Time::Min()),
      m_currRtt(Time::Max()),
      m_sampleCnt(0)
{
    NS_LOG_FUNCTION(this);
}

std::string
TcpCubic::GetName() const
{
    return "TcpCubic";
}

void TcpCubic::Init(Ptr<TcpSocketState> tcb)
{
    m_betaScale = (1 + m_beta) / (3 * (1 - m_beta));
}

void
TcpCubic::HystartReset(Ptr<const TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this);

    m_roundStart = m_lastAck = Simulator::Now();
    m_endSeq = tcb->m_highTxMark;
    m_currRtt = Time::Max();
    m_sampleCnt = 0;
}

void
TcpCubic::IncreaseWindow(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
    NS_LOG_FUNCTION(this << tcb << segmentsAcked);

    if (segmentsAcked == 0) {
        return;
    }

    uint32_t cwnd = tcb->m_cWnd.Get();
    uint32_t ssthresh = tcb->m_ssThresh.Get();
    if (cwnd < ssthresh) {
        cwnd = std::min(cwnd + segmentsAcked * tcb->m_segmentSize, ssthresh);
        segmentsAcked -= (cwnd - tcb->m_cWnd.Get()) / tcb->m_segmentSize;
        tcb->m_cWnd = cwnd;
    }
    if (segmentsAcked == 0) {
        return;
    }

    NS_ASSERT(segmentsAcked > 0);

    m_cWndCnt += segmentsAcked;
    uint32_t cnt = Update(tcb, segmentsAcked);
    if (m_cWndCnt >= cnt)
    {
        tcb->m_cWnd += tcb->m_segmentSize;
        m_cWndCnt -= cnt;
    }
}

uint32_t
TcpCubic::Update(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
    NS_LOG_FUNCTION(this);
    Time t;
    uint32_t delta;
    uint32_t bicTarget;
    uint32_t cnt = 0;
    double offs;
    uint32_t segCwnd = tcb->GetCwndInSegments();

    m_ackCnt += segmentsAcked;

    if (m_epochStart == Time::Min())
    {
        m_epochStart = Simulator::Now(); // record the beginning of an epoch
        m_ackCnt = segmentsAcked;
        m_tcpSegCwnd = segCwnd;

        if (m_lastMaxCwnd <= segCwnd)
        {
            NS_LOG_DEBUG("lastMaxCwnd <= m_cWnd. K=0 and origin=" << segCwnd);
            m_bicK = 0.0;
            m_bicOriginPoint = segCwnd;
        }
        else
        {
            m_bicK = std::cbrt((m_lastMaxCwnd - segCwnd) / m_c);
            m_bicOriginPoint = m_lastMaxCwnd;
            NS_LOG_DEBUG("lastMaxCwnd > m_cWnd. K=" << m_bicK << " and origin=" << m_lastMaxCwnd);
        }
    }

    t = Simulator::Now() - m_epochStart + m_delayMin;

    if (t.GetSeconds() < m_bicK) /* t - K */
    {
        offs = m_bicK - t.GetSeconds();
        NS_LOG_DEBUG("t=" << t.GetSeconds() << " <k: offs=" << offs);
    }
    else
    {
        offs = t.GetSeconds() - m_bicK;
        NS_LOG_DEBUG("t=" << t.GetSeconds() << " >= k: offs=" << offs);
    }

    /* Constant value taken from Experimental Evaluation of Cubic Tcp, available at
     * eprints.nuim.ie/1716/1/Hamiltonpfldnet2007_cubic_final.pdf */
    delta = m_c * std::pow(offs, 3);

    NS_LOG_DEBUG("delta: " << delta);

    if (t.GetSeconds() < m_bicK)
    {
        // below origin
        bicTarget = m_bicOriginPoint - delta;
        NS_LOG_DEBUG("t < k: Bic Target: " << bicTarget);
    }
    else
    {
        // above origin
        bicTarget = m_bicOriginPoint + delta;
        NS_LOG_DEBUG("t >= k: Bic Target: " << bicTarget);
    }

    // Next the window target is converted into a cnt or count value. CUBIC will
    // wait until enough new ACKs have arrived that a counter meets or exceeds
    // this cnt value. This is how the CUBIC implementation simulates growing
    // cwnd by values other than 1 segment size.
    if (bicTarget > segCwnd)
    {
        cnt = segCwnd / (bicTarget - segCwnd);
        NS_LOG_DEBUG("target>cwnd. cnt=" << cnt);
    }
    else
    {
        cnt = 100 * segCwnd;
    }

    if (m_lastMaxCwnd == 0 && cnt > m_cntClamp)
    {
        cnt = m_cntClamp;
    }

    if (m_tcpFriendliness)
    {
        auto delta = static_cast<uint32_t>(segCwnd * m_betaScale);
        if (delta == 0) {
            std::cerr << "TcpCubic::Update() unexpected zero delta\n";
            delta = 1;
        }
        m_tcpSegCwnd += m_ackCnt / delta;
        m_ackCnt = m_ackCnt % delta;

        if (m_tcpSegCwnd > segCwnd) {
            delta = m_tcpSegCwnd - segCwnd;
            uint32_t maxCnt = segCwnd / delta;
            if (cnt > maxCnt) {
                cnt = maxCnt;
            }
        }
    }

    // The maximum rate of cwnd increase CUBIC allows is 1 packet per
    // 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
    return std::max(cnt, 2U);
}

void
TcpCubic::PktsAcked(Ptr<TcpSocketState> tcb, uint32_t segmentsAcked, const Time& rtt)
{
    NS_LOG_FUNCTION(this << tcb << segmentsAcked << rtt);

    if (rtt.IsNegative())
    {
        return;
    }

    /* Discard delay samples right after fast recovery */
    if (m_epochStart != Time::Min() && (Simulator::Now() - m_epochStart) < m_cubicDelta)
    {
        return;
    }

    /* first time call or link delay decreases */
    if (m_delayMin.IsZero() || m_delayMin > rtt)
    {
        m_delayMin = rtt;
    }

    /* hystart triggers when cwnd is larger than some threshold */
    if (!m_found && m_hystart && tcb->m_cWnd < tcb->m_ssThresh &&
        tcb->m_cWnd >= m_hystartLowWindow * tcb->m_segmentSize)
    {
        HystartUpdate(tcb, rtt);
    }
}

void
TcpCubic::HystartUpdate(Ptr<TcpSocketState> tcb, const Time& delay)
{
    NS_LOG_FUNCTION(this << delay);

    if (tcb->m_lastAckedSeq > m_endSeq)
    {
        HystartReset(tcb);
    }

    if (m_hystartDetect & PACKET_TRAIN)
    {
        Time now = Simulator::Now();

        /* first detection parameter - ack-train detection */
        if ((now - m_lastAck) <= m_hystartAckDelta)
        {
            m_lastAck = now;
            Time hystartAckDelay{0};
            if (DataRate rate = tcb->m_pacingRate.Get(); rate.GetBitRate() != 0) {
                hystartAckDelay = std::min(MilliSeconds(1), rate.CalculateBytesTxTime(tcb->m_segmentSize * 4));
            }
            if (!tcb->m_pacing) {
                hystartAckDelay = hystartAckDelay / 2;
            }
            Time threshold = m_delayMin + hystartAckDelay;
            if ((now - m_roundStart) > threshold)
            {
                m_found = true;
                tcb->m_ssThresh = tcb->m_cWnd;
            }
        }
    }

    if (m_hystartDetect & DELAY)
    {
        m_currRtt = std::min(m_currRtt, delay);
        if (m_sampleCnt < m_hystartMinSamples)
        {
            m_sampleCnt++;
        }
        else
        {
            if (m_currRtt > m_delayMin + HystartDelayThresh(m_delayMin / 8))
            {
                m_found = true;
                tcb->m_ssThresh = tcb->m_cWnd;
            }
        }
    }
}

Time
TcpCubic::HystartDelayThresh(const Time& t) const
{
    NS_LOG_FUNCTION(this << t);

    Time ret = t;
    if (t > m_hystartDelayMax)
    {
        ret = m_hystartDelayMax;
    }
    else if (t < m_hystartDelayMin)
    {
        ret = m_hystartDelayMin;
    }

    return ret;
}

uint32_t
TcpCubic::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);

    uint32_t segCwnd = tcb->GetCwndInSegments();
    NS_LOG_DEBUG("Loss at cWnd=" << segCwnd
                                 << " segments in flight=" << bytesInFlight / tcb->m_segmentSize);

    /* Wmax and fast convergence */
    if (segCwnd < m_lastMaxCwnd && m_fastConvergence)
    {
        m_lastMaxCwnd = (segCwnd * (1 + m_beta)) / 2; // Section 4.6 in RFC 8312
    }
    else
    {
        m_lastMaxCwnd = segCwnd;
    }

    m_epochStart = Time::Min(); // end of epoch

    /* Formula taken from the Linux kernel */
    uint32_t ssThresh = std::max(static_cast<uint32_t>(segCwnd * m_beta), 2U) * tcb->m_segmentSize;

    NS_LOG_DEBUG("SsThresh = " << ssThresh);

    return ssThresh;
}

void
TcpCubic::CongestionStateSet(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCongState_t newState)
{
    NS_LOG_FUNCTION(this << tcb << newState);

    if (newState == TcpSocketState::CA_LOSS)
    {
        CubicReset(tcb);
        HystartReset(tcb);
    }
}

void
TcpCubic::CubicReset(Ptr<const TcpSocketState> tcb)
{
    NS_LOG_FUNCTION(this << tcb);

    m_cWndCnt = 0;
    m_lastMaxCwnd = 0;
    m_bicOriginPoint = 0;
    m_bicK = 0;
    m_delayMin = Time{0};
    m_epochStart = Time::Min();
    m_ackCnt = 0;
    m_tcpSegCwnd = 0;
    m_found = false;
}

Ptr<TcpCongestionOps>
TcpCubic::Fork()
{
    NS_LOG_FUNCTION(this);
    return CopyObject<TcpCubic>(this);
}

} // namespace ns3
