// SPDX-License-Identifier: Apache-2.0
#include "protocol/ub_net/if.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net = openurma::proto::net;

namespace {

std::atomic<bool> running{true};
void Stop(int) { running.store(false); }

struct EndpointOption {
    std::string socket;
    std::uint32_t eid{};
};

struct Options {
    std::vector<EndpointOption> endpoints;
    std::uint64_t link_latency_ps{100000};
    std::uint64_t sync_interval_ps{100000};
    SimbricksBaseIfSyncMode sync_mode{kSimbricksBaseIfSyncOptional};
};

bool ParseUnsigned(const std::string& value, std::uint64_t& output,
                   bool allow_zero = false)
{
    try {
        std::size_t consumed = 0;
        output = std::stoull(value, &consumed, 0);
        return consumed == value.size() && (allow_zero || output != 0);
    } catch (...) {
        return false;
    }
}

bool ParseEndpoint(const std::string& value, EndpointOption& endpoint)
{
    const auto separator = value.rfind(',');
    if (separator == std::string::npos || separator == 0) return false;
    std::uint64_t eid = 0;
    if (!ParseUnsigned(value.substr(separator + 1), eid) || eid > 0xcffff)
        return false;
    endpoint.socket = value.substr(0, separator);
    endpoint.eid = static_cast<std::uint32_t>(eid);
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--endpoint" && i + 1 < argc) {
            EndpointOption endpoint;
            if (!ParseEndpoint(argv[++i], endpoint)) return false;
            options.endpoints.push_back(std::move(endpoint));
        } else if (arg == "--link-latency-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.link_latency_ps, true)) return false;
        } else if (arg == "--sync-interval-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.sync_interval_ps)) return false;
        } else if (arg == "--sync" && i + 1 < argc) {
            const std::string mode(argv[++i]);
            if (mode == "off") options.sync_mode = kSimbricksBaseIfSyncDisabled;
            else if (mode == "optional") options.sync_mode = kSimbricksBaseIfSyncOptional;
            else if (mode == "required") options.sync_mode = kSimbricksBaseIfSyncRequired;
            else return false;
        } else {
            return false;
        }
    }
    if (options.endpoints.size() < 2) return false;
    std::unordered_map<std::uint32_t, bool> eids;
    for (const auto& endpoint : options.endpoints)
        if (!eids.emplace(endpoint.eid, true).second) return false;
    return true;
}

struct QueuedMessage {
    net::MessageType type{net::MessageType::Frame};
    net::Frame frame{};
    net::LinkControl link{};
    std::vector<std::uint8_t> payload;
};

struct Endpoint {
    net::Interface interface{};
    SimbricksBaseIfParams params{};
    net::Intro peer_intro{};
    std::string socket;
    std::uint32_t eid{};
    std::deque<QueuedMessage> outgoing;
};

template <typename T>
void ZeroVolatile(volatile T& object)
{
    const std::uint64_t timestamp = object.timestamp;
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
    object.timestamp = timestamp;
}

void CopyFrameFromVolatile(net::Frame& destination,
                           const volatile net::Frame& source)
{
    auto* output = reinterpret_cast<std::uint8_t*>(&destination);
    const auto* input = reinterpret_cast<const volatile std::uint8_t*>(&source);
    for (std::size_t i = 0; i < sizeof(destination); ++i) output[i] = input[i];
}

void CopyLinkFromVolatile(net::LinkControl& destination,
                          const volatile net::LinkControl& source)
{
    auto* output = reinterpret_cast<std::uint8_t*>(&destination);
    const auto* input = reinterpret_cast<const volatile std::uint8_t*>(&source);
    for (std::size_t i = 0; i < sizeof(destination); ++i) output[i] = input[i];
}

bool Flush(Endpoint& endpoint, std::uint64_t now)
{
    bool progress = false;
    while (!endpoint.outgoing.empty()) {
        auto* output = net::UbNetOutAlloc(&endpoint.interface, now);
        if (output == nullptr) break;
        auto& queued = endpoint.outgoing.front();
        if (queued.type == net::MessageType::Frame) {
            ZeroVolatile(output->frame);
            const auto saved_timestamp = output->frame.timestamp;
            std::memcpy(const_cast<net::Frame*>(&output->frame), &queued.frame,
                        sizeof(queued.frame));
            output->frame.timestamp = saved_timestamp;
            auto* destination = reinterpret_cast<volatile std::uint8_t*>(output) +
                                sizeof(net::Message);
            for (std::size_t i = 0; i < queued.payload.size(); ++i)
                destination[i] = queued.payload[i];
        } else {
            ZeroVolatile(output->link);
            const auto saved_timestamp = output->link.timestamp;
            std::memcpy(const_cast<net::LinkControl*>(&output->link), &queued.link,
                        sizeof(queued.link));
            output->link.timestamp = saved_timestamp;
        }
        net::UbNetOutSend(&endpoint.interface, output,
                          static_cast<std::uint8_t>(queued.type));
        endpoint.outgoing.pop_front();
        progress = true;
    }
    return progress;
}

