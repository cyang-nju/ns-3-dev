/*
 * Copyright (c) 2018 NITK Surathkal
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
 * Authors: Vivek Jain <jain.vivek.anand@gmail.com>
 *          Viyom Mittal <viyommittal@gmail.com>
 *          Mohit P. Tahiliani <tahiliani@nitk.edu.in>
 */

#include "tcp-bbr.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

// ACK aggregation:
// https://datatracker.ietf.org/meeting/101/materials/slides-101-iccrg-an-update-on-bbr-work-at-google-00

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TcpBbr");
NS_OBJECT_ENSURE_REGISTERED(TcpBbr);

/* We use a HIGH_GAIN value of 2/ln(2) because it's the smallest pacing gain
 * that will allow a smoothly increasing pacing rate that will double each RTT
 * and send the same number of packets per RTT that an un-paced, slow-starting
 * Reno or CUBIC flow would.
 */
static constexpr double HIGH_GAIN = 2.88539;

/* The pacing gain of 1/HIGH_GAIN in BBR_DRAIN is calculated to typically drain
 * the queue created in BBR_STARTUP in a single round:
 */
static constexpr double DRAIN_GAIN = 0.3465736;

/* The gain for deriving steady-state cwnd tolerates delayed/stretched ACKs: */
static constexpr double CWND_GAIN  = 2.0;

/* The pacing_gain values for the PROBE_BW gain cycle, to discover/share bw: */
static constexpr double PACING_GAIN[] = {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};


/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight:
 */
static constexpr int CWND_MIN_TARGET_PKTS = 4;

/* To estimate if BBR_STARTUP mode (i.e. high_gain) has filled pipe... */
/* If bw has increased significantly (1.25x), there may be more bw available: */
static constexpr double FULL_BW_THRESH = 1.25;
/* But after 3 rounds w/o significant bw growth, estimate pipe is full: */
static constexpr int FULL_BW_CNT = 3;


static constexpr int EXTRA_ACK_GAIN = 1; // Gain factor for adding extra_acked to target cwnd
static constexpr int EXTRA_ACKED_WIN_RTTS = 5; // Window length of extra_acked window
static constexpr int ACK_EPOCH_ACKED_RESET_THRESH_PKTS = (1 << 20);

// bbr_tso_segs_goal


TypeId
TcpBbr::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::TcpBbr")
            .SetParent<TcpCongestionOps>()
            .AddConstructor<TcpBbr>()
            .SetGroupName("Internet")
            .AddAttribute("Stream",
                          "Random number stream (default is set to 4 to align with Linux results)",
                          UintegerValue(4),
                          MakeUintegerAccessor(&TcpBbr::SetStream),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("BwWindowLength",
                          "Length of bandwidth windowed filter",
                          UintegerValue(10),
                          MakeUintegerAccessor(&TcpBbr::m_bwWinLen),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RttWindowLength",
                          "Length of RTT windowed filter",
                          TimeValue(Seconds(10)),
                          MakeTimeAccessor(&TcpBbr::m_minRttWinLen),
                          MakeTimeChecker())
            .AddAttribute("ProbeRttDuration",
                          "Time to be spent in PROBE_RTT phase",
                          TimeValue(MilliSeconds(200)),
                          MakeTimeAccessor(&TcpBbr::m_probeRttDuration),
                          MakeTimeChecker())
            .AddAttribute("EnableAckAggrModel",
                          "Enable (true) or disable (false) ACK aggregation model",
                          BooleanValue(true),
                          MakeBooleanAccessor(&TcpBbr::m_enableAckAggrModel),
                          MakeBooleanChecker())
            .AddAttribute("EnableLongTermBwMeasure",
                          "Enable (true) or disable (false) long-term bandwidth measurement",
                          BooleanValue(false),
                          MakeBooleanAccessor(&TcpBbr::m_enableLongTermBwMeasure),
                          MakeBooleanChecker())
    ;
    return tid;
}

TcpBbr::TcpBbr()
    : TcpCongestionOps()
{
    NS_LOG_FUNCTION(this);
    m_uv = CreateObject<UniformRandomVariable>();
}


