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
 * Author: Viyom Mittal <viyommittal@gmail.com>
 *         Vivek Jain <jain.vivek.anand@gmail.com>
 *         Mohit P. Tahiliani <tahiliani@nitk.edu.in>
 *
 */
#ifndef TCP_PRR_RECOVERY_H
#define TCP_PRR_RECOVERY_H

#include "tcp-recovery-ops.h"

namespace ns3
{

/**
 * \ingroup recoveryOps
 *
 * \brief An implementation of PRR
 *
 * PRR is an algorithm that determines TCP's sending rate in fast
 * recovery. PRR avoids excessive window reductions and aims for
 * the actual congestion window size at the end of recovery to be as
 * close as possible to the window determined by the congestion control
 * algorithm. PRR also improves accuracy of the amount of data sent
 * during loss recovery.
 */
class TcpPrrRecovery : public TcpClassicRecovery
{
  public:
    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();

    /**
     * Create an unbound tcp socket.
     */
    TcpPrrRecovery();

    /**
     * \brief Copy constructor
     * \param sock the object to copy
     */
    TcpPrrRecovery(const TcpPrrRecovery& sock);

    ~TcpPrrRecovery() override;

    std::string GetName() const override;

    void EnterRecovery(Ptr<TcpSocketState> tcb,
                       uint32_t dupAckCount,
                       uint32_t unAckDataCount,
                       uint32_t deliveredBytes) override;

    void DoRecovery(Ptr<TcpSocketState> tcb, uint32_t deliveredBytes) override;

    void ExitRecovery(Ptr<TcpSocketState> tcb) override;

    void UpdateBytesSent(uint32_t bytesSent) override;

    Ptr<TcpRecoveryOps> Fork() override;

  private:
    uint32_t m_prrDelivered{0};       //!< total bytes delivered during recovery phase
    uint32_t m_prrOut{0};             //!< total bytes sent during recovery phase
    uint32_t m_priorCwnd{0}; //!< value of bytesInFlight at the start of recovery phase
};
} // namespace ns3

#endif /* TCP_PRR_RECOVERY_H */
