// SPDX-License-Identifier: GPL-2.0-only
// Compatibility bridge between gem5 OpenURMA endpoint rings and ns-3 time.

#include "ns3/core-module.h"
#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/string.h"
#include "ns3/tag.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-external-adapter-protocol.h"
#include "ns3/ub-header.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ns3;
namespace protocol = ns3::ub_external;

namespace
{

volatile std::sig_atomic_t g_stopRequested = 0;

void
RequestStop(int)
{
    g_stopRequested = 1;
}

uint64_t
ParseUnsigned(const std::string& text, const char* name)
{
    std::size_t consumed = 0;
    uint64_t value = 0;
    try
    {
        value = std::stoull(text, &consumed, 0);
    }
    catch (const std::exception&)
    {
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    }
    if (consumed != text.size())
    {
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    }
    return value;
}

uint64_t
ParsePicoseconds(const std::string& text, const char* name)
{
    struct Suffix
    {
        std::string_view text;
        uint64_t multiplier;
    };

    constexpr Suffix suffixes[] = {
        {"ps", 1},
        {"ns", 1000},
        {"us", 1000000},
        {"ms", 1000000000ULL},
        {"s", 1000000000000ULL},
        {"t", 1},
    };
    for (const auto& suffix : suffixes)
    {
        if (text.size() <= suffix.text.size() ||
            text.compare(text.size() - suffix.text.size(), suffix.text.size(), suffix.text) != 0)
        {
            continue;
        }
        const std::string number = text.substr(0, text.size() - suffix.text.size());
        std::size_t consumed = 0;
        long double amount = 0;
        try
        {
            amount = std::stold(number, &consumed);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error(std::string("invalid ") + name + ": " + text);
        }
        if (consumed != number.size() || amount < 0 ||
            amount >
                static_cast<long double>(std::numeric_limits<uint64_t>::max()) / suffix.multiplier)
        {
            throw std::runtime_error(std::string(name) + " overflows picoseconds");
        }
        return static_cast<uint64_t>(std::ceil(amount * suffix.multiplier));
    }
    return ParseUnsigned(text, name);
}

uint64_t
CheckedAdd(uint64_t left, uint64_t right)
{
    if (right > std::numeric_limits<uint64_t>::max() - left)
    {
        throw std::runtime_error("virtual-time overflow");
    }
    return left + right;
}

std::vector<uint32_t>
ParseList(const std::string& text, const char* name)
{
    if (text.empty())
    {
        throw std::runtime_error(std::string(name) + " cannot be empty");
    }
    std::vector<uint32_t> values;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t end = text.find(',', begin);
        const std::string item =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const uint64_t value = ParseUnsigned(item, name);
        if (value > std::numeric_limits<uint32_t>::max())
        {
            throw std::runtime_error(std::string(name) + " exceeds 32 bits");
        }
        values.push_back(static_cast<uint32_t>(value));
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return values;
}

class Mapping
{
  public:
    Mapping(std::string path, uint32_t ports)
        : m_path(std::move(path)),
          m_ports(ports),
          m_bytes(protocol::MappedBytes(ports))
    {
        m_fd = ::open(m_path.c_str(), O_RDWR);
        if (m_fd < 0)
        {
            throw std::runtime_error("open " + m_path + ": " + std::strerror(errno));
        }
        struct stat status{};
        if (::fstat(m_fd, &status) != 0 || status.st_size != static_cast<off_t>(m_bytes))
        {
            throw std::runtime_error(m_path + " has unexpected size");
        }
        void* address = ::mmap(nullptr, m_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (address == MAP_FAILED)
        {
            throw std::runtime_error("mmap " + m_path + ": " + std::strerror(errno));
        }
        m_ring = static_cast<protocol::Ring*>(address);
    }

    ~Mapping()
    {
        if (m_ring)
        {
            ::munmap(m_ring, m_bytes);
        }
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    protocol::Ring* Ring() const
    {
        return m_ring;
    }

    uint8_t* Slot(uint32_t direction, uint32_t port, uint64_t index) const
    {
        return reinterpret_cast<uint8_t*>(m_ring) +
               protocol::SlotOffset(m_ports, direction, port, index);
    }

  private:
    std::string m_path;
    uint32_t m_ports;
    std::size_t m_bytes;
    int m_fd{-1};
    protocol::Ring* m_ring{nullptr};
};

struct PendingRecord
{
    protocol::MessageHeader header{};
    std::vector<uint8_t> payload;
    Ptr<Packet> networkPacket;
    uint32_t destinationEndpoint{0};
    uint32_t destinationPort{0};
};

class AdapterMetadataTag : public Tag
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::AdapterMetadataTag")
                                .SetParent<Tag>()
                                .SetGroupName("UnifiedBus")
                                .AddConstructor<AdapterMetadataTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    uint32_t GetSerializedSize() const override
    {
        return 32;
    }

    void Serialize(TagBuffer buffer) const override
    {
        buffer.WriteU64(m_sendTick);
        buffer.WriteU64(m_sequence);
        buffer.WriteU32(m_sourceEid);
        buffer.WriteU32(m_destinationEid);
        buffer.WriteU16(m_sourcePort);
        buffer.WriteU16(m_flags);
        buffer.WriteU32(0);
    }

    void Deserialize(TagBuffer buffer) override
    {
        m_sendTick = buffer.ReadU64();
        m_sequence = buffer.ReadU64();
        m_sourceEid = buffer.ReadU32();
        m_destinationEid = buffer.ReadU32();
        m_sourcePort = buffer.ReadU16();
        m_flags = buffer.ReadU16();
        (void)buffer.ReadU32();
    }

    void Print(std::ostream& stream) const override
    {
        stream << "sequence=" << m_sequence << " sourceEid=" << m_sourceEid
               << " destinationEid=" << m_destinationEid;
    }

    uint64_t m_sendTick{0};
    uint64_t m_sequence{0};
    uint32_t m_sourceEid{0};
    uint32_t m_destinationEid{0};
    uint16_t m_sourcePort{0};
    uint16_t m_flags{0};
};

NS_OBJECT_ENSURE_REGISTERED(AdapterMetadataTag);

class ExternalFabric
{
  public:
    ExternalFabric(std::vector<std::string> paths,
                   std::vector<uint32_t> endpointEids,
                   uint32_t ports,
                   uint64_t linkDelayPs,
                   uint64_t switchDelayPs,
                   uint64_t rateGbps,
                   uint32_t overheadBytes,
                   std::vector<uint32_t> portMap,
                   uint32_t serializationStages)
        : m_endpointEids(std::move(endpointEids)),
          m_ports(ports),
          m_linkDelayPs(linkDelayPs),
          m_switchDelayPs(switchDelayPs),
          m_rateGbps(rateGbps),
          m_overheadBytes(overheadBytes),
          m_portMap(std::move(portMap)),
          m_serializationStages(serializationStages),
          m_egressFree(
              paths.size(),
              std::vector<std::vector<uint64_t>>(ports,
                                                 std::vector<uint64_t>(serializationStages))),
          m_lastOutput(paths.size(), std::vector<uint64_t>(ports)),
          m_reservedOutputs(paths.size(), std::vector<uint64_t>(ports)),
          m_lastGrant(paths.size(), std::vector<uint64_t>(ports)),
          m_ingressPromise(paths.size(), std::vector<uint64_t>(ports))
    {
        if (paths.size() < 2 || paths.size() != m_endpointEids.size())
        {
            throw std::runtime_error("ring and endpoint-EID counts must match and be at least two");
        }
        for (const auto& path : paths)
        {
            m_links.emplace_back(std::make_unique<Mapping>(path, ports));
        }
        for (std::size_t endpoint = 0; endpoint < m_endpointEids.size(); ++endpoint)
        {
            if (m_endpointEids[endpoint] == 0 || m_endpointEids[endpoint] > 0xfffff)
            {
                throw std::runtime_error("endpoint EIDs must be non-zero 20-bit values");
            }
            for (std::size_t previous = 0; previous < endpoint; ++previous)
            {
                if ((m_endpointEids[previous] & 0xffffU) == (m_endpointEids[endpoint] & 0xffffU))
                {
                    throw std::runtime_error("endpoint EIDs need unique low 16 bits");
                }
            }
        }
        if (m_serializationStages != 0 && m_rateGbps == 0)
        {
            throw std::runtime_error("serialization requires a positive line rate");
        }
    }

