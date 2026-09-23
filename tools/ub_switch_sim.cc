// SPDX-License-Identifier: Apache-2.0
// Minimal discrete-event UB L1 switch for the cross-process adapter ABI.

#include "../sources/OpenURMA/eval/twonode/gem5_scaffold/src/UbAdapterProtocol.hh"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace oa = openurma_adapter;
namespace {

volatile std::sig_atomic_t stop_requested = 0;

void stopHandler(int) { stop_requested = 1; }

uint64_t parseUnsigned(const char *text, const char *name)
{
    std::size_t used = 0;
    const std::string value(text);
    uint64_t result = 0;
    try {
        result = std::stoull(value, &used, 0);
    } catch (const std::exception &) {
        throw std::runtime_error(std::string("invalid ") + name + ": " + value);
    }
    if (used != value.size())
        throw std::runtime_error(std::string("invalid ") + name + ": " + value);
    return result;
}

uint64_t parseTimeTicks(const char *text, const char *name)
{
    const std::string value(text);
    struct Suffix { std::string_view text; uint64_t ticks; };
    constexpr Suffix suffixes[] = {
        {"ps", 1}, {"ns", 1000}, {"us", 1000000},
        {"ms", 1000000000ULL}, {"s", 1000000000000ULL}, {"t", 1}
    };
    for (const auto &suffix : suffixes) {
        if (value.size() > suffix.text.size() &&
            value.compare(value.size() - suffix.text.size(),
                          suffix.text.size(), suffix.text) == 0) {
            const std::string number = value.substr(
                0, value.size() - suffix.text.size());
            std::size_t used = 0;
            long double amount = 0;
            try {
                amount = std::stold(number, &used);
            } catch (const std::exception &) {
                throw std::runtime_error(std::string("invalid ") + name +
                                         ": " + value);
            }
            if (used != number.size() || amount < 0 ||
                amount > static_cast<long double>(UINT64_MAX) / suffix.ticks)
                throw std::runtime_error(std::string(name) + " overflows ticks");
            return static_cast<uint64_t>(std::ceil(amount * suffix.ticks));
        }
    }
    return parseUnsigned(text, name);
}

struct Mapping {
    int fd = -1;
    oa::Ring *ring = nullptr;
    std::size_t bytes = 0;
    uint32_t ports = 0;
    std::string path;

    Mapping(const std::string &p, uint32_t count) : ports(count), path(p)
    {
        bytes = oa::mappedBytes(ports);
        fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0)
            throw std::runtime_error("open " + path + ": " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size != static_cast<off_t>(bytes))
            throw std::runtime_error(path + " has unexpected size");
        void *address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
        if (address == MAP_FAILED)
            throw std::runtime_error("mmap " + path + ": " + std::strerror(errno));
        ring = static_cast<oa::Ring *>(address);
    }

    ~Mapping()
    {
        if (ring)
            ::munmap(ring, bytes);
        if (fd >= 0)
            ::close(fd);
    }

    uint8_t *slot(uint32_t direction, uint32_t port, uint64_t index)
    {
        return reinterpret_cast<uint8_t *>(ring) +
            oa::slotOffset(ports, direction, port, index);
    }
};

uint64_t checkedAdd(uint64_t lhs, uint64_t rhs)
{
    if (rhs > UINT64_MAX - lhs)
        throw std::runtime_error("virtual-time overflow");
    return lhs + rhs;
}

struct Switch {
    std::vector<std::unique_ptr<Mapping>> links;
    std::vector<uint32_t> endpoint_eids;
    uint32_t ports;
    uint64_t link_latency_ticks;
    uint64_t switch_delay_ticks;
    uint64_t rate_gbps;
    uint32_t overhead_bytes;
    uint32_t serialization_stages;
    std::vector<uint32_t> port_map;
    std::vector<std::vector<std::vector<uint64_t>>> egress_free;
    std::vector<std::vector<uint64_t>> last_output_timestamp;
    std::vector<std::vector<uint64_t>> last_sync_source_time;
    std::vector<std::vector<uint64_t>> input_grant;
    uint64_t virtual_time = 0;
    bool virtual_time_started = false;
    uint64_t forwarded = 0;
    uint64_t bytes = 0;
    uint64_t sync_messages = 0;

