// SPDX-License-Identifier: GPL-2.0-only
// Native CTP per-Jetty sequence and completion contract.
#include "ns3/core-module.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-function.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
using namespace ns3;

struct MultiJettyEntityTest {
    bool splitJettys{};
    bool retryJettys{};
    std::array<Ptr<Node>, 2> hosts;
    Ptr<UbController> source;
    UbCtpEntityKey key{};
    std::map<uint32_t, uint32_t> requestSequence;
    std::map<uint32_t, uint32_t> requestOrigin;
    std::map<uint32_t, unsigned> completed;

    static void Require(bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    }
    void Target(Ptr<const UbWqeSegment> request, const UbTransactionRule&,
                UbTargetCompletion done) {
        const uint32_t task = request->GetTaskId();
        Require(task == 70 || task == 80, "unexpected native task");
        requestSequence[task] = request->GetRequestTassn();
        requestOrigin[task] = request->GetOriginJettyNum();
        if (task == 70)
            Simulator::Schedule(MicroSeconds(20), [done] { done(UbWorkExecutionResult{}); });
        else
            done(UbWorkExecutionResult{});
    }
    void Complete(UbWqeCompletion result) {
        Require(result.outcome == UbWorkOutcome::COMPLETED,
                "native WQE failed");
        Require(++completed[result.wqeId] == 1, "duplicate native completion");
    }
    void Submit(uint32_t jettyId, uint32_t taskId) {
        auto function = source->GetUbFunction();
        auto wqe = function->CreateWqe(hosts[0]->GetId(), hosts[1]->GetId(), 128,
                                       taskId, UbTransactionOpcode::READ);
        wqe->SetSport(0); wqe->SetDport(CTP_WILDCARD_PORT); wqe->SetPriority(1);
        wqe->SetSrcEntityId(1); wqe->SetDstEntityId(1);
        wqe->SetRemoteAddress(0x1000 + taskId * 128); wqe->SetRemoteTokenId(123);
        Require(source->SubmitUrmaWqe(jettyId, wqe, {}), "native submit failed");
        Require(source->GetCtpTransportService()->NotifyJettyWork(jettyId),
                "native notify failed");
    }
    void Run() {
        auto fabric = CreateObject<Node>(); auto sw = CreateObject<UbSwitch>();
        fabric->AggregateObject(sw); sw->SetNodeType(UB_SWITCH);
        sw->SetAttribute("FlowControl", EnumValue(FcType::CBFC));
        for (unsigned i = 0; i < 2; ++i) {
            hosts[i] = CreateObject<Node>(); auto device = CreateObject<UbSwitch>();
            device->SetNodeType(UB_DEVICE); hosts[i]->AggregateObject(device);
            auto controller = CreateObject<UbController>(); hosts[i]->AggregateObject(controller);
            controller->CreateUbFunction(); controller->CreateUbTransaction();
            auto hostPort = CreateObject<UbPort>(); auto switchPort = CreateObject<UbPort>();
            hostPort->SetAddress(Mac48Address::Allocate());
            switchPort->SetAddress(Mac48Address::Allocate());
            hostPort->SetDataRate(DataRate("400Gbps"));
            switchPort->SetDataRate(DataRate("400Gbps"));
            hosts[i]->AddDevice(hostPort); fabric->AddDevice(switchPort);
            auto link = CreateObject<UbLink>();
            link->SetAttribute("Delay", TimeValue(NanoSeconds(100)));
            hostPort->Attach(link); switchPort->Attach(link);
            controller->CreateCtpEntity(1, {0}); controller->FreezeCtpEntities();
            device->Init();
        }
        sw->Init();
        for (unsigned i = 0; i < 2; ++i) {
            for (auto port : {UINT32_MAX, 0U}) {
                const auto destination = port == UINT32_MAX
                    ? utils::NodeIdToIp(hosts[i]->GetId()).Get()
                    : utils::NodeIdToIp(hosts[i]->GetId(), port).Get();
                sw->GetRoutingProcess()->AddShortestRoute(destination,
                    {static_cast<uint16_t>(i)});
                hosts[1-i]->GetObject<UbSwitch>()->GetRoutingProcess()->AddShortestRoute(
                    destination, {0});
            }
        }
        source = hosts[0]->GetObject<UbController>();
        if (retryJettys) {
            auto senderService = source->GetCtpTransportService();
            senderService->SetRetransmissionEnabled(true);
            senderService->SetRetransmissionTimeout(MicroSeconds(5));
            auto targetService = hosts[1]->GetObject<UbController>()->GetCtpTransportService();
            targetService->SetRetransmissionTimeout(MicroSeconds(5));
            targetService->SetDropNextReadResponseForTest(true);
        }
        hosts[1]->GetObject<UbController>()->GetUbTransaction()->SetTargetExecutor(
            MakeCallback(&MultiJettyEntityTest::Target, this));
        key = {1, hosts[1]->GetId(), 1, 1};
        for (uint32_t id : {7U, 8U}) {
            if (id == 8 && !splitJettys) continue;
            auto function = source->GetUbFunction();
            function->CreateJetty(hosts[0]->GetId(), hosts[1]->GetId(), id);
            auto jetty = function->GetJetty(id);
            jetty->SetWqeCompletionCallback(
                MakeCallback(&MultiJettyEntityTest::Complete, this));
            Require(source->GetCtpTransportService()->PrepareJetty(jetty, key),
                    "native preparation failed");
        }
        Submit(7, 70);
        Submit(splitJettys ? 8 : 7, 80);
        Simulator::Schedule(MicroSeconds(10), [this] {
            const auto* state = source->GetUbTransaction()->FindCtpTransactionState(key, 7);
            std::cerr << "split=" << splitJettys
                      << " target70=" << requestOrigin.count(70)
                      << " target80=" << requestOrigin.count(80)
                      << " sendNext=" << (state && state->context
                                              ? state->context->GetSendNext() : UINT32_MAX)
                      << " completeUna=" << (state && state->context
                                                 ? state->context->GetCompleteUna() : UINT32_MAX)
                      << '\n';
            Require(requestOrigin.count(70) == 1,
                    "first native Jetty request did not reach target");
            Require(requestOrigin.count(80) == 1,
                    "second native Jetty stalled: local TASSN 0 conflicts with Entity sendNext 1");
            Require(requestOrigin[70] == 7 &&
                    requestOrigin[80] == (splitJettys ? 8U : 7U),
                    "native Jetty identity lost on wire");
            Require(requestSequence[70] == 0 &&
                    requestSequence[80] == (splitJettys ? 0U : 1U),
                    "native Jetty TASSN allocation differs from its local sequence");
            Require(completed[70] == 0,
                    "held first target operation completed early");
            Require(state && state->context &&
                    state->context->GetSendNext() == (splitJettys ? 1U : 2U) &&
                    state->context->GetCompleteUna() == 0 &&
                    state->context->GetOutstandingCount() == (splitJettys ? 1U : 2U),
                    "first Jetty completion window was corrupted by the second");
            if (splitJettys)
                Require(completed[80] == 1,
                        "second Jetty completion waited for first Jetty ACK");
        });
        Simulator::Stop(MicroSeconds(30)); Simulator::Run();
        Require(completed[70] == 1 && completed[80] == 1,
                "native completion missing");
        for (uint32_t id : {7U, 8U}) {
            if (id == 8 && !splitJettys) continue;
            const auto* state = source->GetUbTransaction()->FindCtpTransactionState(key, id);
            std::cerr << "retire jetty=" << id << " state=" << (state != nullptr);
            if (state) std::cerr << " pending=" << state->pendingSegments.size()
                << " outstanding=" << state->outstandingSegments.size()
                << " segment_count=" << state->segmentOutstandingPackets.size()
                << " events=" << state->pendingCompletionEvents
                << " context_outstanding=" << (state->context ? state->context->GetOutstandingCount() : UINT32_MAX);
            std::cerr << '\n';
        }
        Require(source->GetUbTransaction()->FindCtpTransactionState(key, 7) == nullptr &&
                (!splitJettys ||
                 source->GetUbTransaction()->FindCtpTransactionState(key, 8) == nullptr),
                "completed native Jetty transaction state was not retired");
        if (retryJettys) {
            auto senderService = source->GetCtpTransportService();
            auto targetService = hosts[1]->GetObject<UbController>()->GetCtpTransportService();
            Require(senderService->GetRetransmissionCount() >= 1,
                    "second Jetty's lost response did not retransmit");
            Require(targetService->GetDuplicateReadSuppressedCount() >= 1 &&
                    targetService->GetDuplicateReadResponseReplayCount() >= 1,
                    "per-Jetty duplicate suppression or replay missing");
        }
        std::cout << (splitJettys ? "multi-Jetty" : "single-Jetty control")
                  << " per-Jetty TASSN/window, completion and replay: PASS\n";
        Simulator::Destroy();
    }
};
int main(int argc, char** argv) {
    Time::SetResolution(Time::PS);
    MultiJettyEntityTest test;
    test.splitJettys = argc > 1 && std::string(argv[1]) == "--split-jettys";
    test.retryJettys = argc > 1 && std::string(argv[1]) == "--retry-jettys";
    if (test.retryJettys) test.splitJettys = true;
    try {
        test.Run();
    } catch (const std::exception& error) {
        std::cerr << "CTP multi-Jetty contract: " << error.what() << '\n' << std::flush;
        // Avoid a teardown callback masking the contract failure.
        std::_Exit(1);
    }
}