void TcpBbr::BbrInit(Ptr<TcpSocketState> tcb) {
    m_minRtt = tcb->m_minRtt;
    m_minRttTimestamp = Simulator::Now();

    m_bwFilter = MaxBandwidthFilter_t(m_bwWinLen);
    m_bwFilter.Reset(DataRate{0}, 0);

    InitPacingRateFromRtt(tcb);

    ResetLtBwSampling(tcb);

    m_ackEpochTimestamp = Simulator::Now();
    tcb->m_pacing = true;
}

DataRate TcpBbr::Bw() const {
    return (m_ltUseBw ? m_ltBw : m_bwFilter.GetBest());
}

uint32_t TcpBbr::Bdp(Ptr<TcpSocketState> tcb, DataRate bw, double gain) const {
    if (m_minRtt == Time::Max()) {
        return tcb->m_initialCWnd * tcb->m_segmentSize;
    }
    auto bdp = static_cast<uint32_t>(bw.GetBitRate()/8.0 * m_minRtt.GetSeconds() * gain);
    uint32_t tmp = bdp + tcb->m_segmentSize - 1;
    return tmp - (tmp % tcb->m_segmentSize);
}


/* To achieve full performance in high-speed paths, we budget enough cwnd to
 * fit full-sized skbs in-flight on both end hosts to fully utilize the path:
 *   - one skb in sending host Qdisc,
 *   - one skb in sending host TSO/GSO engine
 *   - one skb being received by receiver host LRO/GRO/delayed-ACK engine
 * Don't worry, at low rates (bbr_min_tso_rate) this won't bloat cwnd because
 * in such cases tso_segs_goal is 1. The minimum cwnd is 4 packets,
 * which allows 2 outstanding 2-packet sequences, to try to keep pipe
 * full even with ACK-every-other-packet delayed ACKs.
 */
uint32_t TcpBbr::GetQuantizationBudget(Ptr<TcpSocketState> tcb, uint32_t cwnd) const {
    // As ns-3 does not have TSO/GSO or LRO/GRO, only 1 (or 2) segment is added here.
    cwnd += 2 * tcb->m_segmentSize; // 3
    // uint32_t segs = cwnd / tcb->m_segmentSize;
    // cwnd = ((segs + 1) & (~1U)) * tcb->m_segmentSize;
    if (m_mode == BBR_PROBE_BW && m_cycleIdx == 0) {
        cwnd += 2 * tcb->m_segmentSize;
    }
    return cwnd;
}

void TcpBbr::UpdateModel(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    UpdateBw(tcb, rs);
    UpdateAckAggregation(tcb, rs);
    UpdateCyclePhase(tcb, rs);
    CheckFullBwReached(tcb, rs);
    CheckDrain(tcb, rs);
    UpdateMinRtt(tcb, rs);
    UpdateGains();
}

void TcpBbr::UpdateBw(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    const auto& rc = tcb->m_rateOps->m_rate;
    m_isRoundStart = false;
    if (rs.m_delivered < 0 || rs.m_interval.IsNegative()) {
        return; // Not a valid observation
    }

    if (rs.m_priorDelivered >= m_nextRttDelivered) {
        m_nextRttDelivered = rc.m_delivered;
        m_rttCnt++;
        m_isRoundStart = true;
        m_packetConservation = false;
    }

    LtBwSampling(tcb, rs);

    if (!rs.m_isAppLimited || rs.m_deliveryRate >= m_bwFilter.GetBest()) {
        m_bwFilter.Update(rs.m_deliveryRate, m_rttCnt);
    }
}