    Switch(const std::vector<std::string> &paths,
           std::vector<uint32_t> eids, uint32_t p,
           uint64_t latency, uint64_t delay, uint64_t rate,
           uint32_t overhead, std::vector<uint32_t> mapping,
           uint32_t stages)
      : endpoint_eids(std::move(eids)), ports(p),
        link_latency_ticks(latency), switch_delay_ticks(delay),
        rate_gbps(rate), overhead_bytes(overhead),
        serialization_stages(stages), port_map(std::move(mapping)),
        egress_free(paths.size(), std::vector<std::vector<uint64_t>>(
            p, std::vector<uint64_t>(stages))),
        last_output_timestamp(paths.size(), std::vector<uint64_t>(p)),
        last_sync_source_time(paths.size(),
                              std::vector<uint64_t>(p, UINT64_MAX)),
        input_grant(paths.size(), std::vector<uint64_t>(p))
    {
        if (paths.size() < 2 || paths.size() != endpoint_eids.size())
            throw std::runtime_error(
                "ring path and endpoint-EID counts must match and be at least two");
        links.reserve(paths.size());
        for (const auto &path : paths)
            links.emplace_back(std::make_unique<Mapping>(path, p));
        for (uint32_t endpoint = 0; endpoint < endpoint_eids.size(); ++endpoint) {
            if (endpoint_eids[endpoint] == 0 || endpoint_eids[endpoint] > 0xfffff)
                throw std::runtime_error("endpoint EIDs must be non-zero 20-bit values");
            for (uint32_t previous = 0; previous < endpoint; ++previous)
                if ((endpoint_eids[previous] & 0xffffU) ==
                    (endpoint_eids[endpoint] & 0xffffU))
                    throw std::runtime_error(
                        "endpoint EIDs must have unique low 16 bits");
        }
        if (serialization_stages != 0 && rate_gbps == 0)
            throw std::runtime_error("serialization requires a positive line rate");
    }

    uint32_t routeEndpoint(uint32_t eid) const
    {
        if (eid == 0 || eid > 0xfffff)
            throw std::runtime_error("packet carries an invalid destination EID");
        // The modeled UDMA device exposes a primary EID plus physical-port
        // aliases at +0x10000 increments. They all terminate at the same
        // endpoint adapter, so the low 16 bits are the route key.
        for (uint32_t endpoint = 0; endpoint < endpoint_eids.size(); ++endpoint)
            if ((endpoint_eids[endpoint] & 0xffffU) == (eid & 0xffffU))
                return endpoint;
        throw std::runtime_error("destination EID is not registered on this switch");
    }

    uint64_t serializationTicks(uint64_t wire_bytes) const
    {
        if (rate_gbps == 0 || wire_bytes == 0)
            return 0;
        // gem5's default tick is 1 ps: one byte at 1 Gbit/s takes 8000 ps.
        return (wire_bytes * 8000ULL + rate_gbps - 1) / rate_gbps;
    }

