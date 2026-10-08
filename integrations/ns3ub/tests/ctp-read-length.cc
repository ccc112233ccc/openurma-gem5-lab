// SPDX-License-Identifier: GPL-2.0-only
// Native wire/target contract; this does not execute a Linux driver or DMA.
#include "ns3/core-module.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-datalink.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace ns3;

struct ReadLengthTest {
    const std::vector<uint32_t> sizes{1,2,4,8,16,32,63,64,65,127,128,129,
                                      4095,4096,4097,4098,8193};
    std::vector<uint32_t> received, completed;
    Ptr<UbCtpTransportService> sender;
    void BadResponses(Ptr<const UbWqeSegment> request) {
        const auto cnps=sender->GetCnpSentCount();
        for(unsigned kind=0;kind<5;++kind) {
            auto packet=Create<Packet>(kind==4 ? 2 : 1);
            packet->AddPacketTag(UbFlowTag(request->GetTaskId(),request->GetWqeSize()));
            UbAckTransactionHeader ack;
            ack.SetUbTransactionOpcode(UbTransactionOpcode::READ_RESPONSE);
            ack.SetIniTaSsn(request->GetRequestTassn());
            ack.SetIniRcType(kind==1 ? 0 : 1); ack.SetIniRcId(kind==3 ? 8 : 7);
            ack.SetRspStatus(kind==0 ? 1 : 0); ack.SetPoison(kind==2);
            packet->AddHeader(ack);
            UbCompactEidHeader eid; eid.SetSourceEid(1); eid.SetDestinationEid(1);
            packet->AddHeader(eid);
            UbCompactUpiHeader upi; upi.SetUpi(0); packet->AddHeader(upi);
            UbCtpHeader ctp; ctp.SetTPOpcode(CtpOpcode::CTP_DATA);
            ctp.SetNlp(UB_CTPH_NLP_UPI16_EID40_TAH);
            const auto pad=(4-(packet->GetSize()+ctp.GetSerializedSize())%4)%4;
            ctp.SetPadding(pad); packet->AddPaddingAtEnd(pad); packet->AddHeader(ctp);
            UbCna16NetworkHeader cna;
            cna.SetScna(utils::NodeIdToCna16(request->GetDest(),0));
            cna.SetDcna(utils::NodeIdToCna16(request->GetSrc(),0));
            cna.SetServiceLevel(1); cna.SetNlp(UB_CNA_NLP_CTPH);
            if(kind==4) { cna.SetMode(4); cna.SetFecn(1); }
            packet->AddHeader(cna);
            UbDataLink::AddPacketHeader(packet,false,false,1,1,
                static_cast<RoutingType>(0),UbDatalinkHeaderConfig::PACKET_CNA16);
            sender->HandleReceivedPacket(packet,0);
        }
        if(sender->GetCnpSentCount()!=cnps+1)
            throw std::runtime_error("marked READ response lost congestion feedback");
        Simulator::Schedule(NanoSeconds(1),[this] {
            if(completed[0]!=0) throw std::runtime_error("malformed response completed READ");
        });
    }
    static uint64_t Address(unsigned id) {
        // Include unaligned low bits and address bits above the compact field.
        return 0xf123000000001003ULL + id * 0x10000ULL;
    }
    void Complete(UbWqeCompletion c) {
        if(c.wqeId >= sizes.size() || c.outcome != UbWorkOutcome::COMPLETED)
            throw std::runtime_error("unexpected READ completion");
        if(++completed[c.wqeId] != 1) throw std::runtime_error("duplicate completion");
    }
    void Target(Ptr<const UbWqeSegment> r, const UbTransactionRule&,
                UbTargetCompletion done) {
        const auto id=r->GetTaskId();
        if(id >= sizes.size()) throw std::runtime_error("unexpected task");
        const auto offset=received[id];
        const auto bytes=std::min(4096U,sizes[id]-offset);
        if(r->GetType()!=UbTransactionOpcode::READ || r->GetPayloadBytes()!=0 ||
           r->GetLogicalBytes()!=bytes || r->GetResponseBytes()!=bytes ||
           r->GetRemoteAddress()!=Address(id)+offset || r->GetRemoteTokenId()!=123 ||
           r->GetOriginJettyNum()!=7) {
            std::cerr<<"task="<<id<<" offset="<<offset<<" expected_bytes="<<bytes
                     <<" actual="<<r->GetLogicalBytes()<<" response="<<r->GetResponseBytes()
                     <<" address="<<std::hex<<r->GetRemoteAddress()<<" expected="<<Address(id)+offset
                     <<std::dec<<" token="<<r->GetRemoteTokenId()<<" jetty="<<r->GetOriginJettyNum()<<'\n';
            throw std::runtime_error("wire READ length/address/context mismatch");
        }
        received[id]+=bytes;
        if(id==0) BadResponses(r);
        done(UbWorkExecutionResult{});
    }
    void Run() {
        Time::SetResolution(Time::PS);
        received.resize(sizes.size()); completed.resize(sizes.size());
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
        }
        sw->Init();
        for(unsigned i=0;i<2;++i) {
            for(auto port : {UINT32_MAX,0U}) {
                auto dst=port==UINT32_MAX ? utils::NodeIdToIp(hosts[i]->GetId()).Get()
                                         : utils::NodeIdToIp(hosts[i]->GetId(),port).Get();
                sw->GetRoutingProcess()->AddShortestRoute(dst,{static_cast<uint16_t>(i)});
                hosts[1-i]->GetObject<UbSwitch>()->GetRoutingProcess()->AddShortestRoute(dst,{0});
            }
        }
        hosts[1]->GetObject<UbController>()->GetUbTransaction()->SetTargetExecutor(
            MakeCallback(&ReadLengthTest::Target,this));
        auto ctrl=hosts[0]->GetObject<UbController>(); auto fn=ctrl->GetUbFunction();
        sender=ctrl->GetCtpTransportService();
        fn->CreateJetty(hosts[0]->GetId(),hosts[1]->GetId(),7);
        auto jetty=fn->GetJetty(7);
        jetty->SetWqeCompletionCallback(MakeCallback(&ReadLengthTest::Complete,this));
        if(!ctrl->GetCtpTransportService()->PrepareJetty(jetty,{1,hosts[1]->GetId(),1,1}))
            throw std::runtime_error("prepare");
        for(unsigned id=0;id<sizes.size();++id) {
            auto w=fn->CreateWqe(hosts[0]->GetId(),hosts[1]->GetId(),sizes[id],id,UbTransactionOpcode::READ);
            w->SetSport(0); w->SetDport(CTP_WILDCARD_PORT); w->SetPriority(1);
            w->SetSrcEntityId(1); w->SetDstEntityId(1);
            w->SetRemoteAddress(Address(id)); w->SetRemoteTokenId(123);
            if(!ctrl->SubmitUrmaWqe(7,w,{})) throw std::runtime_error("submit");
            ctrl->GetCtpTransportService()->NotifyJettyWork(7);
        }
        Simulator::Stop(MilliSeconds(1)); Simulator::Run();
        for(unsigned id=0;id<sizes.size();++id)
            if(received[id]!=sizes[id] || completed[id]!=1)
                throw std::runtime_error("READ incomplete or rounded beyond target range");
        std::cout<<"exact READ lengths/unaligned addresses/short tails: "<<sizes.size()<<" PASS\n";
        Simulator::Destroy();
    }
};
int main() { ReadLengthTest test; test.Run(); }