void TcpBbr::UpdateAckAggregation(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    if (!m_enableAckAggrModel) {
        return;
    }

    const auto& rc = tcb->m_rateOps->m_rate;

    if (EXTRA_ACK_GAIN == 0 || rs.m_ackedSacked <= 0 ||
        rs.m_delivered < 0 || rs.m_interval.IsNegative()) {
        return;
    }

    if (m_isRoundStart) {
        m_extraAckedWinRtts = std::min<uint32_t>(m_extraAckedWinRtts + 1, 0x1F);
        if (m_extraAckedWinRtts >= EXTRA_ACKED_WIN_RTTS) {
            m_extraAckedWinRtts = 0;
            m_extraAckedWinIdx = (1 - m_extraAckedWinIdx);
            m_extraAcked[m_extraAckedWinIdx] = 0;
        }
    }

    // Compute how many packets we expected to be delivered over epoch
    Time epochTime = rc.m_deliveredTime - m_ackEpochTimestamp;
    uint32_t expectedAcked =static_cast<uint32_t>(Bw().GetBitRate()/8.0 * epochTime.GetSeconds());

    /* Reset the aggregation epoch if ACK rate is below expected rate or
	 * significantly large no. of ack received since epoch (potentially
	 * quite old epoch).
	 */
    uint32_t thresh = tcb->m_segmentSize * ACK_EPOCH_ACKED_RESET_THRESH_PKTS;
    if (m_ackEpochAcked <= expectedAcked || m_ackEpochAcked + rs.m_ackedSacked > thresh) {
        m_ackEpochAcked = 0;
        m_ackEpochTimestamp = rc.m_deliveredTime;
        expectedAcked = 0;
    }

    m_ackEpochAcked = std::min(m_ackEpochAcked + rs.m_ackedSacked, thresh - tcb->m_segmentSize);
    uint32_t extraAcked = std::min(m_ackEpochAcked - expectedAcked, tcb->m_cWnd.Get());
    if (extraAcked > m_extraAcked[m_extraAckedWinIdx]) {
        m_extraAcked[m_extraAckedWinIdx] = extraAcked;
    }
}

void TcpBbr::UpdateCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    if (m_mode == BBR_PROBE_BW && IsNextCyclePhase(tcb, rs)) {
        AdvanceCyclePhase();
    }
}

bool TcpBbr::IsNextCyclePhase(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    const auto& rc = tcb->m_rateOps->m_rate;
    bool isFullLength = (rc.m_deliveredTime - m_cycleTimestamp > m_minRtt);

    /* The pacing_gain of 1.0 paces at the estimated bw to try to fully
	 * use the pipe without increasing the queue.
	 */
    if (m_pacingGain == 1.0) {
        return isFullLength;
    }

    uint32_t inflight = BytesInNetAtEarliestDepartTime(tcb, rs.m_priorInFlight);
    DataRate bw = m_bwFilter.GetBest();

    /* A pacing_gain > 1.0 probes for bw by trying to raise inflight to at
	 * least pacing_gain*BDP; this may take more than min_rtt if min_rtt is
	 * small (e.g. on a LAN). We do not persist if packets are lost, since
	 * a path with small buffers may not hold that much.
	 */
    if (m_pacingGain > 1.0) {
        return isFullLength && (rs.m_bytesLoss != 0 || inflight >= Inflight(tcb, bw, m_pacingGain));
    }

    /* A pacing_gain < 1.0 tries to drain extra queue we added if bw
	 * probing didn't find more bw. If inflight falls to match BDP then we
	 * estimate queue is drained; persisting would underutilize the pipe.
	 */
	return isFullLength || inflight <= Inflight(tcb, bw, 1.0);
}

uint32_t TcpBbr::BytesInNetAtEarliestDepartTime(Ptr<TcpSocketState> tcb, uint32_t inflightNow) {
    Time earliestDepartTime = std::max(tcb->m_txTimestamp, Simulator::Now());
    Time interval = earliestDepartTime - Simulator::Now();
    uint32_t intervalDelivered = static_cast<uint32_t>(Bw().GetBitRate()/8.0 * interval.GetSeconds());
    uint32_t inflightAtEdt = inflightNow;
    if (m_pacingGain > 1.0) {
        inflightAtEdt += tcb->m_segmentSize;
    }
    if (intervalDelivered >= inflightAtEdt) {
        return 0;
    }
    return inflightAtEdt - intervalDelivered;
}

