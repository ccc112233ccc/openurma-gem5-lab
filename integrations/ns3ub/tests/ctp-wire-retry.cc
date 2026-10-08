// SPDX-License-Identifier: GPL-2.0-only
// Real native ports/allocator/link/CTP; not an official-driver acceptance test.
#include "ns3/core-module.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-cbfc.h"
#include "ns3/ub-datalink.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace ns3;

struct WireRetryTest {
    enum Mode { INITIAL_QUEUE, RETRY_QUEUE_ACK, RETRY_QUEUE_ABORT, DROP_FIRST, DROP_ALL,
                RETRY_INFLIGHT } mode;
    Ptr<UbCtpTransportService> sender;
    Ptr<UbPort> sourcePort;
    Ptr<UbController> source;
    uint32_t destination{};
    unsigned followups{}, followupExecuted{}, followupTransmitted{};
    unsigned transmitted{}, executed{}, terminal{};
    unsigned enqueued{};
    bool shared{};
    unsigned reverseCompleted{};
    bool cancelledHadCredit{};
    uint8_t cancelledCreditVl{};
    uint32_t cancelledCells{};
    int32_t beforeDedicated{}, beforeShared{}, beforePending{};
    explicit WireRetryTest(Mode value, bool sharedCredit) : mode(value), shared(sharedCredit) {}
    UbWorkOutcome outcome{UbWorkOutcome::COMPLETED};
    Time firstWire;
    explicit WireRetryTest(Mode value) : mode(value) {}
    static void Require(bool value, const char* message) {
        if(!value) throw std::runtime_error(message);
    }
    bool QueuedRetry() const { return mode==RETRY_QUEUE_ACK || mode==RETRY_QUEUE_ABORT; }
    void Wire(Ptr<Packet> packet, Time) {
        UbFlowTag flow;
        if(!packet->PeekPacketTag(flow)) return;
        if(flow.GetFlowId()!=77) { if(flow.GetFlowId()>=100) ++followupTransmitted; return; }
        if(++transmitted==1) {
            firstWire=Simulator::Now();
        }
    }
    void Egress(Ptr<const Packet> packet, uint32_t) {
        UbFlowTag flow;
        if(!packet->PeekPacketTag(flow) || flow.GetFlowId()!=77) return;
        if(++enqueued==2 && QueuedRetry()) {
            sourcePort->SetOutputBlocked(true);
            // Header mutation by FC occurs after the enqueue trace; inspect
            // the actual queued packet after that same simulator event ends.
            Simulator::ScheduleNow([this,packet] {
                UbDatalinkPacketHeader h; packet->PeekHeader(h);
                cancelledHadCredit=h.GetCredit(); cancelledCreditVl=h.GetCreditTargetVL();
                cancelledCells=(packet->GetSize()+159)/160;
            });
        }
    }
    void Target(Ptr<const UbWqeSegment> request, const UbTransactionRule&, UbTargetCompletion done) {
        if(request->GetTaskId()!=77) {
            ++followupExecuted;
            done(UbWorkExecutionResult{});
            return;
        }
        ++executed;
        Require(request->GetType()==UbTransactionOpcode::READ &&
                request->GetLogicalBytes()==64 && request->GetPayloadBytes()==0 &&
                request->GetRemoteAddress()==0x123003 && request->GetRemoteTokenId()==123,
                "wire READ contract changed");
        UbWorkExecutionResult result;
        if(mode==RETRY_QUEUE_ABORT) {
            result.outcome=UbWorkOutcome::FAILED;
            result.protocolStatus=UbTransactionProtocolStatus::REMOTE_ABORT;
        }
        if(QueuedRetry()) Simulator::Schedule(MicroSeconds(15), [done,result] { done(result); });
        else done(result);
    }
    void Complete(UbWqeCompletion value) {
        if(value.wqeId!=77) {
            Require(value.wqeId==100+followups && value.outcome==UbWorkOutcome::COMPLETED,
                    "successor WQE failed or completed out of order");
            if(++followups<32) Simulator::ScheduleNow([this] { Submit(100+followups); });
            return;
        }
        Require(value.wqeId==77 && ++terminal==1,"duplicate or wrong terminal");
        outcome=value.outcome;
    }
    void Release() { sourcePort->SetOutputBlocked(false); sourcePort->TriggerTransmit(); }
    void ReverseTarget(Ptr<const UbWqeSegment> request, const UbTransactionRule&, UbTargetCompletion done) {
        Require(request->GetTaskId()==88,"unexpected reverse request");
        Simulator::Schedule(MicroSeconds(31),[done] { done(UbWorkExecutionResult{}); });
    }
    void ReverseComplete(UbWqeCompletion value) {
        Require(value.wqeId==88 && value.outcome==UbWorkOutcome::COMPLETED && ++reverseCompleted==1,
                "reverse CCI-seeding request failed");
    }
    void Submit(uint32_t id) {
        auto w=source->GetUbFunction()->CreateWqe(sourcePort->GetNode()->GetId(),destination,
                                                 64,id,UbTransactionOpcode::READ);
        w->SetSport(0); w->SetDport(CTP_WILDCARD_PORT); w->SetPriority(1);
        w->SetSrcEntityId(1); w->SetDstEntityId(1);
        w->SetRemoteAddress(0x123003); w->SetRemoteTokenId(123);
        Require(source->SubmitUrmaWqe(7,w,{}),"submit failed"); sender->NotifyJettyWork(7);
    }
    void Run() {
        std::array<Ptr<Node>,2> hosts;
        auto fabric=CreateObject<Node>(); auto sw=CreateObject<UbSwitch>();
        fabric->AggregateObject(sw); sw->SetNodeType(UB_SWITCH);
        sw->SetAttribute("FlowControl",EnumValue(shared ? FcType::CBFC_SHARED : FcType::CBFC));
        for(unsigned i=0;i<2;++i) {
            hosts[i]=CreateObject<Node>(); auto hs=CreateObject<UbSwitch>();
            hs->SetNodeType(UB_DEVICE); hosts[i]->AggregateObject(hs);
            hs->SetAttribute("FlowControl",EnumValue(shared ? FcType::CBFC_SHARED : FcType::CBFC));
            auto ctrl=CreateObject<UbController>(); hosts[i]->AggregateObject(ctrl);
            ctrl->CreateUbFunction(); ctrl->CreateUbTransaction();
            auto hp=CreateObject<UbPort>(); auto sp=CreateObject<UbPort>();
            hp->SetAddress(Mac48Address::Allocate()); sp->SetAddress(Mac48Address::Allocate());
            hp->SetDataRate(DataRate("400Gbps")); sp->SetDataRate(DataRate("400Gbps"));
            for(auto p : {hp,sp}) {
                p->SetAttribute("CbfcRetCellGrainDataPacket",UintegerValue(1));
                if(shared) p->SetAttribute("CbfcInitCreditCell",IntegerValue(0));
            }
            hosts[i]->AddDevice(hp); fabric->AddDevice(sp);
            auto link=CreateObject<UbLink>();
            link->SetAttribute("Delay",TimeValue(mode==RETRY_INFLIGHT ? MicroSeconds(3) : NanoSeconds(100)));
            hp->Attach(link); sp->Attach(link);
            ctrl->CreateCtpEntity(1,{0}); ctrl->FreezeCtpEntities(); hs->Init();
            if(i==0) sourcePort=hp;
        }
        sw->Init();
        for(unsigned i=0;i<2;++i) for(auto port : {UINT32_MAX,0U}) {
            const auto dst=port==UINT32_MAX ? utils::NodeIdToIp(hosts[i]->GetId()).Get()
                : utils::NodeIdToIp(hosts[i]->GetId(),port).Get();
            sw->GetRoutingProcess()->AddShortestRoute(dst,{static_cast<uint16_t>(i)});
            hosts[1-i]->GetObject<UbSwitch>()->GetRoutingProcess()->AddShortestRoute(dst,{0});
        }
        hosts[1]->GetObject<UbController>()->GetUbTransaction()->SetTargetExecutor(
            MakeCallback(&WireRetryTest::Target,this));
        auto ctrl=hosts[0]->GetObject<UbController>(); auto fn=ctrl->GetUbFunction();
        source=ctrl; destination=hosts[1]->GetId();
        sender=ctrl->GetCtpTransportService();
        sender->SetRetransmissionEnabled(true); sender->SetRetransmissionTimeout(MicroSeconds(10));
        sender->SetMaxRetransmissionAttempts(2);
        sender->SetDropNextRequestForTest(mode==DROP_FIRST);
        sender->SetDropAllRequestsForTest(mode==DROP_ALL);
        fn->CreateJetty(hosts[0]->GetId(),hosts[1]->GetId(),7);
        auto jetty=fn->GetJetty(7);
        jetty->SetWqeCompletionCallback(MakeCallback(&WireRetryTest::Complete,this));
        Require(sender->PrepareJetty(jetty,{1,hosts[1]->GetId(),1,1}),"prepare failed");
        sourcePort->TraceConnectWithoutContext("TraComEventNotify",MakeCallback(&WireRetryTest::Wire,this));
        sourcePort->GetUbQueue()->TraceConnectWithoutContext("UbEnqueue",MakeCallback(&WireRetryTest::Egress,this));
        if(mode==INITIAL_QUEUE) {
            sourcePort->SetOutputBlocked(true);
            Simulator::Schedule(MicroSeconds(49),[this] {
                Require(transmitted==0 && terminal==0 && executed==0 &&
                        sender->GetRetransmissionCount()==0 && sender->GetRetransmissionExhaustedCount()==0,
                        "local queue wait consumed retry budget before wire send");
            });
            Simulator::Schedule(MicroSeconds(50),&WireRetryTest::Release,this);
        }
        if(QueuedRetry()) {
            // A real reverse request leaves a real pending credit at source;
            // the queued retry piggybacks it before being cancelled.
            source->GetUbTransaction()->SetTargetExecutor(MakeCallback(&WireRetryTest::ReverseTarget,this));
            auto peer=hosts[1]->GetObject<UbController>(); auto peerFn=peer->GetUbFunction();
            peerFn->CreateJetty(destination,hosts[0]->GetId(),8);
            auto peerJetty=peerFn->GetJetty(8);
            peerJetty->SetWqeCompletionCallback(MakeCallback(&WireRetryTest::ReverseComplete,this));
            Require(peer->GetCtpTransportService()->PrepareJetty(peerJetty,{1,hosts[0]->GetId(),1,1}),"reverse prepare failed");
            Simulator::Schedule(MicroSeconds(9),[this,peer,peerFn] {
                auto w=peerFn->CreateWqe(destination,sourcePort->GetNode()->GetId(),64,88,UbTransactionOpcode::READ);
                w->SetSport(0); w->SetDport(CTP_WILDCARD_PORT); w->SetPriority(1);
                w->SetSrcEntityId(1); w->SetDstEntityId(1);
                w->SetRemoteAddress(0x123003); w->SetRemoteTokenId(123);
                Require(peer->SubmitUrmaWqe(8,w,{}),"reverse submit failed");
                peer->GetCtpTransportService()->NotifyJettyWork(8);
            });
            Simulator::Schedule(MicroSeconds(14),[this] {
                Require(transmitted==1 && terminal==0 && sender->GetRetransmissionCount()==0,
                        "queued retry was counted or retransmitted before departure");
                Require(sourcePort->GetUbQueue()->GetCurrentBytes()>0,
                        "counterexample did not reach the real port egress queue");
            });
            Simulator::Schedule(MicroSeconds(29),[this] {
                Require(terminal==1 && sender->GetRetransmissionCount()==0 &&
                        sender->GetRetransmissionExhaustedCount()==0,"ACK/error did not retire queued retry timer");
            });
            Simulator::Schedule(MicroSeconds(35),&WireRetryTest::Release,this);
            Simulator::Schedule(MicroSeconds(34),[this] {
                auto fc=DynamicCast<UbCbfc>(sourcePort->GetFlowControl());
                Require(fc!=nullptr && cancelledHadCredit,"missing real cancelled CCI reservation");
                beforeDedicated=fc->GetAvailableCredits(1);
                beforeShared=fc->GetAvailableSharedCredits();
                beforePending=fc->GetCrdToReturn(cancelledCreditVl);
            });
            Simulator::Schedule(MicroSeconds(35)+NanoSeconds(100),[this] {
                auto fc=DynamicCast<UbCbfc>(sourcePort->GetFlowControl());
                Require(fc->GetAvailableCredits(1)==beforeDedicated+(shared ? 0 : cancelledCells),
                        "cancel did not return exactly the dedicated debit");
                Require(fc->GetAvailableSharedCredits()==beforeShared+(shared ? cancelledCells : 0),
                        "cancel did not return exactly the shared debit");
                Require(fc->GetCrdToReturn(cancelledCreditVl)==beforePending+1,
                        "cancel lost or duplicated unsent piggyback credit");
                // A second wakeup must not cancel/refund the same packet again.
                sourcePort->TriggerTransmit();
                Require(fc->GetAvailableCredits(1)==beforeDedicated+(shared ? 0 : cancelledCells) &&
                        fc->GetAvailableSharedCredits()==beforeShared+(shared ? cancelledCells : 0) &&
                        fc->GetCrdToReturn(cancelledCreditVl)==beforePending+1,
                        "cancel refunded the same reservation twice");
            });
            Simulator::Schedule(MicroSeconds(36),[this] { Submit(100); });
        }
        Submit(77);
        Simulator::Stop(MicroSeconds(150)); Simulator::Run();
        const bool failed=mode==DROP_ALL || mode==RETRY_QUEUE_ABORT;
        Require(terminal==1 && outcome==(failed ? UbWorkOutcome::FAILED : UbWorkOutcome::COMPLETED),
                "incorrect final outcome");
        Require(executed==(mode==DROP_ALL ? 0U : 1U),"duplicate target execution");
        Require(sender->GetRetransmissionCount()==(mode==DROP_ALL ? 2U :
                (mode==DROP_FIRST || mode==RETRY_INFLIGHT) ? 1U : 0U),
                "wrong actual retry count or stale timer resurrection");
        Require(sender->GetRetransmissionExhaustedCount()==(mode==DROP_ALL ? 1U : 0U),
                "unexpected retry exhaustion");
        Require(sender->GetStaleSentRetryCount()==0,"a stale retry escaped onto the wire");
        Require(sender->GetQueuedRetryCount()==(mode==DROP_ALL ? 2U :
                (QueuedRetry() || mode==DROP_FIRST || mode==RETRY_INFLIGHT) ? 1U : 0U),
                "queued retry accounting differs from actual scheduled attempts");
        Require(sender->GetCancelledRetryCount()==(QueuedRetry() ? 1U : 0U),
                "wrong cancellation count or an in-flight retry was cancelled");
        if(mode==INITIAL_QUEUE) Require(firstWire>=MicroSeconds(50),"send escaped port block");
        if(QueuedRetry()) {
            Require(transmitted==1,"terminal retry still consumed physical source bandwidth");
            Require(followups==32 && followupExecuted==32 && followupTransmitted==32,
                    "cancelled retry leaked resources or blocked successor traffic");
            Require(sourcePort->GetUbQueue()->GetCurrentBytes()==0,
                    "port accounting did not drain after cancelled retry and successors");
            Require(reverseCompleted==1 && cancelledHadCredit,"real CCI piggyback cancellation was not exercised");
        }
        if(mode==RETRY_INFLIGHT) Require(transmitted==2,
                                       "already transmitted retry was incorrectly retracted");
        Simulator::Destroy();
        std::cout<<"actual-port retry mode="<<mode<<" PASS\n";
    }
};
int main() {
    Time::SetResolution(Time::PS);
    for(auto mode : {WireRetryTest::INITIAL_QUEUE, WireRetryTest::RETRY_QUEUE_ACK,
                    WireRetryTest::RETRY_QUEUE_ABORT, WireRetryTest::DROP_FIRST, WireRetryTest::DROP_ALL,
                    WireRetryTest::RETRY_INFLIGHT}) {
        WireRetryTest test(mode); test.Run();
    }
    for(auto mode : {WireRetryTest::RETRY_QUEUE_ACK,WireRetryTest::RETRY_QUEUE_ABORT}) {
        WireRetryTest test(mode,true); test.Run();
    }
}
