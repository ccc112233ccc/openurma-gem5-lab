// SPDX-License-Identifier: GPL-2.0-only
// Native ns-3-UB fabric attached through the simulator-neutral UB-NET v1 ABI.

#include "ns3/core-module.h"
#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/tag.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-header.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-utils.h"

#include "protocol/ub_net/if.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ns3;
namespace ubnet = openurma::proto::net;

namespace {

std::atomic<bool> running{true};
void StopProcess(int) { running.store(false); }

std::uint64_t ParseUnsigned(const std::string& text, const char* name,
                            bool allow_zero = false)
{
    try {
        std::size_t consumed = 0;
        const std::uint64_t value = std::stoull(text, &consumed, 0);
        if (consumed != text.size() || (!allow_zero && value == 0))
            throw std::runtime_error("range");
        return value;
    } catch (...) {
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    }
}

struct EndpointOption {
    std::string socket;
    std::uint32_t eid{};
};

struct Options {
    std::vector<EndpointOption> endpoints;
    std::uint32_t ports{2};
    std::uint64_t link_delay_ps{100000};
    std::uint64_t switch_delay_ps{0};
    std::uint64_t rate_gbps{400};
    std::uint64_t sync_interval_ps{100000};
    SimbricksBaseIfSyncMode sync_mode{kSimbricksBaseIfSyncOptional};
};

EndpointOption ParseEndpoint(const std::string& value)
{
    const auto comma = value.rfind(',');
    if (comma == std::string::npos || comma == 0)
        throw std::runtime_error("endpoint must be SOCKET,EID");
    const auto eid = ParseUnsigned(value.substr(comma + 1), "endpoint EID");
    if (eid > 0xcffff) throw std::runtime_error("endpoint EID exceeds 20 bits");
    return {value.substr(0, comma), static_cast<std::uint32_t>(eid)};
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--endpoint" && i + 1 < argc)
            options.endpoints.push_back(ParseEndpoint(argv[++i]));
        else if (arg == "--ports" && i + 1 < argc)
            options.ports = static_cast<std::uint32_t>(
                ParseUnsigned(argv[++i], "ports"));
        else if (arg == "--link-delay-ps" && i + 1 < argc)
            options.link_delay_ps = ParseUnsigned(argv[++i], "link delay", true);
        else if (arg == "--switch-delay-ps" && i + 1 < argc)
            options.switch_delay_ps = ParseUnsigned(argv[++i], "switch delay", true);
        else if (arg == "--rate-gbps" && i + 1 < argc)
            options.rate_gbps = ParseUnsigned(argv[++i], "line rate");
        else if (arg == "--sync-interval-ps" && i + 1 < argc)
            options.sync_interval_ps = ParseUnsigned(argv[++i], "sync interval");
        else if (arg == "--sync" && i + 1 < argc) {
            const std::string mode(argv[++i]);
            if (mode == "off") options.sync_mode = kSimbricksBaseIfSyncDisabled;
            else if (mode == "optional") options.sync_mode = kSimbricksBaseIfSyncOptional;
            else if (mode == "required") options.sync_mode = kSimbricksBaseIfSyncRequired;
            else throw std::runtime_error("sync must be off, optional, or required");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + arg);
        }
    }
    if (options.endpoints.size() < 2)
        throw std::runtime_error("at least two endpoints are required");
    if (options.ports == 0 || options.ports > 16)
        throw std::runtime_error("ports must be in [1,16]");
    std::unordered_map<std::uint32_t, bool> eids;
    for (const auto& endpoint : options.endpoints)
        if (!eids.emplace(endpoint.eid, true).second)
            throw std::runtime_error("endpoint EIDs must be unique");
    return options;
}