uint32_t TcpBbr::Inflight(Ptr<TcpSocketState> tcb, DataRate bw, double gain) {
    uint32_t inflight = Bdp(tcb, bw, gain);
    inflight = GetQuantizationBudget(tcb, inflight);
    return inflight;
}

void TcpBbr::AdvanceCyclePhase() {
    m_cycleIdx = (m_cycleIdx + 1) % GAIN_CYCLE_LENGTH;
    m_cycleTimestamp = Simulator::Now(); // rc.m_deliveredTime
}

void TcpBbr::CheckFullBwReached(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    if (m_isFullBwReached || !m_isRoundStart || rs.m_isAppLimited) {
        return;
    }

    if (m_bwFilter.GetBest().GetBitRate() >= m_fullBw.GetBitRate() * FULL_BW_THRESH) {
        m_fullBw = m_bwFilter.GetBest();
        m_fullBwCnt = 0;
        return;
    }
    m_fullBwCnt++;
    m_isFullBwReached = (m_fullBwCnt >= FULL_BW_CNT);
}

void TcpBbr::CheckDrain(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    if (m_mode == BBR_STARTUP && m_isFullBwReached) {
        m_mode = BBR_DRAIN;
        tcb->m_ssThresh = Inflight(tcb, m_bwFilter.GetBest(), 1.0);
    }
    if (m_mode == BBR_DRAIN) {
        uint32_t inflightAtEdt = BytesInNetAtEarliestDepartTime(tcb, tcb->m_bytesInFlight);
        if (inflightAtEdt <= Inflight(tcb, m_bwFilter.GetBest(), 1.0)) {
            ResetProbeBwMode();
        }
    }
}

void TcpBbr::ResetProbeBwMode() {
    m_mode = BBR_PROBE_BW;
    m_cycleIdx = GAIN_CYCLE_LENGTH - 1 - (int)m_uv->GetValue(0, 7);
    AdvanceCyclePhase();
}

void TcpBbr::UpdateMinRtt(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    auto& rc = tcb->m_rateOps->m_rate;

    bool filterExpired = (Simulator::Now() > m_minRttTimestamp + m_minRttWinLen);
    bool isAckDelayed = false; // rs->isAckDelayed is not implemented currently
    if (rs.m_rtt.IsStrictlyPositive() &&
        (rs.m_rtt < m_minRtt || (filterExpired && !isAckDelayed)))
    {
        m_minRtt = rs.m_rtt;
        m_minRttTimestamp = Simulator::Now();
    }

    if (m_probeRttDuration.IsStrictlyPositive() && filterExpired &&
        !m_isIdleRestart && m_mode != BBR_PROBE_RTT)
    {
        m_mode = BBR_PROBE_RTT;
        SaveCwnd(tcb);
        m_probeRttDoneTimestamp = Time{0};
    }

    if (m_mode == BBR_PROBE_RTT) {
        rc.m_appLimited = std::max<uint64_t>(rc.m_delivered + tcb->m_bytesInFlight.Get(), 1);
        uint32_t cwndMinTarget = tcb->m_segmentSize * CWND_MIN_TARGET_PKTS;
        if (m_probeRttDoneTimestamp.IsZero() && tcb->m_bytesInFlight <= cwndMinTarget) {
            m_probeRttDoneTimestamp = Simulator::Now() + m_probeRttDuration;
            m_isProbeRttRoundDone = false;
            m_nextRttDelivered = rc.m_delivered;
        } else if (!m_probeRttDoneTimestamp.IsZero()) {
            if (m_isRoundStart) {
                m_isProbeRttRoundDone = true;
            }
            if (m_isProbeRttRoundDone) {
                CheckProbeRttDone(tcb);
            }
        }
    }

    if (rs.m_delivered > 0) {
        m_isIdleRestart = false;
    }
}

void TcpBbr::SaveCwnd(Ptr<const TcpSocketState> tcb) {
    if (m_prevCaState < TcpSocketState::CA_RECOVERY && m_mode != BbrMode_t::BBR_PROBE_RTT) {
        m_priorCwnd = tcb->m_cWnd;
    } else {
        m_priorCwnd = std::max(m_priorCwnd, tcb->m_cWnd.Get());
    }
}