    void Run()
    {
        std::cerr << "[NS3_UB_ADAPTER] ready endpoints=" << m_links.size() << " ports=" << m_ports
                  << " rate_gbps=" << m_rateGbps << " link_delay_ps=" << m_linkDelayPs
                  << " switch_delay_ps=" << m_switchDelayPs << " boundary=version4-lifetime\n";
        while (!g_stopRequested)
        {
            bool progress = false;
            bool consumed = false;
            do
            {
                consumed = false;
                for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
                    for (uint32_t port = 0; port < m_ports; ++port)
                        consumed |= PullOne(endpoint, port);
                progress |= consumed;
            } while (consumed);
            if (m_scheduled != 0)
            {
                Simulator::Run();
                progress = true;
            }
            const uint64_t safe = SafeHorizon();
            if (safe > m_virtualTime)
            {
                m_virtualTime = safe;
                m_started = true;
                progress = true;
                continue;
            }
            if (m_started)
                for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
                    for (uint32_t port = 0; port < m_ports; ++port)
                        progress |= PublishGrant(endpoint, port);
            if (!progress)
            {
                std::this_thread::yield();
            }
        }
        Simulator::Destroy();
        std::cerr << "[NS3_UB_ADAPTER_STATS] forwarded=" << m_forwarded
                  << " payload_bytes=" << m_payloadBytes << " ns3_events=" << m_events
                  << " virtual_ps=" << m_virtualTime << '\n';
    }

  private:
    static constexpr uint32_t SWITCH_TO_ENDPOINT = 0;
    static constexpr uint32_t ENDPOINT_TO_SWITCH = 1;

    uint32_t RouteEndpoint(uint32_t eid) const
    {
        if (eid == 0 || eid > 0xfffff)
        {
            throw std::runtime_error("record carries an invalid destination EID");
        }
        for (uint32_t endpoint = 0; endpoint < m_endpointEids.size(); ++endpoint)
        {
            if ((m_endpointEids[endpoint] & 0xffffU) == (eid & 0xffffU))
            {
                return endpoint;
            }
        }
        throw std::runtime_error("destination EID is not registered");
    }

    uint64_t SerializationPs(uint64_t bytes) const
    {
        if (bytes == 0 || m_rateGbps == 0)
        {
            return 0;
        }
        return (bytes * 8000ULL + m_rateGbps - 1) / m_rateGbps;
    }

