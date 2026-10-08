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
#include <array>
#include <iostream>
#include <stdexcept>
using namespace ns3;

struct WireRetryTest {
    enum Mode { INITIAL_QUEUE, RETRY_QUEUE_ACK, RETRY_QUEUE_ABORT, DROP_FIRST, DROP_ALL } mode;
    Ptr<UbCtpTransportService> sender;
    Ptr<UbPort> sourcePort;
    unsigned transmitted{}, executed{}, terminal{};
    UbWorkOutcome outcome{UbWorkOutcome::COMPLETED};
    Time firstWire;
    explicit WireRetryTest(Mode value) : mode(value) {}
    static void Require(bool value, const char* message) {
        if(!value) throw std::runtime_error(message);
    }
    bool QueuedRetry() const { return mode==RETRY_QUEUE_ACK || mode==RETRY_QUEUE_ABORT; }
    void Wire(Ptr<Packet> packet, Time) {
        UbFlowTag flow;
        if(!packet->PeekPacketTag(flow) || flow.GetFlowId()!=77) return;
        if(++transmitted==1) {
            firstWire=Simulator::Now();
            if(QueuedRetry()) sourcePort->SetOutputBlocked(true);
        }
    }
    void Target(Ptr<const UbWqeSegment> request, const UbTransactionRule&, UbTargetCompletion done) {
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
        Require(value.wqeId==77 && ++terminal==1,"duplicate or wrong terminal");
        outcome=value.outcome;
    }
    void Release() { sourcePort->SetOutputBlocked(false); sourcePort->TriggerTransmit(); }
    void Run() {
        std::array<Ptr<Node>,2> hosts;
        auto fabric=CreateObject<Node>(); auto sw=CreateObject<UbSwitch>();
        fabric->AggregateObject(sw); sw->SetNodeType(UB_SWITCH);
        sw->SetAttribute("FlowControl",EnumValue(FcType::CBFC));
        for(unsigned i=0;i<2;++i) {
            hosts[i]=CreateObject<Node>(); auto hs=CreateObject<UbSwitch>();
            hs->SetNodeType(UB_DEVICE); hosts[i]->AggregateObject(hs);
            auto ctrl=CreateObject<UbController>(); hosts[i]->AggregateObject(ctrl);
            ctrl->CreateUbFunction(); ctrl->CreateUbTransaction();
            auto hp=CreateObject<UbPort>(); auto sp=CreateObject<UbPort>();
            hp->SetAddress(Mac48Address::Allocate()); sp->SetAddress(Mac48Address::Allocate());
            hp->SetDataRate(DataRate("400Gbps")); sp->SetDataRate(DataRate("400Gbps"));
            hosts[i]->AddDevice(hp); fabric->AddDevice(sp);
            auto link=CreateObject<UbLink>(); link->SetAttribute("Delay",TimeValue(NanoSeconds(100)));
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
            Simulator::Schedule(MicroSeconds(14),[this] {
                Require(transmitted==1 && terminal==0 && sender->GetRetransmissionCount()==0,
                        "queued retry was counted or retransmitted before departure");
            });
            Simulator::Schedule(MicroSeconds(29),[this] {
                Require(terminal==1 && sender->GetRetransmissionCount()==0 &&
                        sender->GetRetransmissionExhaustedCount()==0,"ACK/error did not retire queued retry timer");
            });
            Simulator::Schedule(MicroSeconds(35),&WireRetryTest::Release,this);
        }
        auto w=fn->CreateWqe(hosts[0]->GetId(),hosts[1]->GetId(),64,77,UbTransactionOpcode::READ);
        w->SetSport(0); w->SetDport(CTP_WILDCARD_PORT); w->SetPriority(1);
        w->SetSrcEntityId(1); w->SetDstEntityId(1);
        w->SetRemoteAddress(0x123003); w->SetRemoteTokenId(123);
        Require(ctrl->SubmitUrmaWqe(7,w,{}),"submit failed"); sender->NotifyJettyWork(7);
        Simulator::Stop(MicroSeconds(150)); Simulator::Run();
        const bool failed=mode==DROP_ALL || mode==RETRY_QUEUE_ABORT;
        Require(terminal==1 && outcome==(failed ? UbWorkOutcome::FAILED : UbWorkOutcome::COMPLETED),
                "incorrect final outcome");
        Require(executed==(mode==DROP_ALL ? 0U : 1U),"duplicate target execution");
        Require(sender->GetRetransmissionCount()==(mode==DROP_ALL ? 2U : mode==DROP_FIRST ? 1U : 0U),
                "wrong actual retry count or stale timer resurrection");
        Require(sender->GetRetransmissionExhaustedCount()==(mode==DROP_ALL ? 1U : 0U),
                "unexpected retry exhaustion");
        if(mode==INITIAL_QUEUE) Require(firstWire>=MicroSeconds(50),"send escaped port block");
        Simulator::Destroy();
        std::cout<<"actual-port retry mode="<<mode<<" PASS\n";
    }
};
int main() {
    Time::SetResolution(Time::PS);
    for(auto mode : {WireRetryTest::INITIAL_QUEUE, WireRetryTest::RETRY_QUEUE_ACK,
                    WireRetryTest::RETRY_QUEUE_ABORT, WireRetryTest::DROP_FIRST, WireRetryTest::DROP_ALL}) {
        WireRetryTest test(mode); test.Run();
    }
}