class FrameTag final : public Tag {
  public:
    static TypeId GetTypeId()
    {
        static TypeId id = TypeId("ns3::OpenUrmaUbNetFrameTag")
            .SetParent<Tag>().SetGroupName("UnifiedBus").AddConstructor<FrameTag>();
        return id;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    std::uint32_t GetSerializedSize() const override { return 32; }
    void Serialize(TagBuffer buffer) const override
    {
        buffer.WriteU64(sequence);
        buffer.WriteU32(source_eid); buffer.WriteU32(destination_eid);
        buffer.WriteU16(source_port); buffer.WriteU16(destination_port);
        buffer.WriteU16(traffic_class); buffer.WriteU16(flags);
        buffer.WriteU32(0);
    }
    void Deserialize(TagBuffer buffer) override
    {
        sequence = buffer.ReadU64();
        source_eid = buffer.ReadU32(); destination_eid = buffer.ReadU32();
        source_port = buffer.ReadU16(); destination_port = buffer.ReadU16();
        traffic_class = buffer.ReadU16(); flags = buffer.ReadU16();
        (void)buffer.ReadU32();
    }
    void Print(std::ostream& stream) const override
    {
        stream << "sequence=" << sequence << " source=" << source_eid
               << " destination=" << destination_eid;
    }
    std::uint64_t sequence{};
    std::uint32_t source_eid{};
    std::uint32_t destination_eid{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    std::uint16_t traffic_class{};
    std::uint16_t flags{};
};

NS_OBJECT_ENSURE_REGISTERED(FrameTag);

struct QueuedFrame {
    ubnet::Frame header{};
    std::vector<std::uint8_t> payload;
};

struct Endpoint {
    ubnet::Interface interface{};
    SimbricksBaseIfParams params{};
    ubnet::Intro peer_intro{};
    std::string socket;
    std::uint32_t eid{};
    std::deque<QueuedFrame> outgoing;
};

template <typename T>
void ZeroVolatile(volatile T& object)
{
    const std::uint64_t timestamp = object.timestamp;
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
    object.timestamp = timestamp;
}

template <typename T>
T SnapshotVolatile(const volatile T& object)
{
    T snapshot{};
    const auto* source = reinterpret_cast<const volatile std::uint8_t*>(&object);
    auto* destination = reinterpret_cast<std::uint8_t*>(&snapshot);
    for (std::size_t i = 0; i < sizeof(T); ++i) destination[i] = source[i];
    return snapshot;
}

class UbNetFabric {
  public:
    explicit UbNetFabric(const Options& options)
        : options_(options), endpoints_(options.endpoints.size()),
          endpoint_ports_(options.endpoints.size())
    {
        for (std::size_t i = 0; i < endpoints_.size(); ++i) {
            endpoints_[i].socket = options_.endpoints[i].socket;
            endpoints_[i].eid = options_.endpoints[i].eid;
        }
        BuildTopology();
        for (std::size_t i = 0; i < endpoints_.size(); ++i)
            route_.emplace(endpoints_[i].eid, i);
        // Establish is deliberately last: peers may begin polling as soon as
        // the handshake returns, so the complete ns-3 fabric must already be
        // ready before they can observe the connection.
        Connect();
    }

    ~UbNetFabric()
    {
        for (auto& endpoint : endpoints_)
            SimbricksBaseIfClose(&endpoint.interface.base);
        Simulator::Destroy();
    }

    void Run()
    {
        PublishLinks();
        std::cerr << "[NS3_UB_NET] connected " << endpoints_.size()
                  << " UB-NET endpoints"
                  << " ports=" << options_.ports
                  << " rate_gbps=" << options_.rate_gbps
                  << " boundary=ub-net-v1\n";
        while (running.load()) {
            const std::uint64_t now = NowPs();
            bool progress = false;
            bool all_terminated = true;
            for (std::size_t i = 0; i < endpoints_.size(); ++i) {
                while (PollOne(i, now)) progress = true;
                all_terminated = all_terminated &&
                    SimbricksBaseIfInTerminated(&endpoints_[i].interface.base);
            }
            for (auto& endpoint : endpoints_)
                progress = Flush(endpoint, now) || progress;
            if (all_terminated && scheduled_packets_ == 0 && OutputsEmpty()) break;
            if (!OutputsEmpty()) {
                if (!progress) std::this_thread::yield();
                continue;
            }
            for (auto& endpoint : endpoints_)
                ubnet::UbNetOutSync(&endpoint.interface, now);

            bool synchronized = false;
            std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
            for (auto& endpoint : endpoints_) {
                if (!SimbricksBaseIfSyncEnabled(&endpoint.interface.base)) continue;
                synchronized = true;
                next = std::min(next, ubnet::UbNetInTimestamp(&endpoint.interface));
                next = std::min(next, ubnet::UbNetOutNextSync(&endpoint.interface));
            }
            if (synchronized && next > now &&
                next != std::numeric_limits<std::uint64_t>::max()) {
                AdvanceTo(next);
                progress = true;
            } else if (!synchronized && scheduled_packets_) {
                // ns-3-UB owns periodic maintenance events, so an unbounded
                // Simulator::Run() never drains. Advance one finite quantum
                // and return to the process boundary to flush completed
                // frames and observe newly arrived work.
                AdvanceTo(now + options_.sync_interval_ps);
                progress = true;
            }
            if (!progress) std::this_thread::yield();
        }
        std::cerr << "[NS3_UB_NET_STATS] forwarded=" << forwarded_
                  << " delivered=" << delivered_ << " payload_bytes="
                  << payload_bytes_ << " virtual_ps=" << NowPs() << '\n';
    }

  private:
    void Connect()
    {
        std::vector<SimBricksBaseIfEstablishData> establish(endpoints_.size());
        introductions_.resize(endpoints_.size());
        for (std::size_t i = 0; i < endpoints_.size(); ++i) {
            auto& endpoint = endpoints_[i];
            ubnet::DefaultParams(&endpoint.params);
            endpoint.params.sock_path = endpoint.socket.c_str();
            // This boundary is the endpoint-to-fabric link: its timestamp
            // carries propagation delay and provides conservative lookahead.
            // ns-3 owns serialization and switch queueing, while its internal
            // UbLink uses zero propagation delay to avoid double counting.
            const std::uint64_t boundary_latency =
                std::max<std::uint64_t>(1, options_.link_delay_ps);
            endpoint.params.link_latency = boundary_latency;
            endpoint.params.sync_interval =
                std::min(options_.sync_interval_ps, boundary_latency);
            endpoint.params.sync_mode = options_.sync_mode;
            if (SimbricksBaseIfInit(&endpoint.interface.base, &endpoint.params) != 0 ||
                SimbricksBaseIfConnect(&endpoint.interface.base) != 0)
                throw std::runtime_error("failed to connect UB-NET endpoint " +
                                         endpoint.socket);
            introductions_[i] = {ubnet::kVersion, options_.ports, 16384, 32, 0};
            establish[i] = {&endpoint.interface.base, &introductions_[i],
                            sizeof(introductions_[i]), &endpoint.peer_intro,
                            sizeof(endpoint.peer_intro)};
        }
        if (SimBricksBaseIfEstablish(establish.data(), establish.size()) != 0)
            throw std::runtime_error("UB-NET handshake failed");
        for (const auto& endpoint : endpoints_) {
            if (endpoint.peer_intro.version != ubnet::kVersion)
                throw std::runtime_error("UB-NET version mismatch");
            if (endpoint.peer_intro.port_count < options_.ports)
                throw std::runtime_error("endpoint exposes too few UB ports");
        }
    }

    void BuildTopology()
    {
        const DataRate rate(std::to_string(options_.rate_gbps) + "Gbps");
        for (std::size_t endpoint = 0; endpoint < endpoints_.size(); ++endpoint) {
            Ptr<Node> node = CreateObject<Node>();
            endpoint_nodes_.push_back(node);
            endpoint_by_node_.emplace(node->GetId(), endpoint);
            for (std::uint32_t port = 0; port < options_.ports; ++port) {
                Ptr<UbPort> endpoint_port = CreateObject<UbPort>();
                endpoint_port->SetAddress(Mac48Address::Allocate());
                endpoint_port->SetDataRate(rate);
                node->AddDevice(endpoint_port);
                endpoint_port->SetReceiveHandler(
                    MakeCallback(&UbNetFabric::ReceiveAtEndpoint, this));
                endpoint_ports_[endpoint].push_back(endpoint_port);
            }
        }
        switch_node_ = CreateObject<Node>();
        switch_ = CreateObject<UbSwitch>();
        switch_node_->AggregateObject(switch_);
        switch_->SetNodeType(UB_SWITCH);
        switch_->SetAttribute("FlowControl", EnumValue(FcType::NONE));
        switch_->SetAttribute("InPortProcessingDelay",
                              TimeValue(PicoSeconds(options_.switch_delay_ps)));
        for (std::size_t endpoint = 0; endpoint < endpoints_.size(); ++endpoint) {
            for (std::uint32_t port = 0; port < options_.ports; ++port) {
                Ptr<UbPort> switch_port = CreateObject<UbPort>();
                switch_port->SetAddress(Mac48Address::Allocate());
                switch_port->SetDataRate(rate);
                switch_node_->AddDevice(switch_port);
                switch_ports_.push_back(switch_port);
                Ptr<UbLink> link = CreateObject<UbLink>();
                link->SetAttribute("Delay", TimeValue(PicoSeconds(0)));
                switch_port->Attach(link);
                endpoint_ports_[endpoint][port]->Attach(link);
            }
        }
        switch_->Init();
        for (std::size_t endpoint = 0; endpoint < endpoints_.size(); ++endpoint) {
            std::vector<std::uint16_t> outputs;
            for (std::uint32_t port = 0; port < options_.ports; ++port)
                outputs.push_back(static_cast<std::uint16_t>(
                    endpoint * options_.ports + port));
            switch_->GetRoutingProcess()->AddShortestRoute(
                utils::NodeIdToIp(endpoint_nodes_[endpoint]->GetId()).Get(), outputs);
        }
    }

    void PublishLinks()
    {
        for (auto& endpoint : endpoints_) {
            for (std::uint16_t port = 0; port < options_.ports; ++port) {
                auto* message = ubnet::UbNetOutAlloc(&endpoint.interface, NowPs());
                if (!message) throw std::runtime_error("UB-NET link queue is full");
                ZeroVolatile(message->link);
                message->link.port = port;
                message->link.state = static_cast<std::uint8_t>(ubnet::LinkState::Up);
                ubnet::UbNetOutSend(&endpoint.interface, message,
                    static_cast<std::uint8_t>(ubnet::MessageType::LinkState));
                std::cerr << "[NS3_UB_NET_LINK] eid=0x" << std::hex
                          << endpoint.eid << std::dec << " port=" << port
                          << " state=up\n";
            }
        }
    }

    bool PollOne(std::size_t source, std::uint64_t now)
    {
        auto& endpoint = endpoints_[source];
        auto* message = ubnet::UbNetInPoll(&endpoint.interface, now);
        if (!message) return false;
        const auto type = static_cast<ubnet::MessageType>(
            ubnet::UbNetInType(&endpoint.interface, message));
        if (type == ubnet::MessageType::Frame) {
            const auto ingress = SnapshotVolatile(message->frame);
            if (ingress.length > endpoint.peer_intro.max_frame_bytes ||
                ingress.source_port >= options_.ports ||
                ingress.source_eid != endpoint.eid) {
                ubnet::UbNetInDone(&endpoint.interface, message);
                throw std::runtime_error("invalid UB-NET ingress frame");
            }
            const auto destination = route_.find(ingress.destination_eid);
            if (destination == route_.end() || destination->second == source) {
                ubnet::UbNetInDone(&endpoint.interface, message);
                throw std::runtime_error("UB-NET destination EID is not routable");
            }
            std::vector<std::uint8_t> payload(ingress.length);
            const auto* bytes = reinterpret_cast<const volatile std::uint8_t*>(message) +
                                sizeof(ubnet::Message);
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = bytes[i];
            FrameTag tag;
            tag.sequence = ingress.sequence;
            tag.source_eid = ingress.source_eid;
            tag.destination_eid = ingress.destination_eid;
            tag.source_port = ingress.source_port;
            tag.destination_port = ingress.destination_port;
            tag.traffic_class = ingress.traffic_class;
            tag.flags = ingress.flags;
            Inject(source, std::move(payload), tag);
        }
        ubnet::UbNetInDone(&endpoint.interface, message);
        return true;
    }

    void Inject(std::size_t source, std::vector<std::uint8_t> payload,
                const FrameTag& tag)
    {
        Ptr<Packet> packet = Create<Packet>(payload.data(), payload.size());
        packet->AddPacketTag(tag);
        UbCtpHeader ctp;
        ctp.SetTPOpcode(CtpOpcode::CTP_DATA); ctp.SetPadding(0); ctp.SetNlp(0);
        packet->AddHeader(ctp);
        UbCna16NetworkHeader cna;
        cna.SetScna(static_cast<std::uint16_t>(utils::NodeIdToCna16(
            endpoint_nodes_[source]->GetId(), tag.source_port)));
        const auto destination = route_.at(tag.destination_eid);
        cna.SetDcna(static_cast<std::uint16_t>(utils::NodeIdToCna16(
            endpoint_nodes_[destination]->GetId())));
        cna.SetLb(static_cast<std::uint8_t>(tag.sequence));
        cna.SetServiceLevel(static_cast<std::uint8_t>(tag.traffic_class));
        cna.SetNlp(UB_CNA_NLP_CTPH);
        packet->AddHeader(cna);
        UbDataLink::AddPacketHeader(packet, false, false, 1, 1,
            RoutingType::PER_FLOW_SHORTEST_PATHS,
            UbDatalinkHeaderConfig::PACKET_CNA16);
        ++scheduled_packets_;
        ++forwarded_;
        payload_bytes_ += payload.size();
        switch_->SwitchHandlePacket(
            switch_ports_[source * options_.ports + tag.source_port], packet);
    }

    void ReceiveAtEndpoint(Ptr<UbPort> port, Ptr<Packet> packet)
    {
        const auto endpoint_it = endpoint_by_node_.find(port->GetNode()->GetId());
        NS_ABORT_MSG_IF(endpoint_it == endpoint_by_node_.end(),
                        "packet reached an unknown UB-NET endpoint");
        FrameTag tag;
        NS_ABORT_MSG_IF(!packet->RemovePacketTag(tag), "UB-NET metadata was lost");
        UbDatalinkPacketHeader data_link;
        UbCna16NetworkHeader cna;
        UbCtpHeader ctp;
        packet->RemoveHeader(data_link); packet->RemoveHeader(cna);
        packet->RemoveHeader(ctp);
        QueuedFrame frame;
        frame.header.sequence = tag.sequence;
        frame.header.length = packet->GetSize();
        frame.header.source_eid = tag.source_eid;
        frame.header.destination_eid = tag.destination_eid;
        frame.header.source_port = tag.source_port;
        frame.header.destination_port = port->GetIfIndex();
        frame.header.traffic_class = tag.traffic_class;
        frame.header.flags = tag.flags;
        frame.payload.resize(packet->GetSize());
        packet->CopyData(frame.payload.data(), frame.payload.size());
        endpoints_[endpoint_it->second].outgoing.push_back(std::move(frame));
        --scheduled_packets_;
        ++delivered_;
    }

    bool Flush(Endpoint& endpoint, std::uint64_t now)
    {
        bool progress = false;
        while (!endpoint.outgoing.empty()) {
            auto* output = ubnet::UbNetOutAlloc(&endpoint.interface, now);
            if (!output) break;
            const auto& frame = endpoint.outgoing.front();
            ZeroVolatile(output->frame);
            output->frame.sequence = frame.header.sequence;
            output->frame.length = frame.header.length;
            output->frame.source_eid = frame.header.source_eid;
            output->frame.destination_eid = frame.header.destination_eid;
            output->frame.source_port = frame.header.source_port;
            output->frame.destination_port = frame.header.destination_port;
            output->frame.traffic_class = frame.header.traffic_class;
            output->frame.flags = frame.header.flags;
            auto* bytes = reinterpret_cast<volatile std::uint8_t*>(output) +
                          sizeof(ubnet::Message);
            for (std::size_t i = 0; i < frame.payload.size(); ++i)
                bytes[i] = frame.payload[i];
            ubnet::UbNetOutSend(&endpoint.interface, output,
                static_cast<std::uint8_t>(ubnet::MessageType::Frame));
            endpoint.outgoing.pop_front();
            progress = true;
        }
        return progress;
    }

    bool OutputsEmpty() const
    {
        return std::all_of(endpoints_.begin(), endpoints_.end(),
            [](const Endpoint& endpoint) { return endpoint.outgoing.empty(); });
    }

    static std::uint64_t NowPs()
    {
        return static_cast<std::uint64_t>(Simulator::Now().GetPicoSeconds());
    }

    static void AdvanceTo(std::uint64_t target)
    {
        const std::uint64_t now = NowPs();
        if (target <= now) return;
        Simulator::Schedule(PicoSeconds(target - now), [] { Simulator::Stop(); });
        Simulator::Run();
    }

    Options options_;
    std::vector<Endpoint> endpoints_;
    std::vector<ubnet::Intro> introductions_;
    std::unordered_map<std::uint32_t, std::size_t> route_;
    std::vector<Ptr<Node>> endpoint_nodes_;
    std::vector<std::vector<Ptr<UbPort>>> endpoint_ports_;
    std::unordered_map<std::uint32_t, std::size_t> endpoint_by_node_;
    Ptr<Node> switch_node_;
    Ptr<UbSwitch> switch_;
    std::vector<Ptr<UbPort>> switch_ports_;
    std::uint64_t scheduled_packets_{};
    std::uint64_t forwarded_{};
    std::uint64_t delivered_{};
    std::uint64_t payload_bytes_{};
};

} // namespace

int main(int argc, char** argv)
{
    try {
        std::signal(SIGINT, StopProcess);
        std::signal(SIGTERM, StopProcess);
        Time::SetResolution(Time::PS);
        UbNetFabric fabric(ParseOptions(argc, argv));
        fabric.Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ub-net-adapter: " << error.what() << '\n';
        std::cerr << "usage: ub-net-adapter --endpoint SOCKET,EID "
                     "--endpoint SOCKET,EID [--endpoint ...] [--ports N] "
                     "[--link-delay-ps N] [--switch-delay-ps N] "
                     "[--rate-gbps N] [--sync off|optional|required] "
                     "[--sync-interval-ps N]\n";
        return 1;
    }
}
