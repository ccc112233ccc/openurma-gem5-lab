// SPDX-License-Identifier: Apache-2.0
#include "protocol/ub_net/if.h"
#include "protocol/ub_net/udma_wire.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace net = ubsim::proto::net;

namespace {

constexpr std::uint64_t kAsyncMessageTimePs = 5000000000ULL;

template <typename T>
void ZeroVolatile(volatile T& object)
{
    const std::uint64_t timestamp = object.timestamp;
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
    object.timestamp = timestamp;
}

int Run(const std::string& role, const std::string& socket,
        const std::string& shm_path, const std::string& sync_mode,
        const std::string& profile)
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

    const bool ctp_order = profile == "ctp-order";
    const bool ctp_retrans = profile == "ctp-retrans";
    const bool ctp_taack_loss = profile == "ctp-taack-loss";
    const bool ctp_exhaust = profile == "ctp-exhaust";
    if (!ctp_order && !ctp_retrans && !ctp_taack_loss && !ctp_exhaust &&
        profile != "raw") return 2;
    const std::vector<std::uint8_t> request{'u', 'b', '-', 'n', 'e', 't'};
    const std::vector<std::uint8_t> response{'o', 'k'};
    bool link_up = false;
    bool sent = false;
    std::uint8_t next_order = 0;
    std::uint8_t seen_orders = 0;
    bool relax_seen = false;
    std::uint64_t now = 0;
    for (std::uint64_t spins = 0; spins < 20000000; ++spins) {
        net::UbNetOutSync(&interface, now);
        if (sender && link_up && !sent) {
            const std::uint64_t send_time =
                sync_mode == "off" ? kAsyncMessageTimePs : now;
            if (ctp_order) {
                while (next_order <= 2) {
                    auto* output = net::UbNetOutAlloc(&interface, send_time);
                    if (output == nullptr) break;
                    const std::uint8_t order = next_order++;
                    ZeroVolatile(output->frame);
                    output->frame.sequence = 100 + order;
                    output->frame.source_eid = local_eid;
                    output->frame.destination_eid = remote_eid;
                    output->frame.source_port = 1;
                    output->frame.length = sizeof(net::UdmaWireHeader) + 32;
                    net::UdmaWireHeader wire{};
                    wire.magic = net::kUdmaWireMagic;
                    wire.version = net::kUdmaWireVersion;
                    wire.operation = static_cast<std::uint8_t>(net::UdmaOperation::Send);
                    wire.flags = net::kUdmaWireLastFragment |
                                 net::UdmaWireOrder(order);
                    wire.source_jetty = 7;
                    wire.destination_jetty = 9;
                    wire.request_id = 1000 + order;
                    wire.transfer_length = 32;
                    wire.payload_length = 32;
                    auto* payload = reinterpret_cast<volatile std::uint8_t*>(output) +
                                    sizeof(net::Message);
                    const auto* wire_bytes = reinterpret_cast<const std::uint8_t*>(&wire);
                    for (std::size_t i = 0; i < sizeof(wire); ++i)
                        payload[i] = wire_bytes[i];
                    for (std::size_t i = 0; i < 32; ++i)
                        payload[sizeof(wire) + i] = order;
                    net::UbNetOutSend(&interface, output,
                        static_cast<std::uint8_t>(net::MessageType::Frame));
                }
                sent = next_order == 3;
            } else if (ctp_retrans || ctp_taack_loss || ctp_exhaust) {
                auto* output = net::UbNetOutAlloc(&interface, send_time);
                if (output != nullptr) {
                    ZeroVolatile(output->frame);
                    output->frame.sequence = 200;
                    output->frame.source_eid = local_eid;
                    output->frame.destination_eid = remote_eid;
                    output->frame.source_port = 1;
                    output->frame.length = sizeof(net::UdmaWireHeader) + 32;
                    net::UdmaWireHeader wire{};
                    wire.magic = net::kUdmaWireMagic;
                    wire.version = net::kUdmaWireVersion;
                    wire.operation = static_cast<std::uint8_t>(net::UdmaOperation::Write);
                    wire.flags = net::kUdmaWireLastFragment;
                    wire.source_jetty = 7;
                    wire.destination_jetty = 9;
                    wire.segment = 11;
                    wire.remote_address = 0x10000;
                    wire.request_id = 2000;
                    wire.transfer_length = 32;
                    wire.payload_length = 32;
                    auto* payload = reinterpret_cast<volatile std::uint8_t*>(output) +
                                    sizeof(net::Message);
                    const auto* wire_bytes = reinterpret_cast<const std::uint8_t*>(&wire);
                    for (std::size_t i = 0; i < sizeof(wire); ++i)
                        payload[i] = wire_bytes[i];
                    for (std::size_t i = 0; i < 32; ++i)
                        payload[sizeof(wire) + i] = 0x5a;
                    net::UbNetOutSend(&interface, output,
                        static_cast<std::uint8_t>(net::MessageType::Frame));
                    sent = true;
                }
            } else {
                auto* output = net::UbNetOutAlloc(&interface, send_time);
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
        if (!SimbricksBaseIfSyncEnabled(&interface.base))
            now = std::max(now,
                static_cast<std::uint64_t>(input->base.header.timestamp));
        if (type == net::MessageType::LinkState) {
            link_up = input->link.state ==
                      static_cast<std::uint8_t>(net::LinkState::Up);
            std::cerr << role << ": link state "
                      << (link_up ? "up" : "down") << '\n';
        } else if (type == net::MessageType::Frame) {
            if (ctp_retrans || ctp_taack_loss || ctp_exhaust) {
                bool valid = link_up &&
                             input->frame.source_eid == remote_eid &&
                             input->frame.destination_eid == local_eid &&
                             input->frame.length >= sizeof(net::UdmaWireHeader);
                net::UdmaWireHeader wire{};
                const auto* payload = reinterpret_cast<const volatile std::uint8_t*>(input) +
                                      sizeof(net::Message);
                auto* wire_bytes = reinterpret_cast<std::uint8_t*>(&wire);
                for (std::size_t i = 0; valid && i < sizeof(wire); ++i)
                    wire_bytes[i] = payload[i];
                valid = valid && wire.magic == net::kUdmaWireMagic &&
                        wire.version == net::kUdmaWireVersion &&
                        (wire.flags & net::kUdmaWireCtpSegment) &&
                        wire.request_id == 2000;
                if (sender) {
                    if (ctp_exhaust) {
                        valid = valid && wire.operation ==
                            static_cast<std::uint8_t>(net::UdmaOperation::RmaError) &&
                            wire.payload_length == 0;
                        net::UbNetInDone(&interface, input);
                        if (!valid) return 8;
                        std::cout << "sender: CTP retry exhaustion returned RMA error PASS\n";
                        SimbricksBaseIfClose(&interface.base);
                        SimbricksBaseIfSHMPoolUnmap(&pool);
                        SimbricksBaseIfSHMPoolUnlink(&pool);
                        return 0;
                    }
                    valid = valid && wire.operation ==
                        static_cast<std::uint8_t>(net::UdmaOperation::WriteAck) &&
                        wire.payload_length == 0;
                    net::UbNetInDone(&interface, input);
                    if (!valid) return 8;
                    std::cout << "sender: CTP WRITE completed after injected "
                              << (ctp_taack_loss ? "TAACK" : "request")
                              << " loss PASS\n";
                    SimbricksBaseIfClose(&interface.base);
                    SimbricksBaseIfSHMPoolUnmap(&pool);
                    SimbricksBaseIfSHMPoolUnlink(&pool);
                    return 0;
                }
                valid = valid && wire.operation ==
                    static_cast<std::uint8_t>(net::UdmaOperation::Write) &&
                    wire.payload_length == 32 &&
                    input->frame.length == sizeof(net::UdmaWireHeader) + 32;
                for (std::size_t i = 0; valid && i < 32; ++i)
                    valid = payload[sizeof(wire) + i] == 0x5a;
                net::UbNetInDone(&interface, input);
                if (!valid) return 8;
                for (;;) {
                    auto* output = net::UbNetOutAlloc(&interface, now);
                    if (output == nullptr) {
                        std::this_thread::yield();
                        continue;
                    }
                    ZeroVolatile(output->frame);
                    output->frame.sequence = 200;
                    output->frame.source_eid = local_eid;
                    output->frame.destination_eid = remote_eid;
                    output->frame.length = sizeof(net::UdmaWireHeader);
                    std::swap(wire.source_jetty, wire.destination_jetty);
                    wire.operation =
                        static_cast<std::uint8_t>(net::UdmaOperation::WriteAck);
                    wire.flags = net::kUdmaWireCtpSegment |
                                 net::kUdmaWireLastFragment;
                    wire.payload_length = 0;
                    auto* output_payload =
                        reinterpret_cast<volatile std::uint8_t*>(output) +
                        sizeof(net::Message);
                    const auto* response_bytes =
                        reinterpret_cast<const std::uint8_t*>(&wire);
                    for (std::size_t i = 0; i < sizeof(wire); ++i)
                        output_payload[i] = response_bytes[i];
                    net::UbNetOutSend(&interface, output,
                        static_cast<std::uint8_t>(net::MessageType::Frame));
                    std::cout << "receiver: executed one CTP WRITE for "
                              << (ctp_taack_loss ? "TAACK-loss" : "request-loss")
                              << " contract PASS\n";
                    for (std::uint64_t delay = 0; delay < 100000; ++delay)
                        std::this_thread::yield();
                    SimbricksBaseIfClose(&interface.base);
                    SimbricksBaseIfSHMPoolUnmap(&pool);
                    SimbricksBaseIfSHMPoolUnlink(&pool);
                    return 0;
                }
            }
            if (ctp_order) {
                bool valid = !sender && link_up &&
                             input->frame.source_eid == remote_eid &&
                             input->frame.destination_eid == local_eid &&
                             input->frame.length >= sizeof(net::UdmaWireHeader);
                net::UdmaWireHeader wire{};
                const auto* payload = reinterpret_cast<const volatile std::uint8_t*>(input) +
                                      sizeof(net::Message);
                auto* wire_bytes = reinterpret_cast<std::uint8_t*>(&wire);
                for (std::size_t i = 0; valid && i < sizeof(wire); ++i)
                    wire_bytes[i] = payload[i];
                valid = valid && wire.magic == net::kUdmaWireMagic &&
                        wire.version == net::kUdmaWireVersion &&
                        (wire.flags & net::kUdmaWireCtpSegment) &&
                        net::UdmaWireOrder(wire.flags) <= 2;
                const std::uint8_t order = net::UdmaWireOrder(wire.flags);
                valid = valid && order <= 2 &&
                        (seen_orders & static_cast<std::uint8_t>(1U << order)) == 0 &&
                        (order != 2 || relax_seen) &&
                        wire.payload_length == 32 &&
                        input->frame.length == sizeof(net::UdmaWireHeader) + 32;
                for (std::size_t i = 0; valid && i < 32; ++i)
                    valid = payload[sizeof(wire) + i] == order;
                if (!valid) {
                    std::cerr << "receiver: invalid native CTP frame"
                              << " src=0x" << std::hex << input->frame.source_eid
                              << " dst=0x" << input->frame.destination_eid
                              << std::dec << " frame_length=" << input->frame.length
                              << " sequence=" << input->frame.sequence
                              << " magic=0x" << std::hex << wire.magic << std::dec
                              << " version=" << static_cast<unsigned>(wire.version)
                              << " flags=0x" << std::hex << wire.flags << std::dec
                              << " order=" << static_cast<unsigned>(order)
                              << " payload_length=" << wire.payload_length
                              << " request_id=" << wire.request_id
                              << " seen=0x" << std::hex
                              << static_cast<unsigned>(seen_orders) << std::dec << '\n';
                    net::UbNetInDone(&interface, input);
                    return 7;
                }
                net::UbNetInDone(&interface, input);
                seen_orders |= static_cast<std::uint8_t>(1U << order);
                relax_seen = relax_seen || order == 1;
                if (seen_orders == 0x7) {
                    std::cout << "receiver: native CTP preserved NO/RO/SO ordering PASS\n";
                    SimbricksBaseIfClose(&interface.base);
                    SimbricksBaseIfSHMPoolUnmap(&pool);
                    SimbricksBaseIfSHMPoolUnlink(&pool);
                    return 0;
                }
                continue;
            }
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
    std::cerr << role << ": timed out link_up=" << link_up
              << " sent=" << sent << " now=" << now << '\n';
    return 6;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 5 && argc != 6) {
        std::cerr << "usage: ub-net-contract-peer sender|receiver SOCKET SHM "
                     "off|required [raw|ctp-order|ctp-retrans|ctp-taack-loss|ctp-exhaust]\n";
        return 2;
    }
    return Run(argv[1], argv[2], argv[3], argv[4],
               argc == 6 ? argv[5] : "raw");
}
