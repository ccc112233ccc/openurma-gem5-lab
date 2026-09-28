// SPDX-License-Identifier: Apache-2.0
#include "protocol/ub_net/if.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace net = openurma::proto::net;

namespace {

template <typename T>
void ZeroVolatile(volatile T& object)
{
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
}

int Run(const std::string& role, const std::string& socket,
        const std::string& shm_path, const std::string& sync_mode)
{
    const bool sender = role == "sender";
    if (!sender && role != "receiver") return 2;
    const std::uint32_t local_eid = sender ? 0x101 : 0x202;
    const std::uint32_t remote_eid = sender ? 0x202 : 0x101;
    net::Interface interface{};
    SimbricksBaseIfParams params{};
    net::DefaultParams(&params);
    params.sock_path = socket.c_str();
    if (sync_mode == "off")
        params.sync_mode = kSimbricksBaseIfSyncDisabled;
    else if (sync_mode == "required")
        params.sync_mode = kSimbricksBaseIfSyncRequired;
    else
        return 2;
    params.link_latency = 100000;
    params.sync_interval = 100000;
    SimbricksBaseIfSHMPool pool{};
    if (SimbricksBaseIfSHMPoolCreate(&pool, shm_path.c_str(),
                                     SimbricksBaseIfSHMSize(&params)) != 0 ||
        SimbricksBaseIfInit(&interface.base, &params) != 0 ||
        SimbricksBaseIfListen(&interface.base, &pool) != 0)
        return 3;
    net::Intro intro{net::kVersion, 2, 16384, 32, 0};
    net::Intro peer_intro{};
    SimBricksBaseIfEstablishData establish{
        &interface.base, &intro, sizeof(intro), &peer_intro, sizeof(peer_intro)};
    if (SimBricksBaseIfEstablish(&establish, 1) != 0 ||
        peer_intro.version != net::kVersion)
        return 4;

    const std::vector<std::uint8_t> request{'u', 'b', '-', 'n', 'e', 't'};
    const std::vector<std::uint8_t> response{'o', 'k'};
    bool link_up = false;
    bool sent = false;
    std::uint64_t now = 0;
    for (std::uint64_t spins = 0; spins < 20000000; ++spins) {
        net::UbNetOutSync(&interface, now);
        if (sender && link_up && !sent) {
            auto* output = net::UbNetOutAlloc(&interface, now);
            if (output != nullptr) {
                ZeroVolatile(output->frame);
                output->frame.sequence = 7;
                output->frame.source_eid = local_eid;
                output->frame.destination_eid = remote_eid;
                output->frame.source_port = 1;
                output->frame.length = static_cast<std::uint32_t>(request.size());
                auto* payload = reinterpret_cast<volatile std::uint8_t*>(output) +
                                sizeof(net::Message);
                for (std::size_t i = 0; i < request.size(); ++i) payload[i] = request[i];
                net::UbNetOutSend(&interface, output,
                    static_cast<std::uint8_t>(net::MessageType::Frame));
                sent = true;
            }
        }
        auto* input = net::UbNetInPoll(&interface, now);
        if (input == nullptr) {
            if (SimbricksBaseIfSyncEnabled(&interface.base)) {
                const std::uint64_t next = std::min(
                    net::UbNetInTimestamp(&interface),
                    net::UbNetOutNextSync(&interface));
                if (next > now && next != std::numeric_limits<std::uint64_t>::max())
                    now = next;
            } else {
                now += 100000;
            }
            std::this_thread::yield();
            continue;
        }
        const auto type = static_cast<net::MessageType>(
            net::UbNetInType(&interface, input));
        if (type == net::MessageType::LinkState) {
            link_up = input->link.state ==
                      static_cast<std::uint8_t>(net::LinkState::Up);
        } else if (type == net::MessageType::Frame) {
            const auto& expected = sender ? response : request;
            bool valid = link_up && input->frame.source_eid == remote_eid &&
                         input->frame.destination_eid == local_eid &&
                         input->frame.length == expected.size();
            const auto* payload = reinterpret_cast<const volatile std::uint8_t*>(input) +
                                  sizeof(net::Message);
            for (std::size_t i = 0; valid && i < expected.size(); ++i)
                valid = payload[i] == expected[i];
            net::UbNetInDone(&interface, input);
            if (!valid) return 5;
            if (sender) {
                std::cout << "sender: routed reply and link-state PASS\n";
                SimbricksBaseIfClose(&interface.base);
                SimbricksBaseIfSHMPoolUnmap(&pool);
                SimbricksBaseIfSHMPoolUnlink(&pool);
                return 0;
            }
            for (;;) {
                auto* output = net::UbNetOutAlloc(&interface, now);
                if (output == nullptr) {
                    std::this_thread::yield();
                    continue;
                }
                ZeroVolatile(output->frame);
                output->frame.sequence = 8;
                output->frame.source_eid = local_eid;
                output->frame.destination_eid = remote_eid;
                output->frame.length = static_cast<std::uint32_t>(response.size());
                auto* output_payload =
                    reinterpret_cast<volatile std::uint8_t*>(output) + sizeof(net::Message);
                for (std::size_t i = 0; i < response.size(); ++i)
                    output_payload[i] = response[i];
                net::UbNetOutSend(&interface, output,
                    static_cast<std::uint8_t>(net::MessageType::Frame));
                std::cout << "receiver: routed request and reply PASS\n";
                for (std::uint64_t delay = 0; delay < 100000; ++delay)
                    std::this_thread::yield();
                SimbricksBaseIfClose(&interface.base);
                SimbricksBaseIfSHMPoolUnmap(&pool);
                SimbricksBaseIfSHMPoolUnlink(&pool);
                return 0;
            }
        } else {
            net::UbNetInDone(&interface, input);
        }
    }
    return 6;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::cerr << "usage: ub-net-contract-peer sender|receiver SOCKET SHM "
                     "off|required\n";
        return 2;
    }
    return Run(argv[1], argv[2], argv[3], argv[4]);
}
