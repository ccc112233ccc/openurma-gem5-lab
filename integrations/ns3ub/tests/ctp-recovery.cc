// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-transport.h"
#include "ns3/ub-function.h"
#include "ns3/ub-ctp.h"
#include "ns3/simulator.h"
#include <iostream>
#include <stdexcept>

using namespace ns3;

int main()
{
    Time::SetResolution(Time::PS);
    auto service = CreateObject<UbCtpTransportService>();
    const UbCtpEntityKey key{.srcEntityId=1, .dstNodeId=2, .dstEntityId=3, .vl=1};
    auto shared = key;
    shared.srcEntityId = 9;
    auto isolated = key;
    isolated.vl = 2;
    auto check = [&](const UbCtpEntityKey& k, uint64_t expected) {
        const auto actual = service->GetCongestionRate(k);
        if (actual != expected)
            throw std::runtime_error("rate mismatch: expected " + std::to_string(expected) +
                                     " got " + std::to_string(actual));
    };
    service->SetCongestionLineRate(2000000000ULL);
    service->SetCongestionMinimumRate(125000000ULL);
    service->SetCongestionRecovery(NanoSeconds(100), 250000000ULL);
    service->RecordCnpForTest(key);
    check(key, 1000000000ULL);
    check(shared, 1000000000ULL);
    check(isolated, 2000000000ULL);
    Simulator::Schedule(NanoSeconds(99), [&] { check(key, 1000000000ULL); });
    Simulator::Schedule(NanoSeconds(100), [&] { check(key, 1250000000ULL); });
    Simulator::Schedule(NanoSeconds(150), [&] {
        service->RecordCnpForTest(shared);
        check(key, 625000000ULL);
    });
    Simulator::Schedule(NanoSeconds(249), [&] { check(key, 625000000ULL); });
    Simulator::Schedule(NanoSeconds(250), [&] { check(key, 875000000ULL); });
    Simulator::Schedule(NanoSeconds(1000), [&] {
        check(key, 2000000000ULL);
        for (int i=0; i<20; ++i) service->RecordCnpForTest(key);
        check(key, 125000000ULL);
        service->SetCongestionRecovery(Seconds(0), 0);
    });
    Simulator::Schedule(Seconds(1), [&] { check(key, 125000000ULL); });
    Simulator::Run();
    service->Dispose();
    Simulator::Destroy();
    std::cout << "CTP recovery: cut, additive recovery, cap, floor, shared Entity, VL isolation, reset and disable PASS\n";
}