void TcpBbr::CheckProbeRttDone(Ptr<TcpSocketState> tcb) {
    if (m_probeRttDoneTimestamp.IsZero() || Simulator::Now() <= m_probeRttDoneTimestamp) {
        return;
    }

    m_minRttTimestamp = Simulator::Now();
    tcb->m_cWnd = std::max(tcb->m_cWnd.Get(), m_priorCwnd);
    ResetMode();
}

void TcpBbr::ResetMode() {
    if (!m_isFullBwReached) {
        m_mode = BBR_STARTUP;
    } else {
        ResetProbeBwMode();
    }
}

void TcpBbr::UpdateGains() {
    switch (m_mode) {
    case BBR_STARTUP:
        m_pacingGain = HIGH_GAIN;
        m_cwndGain = HIGH_GAIN;
        break;
    case BBR_DRAIN:
        m_pacingGain = DRAIN_GAIN;
        m_cwndGain = HIGH_GAIN;    // keep cwnd
        break;
    case BBR_PROBE_BW:
        if (m_ltUseBw) {
            m_pacingGain = 1.0;
        } else {
            m_pacingGain = PACING_GAIN[m_cycleIdx];
        }
        m_cwndGain = CWND_GAIN;
        break;
    case BBR_PROBE_RTT:
        m_pacingGain = 1.0;
        m_cwndGain = 1.0;
    }
}

void TcpBbr::CongControl(Ptr<TcpSocketState> tcb, const TcpRateConnection& rc, const TcpRateSample& rs) {
    UpdateModel(tcb, rs);
    SetPacingRate(tcb, m_pacingGain);
    SetCwnd(tcb, rs);
}

void TcpBbr::SetPacingRate(Ptr<TcpSocketState> tcb, double gain) {
    uint64_t bps = static_cast<uint64_t>(Bw().GetBitRate() * gain);
    DataRate rate = std::min(DataRate{bps}, tcb->m_maxPacingRate);

    if (!m_hasSeenRtt && !tcb->m_sRtt.Get().IsZero()) {
        InitPacingRateFromRtt(tcb);
    }
    if (m_isFullBwReached || rate > tcb->m_pacingRate) {
        tcb->m_pacingRate = rate;
    }
}

void TcpBbr::InitPacingRateFromRtt(Ptr<TcpSocketState> tcb) {
    Time rtt = tcb->m_sRtt.Get();
    if (!rtt.IsZero()) {
        m_hasSeenRtt = true;
    } else {
        // no RTT sample yet
        rtt = MilliSeconds(1); // use nominal default RTT 
    }

    double bps = tcb->m_cWnd.Get()*8.0 / rtt.GetSeconds() * HIGH_GAIN * 0.99;
    tcb->m_pacingRate = std::min(DataRate{static_cast<uint64_t>(bps)}, tcb->m_maxPacingRate);
}

void TcpBbr::SetCwnd(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    const auto& rc = tcb->m_rateOps->m_rate;

    uint32_t cwnd = tcb->m_cWnd.Get();
    [&] {
        if (!rs.m_ackedSacked) {
            return;
        }

        if (SetCwndToRecoverOrRestore(tcb, rs, std::ref(cwnd))) {
            return;
        }

        uint32_t targetCwnd = Bdp(tcb, Bw(), m_cwndGain);
        targetCwnd += AckAggregationCwnd(tcb);
        targetCwnd = GetQuantizationBudget(tcb, targetCwnd);

        if (m_isFullBwReached) {
            cwnd = std::min(cwnd + rs.m_ackedSacked, targetCwnd);
        } else if (cwnd < targetCwnd || rc.m_delivered < tcb->m_initialCWnd * tcb->m_segmentSize) {
            cwnd = cwnd + rs.m_ackedSacked;
        }
        cwnd = std::max(cwnd, tcb->m_segmentSize * CWND_MIN_TARGET_PKTS);
    }();

    tcb->m_cWnd = cwnd;
    if (m_mode == BbrMode_t::BBR_PROBE_RTT) {
        tcb->m_cWnd = std::min(tcb->m_cWnd.Get(), tcb->m_segmentSize * CWND_MIN_TARGET_PKTS);
    }
}