    bool OutputHasRoom(uint32_t endpoint, uint32_t port) const
    {
        const auto& direction = m_links[endpoint]->Ring()->direction[SWITCH_TO_ENDPOINT][port];
        const uint64_t head = __atomic_load_n(&direction.head.value, __ATOMIC_ACQUIRE);
        const uint64_t tail = __atomic_load_n(&direction.tail.value, __ATOMIC_ACQUIRE);
        return head - tail + m_reservedOutputs[endpoint][port] < protocol::RING_SLOTS;
    }

    bool PullOne(uint32_t sourceEndpoint, uint32_t sourcePort)
    {
        auto& input = m_links[sourceEndpoint]->Ring()->direction[ENDPOINT_TO_SWITCH][sourcePort];
        const uint64_t tail = __atomic_load_n(&input.tail.value, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&input.head.value, __ATOMIC_ACQUIRE);
        if (tail == head)
        {
            return false;
        }

        const auto* source = reinterpret_cast<const protocol::MessageHeader*>(
            m_links[sourceEndpoint]->Slot(ENDPOINT_TO_SWITCH, sourcePort, tail));
        if (source->protocolVersion != protocol::VERSION)
        {
            throw std::runtime_error("adapter protocol version mismatch");
        }
        if (source->sourcePort != sourcePort)
        {
            throw std::runtime_error("record identity does not match ingress adapter");
        }
        if (source->receiveTick > m_virtualTime)
            return false;
        const auto type = static_cast<protocol::MessageType>(source->type);
        m_ingressPromise[sourceEndpoint][sourcePort] = std::max(
            m_ingressPromise[sourceEndpoint][sourcePort], source->receiveTick);
        if (type == protocol::MessageType::SYNC)
        {
            if (source->payloadLength != 0 || source->sourceEid != 0 ||
                source->destinationEid != 0)
                throw std::runtime_error("link-local SYNC carries payload or EIDs");
            __atomic_store_n(&input.tail.value, tail + 1, __ATOMIC_RELEASE);
            return true;
        }
        if (type != protocol::MessageType::DATA)
            throw std::runtime_error("unsupported adapter message type");
        if ((source->sourceEid & 0xffffU) !=
            (m_endpointEids[sourceEndpoint] & 0xffffU))
            throw std::runtime_error("record source EID does not match ingress adapter");
        const uint32_t destinationEndpoint = RouteEndpoint(source->destinationEid);
        if (destinationEndpoint == sourceEndpoint)
        {
            throw std::runtime_error("external fabric record loops back to its source");
        }
        const uint32_t destinationPort = m_portMap[sourcePort];
        if (!OutputHasRoom(destinationEndpoint, destinationPort))
        {
            return false;
        }

        auto record = std::make_shared<PendingRecord>();
        record->header = *source;
        record->header.destinationPort = destinationPort;
        record->destinationEndpoint = destinationEndpoint;
        record->destinationPort = destinationPort;

        uint64_t outputTime = CheckedAdd(source->receiveTick, m_switchDelayPs);
        if (source->payloadLength < 40 ||
            source->payloadLength > protocol::SLOT_BYTES - sizeof(*source))
        {
            throw std::runtime_error("invalid DATA payload length");
        }
        const uint8_t* payload = reinterpret_cast<const uint8_t*>(source) + sizeof(*source);
        record->payload.assign(payload, payload + source->payloadLength);
        const uint32_t applicationBytes = source->payloadLength - 40;
        record->networkPacket = Create<Packet>(applicationBytes + m_overheadBytes);
        for (uint32_t stage = 0; stage < m_serializationStages; ++stage)
        {
            auto& free = m_egressFree[destinationEndpoint][destinationPort][stage];
            outputTime = std::max(outputTime, free);
            outputTime = CheckedAdd(outputTime, SerializationPs(record->networkPacket->GetSize()));
            free = outputTime;
        }
        ++m_forwarded;
        m_payloadBytes += applicationBytes;

        outputTime = CheckedAdd(outputTime, m_linkDelayPs);
        outputTime = std::max(outputTime, m_lastOutput[destinationEndpoint][destinationPort]);
        m_lastOutput[destinationEndpoint][destinationPort] = outputTime;
        record->header.receiveTick = outputTime;

        ++m_reservedOutputs[destinationEndpoint][destinationPort];
        ++m_scheduled;
        // Version 3 gives the bridge per-FIFO promises, not one global safe
        // horizon across all attached endpoints. Execute this compatibility
        // callback at the current ns-3 time and carry the calculated virtual
        // arrival in the record. Advancing the global queue to one endpoint's
        // future would incorrectly reject a later-published record from
        // another endpoint whose valid timestamp is earlier. The native
        // external-port revision will advance physical events only after all
        // active ingresses have supplied a conservative horizon.
        Simulator::ScheduleNow(&ExternalFabric::Publish, this, record);
        __atomic_store_n(&input.tail.value, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    void Publish(std::shared_ptr<PendingRecord> record)
    {
        auto& output = m_links[record->destinationEndpoint]
                           ->Ring()
                           ->direction[SWITCH_TO_ENDPOINT][record->destinationPort];
        const uint64_t head = __atomic_load_n(&output.head.value, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&output.tail.value, __ATOMIC_ACQUIRE);
        NS_ABORT_MSG_IF(head - tail >= protocol::RING_SLOTS,
                        "reserved external output unexpectedly became full");
        uint8_t* slot = m_links[record->destinationEndpoint]->Slot(SWITCH_TO_ENDPOINT,
                                                                   record->destinationPort,
                                                                   head);
        std::memcpy(slot, &record->header, sizeof(record->header));
        if (!record->payload.empty())
        {
            std::memcpy(slot + sizeof(record->header),
                        record->payload.data(),
                        record->payload.size());
        }
        std::atomic_thread_fence(std::memory_order_release);
        __atomic_store_n(&output.head.value, head + 1, __ATOMIC_RELEASE);
        --m_reservedOutputs[record->destinationEndpoint][record->destinationPort];
        --m_scheduled;
        ++m_events;
    }

    uint64_t SafeHorizon()
    {
        uint64_t safe = std::numeric_limits<uint64_t>::max();
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                auto& input = m_links[endpoint]->Ring()->direction[ENDPOINT_TO_SWITCH][port];
                const uint64_t tail = __atomic_load_n(&input.tail.value, __ATOMIC_RELAXED);
                const uint64_t head = __atomic_load_n(&input.head.value, __ATOMIC_ACQUIRE);
                uint64_t horizon = m_ingressPromise[endpoint][port];
                if (tail != head)
                {
                    const auto* message = reinterpret_cast<const protocol::MessageHeader*>(
                        m_links[endpoint]->Slot(ENDPOINT_TO_SWITCH, port, tail));
                    if (message->protocolVersion != protocol::VERSION)
                        throw std::runtime_error("adapter protocol version mismatch");
                    if (message->receiveTick < horizon)
                        throw std::runtime_error("input FIFO moved behind its promise");
                    horizon = message->receiveTick;
                }
                safe = std::min(safe, horizon);
            }
        }
        return safe;
    }

