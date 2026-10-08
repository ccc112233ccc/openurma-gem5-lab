// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/packet.h"
#include <algorithm>

namespace ns3 {
// Explicit reference policy: deterministic FECN above a byte threshold.
// The hook runs after native switch enqueue, never from the IPC adapter poll.
class UbCtpQueueFeedback : public UbCongestionControl {
public:
    static TypeId GetTypeId() {
        static TypeId tid = TypeId("ns3::UbCtpQueueFeedback")
            .SetParent<UbCongestionControl>().AddConstructor<UbCtpQueueFeedback>();
        return tid;
    }
    void Configure(Ptr<UbSwitch> sw, uint64_t threshold) {
        m_switch = sw;
        m_threshold = threshold;
    }
    void OnSwitchPostEnqueue(uint32_t, uint32_t outPort, Ptr<Packet> p) override {
        const auto backlog = m_switch->GetQueueManager()->GetOutPortQueueBacklogBytes(outPort);
        peakBytes = std::max(peakBytes, backlog);
        if (!m_threshold || backlog <= m_threshold) return;
        auto view = p->Copy();
        UbDatalinkPacketHeader dl;
        UbCna16NetworkHeader cna;
        UbCtpHeader ctp;
        if (!view->RemoveHeader(dl) ||
            dl.GetConfig() != static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16) ||
            !view->RemoveHeader(cna) || cna.GetNlp() != UB_CNA_NLP_CTPH ||
            !view->RemoveHeader(ctp) ||
            ctp.GetTPOpcode() != static_cast<uint8_t>(CtpOpcode::CTP_DATA)) return;
        if (ctp.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH) {
            UbCompactUpiHeader upi;
            UbCompactEidHeader eid;
            if (!view->RemoveHeader(upi) || !view->RemoveHeader(eid)) return;
        } else if (ctp.GetNlp() != UB_CTPH_NLP_COMPACT_TAH) return;
        UbCompactAckTransactionHeader ta;
        if (!view->PeekHeader(ta) ||
            ta.GetUbTransactionOpcode() == static_cast<uint8_t>(UbTransactionOpcode::TAACK)) return;
        p->RemoveHeader(dl);
        p->RemoveHeader(cna);
        cna.SetMode(4); // FECN mode, as in native DCQCN CNA/IP header definitions.
        cna.SetLocation(false);
        cna.SetFecn(1);
        p->AddHeader(cna);
        p->AddHeader(dl);
        ++marked;
    }
    uint64_t marked{0}, peakBytes{0};
private:
    void DoDispose() override { m_switch = nullptr; UbCongestionControl::DoDispose(); }
    Ptr<UbSwitch> m_switch;
    uint64_t m_threshold{0};
};
}