int Run(const Options& options)
{
    std::vector<Endpoint> endpoints(options.endpoints.size());
    std::vector<SimBricksBaseIfEstablishData> establish(endpoints.size());
    std::vector<net::Intro> introductions(endpoints.size());

    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto& endpoint = endpoints[i];
        endpoint.socket = options.endpoints[i].socket;
        endpoint.eid = options.endpoints[i].eid;
        net::DefaultParams(&endpoint.params);
        endpoint.params.sock_path = endpoint.socket.c_str();
        endpoint.params.link_latency = options.link_latency_ps;
        endpoint.params.sync_interval = options.sync_interval_ps;
        endpoint.params.sync_mode = options.sync_mode;
        if (SimbricksBaseIfInit(&endpoint.interface.base, &endpoint.params) != 0 ||
            SimbricksBaseIfConnect(&endpoint.interface.base) != 0)
            return 1;
        introductions[i] = {net::kVersion, 1, 16384, 32, 0};
        establish[i] = {&endpoint.interface.base, &introductions[i],
                        sizeof(introductions[i]), &endpoint.peer_intro,
                        sizeof(endpoint.peer_intro)};
    }
    if (SimBricksBaseIfEstablish(establish.data(), establish.size()) != 0) return 1;
    for (const auto& endpoint : endpoints)
        if (endpoint.peer_intro.version != net::kVersion) return 2;

    std::unordered_map<std::uint32_t, std::size_t> routes;
    for (std::size_t i = 0; i < endpoints.size(); ++i) routes[endpoints[i].eid] = i;
    for (auto& endpoint : endpoints) {
        for (std::uint16_t port = 0; port < endpoint.peer_intro.port_count; ++port) {
            QueuedMessage message;
            message.type = net::MessageType::LinkState;
            message.link.port = port;
            message.link.state = static_cast<std::uint8_t>(net::LinkState::Up);
            endpoint.outgoing.push_back(std::move(message));
        }
    }
    std::cout << "ub-switch-sim: connected " << endpoints.size()
              << " UB-NET endpoints\n";

    std::uint64_t now = 0;
    while (running.load()) {
        bool progress = false;
        bool terminated = false;
        for (std::size_t source_index = 0; source_index < endpoints.size(); ++source_index) {
            auto& source = endpoints[source_index];
            while (auto* input = net::UbNetInPoll(&source.interface, now)) {
                const auto type = static_cast<net::MessageType>(
                    net::UbNetInType(&source.interface, input));
                if (type == net::MessageType::Frame) {
                    const std::uint32_t destination_eid =
                        input->frame.destination_eid;
                    const auto route = routes.find(destination_eid);
                    if (route != routes.end() && route->second != source_index &&
                        input->frame.length <= source.peer_intro.max_frame_bytes) {
                        QueuedMessage queued;
                        queued.type = type;
                        CopyFrameFromVolatile(queued.frame, input->frame);
                        const auto* payload =
                            reinterpret_cast<const volatile std::uint8_t*>(input) +
                            sizeof(net::Message);
                        queued.payload.resize(input->frame.length);
                        for (std::size_t i = 0; i < queued.payload.size(); ++i)
                            queued.payload[i] = payload[i];
                        endpoints[route->second].outgoing.push_back(std::move(queued));
                    }
                } else if (type == net::MessageType::LinkState) {
                    QueuedMessage queued;
                    queued.type = type;
                    CopyLinkFromVolatile(queued.link, input->link);
                    source.outgoing.push_back(std::move(queued));
                }
                net::UbNetInDone(&source.interface, input);
                progress = true;
            }
            terminated = terminated || SimbricksBaseIfInTerminated(&source.interface.base);
        }
        for (auto& endpoint : endpoints) {
            progress = Flush(endpoint, now) || progress;
            net::UbNetOutSync(&endpoint.interface, now);
        }
        if (terminated) break;
        bool synchronized = false;
        std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
        for (auto& endpoint : endpoints) {
            if (!SimbricksBaseIfSyncEnabled(&endpoint.interface.base)) continue;
            synchronized = true;
            next = std::min(next,
                net::UbNetInTimestamp(&endpoint.interface));
            next = std::min(next,
                net::UbNetOutNextSync(&endpoint.interface));
        }
        if (!synchronized)
            now += options.sync_interval_ps;
        else if (next > now && next != std::numeric_limits<std::uint64_t>::max())
            now = next;
        if (!progress) std::this_thread::yield();
    }
    for (auto& endpoint : endpoints) SimbricksBaseIfClose(&endpoint.interface.base);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "usage: ub-switch-sim --endpoint SOCKET,EID "
                     "--endpoint SOCKET,EID [--endpoint ...] "
                     "[--sync off|optional|required] "
                     "[--link-latency-ps N] [--sync-interval-ps N]\n";
        return 2;
    }
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    return Run(options);
}