bool TcpBbr::SetCwndToRecoverOrRestore(Ptr<TcpSocketState> tcb, const TcpRateSample& rs, uint32_t& newCwnd)
{
    const auto& rc = tcb->m_rateOps->m_rate;

    auto currCongState = tcb->m_congState.Get();
    uint32_t cwnd = tcb->m_cWnd.Get();

    /* The pacing mechanism of ns-3 is different from Linux's.
     * - ns-3 waits for the pacing interval of the packet that needs to send.
     * - Linux first sends packet (according to CWND and TSQ), then pacing is done by qdisc,
     *   which means that packet is counted in in_flight before it is really send out.
     *
     * If we subtract loss from cwnd here, then cwnd may shrink if another loss is detected
     * before a packet is sent out (pacing timer expires).
     *
     * Note that m_bytesInFlight has already taken loss into account.
     */
    // if (rs.m_bytesLoss > 0) {
    //     cwnd = std::max((int)cwnd - (int)rs.m_bytesLoss, (int)tcb->m_segmentSize);
    // }

    if (currCongState == TcpSocketState::CA_RECOVERY && m_prevCaState != TcpSocketState::CA_RECOVERY) {
        m_packetConservation = true;
        m_nextRttDelivered = rc.m_delivered;
        cwnd = tcb->m_bytesInFlight.Get() + rs.m_ackedSacked;
    } else if (m_prevCaState >= TcpSocketState::CA_RECOVERY && currCongState < TcpSocketState::CA_RECOVERY) {
        cwnd = std::max(cwnd, m_priorCwnd);
        m_packetConservation = false;
    }
    m_prevCaState = currCongState;

    if (m_packetConservation) {
        newCwnd = std::max(cwnd, tcb->m_bytesInFlight.Get() + rs.m_ackedSacked);
        return true;
    }
    newCwnd = cwnd;
    return false;
}

uint32_t TcpBbr::AckAggregationCwnd(Ptr<TcpSocketState> tcb) const {
    if (!m_enableAckAggrModel) {
        return 0;
    }

    if (EXTRA_ACK_GAIN != 0 && m_isFullBwReached) {
        uint32_t maxAggrBytes = (Bw().GetBitRate() / 8) / 10; // 100ms
        uint32_t aggrCwndBytes = EXTRA_ACK_GAIN * std::max(m_extraAcked[0], m_extraAcked[1]);
        aggrCwndBytes = std::min(aggrCwndBytes, maxAggrBytes);
        uint32_t tmp = aggrCwndBytes + tcb->m_segmentSize - 1;
        return tmp - (tmp % tcb->m_segmentSize);
    } else {
        return 0;
    }
}

void TcpBbr::ResetLtBwSamplingInterval(Ptr<TcpSocketState> tcb) {
    if (!m_enableLongTermBwMeasure) {
        return;
    }

    const auto& rc = tcb->m_rateOps->m_rate;
    m_ltLastTimestamp = rc.m_deliveredTime;
    m_ltLastDelivered = rc.m_delivered;
    m_ltLastLost = tcb->m_totalLost;
    m_ltRttCnt = 0;
}

void TcpBbr::ResetLtBwSampling(Ptr<TcpSocketState> tcb) {
    if (!m_enableLongTermBwMeasure) {
        return;
    }

    m_ltBw = DataRate{0};
    m_ltUseBw = false;
    m_ltIsSampling = false;
    ResetLtBwSamplingInterval(tcb);
}

void TcpBbr::LtBwIntervalDone(Ptr<TcpSocketState> tcb, DataRate bw) {
    if (!m_enableLongTermBwMeasure) {
        return;
    }

    if (m_ltBw.GetBitRate() != 0) {
        uint64_t bpsDiff;
        if (bw.GetBitRate() > m_ltBw.GetBitRate()) {
            bpsDiff = bw.GetBitRate() - m_ltBw.GetBitRate();
        } else {
            bpsDiff = m_ltBw.GetBitRate() - bw.GetBitRate();
        }
        
        if (bpsDiff <= 0.125 * m_ltBw.GetBitRate() || bpsDiff <= 4000) {
            m_ltBw = DataRate{(bw.GetBitRate() + m_ltBw.GetBitRate()) / 2};
            m_ltUseBw = true;
            m_pacingGain = 1.0;
            m_ltRttCnt = 0;
            return;
        }
    }
    m_ltBw = bw;
    ResetLtBwSamplingInterval(tcb);
}

