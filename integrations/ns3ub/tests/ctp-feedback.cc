// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/core-module.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"
#include "../ctp-queue-feedback.h"
#include <iostream>
#include <array>
#include <stdexcept>
using namespace ns3;

struct FeedbackTest {
    std::array<Ptr<Node>, 3> hosts;
    std::array<uint32_t, 4> completions{};
    std::array<int64_t, 4> finish{};
    std::array<uint64_t, 2> minRate{10000000000ULL,10000000000ULL};
    void Complete(UbWqeCompletion c) {
        if (c.wqeId >= 4 || c.outcome != UbWorkOutcome::COMPLETED)
            throw std::runtime_error("unexpected or failed task");
        ++completions[c.wqeId];
        finish[c.wqeId] = Simulator::Now().GetPicoSeconds();
    }
    void Run(bool enabled, unsigned senders) {
        Time::SetResolution(Time::PS);
        // Serialization must retain the FECN bits, not the C++ union layout.
        for (uint8_t mode : {2,4}) for (uint8_t ecn : {0,1,2,3}) {
            UbCna16NetworkHeader header;
            header.SetMode(mode); header.SetLocation(true); header.SetFecn(ecn);
            if(mode==2) header.SetTimestamp(777);
            auto packet=Create<Packet>(); packet->AddHeader(header);
            uint8_t bytes[8]; packet->CopyData(bytes,8);
            const uint16_t expected=(mode<<13)|(1<<12)|(mode==2 ? 777<<2 : 0)|ecn;
            if(bytes[4]!=(expected&255) || bytes[5]!=(expected>>8))
                throw std::runtime_error("FECN wire bits");
            UbCna16NetworkHeader parsed; packet->RemoveHeader(parsed);
            if(parsed.GetMode()!=mode || parsed.GetFecn()!=ecn || !parsed.GetLocation() ||
               (mode==2 && parsed.GetTimestamp()!=777)) throw std::runtime_error("FECN roundtrip");
        }
        auto fabric = CreateObject<Node>();
        auto sw = CreateObject<UbSwitch>();
        fabric->AggregateObject(sw);
        sw->SetNodeType(UB_SWITCH);
        sw->SetAttribute("FlowControl", EnumValue(FcType::CBFC));
        for (unsigned i=0; i<3; ++i) {
            hosts[i] = CreateObject<Node>();
            auto hs = CreateObject<UbSwitch>();
            hs->SetNodeType(UB_DEVICE);
            hosts[i]->AggregateObject(hs);
            auto ctrl = CreateObject<UbController>();
            hosts[i]->AggregateObject(ctrl);
            ctrl->CreateUbFunction(); ctrl->CreateUbTransaction();
            auto hp = CreateObject<UbPort>();
            auto sp = CreateObject<UbPort>();
            hp->SetAddress(Mac48Address::Allocate());
            sp->SetAddress(Mac48Address::Allocate());
            hp->SetDataRate(DataRate("10Gbps")); sp->SetDataRate(DataRate("10Gbps"));
            hosts[i]->AddDevice(hp); fabric->AddDevice(sp);
            auto link = CreateObject<UbLink>();
            link->SetAttribute("Delay", TimeValue(NanoSeconds(100)));
            hp->Attach(link); sp->Attach(link);
            ctrl->CreateCtpEntity(1, {0});
            if (i<2) ctrl->CreateCtpEntity(2, {0});
            ctrl->FreezeCtpEntities(); hs->Init();
            auto svc=ctrl->GetCtpTransportService();
            svc->SetCongestionLineRate(10000000000ULL);
            svc->SetCongestionMinimumRate(1000000000ULL);
            svc->SetCongestionRecovery(MicroSeconds(5), 1000000000ULL);
            svc->SetCnpFeedbackInterval(MicroSeconds(20));
        }
        sw->Init();
        auto feedback=CreateObject<UbCtpQueueFeedback>();
        feedback->Configure(sw, enabled ? 8192 : 0);
        sw->SetCongestionCtrl(feedback);
        for (unsigned i=0; i<3; ++i) {
            sw->GetRoutingProcess()->AddShortestRoute(utils::NodeIdToIp(hosts[i]->GetId()).Get(),
                                                       {static_cast<uint16_t>(i)});
            for (unsigned j=0; j<3; ++j) if (j!=i) {
                auto routing=hosts[i]->GetObject<UbSwitch>()->GetRoutingProcess();
                routing->AddShortestRoute(utils::NodeIdToIp(hosts[j]->GetId()).Get(), {0});
                routing->AddShortestRoute(utils::NodeIdToIp(hosts[j]->GetId(),0).Get(), {0});
            }
            sw->GetRoutingProcess()->AddShortestRoute(utils::NodeIdToIp(hosts[i]->GetId(),0).Get(),
                                                       {static_cast<uint16_t>(i)});
        }
        hosts[2]->GetObject<UbController>()->GetUbTransaction()->SetTargetExecutor(
            UbTargetExecutor([](Ptr<const UbWqeSegment>, const UbTransactionRule&, UbTargetCompletion done) {
                done(UbWorkExecutionResult{});
            }));
        for (unsigned i=0; i<senders; ++i) for (unsigned entity=1; entity<=2; ++entity) {
            auto ctrl=hosts[i]->GetObject<UbController>();
            auto fn=ctrl->GetUbFunction();
            fn->CreateJetty(hosts[i]->GetId(), hosts[2]->GetId(), entity);
            auto jetty=fn->GetJetty(entity);
            jetty->SetWqeCompletionCallback(MakeCallback(&FeedbackTest::Complete, this));
            UbCtpEntityKey key{entity,hosts[2]->GetId(),1,1};
            if (!ctrl->GetCtpTransportService()->PrepareJetty(jetty,key))
                throw std::runtime_error("prepare");
            auto wqe=fn->CreateWqe(hosts[i]->GetId(), hosts[2]->GetId(), 1024*1024,
                                  i*2+entity-1, UbTransactionOpcode::WRITE);
            wqe->SetSport(0); wqe->SetDport(CTP_WILDCARD_PORT); wqe->SetPriority(1);
            wqe->SetSrcEntityId(entity); wqe->SetDstEntityId(1);
            if (!ctrl->SubmitUrmaWqe(entity,wqe,{})) throw std::runtime_error("submit");
            ctrl->GetCtpTransportService()->NotifyJettyWork(entity);
        }
        for (unsigned tick=1; tick<=2000; ++tick) Simulator::Schedule(MicroSeconds(tick*5), [this] {
            for (unsigned i=0;i<2;++i) {
                auto svc=hosts[i]->GetObject<UbController>()->GetCtpTransportService();
                auto rate=svc->GetCongestionRate({1,hosts[2]->GetId(),1,1});
                if(rate!=svc->GetCongestionRate({2,hosts[2]->GetId(),1,1}))
                    throw std::runtime_error("source Entities did not share rate");
                minRate[i]=std::min(minRate[i],rate);
            }
        });
        Simulator::Stop(MilliSeconds(11));
        Simulator::Run();
        uint64_t cuts=0,cnps=0,suppressed=0,waits=0;
        for (unsigned i=0;i<3;++i) {
            auto svc=hosts[i]->GetObject<UbController>()->GetCtpTransportService();
            cuts+=svc->GetCongestionRateCutCount(); cnps+=svc->GetCnpSentCount();
            suppressed+=svc->GetCnpSuppressedCount();
            waits+=svc->GetWirePacingWaitCount();
            if(i<2 && svc->GetCongestionRate({1,hosts[2]->GetId(),1,1})!=10000000000ULL)
                throw std::runtime_error("did not recover to line rate");
        }
        for(unsigned i=0;i<4;++i) if(completions[i]!=(i<senders*2 ? 1u : 0u))
            throw std::runtime_error("missing/duplicate completion");
        if(enabled && senders>1 && (minRate[0]>=10000000000ULL || minRate[1]>=10000000000ULL))
            throw std::runtime_error("both senders must reduce rate");
        if(enabled && senders>1 ? (!feedback->marked || !cnps || cuts!=cnps || !waits || !suppressed)
                               : (feedback->marked || cnps || cuts))
            throw std::runtime_error("feedback counters");
        double sum=0,sq=0;
        int64_t last=0;
        for(unsigned i=0;i<senders*2;++i) { const double r=1e12/finish[i]; sum+=r; sq+=r*r; last=std::max(last,finish[i]); }
        const double fairness=sum*sum/(senders*2*sq);
        if(fairness<0.95) throw std::runtime_error("starvation/unfair completion rates");
        std::cout << "feedback=" << enabled << " senders=" << senders << " marks=" << feedback->marked << " cnp=" << cnps
                  << " cuts=" << cuts << " suppressed=" << suppressed << " wire_waits=" << waits
                  << " peak_queue_bytes=" << feedback->peakBytes << " jain_completion_rate=" << fairness
                  << " min_rate0=" << minRate[0] << " min_rate1=" << minRate[1]
                  << " goodput_gbps=" << (senders*2*1048576.0*8*1000/last);
        for(unsigned i=0;i<senders*2;++i) std::cout << " finish_ps=" << finish[i];
        std::cout << " recovered_bps=10000000000 PASS\n";
        Simulator::Destroy();
    }
};
int main(int argc,char**) { FeedbackTest test; test.Run(argc>1, argc>2 ? 1 : 2); }