    bool PublishGrant(uint32_t endpoint, uint32_t port)
    {
        if (m_lastGrant[endpoint][port] == m_virtualTime ||
            !OutputHasRoom(endpoint, port))
            return false;
        auto& output = m_links[endpoint]->Ring()->direction[SWITCH_TO_ENDPOINT][port];
        const uint64_t head = __atomic_load_n(&output.head.value, __ATOMIC_RELAXED);
        uint8_t* slot = m_links[endpoint]->Slot(SWITCH_TO_ENDPOINT, port, head);
        protocol::MessageHeader header{};
        header.type = static_cast<uint32_t>(protocol::MessageType::SYNC);
        header.sendTick = m_virtualTime;
        header.receiveTick = std::max(CheckedAdd(m_virtualTime, m_linkDelayPs),
                                      m_lastOutput[endpoint][port]);
        header.sequence = m_grantSequence++;
        header.sourcePort = port;
        header.destinationPort = port;
        header.protocolVersion = protocol::VERSION;
        std::memcpy(slot, &header, sizeof(header));
        std::atomic_thread_fence(std::memory_order_release);
        __atomic_store_n(&output.head.value, head + 1, __ATOMIC_RELEASE);
        m_lastGrant[endpoint][port] = m_virtualTime;
        __atomic_add_fetch(&m_links[endpoint]->Ring()->switchSyncMessages, 1,
                           __ATOMIC_RELEASE);
        return true;
    }

    std::vector<std::unique_ptr<Mapping>> m_links;
    std::vector<uint32_t> m_endpointEids;
    uint32_t m_ports;
    uint64_t m_linkDelayPs;
    uint64_t m_switchDelayPs;
    uint64_t m_rateGbps;
    uint32_t m_overheadBytes;
    std::vector<uint32_t> m_portMap;
    uint32_t m_serializationStages;
    std::vector<std::vector<std::vector<uint64_t>>> m_egressFree;
    std::vector<std::vector<uint64_t>> m_lastOutput;
    std::vector<std::vector<uint64_t>> m_reservedOutputs;
    std::vector<std::vector<uint64_t>> m_lastGrant;
    std::vector<std::vector<uint64_t>> m_ingressPromise;
    uint64_t m_virtualTime{0};
    bool m_started{false};
    uint64_t m_grantSequence{0};
    uint64_t m_scheduled{0};
    uint64_t m_forwarded{0};
    uint64_t m_payloadBytes{0};
    uint64_t m_events{0};
};

class NativeExternalFabric
{
  public:
    NativeExternalFabric(std::vector<std::string> paths,
                         std::vector<uint32_t> endpointEids,
                         uint32_t ports,
                         uint64_t linkDelayPs,
                         uint64_t switchDelayPs,
                         uint64_t rateGbps,
                         std::vector<uint32_t> portMap)
        : m_endpointEids(std::move(endpointEids)),
          m_ports(ports),
          m_linkDelayPs(linkDelayPs),
          m_switchDelayPs(switchDelayPs),
          m_rateGbps(rateGbps),
          m_portMap(std::move(portMap)),
          m_ingressPromise(paths.size(), std::vector<uint64_t>(ports)),
          m_lastGrant(paths.size(), std::vector<uint64_t>(ports)),
          m_lastOutput(paths.size(), std::vector<uint64_t>(ports)),
          m_pendingOutputs(paths.size())
    {
        if (paths.size() < 2 || paths.size() != m_endpointEids.size())
        {
            throw std::runtime_error("ring and endpoint-EID counts must match and be at least two");
        }
        if (m_rateGbps == 0)
        {
            throw std::runtime_error("native ns-3-UB ports require a positive line rate");
        }
        for (const auto& path : paths)
        {
            m_links.emplace_back(std::make_unique<Mapping>(path, ports));
        }
        for (std::size_t endpoint = 0; endpoint < m_endpointEids.size(); ++endpoint)
        {
            if (m_endpointEids[endpoint] == 0 || m_endpointEids[endpoint] > 0xfffff)
            {
                throw std::runtime_error("endpoint EIDs must be non-zero 20-bit values");
            }
            for (std::size_t previous = 0; previous < endpoint; ++previous)
            {
                if ((m_endpointEids[previous] & 0xffffU) == (m_endpointEids[endpoint] & 0xffffU))
                {
                    throw std::runtime_error("endpoint EIDs need unique low 16 bits");
                }
            }
        }
        for (uint32_t port = 0; port < ports; ++port)
        {
            if (m_portMap[port] != port)
            {
                throw std::runtime_error(
                    "the first native ns-3-UB milestone requires an identity port map");
            }
        }
        BuildTopology();
    }