void TcpBbr::LtBwSampling(Ptr<TcpSocketState> tcb, const TcpRateSample& rs) {
    if (!m_enableLongTermBwMeasure) {
        return;
    }

    const auto& rc = tcb->m_rateOps->m_rate;
    if (m_ltUseBw) {
        if (m_mode == BBR_PROBE_BW && m_isRoundStart) {
            m_ltRttCnt++;
            if (m_ltRttCnt >= 48) {
                ResetLtBwSampling(tcb);
                ResetProbeBwMode();
            }
        }
        return;
    }

    if (!m_ltIsSampling) {
        if (rs.m_bytesLoss == 0) {
            return;
        }
        ResetLtBwSamplingInterval(tcb);
        m_ltIsSampling = true;
    }

    if (rs.m_isAppLimited) {
        ResetLtBwSampling(tcb);
        return;
    }

    if (m_isRoundStart) {
        m_ltRttCnt++;
    }
    if (m_ltRttCnt < 4) {
        return;
    }
    if (m_ltRttCnt > 4*4) {
        ResetLtBwSampling(tcb);
    }

    if (rs.m_bytesLoss == 0) {
        return;
    }

    uint64_t lost = tcb->m_totalLost - m_ltLastLost;
    uint64_t delivered = rc.m_delivered - m_ltLastDelivered;
    if (delivered == 0 || lost * 5 < delivered) {
        return;
    }

    Time t = rc.m_deliveredTime - m_ltLastTimestamp;
    if (t < MilliSeconds(1)) {
        return;
    }
    LtBwIntervalDone(tcb, DataRate{static_cast<uint64_t>(delivered * 8 / t.GetSeconds())});
}




void TcpBbr::SetStream(uint32_t stream) {
    m_uv->SetStream(stream);
}


std::string TcpBbr::GetName() const {
    return "TcpBbr";
}

bool TcpBbr::HasCongControl() const {
    return true;
}



void TcpBbr::CongestionStateSet(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCongState_t newState) {
    NS_LOG_FUNCTION(this << tcb << newState);
    if (newState == TcpSocketState::CA_OPEN && !m_isInitialized) {
        BbrInit(tcb);
        m_isInitialized = true;
        return;
    }

    if (newState == TcpSocketState::CA_LOSS) {
        m_prevCaState = TcpSocketState::CA_LOSS;
        m_fullBw = DataRate{0};
        m_isRoundStart = true;

        TcpRateSample rs;
        rs.m_bytesLoss = tcb->m_segmentSize;
        LtBwSampling(tcb, rs);
    }
}

void TcpBbr::CwndEvent(Ptr<TcpSocketState> tcb, const TcpSocketState::TcpCAEvent_t event) {
    if (event == TcpSocketState::CA_EVENT_TX_START && tcb->m_rateOps->m_rate.m_appLimited) {
        m_isIdleRestart = true;
        m_ackEpochTimestamp = Simulator::Now();
        m_ackEpochAcked = 0;

        if (m_mode == BbrMode_t::BBR_PROBE_BW) {
            SetPacingRate(tcb, 1.0);
        } else if (m_mode == BbrMode_t::BBR_PROBE_RTT) {
            CheckProbeRttDone(tcb);
        }
    }
}

uint32_t
TcpBbr::GetSsThresh(Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
    NS_LOG_FUNCTION(this << tcb << bytesInFlight);
    SaveCwnd(tcb);
    return tcb->m_ssThresh;
}

Ptr<TcpCongestionOps>
TcpBbr::Fork()
{
    return CopyObject<TcpBbr>(this);
}

} // namespace ns3