    bool consumeOne(uint32_t source_link, uint32_t source_port)
    {
        constexpr uint32_t EndpointToSwitch = 1;
        constexpr uint32_t SwitchToEndpoint = 0;
        Mapping &input = *links[source_link];
        auto &in = input.ring->direction[EndpointToSwitch][source_port];
        const uint64_t tail = __atomic_load_n(&in.tail.value, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&in.head.value, __ATOMIC_ACQUIRE);
        if (tail == head)
            return false;

        const auto *source = reinterpret_cast<const oa::MessageHeader *>(
            input.slot(EndpointToSwitch, source_port, tail));
        if (source->protocol_version != oa::Version)
            throw std::runtime_error("adapter protocol version mismatch");
        if (source->source_port != source_port)
            throw std::runtime_error("message published on wrong ingress port");
        if (source->receive_tick > virtual_time)
            return false;

        const auto type = static_cast<oa::MessageType>(source->type);
        input_grant[source_link][source_port] = std::max(
            input_grant[source_link][source_port], source->receive_tick);
        if (type == oa::MessageType::Sync) {
            if (source->payload_length != 0 || source->source_eid != 0 ||
                source->destination_eid != 0)
                throw std::runtime_error(
                    "link-local SYNC must not carry payload or EIDs");
            __atomic_store_n(&in.tail.value, tail + 1, __ATOMIC_RELEASE);
            return true;
        }
        if (type != oa::MessageType::Data)
            throw std::runtime_error("unsupported adapter message type");
        if ((source->source_eid & 0xffffU) !=
            (endpoint_eids[source_link] & 0xffffU))
            throw std::runtime_error("message source EID does not match ingress endpoint");
        const uint32_t destination_link = routeEndpoint(source->destination_eid);
        if (destination_link == source_link)
            throw std::runtime_error("switch-adapter packet loops back to its source");
        Mapping &output = *links[destination_link];
        const uint32_t destination_port = port_map[source_port];
        auto &out = output.ring->direction[SwitchToEndpoint][destination_port];
        const uint64_t out_head = __atomic_load_n(&out.head.value, __ATOMIC_RELAXED);
        const uint64_t out_tail = __atomic_load_n(&out.tail.value, __ATOMIC_ACQUIRE);
        if (out_head - out_tail >= oa::RingSlots)
            return false;

        uint8_t *destination_slot = output.slot(
            SwitchToEndpoint, destination_port, out_head);
        auto *destination = reinterpret_cast<oa::MessageHeader *>(destination_slot);
        *destination = *source;
        destination->destination_port = destination_port;

        if (source->payload_length < 40 ||
            source->payload_length > oa::SlotBytes - sizeof(*source))
            throw std::runtime_error("invalid DATA payload length");
        std::memcpy(destination_slot + sizeof(*destination),
                    reinterpret_cast<const uint8_t *>(source) + sizeof(*source),
                    source->payload_length);
        const uint64_t payload = source->payload_length - 40;
        uint64_t ready = checkedAdd(source->receive_tick, switch_delay_ticks);
        for (uint32_t stage = 0; stage < serialization_stages; ++stage) {
            auto &free = egress_free[destination_link][destination_port][stage];
            ready = std::max(ready, free);
            ready = checkedAdd(ready,
                serializationTicks(payload + overhead_bytes));
            free = ready;
        }
        destination->receive_tick = std::max(
            checkedAdd(ready, link_latency_ticks),
            last_output_timestamp[destination_link][destination_port]);
        last_output_timestamp[destination_link][destination_port] =
            destination->receive_tick;
        ++forwarded;
        bytes += payload;

        std::atomic_thread_fence(std::memory_order_release);
        __atomic_store_n(&out.head.value, out_head + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&in.tail.value, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    uint64_t safeTime()
    {
        constexpr uint32_t EndpointToSwitch = 1;
        uint64_t safe = UINT64_MAX;
        for (uint32_t endpoint = 0; endpoint < links.size(); ++endpoint) {
            for (uint32_t port = 0; port < ports; ++port) {
                Mapping &input = *links[endpoint];
                const auto &in = input.ring->direction[EndpointToSwitch][port];
                const uint64_t tail = __atomic_load_n(
                    &in.tail.value, __ATOMIC_RELAXED);
                const uint64_t head = __atomic_load_n(
                    &in.head.value, __ATOMIC_ACQUIRE);
                uint64_t horizon = input_grant[endpoint][port];
                if (tail != head) {
                    const auto *message =
                        reinterpret_cast<const oa::MessageHeader *>(
                            input.slot(EndpointToSwitch, port, tail));
                    if (message->protocol_version != oa::Version)
                        throw std::runtime_error(
                            "adapter protocol version mismatch");
                    if (message->receive_tick < horizon)
                        throw std::runtime_error(
                            "input FIFO timestamp moved behind its promise");
                    horizon = message->receive_tick;
                }
                safe = std::min(safe, horizon);
            }
        }
        return safe;
    }

    bool publishSync(uint32_t endpoint, uint32_t port)
    {
        constexpr uint32_t SwitchToEndpoint = 0;
        if (last_sync_source_time[endpoint][port] == virtual_time)
            return false;
        Mapping &output = *links[endpoint];
        auto &out = output.ring->direction[SwitchToEndpoint][port];
        const uint64_t head = __atomic_load_n(
            &out.head.value, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(
            &out.tail.value, __ATOMIC_ACQUIRE);
        if (head - tail >= oa::RingSlots)
            return false;
        uint8_t *slot = output.slot(SwitchToEndpoint, port, head);
        auto *message = reinterpret_cast<oa::MessageHeader *>(slot);
        std::memset(message, 0, sizeof(*message));
        message->type = static_cast<uint32_t>(oa::MessageType::Sync);
        message->send_tick = virtual_time;
        message->receive_tick = std::max(
            checkedAdd(virtual_time, link_latency_ticks),
            last_output_timestamp[endpoint][port]);
        message->sequence = ++sync_messages;
        message->source_port = port;
        message->destination_port = port;
        message->protocol_version = oa::Version;
        last_output_timestamp[endpoint][port] = message->receive_tick;
        last_sync_source_time[endpoint][port] = virtual_time;
        std::atomic_thread_fence(std::memory_order_release);
        __atomic_store_n(&out.head.value, head + 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&output.ring->switch_sync_messages, 1,
                           __ATOMIC_RELEASE);
        return true;
    }

    void run()
    {
        std::cerr << "[UB_SWITCH] ready endpoints=" << links.size()
                  << " ports=" << ports
                  << " rate_gbps=" << rate_gbps
                  << " link_latency_ticks=" << link_latency_ticks
                  << " switch_delay_ticks=" << switch_delay_ticks << '\n';
        while (!stop_requested) {
            bool progress = false;
            // Consume every event that is now causally executable. SYNC is
            // terminated here and advances the input link's promise; DATA is
            // routed only after its switch-arrival timestamp is reached.
            bool consumed = false;
            do {
                consumed = false;
                for (uint32_t endpoint = 0; endpoint < links.size(); ++endpoint)
                    for (uint32_t port = 0; port < ports; ++port)
                        consumed |= consumeOne(endpoint, port);
                progress |= consumed;
            } while (consumed);

            // The switch is an independent conservative simulator. It may
            // advance only to the minimum next-message/null-message timestamp
            // across every physical ingress link.
            const uint64_t safe = safeTime();
            if (safe > virtual_time) {
                virtual_time = safe;
                virtual_time_started = true;
                progress = true;
                continue;
            }

            // Advertise the switch's new safe time independently on every
            // egress link.  This is link control, never an EID-routed packet.
            if (virtual_time_started) {
                for (uint32_t endpoint = 0; endpoint < links.size(); ++endpoint)
                    for (uint32_t port = 0; port < ports; ++port)
                        progress |= publishSync(endpoint, port);
            }

            // Like the SimBricks proxy, synchronized operation busy-polls its
            // shared-memory queues; virtual time, not host sleep, gates work.
            if (!progress)
                std::this_thread::yield();
        }
        std::cerr << "[UB_SWITCH_STATS] forwarded=" << forwarded
                  << " payload_bytes=" << bytes
                  << " sync_messages=" << sync_messages
                  << " virtual_ticks=" << virtual_time << '\n';
    }
};

std::vector<uint32_t> parseMap(const std::string &text, uint32_t ports)
{
    if (text.empty()) {
        std::vector<uint32_t> identity(ports);
        for (uint32_t i = 0; i < ports; ++i)
            identity[i] = i;
        return identity;
    }
    std::vector<uint32_t> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string token = text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const uint64_t port = parseUnsigned(token.c_str(), "port map");
        if (port >= ports)
            throw std::runtime_error("port map entry outside port count");
        result.push_back(static_cast<uint32_t>(port));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    if (result.size() != ports)
        throw std::runtime_error("port map must contain one entry per port");
    return result;
}

std::vector<uint32_t> parseEndpointEids(const std::string &text)
{
    if (text.empty())
        throw std::runtime_error("endpoint EID list cannot be empty");
    std::vector<uint32_t> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string token = text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        result.push_back(static_cast<uint32_t>(
            parseUnsigned(token.c_str(), "endpoint EID")));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    const bool multi = argc >= 2 && std::string_view(argv[1]) == "--multi";
    if ((!multi && argc != 10) || (multi && argc < 12)) {
        std::cerr << "usage: ub-switch-sim RING0 RING1 PORTS LINK_LATENCY "
                     "SWITCH_DELAY RATE_GBPS OVERHEAD_BYTES PORT_MAP "
                     "SERIALIZATION_STAGES\n"
                     "   or: ub-switch-sim --multi PORTS LINK_LATENCY "
                     "SWITCH_DELAY RATE_GBPS OVERHEAD_BYTES PORT_MAP "
                     "SERIALIZATION_STAGES ENDPOINT_EIDS RING...\n";
        return 2;
    }
    try {
        const int base = multi ? 2 : 3;
        const uint32_t ports = static_cast<uint32_t>(
            parseUnsigned(argv[base], "ports"));
        if (ports == 0 || ports > oa::MaxPorts)
            throw std::runtime_error("ports must be in [1,16]");
        std::vector<std::string> paths;
        std::vector<uint32_t> peers;
        if (multi) {
            peers = parseEndpointEids(argv[9]);
            for (int index = 10; index < argc; ++index)
                paths.emplace_back(argv[index]);
        } else {
            paths = {argv[1], argv[2]};
            peers = {0x100, 0x101};
        }
        Switch model(paths, std::move(peers), ports,
            parseTimeTicks(argv[base + 1], "link latency"),
            parseTimeTicks(argv[base + 2], "switch delay"),
            parseUnsigned(argv[base + 3], "rate"),
            static_cast<uint32_t>(
                parseUnsigned(argv[base + 4], "overhead")),
            parseMap(argv[base + 5], ports),
            static_cast<uint32_t>(
                parseUnsigned(argv[base + 6], "serialization stages")));
        std::signal(SIGINT, stopHandler);
        std::signal(SIGTERM, stopHandler);
        model.run();
    } catch (const std::exception &error) {
        std::cerr << "ub-switch-sim: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