    void Run()
    {
        std::cerr << "[NS3_UB_ADAPTER] ready endpoints=" << m_links.size() << " ports=" << m_ports
                  << " rate_gbps=" << m_rateGbps << " link_delay_ps=" << m_linkDelayPs
                  << " switch_delay_ps=" << m_switchDelayPs << " boundary=native-ub-switch-v1\n";
        while (!g_stopRequested)
        {
            bool progress = false;
            for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
            {
                for (uint32_t port = 0; port < m_ports; ++port)
                {
                    while (PullOne(endpoint, port))
                    {
                        progress = true;
                    }
                }
            }

            const uint64_t safe = SafeHorizon();
            const uint64_t now = static_cast<uint64_t>(Simulator::Now().GetPicoSeconds());
            if (safe > now && GrantsHaveRoom())
            {
                AdvanceTo(safe);
                PublishGrants(safe);
                progress = true;
            }
            if (!progress)
            {
                std::this_thread::yield();
            }
        }
        Simulator::Destroy();
        std::cerr << "[NS3_UB_ADAPTER_STATS] forwarded=" << m_forwarded
                  << " payload_bytes=" << m_payloadBytes << " ns3_events=" << m_events
                  << " safe_advances=" << m_safeAdvances << '\n';
    }

  private:
    static constexpr uint32_t SWITCH_TO_ENDPOINT = 0;
    static constexpr uint32_t ENDPOINT_TO_SWITCH = 1;

    uint32_t RouteEndpoint(uint32_t eid) const
    {
        if (eid == 0 || eid > 0xfffff)
        {
            throw std::runtime_error("record carries an invalid destination EID");
        }
        for (uint32_t endpoint = 0; endpoint < m_endpointEids.size(); ++endpoint)
        {
            if ((m_endpointEids[endpoint] & 0xffffU) == (eid & 0xffffU))
            {
                return endpoint;
            }
        }
        throw std::runtime_error("destination EID is not registered");
    }

    void BuildTopology()
    {
        const DataRate rate(std::to_string(m_rateGbps) + "Gbps");
        m_endpointNodes.reserve(m_links.size());
        m_endpointPorts.resize(m_links.size());
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            Ptr<Node> node = CreateObject<Node>();
            m_endpointNodes.push_back(node);
            m_endpointByNode.emplace(node->GetId(), endpoint);
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                Ptr<UbPort> endpointPort = CreateObject<UbPort>();
                endpointPort->SetAddress(Mac48Address::Allocate());
                endpointPort->SetDataRate(rate);
                node->AddDevice(endpointPort);
                endpointPort->SetReceiveHandler(
                    MakeCallback(&NativeExternalFabric::ReceiveAtEndpoint, this));
                m_endpointPorts[endpoint].push_back(endpointPort);
            }
        }

        m_switchNode = CreateObject<Node>();
        m_switch = CreateObject<UbSwitch>();
        m_switchNode->AggregateObject(m_switch);
        m_switch->SetNodeType(UB_SWITCH);
        m_switch->SetAttribute("FlowControl", EnumValue(FcType::NONE));
        m_switch->SetAttribute("InPortProcessingDelay", TimeValue(PicoSeconds(m_switchDelayPs)));

        const uint32_t switchPortCount = static_cast<uint32_t>(m_links.size()) * m_ports;
        m_switchPorts.reserve(switchPortCount);
        for (uint32_t index = 0; index < switchPortCount; ++index)
        {
            Ptr<UbPort> switchPort = CreateObject<UbPort>();
            switchPort->SetAddress(Mac48Address::Allocate());
            switchPort->SetDataRate(rate);
            m_switchNode->AddDevice(switchPort);
            m_switchPorts.push_back(switchPort);
        }

        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                const uint32_t switchPortIndex = endpoint * m_ports + port;
                Ptr<UbLink> link = CreateObject<UbLink>();
                link->SetAttribute("Delay", TimeValue(PicoSeconds(m_linkDelayPs)));
                m_switchPorts[switchPortIndex]->Attach(link);
                m_endpointPorts[endpoint][port]->Attach(link);
            }
        }

        m_switch->Init();
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            std::vector<uint16_t> outputs;
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                outputs.push_back(static_cast<uint16_t>(endpoint * m_ports + port));
            }
            m_switch->GetRoutingProcess()->AddShortestRoute(
                utils::NodeIdToIp(m_endpointNodes[endpoint]->GetId()).Get(),
                outputs);
        }
    }

    bool OutputHasRoom(uint32_t endpoint, uint32_t port) const
    {
        const auto& direction = m_links[endpoint]->Ring()->direction[SWITCH_TO_ENDPOINT][port];
        const uint64_t head = __atomic_load_n(&direction.head.value, __ATOMIC_ACQUIRE);
        const uint64_t tail = __atomic_load_n(&direction.tail.value, __ATOMIC_ACQUIRE);
        return head - tail + m_pendingOutputs[endpoint] < protocol::RING_SLOTS;
    }

    uint64_t ToAbsoluteTick(uint32_t endpoint, uint64_t wireTick) const
    {
        (void)endpoint;
        return wireTick;
    }

    uint64_t ToWireTick(uint32_t endpoint, uint64_t absoluteTick) const
    {
        (void)endpoint;
        return absoluteTick;
    }

    bool PullOne(uint32_t sourceEndpoint, uint32_t sourcePort)
    {
        auto& input = m_links[sourceEndpoint]->Ring()->direction[ENDPOINT_TO_SWITCH][sourcePort];
        const uint64_t tail = __atomic_load_n(&input.tail.value, __ATOMIC_RELAXED);
        const uint64_t head = __atomic_load_n(&input.head.value, __ATOMIC_ACQUIRE);
        if (tail == head)
        {
            return false;
        }

        const auto* source = reinterpret_cast<const protocol::MessageHeader*>(
            m_links[sourceEndpoint]->Slot(ENDPOINT_TO_SWITCH, sourcePort, tail));
        if (source->protocolVersion != protocol::VERSION)
        {
            throw std::runtime_error("adapter protocol version mismatch");
        }
        if (source->sourcePort != sourcePort)
        {
            throw std::runtime_error("record identity does not match ingress adapter");
        }
        const auto type = static_cast<protocol::MessageType>(source->type);
        if (type == protocol::MessageType::SYNC)
        {
            if (source->payloadLength != 0 || source->sourceEid != 0 ||
                source->destinationEid != 0)
            {
                throw std::runtime_error("link-local SYNC carries payload or EIDs");
            }
            m_ingressPromise[sourceEndpoint][sourcePort] =
                std::max(m_ingressPromise[sourceEndpoint][sourcePort],
                         source->receiveTick);
            __atomic_store_n(&input.tail.value, tail + 1, __ATOMIC_RELEASE);
            return true;
        }
        if (type != protocol::MessageType::DATA)
        {
            throw std::runtime_error("unsupported adapter message type");
        }
        if ((source->sourceEid & 0xffffU) !=
            (m_endpointEids[sourceEndpoint] & 0xffffU))
        {
            throw std::runtime_error("record source EID does not match ingress adapter");
        }
        if (source->payloadLength < 40 ||
            source->payloadLength > protocol::SLOT_BYTES - sizeof(*source))
        {
            throw std::runtime_error("invalid DATA payload length");
        }

        const uint32_t destinationEndpoint = RouteEndpoint(source->destinationEid);
        if (destinationEndpoint == sourceEndpoint)
        {
            throw std::runtime_error("external fabric record loops back to its source");
        }
        for (uint32_t port = 0; port < m_ports; ++port)
        {
            if (!OutputHasRoom(destinationEndpoint, port))
            {
                return false;
            }
        }
        const uint64_t now = static_cast<uint64_t>(Simulator::Now().GetPicoSeconds());
        const uint64_t absoluteReceiveTick = ToAbsoluteTick(sourceEndpoint, source->receiveTick);
        if (absoluteReceiveTick < now)
        {
            throw std::runtime_error("native ingress record violates conservative virtual time");
        }
        m_ingressPromise[sourceEndpoint][sourcePort] = std::max(
            m_ingressPromise[sourceEndpoint][sourcePort], absoluteReceiveTick);

        auto record = std::make_shared<PendingRecord>();
        record->header = *source;
        record->destinationEndpoint = destinationEndpoint;
        const uint8_t* payload = reinterpret_cast<const uint8_t*>(source) + sizeof(*source);
        record->payload.assign(payload, payload + source->payloadLength);
        ++m_pendingOutputs[destinationEndpoint];
        Simulator::Schedule(PicoSeconds(absoluteReceiveTick - now),
                            &NativeExternalFabric::InjectAtSwitch,
                            this,
                            record,
                            sourceEndpoint,
                            sourcePort);
        __atomic_store_n(&input.tail.value, tail + 1, __ATOMIC_RELEASE);
        ++m_forwarded;
        m_payloadBytes += source->payloadLength - 40;
        return true;
    }

    void InjectAtSwitch(std::shared_ptr<PendingRecord> record,
                        uint32_t sourceEndpoint,
                        uint32_t sourcePort)
    {
        Ptr<Packet> packet = Create<Packet>(record->payload.data(), record->payload.size());
        AdapterMetadataTag tag;
        tag.m_sendTick = record->header.sendTick;
        tag.m_sequence = record->header.sequence;
        tag.m_sourceEid = record->header.sourceEid;
        tag.m_destinationEid = record->header.destinationEid;
        tag.m_sourcePort = record->header.sourcePort;
        tag.m_flags = record->header.flags;
        packet->AddPacketTag(tag);

        UbCtpHeader ctpHeader;
        ctpHeader.SetTPOpcode(CtpOpcode::CTP_DATA);
        ctpHeader.SetPadding(0);
        ctpHeader.SetNlp(0);
        packet->AddHeader(ctpHeader);

        UbCna16NetworkHeader cnaHeader;
        cnaHeader.SetScna(static_cast<uint16_t>(
            utils::NodeIdToCna16(m_endpointNodes[sourceEndpoint]->GetId(), sourcePort)));
        cnaHeader.SetDcna(static_cast<uint16_t>(
            utils::NodeIdToCna16(m_endpointNodes[record->destinationEndpoint]->GetId())));
        cnaHeader.SetLb(static_cast<uint8_t>(record->header.sequence));
        cnaHeader.SetServiceLevel(1);
        cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
        packet->AddHeader(cnaHeader);

        UbDataLink::AddPacketHeader(packet,
                                    false,
                                    false,
                                    1,
                                    1,
                                    RoutingType::PER_FLOW_SHORTEST_PATHS,
                                    UbDatalinkHeaderConfig::PACKET_CNA16);
        const uint32_t switchPortIndex = sourceEndpoint * m_ports + sourcePort;
        m_switch->SwitchHandlePacket(m_switchPorts[switchPortIndex], packet);
    }

    void ReceiveAtEndpoint(Ptr<UbPort> port, Ptr<Packet> packet)
    {
        const auto endpointIt = m_endpointByNode.find(port->GetNode()->GetId());
        NS_ABORT_MSG_IF(endpointIt == m_endpointByNode.end(),
                        "native adapter received on an unknown endpoint node");
        const uint32_t endpoint = endpointIt->second;
        const uint32_t destinationPort = port->GetIfIndex();

        AdapterMetadataTag tag;
        NS_ABORT_MSG_IF(!packet->RemovePacketTag(tag),
                        "native adapter packet lost its endpoint metadata");
        UbDatalinkPacketHeader dataLinkHeader;
        UbCna16NetworkHeader cnaHeader;
        UbCtpHeader ctpHeader;
        packet->RemoveHeader(dataLinkHeader);
        packet->RemoveHeader(cnaHeader);
        packet->RemoveHeader(ctpHeader);
        NS_ABORT_MSG_IF((tag.m_destinationEid & 0xffffU) != (m_endpointEids[endpoint] & 0xffffU),
                        "native switch delivered a packet to the wrong endpoint EID");

        auto& output = m_links[endpoint]->Ring()->direction[SWITCH_TO_ENDPOINT][destinationPort];
        const uint64_t head = __atomic_load_n(&output.head.value, __ATOMIC_RELAXED);
        const uint64_t tail = __atomic_load_n(&output.tail.value, __ATOMIC_ACQUIRE);
        NS_ABORT_MSG_IF(head - tail >= protocol::RING_SLOTS,
                        "reserved native adapter output unexpectedly became full");
        uint8_t* slot = m_links[endpoint]->Slot(SWITCH_TO_ENDPOINT, destinationPort, head);
        protocol::MessageHeader header{};
        header.payloadLength = packet->GetSize();
        header.type = static_cast<uint32_t>(protocol::MessageType::DATA);
        header.sendTick = tag.m_sendTick;
        header.receiveTick =
            ToWireTick(endpoint, static_cast<uint64_t>(Simulator::Now().GetPicoSeconds()));
        header.sequence = tag.m_sequence;
        header.sourcePort = tag.m_sourcePort;
        header.destinationPort = destinationPort;
        header.protocolVersion = protocol::VERSION;
        header.flags = tag.m_flags;
        header.sourceEid = tag.m_sourceEid;
        header.destinationEid = tag.m_destinationEid;
        m_lastOutput[endpoint][destinationPort] = std::max(
            m_lastOutput[endpoint][destinationPort], header.receiveTick);
        std::memcpy(slot, &header, sizeof(header));
        packet->CopyData(slot + sizeof(header), packet->GetSize());
        std::atomic_thread_fence(std::memory_order_release);
        __atomic_store_n(&output.head.value, head + 1, __ATOMIC_RELEASE);
        --m_pendingOutputs[endpoint];
        ++m_events;
    }

    uint64_t SafeHorizon() const
    {
        uint64_t safe = std::numeric_limits<uint64_t>::max();
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                if (m_ingressPromise[endpoint][port] == 0)
                {
                    return 0;
                }
                safe = std::min(safe, m_ingressPromise[endpoint][port]);
            }
        }
        return safe;
    }

    bool GrantsHaveRoom() const
    {
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                if (!OutputHasRoom(endpoint, port))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void AdvanceTo(uint64_t safe)
    {
        const uint64_t now = static_cast<uint64_t>(Simulator::Now().GetPicoSeconds());
        NS_ABORT_MSG_IF(safe <= now, "native adapter safe horizon must advance time");
        Simulator::Schedule(PicoSeconds(safe - now), [] { Simulator::Stop(); });
        Simulator::Run();
        ++m_safeAdvances;
    }

    void PublishGrants(uint64_t safe)
    {
        for (uint32_t endpoint = 0; endpoint < m_links.size(); ++endpoint)
        {
            for (uint32_t port = 0; port < m_ports; ++port)
            {
                if (m_lastGrant[endpoint][port] >= safe)
                    continue;
                auto& output = m_links[endpoint]->Ring()->direction[SWITCH_TO_ENDPOINT][port];
                const uint64_t head = __atomic_load_n(&output.head.value, __ATOMIC_RELAXED);
                uint8_t* slot = m_links[endpoint]->Slot(SWITCH_TO_ENDPOINT, port, head);
                protocol::MessageHeader header{};
                header.type = static_cast<uint32_t>(protocol::MessageType::SYNC);
                header.sendTick = safe;
                // Native ns-3 owns the egress link. A direct null-message
                // grant at the current safe horizon is conservative; using
                // safe+link here could overtake an already injected packet
                // whose ns-3 delivery event lies between those two times.
                header.receiveTick = std::max(safe, m_lastOutput[endpoint][port]);
                header.sequence = m_grantSequence++;
                header.sourcePort = port;
                header.destinationPort = port;
                header.protocolVersion = protocol::VERSION;
                std::memcpy(slot, &header, sizeof(header));
                std::atomic_thread_fence(std::memory_order_release);
                __atomic_store_n(&output.head.value, head + 1, __ATOMIC_RELEASE);
                m_lastGrant[endpoint][port] = safe;
                __atomic_add_fetch(&m_links[endpoint]->Ring()->switchSyncMessages,
                                   1, __ATOMIC_RELEASE);
            }
        }
    }

    std::vector<std::unique_ptr<Mapping>> m_links;
    std::vector<uint32_t> m_endpointEids;
    uint32_t m_ports;
    uint64_t m_linkDelayPs;
    uint64_t m_switchDelayPs;
    uint64_t m_rateGbps;
    std::vector<uint32_t> m_portMap;
    std::vector<Ptr<Node>> m_endpointNodes;
    std::vector<std::vector<Ptr<UbPort>>> m_endpointPorts;
    std::unordered_map<uint32_t, uint32_t> m_endpointByNode;
    Ptr<Node> m_switchNode;
    Ptr<UbSwitch> m_switch;
    std::vector<Ptr<UbPort>> m_switchPorts;
    std::vector<std::vector<uint64_t>> m_ingressPromise;
    std::vector<std::vector<uint64_t>> m_lastGrant;
    std::vector<std::vector<uint64_t>> m_lastOutput;
    std::vector<uint64_t> m_pendingOutputs;
    uint64_t m_grantSequence{0};
    uint64_t m_forwarded{0};
    uint64_t m_payloadBytes{0};
    uint64_t m_events{0};
    uint64_t m_safeAdvances{0};
};

} // namespace

int
main(int argc, char* argv[])
{
    const bool multi = argc >= 2 && std::string_view(argv[1]) == "--multi";
    const bool nativeMulti = argc >= 2 && std::string_view(argv[1]) == "--native-multi";
    if ((!multi && !nativeMulti && argc != 10) || ((multi || nativeMulti) && argc < 12))
    {
        std::cerr << "usage: ub-gem5-adapter RING0 RING1 PORTS LINK_DELAY "
                     "SWITCH_DELAY RATE_GBPS OVERHEAD_BYTES PORT_MAP "
                     "SERIALIZATION_STAGES\n"
                     "   or: ub-gem5-adapter --multi PORTS LINK_DELAY SWITCH_DELAY "
                     "RATE_GBPS OVERHEAD_BYTES PORT_MAP SERIALIZATION_STAGES "
                     "ENDPOINT_EIDS RING...\n"
                     "   or: ub-gem5-adapter --native-multi PORTS LINK_DELAY SWITCH_DELAY "
                     "RATE_GBPS OVERHEAD_BYTES PORT_MAP SERIALIZATION_STAGES "
                     "ENDPOINT_EIDS RING...\n";
        return 2;
    }
    try
    {
        const int base = (multi || nativeMulti) ? 2 : 3;
        const uint32_t ports = static_cast<uint32_t>(ParseUnsigned(argv[base], "ports"));
        if (ports == 0 || ports > protocol::MAX_PORTS)
        {
            throw std::runtime_error("ports must be in [1,16]");
        }
        std::vector<uint32_t> portMap;
        if (std::string(argv[base + 5]).empty())
        {
            for (uint32_t port = 0; port < ports; ++port)
            {
                portMap.push_back(port);
            }
        }
        else
        {
            portMap = ParseList(argv[base + 5], "port map");
        }
        if (portMap.size() != ports ||
            std::any_of(portMap.begin(), portMap.end(), [ports](uint32_t port) {
                return port >= ports;
            }))
        {
            throw std::runtime_error("port map must contain one valid entry per port");
        }
        std::vector<std::string> paths;
        std::vector<uint32_t> endpointEids;
        if (multi || nativeMulti)
        {
            endpointEids = ParseList(argv[9], "endpoint EID");
            for (int index = 10; index < argc; ++index)
            {
                paths.emplace_back(argv[index]);
            }
        }
        else
        {
            endpointEids = {0x100, 0x101};
            paths = {argv[1], argv[2]};
        }
        std::signal(SIGINT, RequestStop);
        std::signal(SIGTERM, RequestStop);
        Time::SetResolution(Time::PS);
        if (nativeMulti)
        {
            NativeExternalFabric fabric(paths,
                                        std::move(endpointEids),
                                        ports,
                                        ParsePicoseconds(argv[base + 1], "link delay"),
                                        ParsePicoseconds(argv[base + 2], "switch delay"),
                                        ParseUnsigned(argv[base + 3], "rate"),
                                        std::move(portMap));
            fabric.Run();
            return 0;
        }
        ExternalFabric fabric(
            paths,
            std::move(endpointEids),
            ports,
            ParsePicoseconds(argv[base + 1], "link delay"),
            ParsePicoseconds(argv[base + 2], "switch delay"),
            ParseUnsigned(argv[base + 3], "rate"),
            static_cast<uint32_t>(ParseUnsigned(argv[base + 4], "overhead")),
            std::move(portMap),
            static_cast<uint32_t>(ParseUnsigned(argv[base + 6], "serialization stages")));
        fabric.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "ub-gem5-adapter: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
