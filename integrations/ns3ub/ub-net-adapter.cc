// SPDX-License-Identifier: GPL-2.0-only
// Native ns-3-UB fabric attached through the simulator-neutral UB-NET v1 ABI.

#include "ns3/core-module.h"
#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-header.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-function.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-utils.h"

#include "protocol/ub_net/if.h"
#include "protocol/ub_net/udma_wire.h"
#include "integrations/ns3ub/ctp-queue-feedback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ns3;
namespace ubnet = ubsim::proto::net;

namespace {

std::atomic<bool> running{true};
constexpr std::size_t kPollBatch = 256;
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
    bool lifecycle_sync{false};
    bool ctp_retransmission{false};
    std::uint64_t ctp_rto_ps{25600000};
    std::uint32_t ctp_max_retransmissions{7};
    bool drop_first_ctp_request{false};
    bool drop_first_ctp_taack{false};
    bool drop_first_ctp_read_response{false};
    bool drop_all_ctp_requests{false};
    bool inject_ctp_cnp_after_first_segment{false};
    std::uint64_t ctp_recovery_interval_ps{0};
    std::uint64_t ctp_recovery_step_bps{0};
    std::uint64_t ctp_mark_threshold_bytes{0};
    std::uint64_t ctp_cnp_interval_ps{1000000};
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
        } else if (arg == "--lifecycle-sync") {
            options.lifecycle_sync = true;
        } else if (arg == "--ctp-retransmission" && i + 1 < argc) {
            const std::string value(argv[++i]);
            if (value == "on") options.ctp_retransmission = true;
            else if (value == "off") options.ctp_retransmission = false;
            else throw std::runtime_error("CTP retransmission must be on or off");
        } else if (arg == "--ctp-rto-ps" && i + 1 < argc) {
            options.ctp_rto_ps = ParseUnsigned(argv[++i], "CTP RTO");
        } else if (arg == "--ctp-max-retransmissions" && i + 1 < argc) {
            const auto value = ParseUnsigned(argv[++i],
                                             "CTP max retransmissions");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error(
                    "CTP max retransmissions exceeds uint32 range");
            options.ctp_max_retransmissions = static_cast<std::uint32_t>(value);
        } else if (arg == "--drop-first-ctp-request") {
            options.drop_first_ctp_request = true;
        } else if (arg == "--drop-first-ctp-taack") {
            options.drop_first_ctp_taack = true;
        } else if (arg == "--drop-first-ctp-read-response") {
            options.drop_first_ctp_read_response = true;
        } else if (arg == "--drop-all-ctp-requests") {
            options.drop_all_ctp_requests = true;
        } else if (arg == "--inject-ctp-cnp-after-first-segment") {
            options.inject_ctp_cnp_after_first_segment = true;
        } else if (arg == "--ctp-mark-threshold-bytes" && i + 1 < argc) {
            options.ctp_mark_threshold_bytes = ParseUnsigned(argv[++i], "CTP queue threshold");
        } else if (arg == "--ctp-cnp-interval-ps" && i + 1 < argc) {
            options.ctp_cnp_interval_ps = ParseUnsigned(argv[++i], "CTP CNP interval");
        } else if (arg == "--ctp-recovery-interval-ps" && i + 1 < argc) {
            options.ctp_recovery_interval_ps = ParseUnsigned(argv[++i], "CTP recovery interval");
        } else if (arg == "--ctp-recovery-step-bps" && i + 1 < argc) {
            options.ctp_recovery_step_bps = ParseUnsigned(argv[++i], "CTP recovery step");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + arg);
        }
    }
    if ((options.ctp_recovery_interval_ps == 0) != (options.ctp_recovery_step_bps == 0))
        throw std::runtime_error("CTP recovery requires both interval and additive step");
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

UbTransactionOpcode TransactionOpcode(std::uint8_t operation)
{
    switch (static_cast<ubnet::UdmaOperation>(operation)) {
      case ubnet::UdmaOperation::Send: return UbTransactionOpcode::SEND;
      case ubnet::UdmaOperation::SendImmediate:
        return UbTransactionOpcode::SEND_WITH_IMMEDIATE;
      case ubnet::UdmaOperation::Write: return UbTransactionOpcode::WRITE;
      case ubnet::UdmaOperation::ReadRequest: return UbTransactionOpcode::READ;
      case ubnet::UdmaOperation::WriteAck: return UbTransactionOpcode::TAACK;
      case ubnet::UdmaOperation::ReadResponse:
        return UbTransactionOpcode::READ_RESPONSE;
      case ubnet::UdmaOperation::RmaError:
        return UbTransactionOpcode::MAX_OPCODE;
    }
    return UbTransactionOpcode::MAX_OPCODE;
}

struct QueuedFrame {
    ubnet::Frame header{};
    std::vector<std::uint8_t> payload;
};

struct PartialWqe {
    ubnet::Frame frame{};
    ubnet::UdmaWireHeader wire{};
    std::vector<std::uint8_t> payload;
    std::uint32_t next_offset{};
};

struct NativeSegment {
    std::uint32_t offset{};
    std::uint32_t bytes{};
    std::vector<std::uint8_t> read_payload;
};

struct NativeTask {
    std::size_t source{};
    std::size_t destination{};
    std::uint64_t sequence{};
    ubnet::UdmaWireHeader wire{};
    std::uint32_t task_id{};
    std::uint32_t next_offset{};
    std::map<std::uint16_t, NativeSegment> segments;
};

struct TargetExecutionKey {
    std::size_t source{};
    std::uint64_t request_id{};
    std::uint16_t ta_ssn{};
    bool operator<(const TargetExecutionKey& other) const
    {
        return std::tie(source, request_id, ta_ssn) <
               std::tie(other.source, other.request_id, other.ta_ssn);
    }
};

struct PendingTargetExecution {
    UbTargetCompletion completion;
    std::uint32_t task_id{};
    std::uint16_t ta_ssn{};
};

struct Endpoint {
    ubnet::Interface interface{};
    SimbricksBaseIfParams params{};
    ubnet::Intro peer_intro{};
    std::string socket;
    std::uint32_t eid{};
    std::deque<QueuedFrame> outgoing;
    bool lifecycle_prepare{false};
    std::uint64_t lifecycle_generation{};
    bool lifecycle_enabled{false};
    bool sync_primed{false};
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
          endpoint_ports_(options.endpoints.size()),
          partial_wqes_(options.endpoints.size()),
          last_native_tassn_(options.endpoints.size())
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
        const auto wall_started = std::chrono::steady_clock::now();
        PublishLinks();
        std::cerr << "[NS3_UB_NET] connected " << endpoints_.size()
                  << " UB-NET endpoints"
                  << " ports=" << options_.ports
                  << " rate_gbps=" << options_.rate_gbps
                  << " ctp_retransmission="
                  << (options_.ctp_retransmission ? "on" : "off")
                  << " ctp_rto_ps=" << options_.ctp_rto_ps
                  << " boundary=ub-net-v1\n";
        while (running.load()) {
            ++loop_iterations_;
            std::uint64_t now = NowPs();
            bool progress = false;
            bool all_terminated = true;
            for (std::size_t i = 0; i < endpoints_.size(); ++i) {
                for (std::size_t count = 0;
                     count < kPollBatch && running.load() && PollOne(i, now);
                     ++count)
                    progress = true;
                all_terminated = all_terminated &&
                    SimbricksBaseIfInTerminated(&endpoints_[i].interface.base);
            }
            // PollOne may jump asynchronous time to an incoming message.
            now = NowPs();
            for (auto& endpoint : endpoints_)
                progress = Flush(endpoint, now) || progress;
            if (options_.lifecycle_sync &&
                scheduled_packets_ == 0 && OutputsEmpty() &&
                std::all_of(endpoints_.begin(), endpoints_.end(),
                    [](const Endpoint& endpoint) {
                        return endpoint.lifecycle_prepare;
                    })) {
                const std::uint64_t generation =
                    endpoints_.front().lifecycle_generation;
                const bool same_generation = std::all_of(
                    endpoints_.begin(), endpoints_.end(),
                    [generation](const Endpoint& endpoint) {
                        return endpoint.lifecycle_generation == generation;
                    });
                if (!same_generation)
                    throw std::runtime_error("lifecycle fence generation mismatch");
                const bool enable = endpoints_.front().lifecycle_enabled;
                const bool same_target = std::all_of(
                    endpoints_.begin(), endpoints_.end(),
                    [enable](const Endpoint& endpoint) {
                        return endpoint.lifecycle_enabled == enable;
                    });
                if (!same_target || enable == lifecycle_active_)
                    throw std::runtime_error("lifecycle fence target mismatch");
                bool commits_sent = true;
                for (auto& endpoint : endpoints_)
                    commits_sent = SendLifecycleCommit(endpoint, now, generation,
                                                       enable) &&
                                   commits_sent;
                if (!commits_sent) {
                    ++output_backpressure_;
                    std::this_thread::yield();
                    continue;
                }
                const std::uint64_t absolute_now = NowPs();
                for (auto& endpoint : endpoints_) {
                    endpoint.interface.base.in_timestamp = enable ? 0 : absolute_now;
                    endpoint.interface.base.out_timestamp = enable ? 0 : absolute_now;
                    endpoint.interface.base.sync = enable;
                    endpoint.lifecycle_prepare = false;
                    endpoint.sync_primed = false;
                }
                epoch_origin_ps_ = enable ? absolute_now : 0;
                if (enable) {
                    time_offset_ps_ = -static_cast<std::int64_t>(
                        AbsoluteNowPs());
                }
                lifecycle_active_ = enable;
                std::cerr << "[NS3_UB_NET_FENCE] generation=" << generation
                          << " active=" << (enable ? 1 : 0)
                          << " absolute_origin_ps=" << epoch_origin_ps_
                          << " endpoints=" << endpoints_.size() << '\n';
            }
            if (all_terminated && scheduled_packets_ == 0 && OutputsEmpty()) break;
            if (!OutputsEmpty()) {
                ++output_backpressure_;
                if (!progress)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            bool sync_blocked = false;
            for (auto& endpoint : endpoints_) {
                if (!SimbricksBaseIfSyncEnabled(&endpoint.interface.base))
                    continue;
                if (endpoint.sync_primed &&
                    now < ubnet::UbNetOutNextSync(&endpoint.interface))
                    continue;
                const int result = ubnet::UbNetOutSync(&endpoint.interface, now);
                if (result == 0) endpoint.sync_primed = true;
                sync_blocked = result != 0 || sync_blocked;
            }
            if (sync_blocked) {
                ++sync_backpressure_;
                continue;
            }

            bool synchronized = false;
            std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
            for (auto& endpoint : endpoints_) {
                if (!SimbricksBaseIfSyncEnabled(&endpoint.interface.base)) continue;
                synchronized = true;
                next = std::min(next,
                    ubnet::UbNetInTimestamp(&endpoint.interface));
                next = std::min(next, ubnet::UbNetOutNextSync(&endpoint.interface));
            }
            if (synchronized && next > now &&
                next != std::numeric_limits<std::uint64_t>::max()) {
                AdvanceTo(next);
                ++sync_steps_;
                progress = true;
            } else if (!synchronized && scheduled_packets_) {
                // ns-3-UB owns periodic maintenance events, so an unbounded
                // Simulator::Run() never drains. Advance one finite quantum
                // and return to the process boundary to flush completed
                // frames and observe newly arrived work.
                AdvanceTo(now + options_.sync_interval_ps);
                progress = true;
            }
            if (!progress)
            {
                ++idle_sleeps_;
                if (!synchronized)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - wall_started).count();
        std::uint64_t ctp_retransmissions = 0;
        std::uint64_t ctp_retransmission_exhausted = 0;
        std::uint64_t duplicate_write_suppressed = 0;
        std::uint64_t duplicate_taack_replays = 0;
        std::uint64_t injected_taack_drops = 0;
        std::uint64_t duplicate_read_suppressed = 0;
        std::uint64_t duplicate_read_response_replays = 0;
        std::uint64_t injected_read_response_drops = 0;
        std::uint64_t ctp_congestion_rate_cuts = 0;
        std::uint64_t ctp_cnp_sent = 0, ctp_cnp_suppressed = 0, ctp_wire_waits = 0;
        std::uint64_t injected_ctp_request_drops = 0;
        for (const auto& node : endpoint_nodes_) {
            Ptr<UbController> controller = node->GetObject<UbController>();
            Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
            ctp_retransmissions += service->GetRetransmissionCount();
            ctp_retransmission_exhausted += service->GetRetransmissionExhaustedCount();
            duplicate_write_suppressed += service->GetDuplicateWriteSuppressedCount();
            duplicate_taack_replays += service->GetDuplicateTaAckReplayCount();
            injected_taack_drops += service->GetInjectedTaAckDropCount();
            duplicate_read_suppressed += service->GetDuplicateReadSuppressedCount();
            duplicate_read_response_replays +=
                service->GetDuplicateReadResponseReplayCount();
            injected_read_response_drops +=
                service->GetInjectedReadResponseDropCount();
            ctp_congestion_rate_cuts += service->GetCongestionRateCutCount();
            ctp_cnp_sent += service->GetCnpSentCount();
            ctp_cnp_suppressed += service->GetCnpSuppressedCount();
            ctp_wire_waits += service->GetWirePacingWaitCount();
            injected_ctp_request_drops += service->GetInjectedRequestDropCount();
        }
        std::cerr << "[NS3_UB_NET_STATS] forwarded=" << forwarded_
                  << " delivered=" << delivered_ << " payload_bytes="
                  << payload_bytes_ << " virtual_ps=" << NowPs()
                  << " wall_ns=" << wall_ns
                  << " loops=" << loop_iterations_
                  << " idle_sleeps=" << idle_sleeps_
                  << " sync_steps=" << sync_steps_
                  << " sync_backpressure=" << sync_backpressure_
                  << " async_timestamp_jumps=" << async_timestamp_jumps_
                  << " async_timestamp_jump_ps=" << async_timestamp_jump_ps_
                  << " output_backpressure=" << output_backpressure_
                  << " ctp_request_segments=" << ctp_request_segments_
                  << " ctp_taacks=" << ctp_taacks_
                  << " ctp_read_responses=" << ctp_read_responses_
                  << " ctp_max_payload=" << ctp_max_payload_
                  << " ctp_native_admitted=" << ctp_native_admitted_
                  << " ctp_native_window_blocked=" << ctp_native_window_blocked_
                  << " ctp_native_max_outstanding=" << ctp_native_max_outstanding_
                  << " ctp_native_completions=" << ctp_native_completions_
                  << " ctp_native_send_delivery_completions="
                  << ctp_native_send_delivery_completions_
                  << " native_wqes_submitted=" << native_wqes_submitted_
                  << " native_wqes_completed=" << native_wqes_completed_
                  << " native_wqes_failed=" << native_wqes_failed_
                  << " native_order_no=" << native_order_no_
                  << " native_order_relax=" << native_order_relax_
                  << " native_order_strong=" << native_order_strong_
                  << " native_trace_size_mismatches="
                  << native_trace_size_mismatches_
                  << " ns3_runtime_drops="
                  << utils::UbUtils::GetRuntimePacketDropCount()
                  << " ctp_tassn_discontinuities="
                  << ctp_tassn_discontinuities_
                  << " ctp_retransmissions=" << ctp_retransmissions
                  << " ctp_retransmission_exhausted="
                  << ctp_retransmission_exhausted
                  << " injected_ctp_request_drops="
                  << injected_ctp_request_drops
                  << " duplicate_write_suppressed=" << duplicate_write_suppressed
                  << " duplicate_taack_replays=" << duplicate_taack_replays
                  << " injected_taack_drops=" << injected_taack_drops
                  << " duplicate_read_suppressed=" << duplicate_read_suppressed
                  << " duplicate_read_response_replays="
                  << duplicate_read_response_replays
                  << " injected_read_response_drops="
                  << injected_read_response_drops
                  << " ctp_congestion_rate_cuts="
                  << ctp_congestion_rate_cuts
                  << " ctp_native_segment_send_span_ps="
                  << (last_native_segment_send_ps_ - first_native_segment_send_ps_)
                  << " ctp_second_segment_gap_ps=" << second_native_segment_gap_ps_
                  << " ctp_queue_marks=" << ctp_feedback_->marked
                  << " ctp_cnp_sent=" << ctp_cnp_sent
                  << " ctp_cnp_suppressed=" << ctp_cnp_suppressed
                  << " ctp_wire_pacing_waits=" << ctp_wire_waits
                  << " ctp_peak_queue_bytes=" << ctp_feedback_->peakBytes
                  << '\n';
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
            // The external UDMA process is the host-facing half of a UB
            // device.  Model its ns-3 half as a UB_DEVICE as well so compact
            // CTP Entity routing uses the same registry and member-port
            // selection as native ns-3-UB endpoints.
            Ptr<UbSwitch> endpoint_switch = CreateObject<UbSwitch>();
            endpoint_switch->SetNodeType(UB_DEVICE);
            node->AggregateObject(endpoint_switch);
            Ptr<UbController> controller = CreateObject<UbController>();
            node->AggregateObject(controller);
            controller->CreateUbFunction();
            controller->CreateUbTransaction();
            endpoint_nodes_.push_back(node);
            endpoint_by_node_.emplace(node->GetId(), endpoint);
            std::vector<std::uint32_t> entity_ports;
            for (std::uint32_t port = 0; port < options_.ports; ++port) {
                Ptr<UbPort> endpoint_port = CreateObject<UbPort>();
                endpoint_port->SetAddress(Mac48Address::Allocate());
                endpoint_port->SetDataRate(rate);
                node->AddDevice(endpoint_port);
                endpoint_ports_[endpoint].push_back(endpoint_port);
                entity_ports.push_back(port);
            }
            controller->CreateCtpEntity(endpoints_[endpoint].eid, entity_ports);
            controller->FreezeCtpEntities();
            endpoint_switch->Init();
            Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
            service->SetRetransmissionEnabled(options_.ctp_retransmission);
            service->SetRetransmissionTimeout(PicoSeconds(options_.ctp_rto_ps));
            service->SetMaxRetransmissionAttempts(options_.ctp_max_retransmissions);
            const std::uint64_t line_rate_bps = options_.rate_gbps * 1000000000ULL;
            service->SetCongestionLineRate(line_rate_bps);
            service->SetCnpFeedbackInterval(PicoSeconds(options_.ctp_cnp_interval_ps));
            service->SetCongestionRecovery(PicoSeconds(options_.ctp_recovery_interval_ps),
                                           options_.ctp_recovery_step_bps);
            service->SetCongestionMinimumRate(
                std::max<std::uint64_t>(1, line_rate_bps / 1024));
            service->SetDropNextTaAckForTest(options_.drop_first_ctp_taack);
            service->SetDropNextReadResponseForTest(
                options_.drop_first_ctp_read_response);
            service->SetDropNextRequestForTest(options_.drop_first_ctp_request);
            service->SetDropAllRequestsForTest(options_.drop_all_ctp_requests);
            controller->GetUbTransaction()->SetTargetExecutor(
                UbTargetExecutor(MakeCallback(&UbNetFabric::OnTargetExecute, this)
                                     .Bind(endpoint)));
            service->TraceConnectWithoutContext(
                "FirstPacketSendsNotify",
                MakeCallback(&UbNetFabric::OnNativeSegmentSent, this));
            service->TraceConnectWithoutContext(
                "LastPacketACKsNotify",
                MakeCallback(&UbNetFabric::OnNativeSegmentComplete, this));
        }
        switch_node_ = CreateObject<Node>();
        switch_ = CreateObject<UbSwitch>();
        switch_node_->AggregateObject(switch_);
        switch_->SetNodeType(UB_SWITCH);
        // Preserve ns-3-UB's lossless-fabric behavior.  Without flow control,
        // a multi-segment WQE can overflow the small port staging queues
        // before the 400-Gbit/s link serializes the burst.  CTP RM would then
        // wait forever for a segment that never reached target execution.
        switch_->SetAttribute("FlowControl", EnumValue(FcType::CBFC));
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
        ctp_feedback_ = CreateObject<UbCtpQueueFeedback>();
        ctp_feedback_->Configure(switch_, options_.ctp_mark_threshold_bytes);
        switch_->SetCongestionCtrl(ctp_feedback_);
        for (std::size_t endpoint = 0; endpoint < endpoints_.size(); ++endpoint) {
            std::vector<std::uint16_t> outputs;
            for (std::uint32_t port = 0; port < options_.ports; ++port)
                outputs.push_back(static_cast<std::uint16_t>(
                    endpoint * options_.ports + port));
            switch_->GetRoutingProcess()->AddShortestRoute(
                utils::NodeIdToIp(endpoint_nodes_[endpoint]->GetId()).Get(), outputs);
            for (std::uint32_t port = 0; port < options_.ports; ++port)
                switch_->GetRoutingProcess()->AddShortestRoute(
                    utils::NodeIdToIp(endpoint_nodes_[endpoint]->GetId(), port).Get(),
                    {static_cast<std::uint16_t>(endpoint * options_.ports + port)});
        }
        // Native CTP performs source-Entity member selection on the endpoint
        // switch before the packet reaches the central fabric.  Give every
        // endpoint routes to every remote Entity member, with all local
        // member ports as equal-cost outputs.  The CTP routing policy then
        // selects the concrete source port rather than bypassing endpoint
        // routing as the former manual injection path did.
        for (std::size_t source = 0; source < endpoints_.size(); ++source) {
            std::vector<std::uint16_t> local_ports;
            for (std::uint32_t port = 0; port < options_.ports; ++port)
                local_ports.push_back(static_cast<std::uint16_t>(port));
            Ptr<UbRoutingProcess> routing =
                endpoint_nodes_[source]->GetObject<UbSwitch>()->GetRoutingProcess();
            for (std::size_t destination = 0; destination < endpoints_.size(); ++destination) {
                if (destination == source) continue;
                const std::uint32_t destination_node =
                    endpoint_nodes_[destination]->GetId();
                routing->AddShortestRoute(
                    utils::NodeIdToIp(destination_node).Get(), local_ports);
                for (std::uint32_t port = 0; port < options_.ports; ++port)
                    routing->AddShortestRoute(
                        utils::NodeIdToIp(destination_node, port).Get(), local_ports);
            }
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
        const std::uint64_t message_time = message->base.header.timestamp;
        if (!SimbricksBaseIfSyncEnabled(&endpoint.interface.base) &&
            message_time > NowPs()) {
            // Async peers do not exchange conservative horizons.  The input
            // timestamp is nevertheless the event time and can be reached in
            // one jump instead of replaying every synchronization quantum.
            async_timestamp_jump_ps_ += message_time - NowPs();
            ++async_timestamp_jumps_;
            AdvanceTo(message_time);
        }
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
            if (payload.size() < sizeof(ubnet::UdmaWireHeader))
                throw std::runtime_error("native adapter requires a UDMA WQE envelope");
            ubnet::UdmaWireHeader wire{};
            std::memcpy(&wire, payload.data(), sizeof(wire));
            if (wire.magic != ubnet::kUdmaWireMagic ||
                wire.version != ubnet::kUdmaWireVersion ||
                payload.size() != sizeof(wire) + wire.payload_length)
                throw std::runtime_error("invalid UDMA WQE envelope");
            payload.erase(payload.begin(), payload.begin() + sizeof(wire));
            if (wire.flags & ubnet::kUdmaWireCtpSegment)
                CompleteTargetExecution(source, ingress, wire, std::move(payload));
            else
                AcceptWqeChunk(source, ingress, wire, std::move(payload));
        } else if (type == ubnet::MessageType::Lifecycle &&
                   message->lifecycle.action == static_cast<std::uint8_t>(
                       ubnet::LifecycleAction::PrepareSync)) {
            endpoint.lifecycle_prepare = true;
            endpoint.lifecycle_generation = message->lifecycle.generation;
            endpoint.lifecycle_enabled = message->lifecycle.enabled != 0;
        }
        ubnet::UbNetInDone(&endpoint.interface, message);
        return true;
    }

    static bool IsCtpRequest(std::uint8_t operation)
    {
        const auto value = static_cast<ubnet::UdmaOperation>(operation);
        return value == ubnet::UdmaOperation::Send ||
               value == ubnet::UdmaOperation::SendImmediate ||
               value == ubnet::UdmaOperation::Write ||
               value == ubnet::UdmaOperation::ReadRequest;
    }

    static bool IsTargetCompletion(std::uint8_t operation)
    {
        const auto value = static_cast<ubnet::UdmaOperation>(operation);
        return value == ubnet::UdmaOperation::WriteAck ||
               value == ubnet::UdmaOperation::ReadResponse;
    }

    void AcceptWqeChunk(std::size_t source,
                        const ubnet::Frame& frame,
                        const ubnet::UdmaWireHeader& wire,
                        std::vector<std::uint8_t> payload)
    {
        if (!IsCtpRequest(wire.operation) || IsTargetCompletion(wire.operation) ||
            wire.payload_offset + wire.payload_length > wire.transfer_length ||
            ubnet::UdmaWireOrder(wire.flags) >
                static_cast<std::uint8_t>(OrderType::ORDER_STRONG))
            throw std::runtime_error("invalid complete-WQE request chunk");
        const bool read = static_cast<ubnet::UdmaOperation>(wire.operation) ==
                          ubnet::UdmaOperation::ReadRequest;
        if (read && (wire.payload_length != 0 || wire.payload_offset != 0))
            throw std::runtime_error("READ WQE envelope unexpectedly carries payload");
        auto& partials = partial_wqes_.at(source);
        auto [it, inserted] = partials.try_emplace(frame.sequence);
        PartialWqe& partial = it->second;
        if (inserted) {
            if (wire.payload_offset != 0)
                throw std::runtime_error("first WQE IPC chunk has non-zero offset");
            partial.frame = frame;
            partial.wire = wire;
            partial.payload.reserve(read ? 0 : wire.transfer_length);
        } else if (wire.payload_offset != partial.next_offset ||
                   wire.operation != partial.wire.operation ||
                   wire.request_id != partial.wire.request_id ||
                   wire.transfer_length != partial.wire.transfer_length) {
            throw std::runtime_error("non-contiguous or mismatched WQE IPC chunks");
        }
        partial.payload.insert(partial.payload.end(), payload.begin(), payload.end());
        partial.next_offset += wire.payload_length;
        if (!(wire.flags & ubnet::kUdmaWireLastFragment)) return;
        const std::size_t expected = read ? 0 : wire.transfer_length;
        if (partial.payload.size() != expected)
            throw std::runtime_error("complete WQE payload length mismatch");
        PartialWqe complete = std::move(partial);
        partials.erase(it);
        SubmitNativeWqe(source, std::move(complete));
    }

    void SubmitNativeWqe(std::size_t source, PartialWqe complete)
    {
        const auto destination = route_.at(complete.frame.destination_eid);
        Ptr<UbController> controller =
            endpoint_nodes_.at(source)->GetObject<UbController>();
        Ptr<UbFunction> function = controller->GetUbFunction();
        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        // This Jetty belongs to the modeled UB endpoint, not to the guest
        // provider.  Keep one reusable native Jetty per endpoint/Entity path;
        // guest Jetty ids remain metadata in NativeTask and may be created and
        // destroyed independently by every perftest invocation.
        constexpr std::uint32_t jetty_id = 1;
        const UbCtpEntityKey key{.srcEntityId = complete.frame.source_eid,
                                 .dstNodeId = endpoint_nodes_[destination]->GetId(),
                                 .dstEntityId = complete.frame.destination_eid,
                                 .vl = 1};
        if (!function->IsJettyExists(jetty_id))
            function->CreateJetty(endpoint_nodes_[source]->GetId(),
                                  endpoint_nodes_[destination]->GetId(), jetty_id);
        Ptr<UbJetty> jetty = function->GetJetty(jetty_id);
        if (!service->HasJettyPreparation(jetty_id) && !service->PrepareJetty(jetty, key))
            throw std::runtime_error("failed to prepare native CTP Jetty");
        jetty->SetWqeCompletionCallback(
            MakeCallback(&UbNetFabric::OnNativeWqeTerminal, this));
        const std::uint32_t task_id = next_native_task_id_++;
        const UbTransactionOpcode opcode = TransactionOpcode(complete.wire.operation);
        Ptr<UbWqe> wqe = function->CreateWqe(endpoint_nodes_[source]->GetId(),
                                              endpoint_nodes_[destination]->GetId(),
                                              complete.wire.transfer_length,
                                              task_id, opcode);
        wqe->SetSrcEntityId(complete.frame.source_eid);
        wqe->SetDstEntityId(complete.frame.destination_eid);
        wqe->SetSport(CTP_WILDCARD_PORT);
        wqe->SetDport(CTP_WILDCARD_PORT);
        wqe->SetPriority(1);
        const auto order = static_cast<OrderType>(
            ubnet::UdmaWireOrder(complete.wire.flags));
        wqe->SetOrderType(order);
        if (order == OrderType::ORDER_NO) ++native_order_no_;
        else if (order == OrderType::ORDER_RELAX) ++native_order_relax_;
        else if (order == OrderType::ORDER_STRONG) ++native_order_strong_;
        wqe->SetRemoteAddress(complete.wire.remote_address);
        wqe->SetRemoteTokenId(complete.wire.segment);
        if (!complete.payload.empty())
            wqe->SetExplicitPayload(Create<Packet>(complete.payload.data(),
                                                   complete.payload.size()));
        if (opcode == UbTransactionOpcode::SEND_WITH_IMMEDIATE) {
            UbTransactionFields fields;
            fields.immediate = UbImmediateFields{complete.wire.immediate, 0,
                                                 complete.wire.transfer_length};
            wqe->SetTransactionFields(fields);
        }
        native_tasks_.emplace(task_id, NativeTask{source, destination,
            complete.frame.sequence, complete.wire, task_id, 0, {}});
        ++scheduled_packets_;
        ++native_wqes_submitted_;
        if (!controller->SubmitUrmaWqe(jetty_id, wqe, {})) {
            native_tasks_.erase(task_id);
            --scheduled_packets_;
            throw std::runtime_error("native CTP WQE submission was rejected");
        }
        service->NotifyJettyWork(jetty_id);
    }

    void QueueUdma(std::size_t endpoint,
                   const ubnet::Frame& header,
                   const ubnet::UdmaWireHeader& wire,
                   const std::vector<std::uint8_t>& payload)
    {
        QueuedFrame frame;
        frame.header = header;
        const auto* begin = reinterpret_cast<const std::uint8_t*>(&wire);
        frame.payload.assign(begin, begin + sizeof(wire));
        frame.payload.insert(frame.payload.end(), payload.begin(), payload.end());
        frame.header.length = static_cast<std::uint32_t>(frame.payload.size());
        endpoints_.at(endpoint).outgoing.push_back(std::move(frame));
    }

    void OnNativeSegmentSent(std::uint32_t node_id, std::uint32_t task_id,
                             std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t,
                             std::uint32_t, std::uint32_t ta_ssn,
                             std::uint32_t, std::uint32_t carrier_bytes,
                             std::uint32_t)
    {
        auto task = native_tasks_.find(task_id);
        if (task == native_tasks_.end() ||
            endpoint_nodes_[task->second.source]->GetId() != node_id)
            throw std::runtime_error("native CTP segment trace has no external WQE");
        const std::uint16_t wire_ssn = static_cast<std::uint16_t>(ta_ssn);
        // A CTP READ request carries only the encoded request carrier (one
        // byte in the native model), while its logical segment describes up
        // to one MTU of target memory and response payload.  Preserve that
        // distinction at the process boundary: the native Jetty still owns
        // segmentation, and this mapping only identifies the corresponding
        // range in the guest WQE buffer.
        const bool read = static_cast<ubnet::UdmaOperation>(
                              task->second.wire.operation) ==
                          ubnet::UdmaOperation::ReadRequest;
        if (task->second.next_offset >= task->second.wire.transfer_length)
            throw std::runtime_error("native CTP emitted excess external WQE segment");
        const std::uint32_t remaining = task->second.wire.transfer_length -
                                        task->second.next_offset;
        const std::uint32_t logical_bytes = read
            ? std::min<std::uint32_t>(UB_MTU_BYTE, remaining)
            : carrier_bytes;
        if (logical_bytes == 0 || logical_bytes > remaining)
            throw std::runtime_error("native CTP segment exceeds external WQE");
        task->second.segments.emplace(
            wire_ssn, NativeSegment{task->second.next_offset, logical_bytes, {}});
        task->second.next_offset += logical_bytes;
        const std::size_t source = task->second.source;
        if (last_native_tassn_[source] &&
            ta_ssn != *last_native_tassn_[source] + 1)
            ++ctp_tassn_discontinuities_;
        last_native_tassn_[source] = ta_ssn;
        ++ctp_request_segments_;
        ++ctp_native_admitted_;
        const std::uint64_t send_ps = static_cast<std::uint64_t>(
            Simulator::Now().GetPicoSeconds());
        if (ctp_request_segments_ == 1)
            first_native_segment_send_ps_ = send_ps;
        if (ctp_request_segments_ == 2)
            second_native_segment_gap_ps_ = send_ps - first_native_segment_send_ps_;
        last_native_segment_send_ps_ = send_ps;
        ++forwarded_;
        payload_bytes_ += logical_bytes;
        ctp_max_payload_ = std::max<std::uint64_t>(ctp_max_payload_, logical_bytes);
        Ptr<UbCtpTransactionContext> context =
            endpoint_nodes_[task->second.source]->GetObject<UbController>()
                ->GetCtpTransportService()->GetOrCreateTransactionContext(
                    {.srcEntityId = endpoints_[task->second.source].eid,
                     .dstNodeId = endpoint_nodes_[task->second.destination]->GetId(),
                     .dstEntityId = endpoints_[task->second.destination].eid,
                     .vl = 1});
        ctp_native_max_outstanding_ = std::max<std::uint64_t>(
            ctp_native_max_outstanding_, context->GetOutstandingCount());
        if (options_.inject_ctp_cnp_after_first_segment && !ctp_cnp_injected_)
        {
            endpoint_nodes_[source]->GetObject<UbController>()
                ->GetCtpTransportService()->RecordCnpForTest(
                    {.srcEntityId = endpoints_[source].eid,
                     .dstNodeId = endpoint_nodes_[task->second.destination]->GetId(),
                     .dstEntityId = endpoints_[task->second.destination].eid,
                     .vl = 1});
            ctp_cnp_injected_ = true;
        }
    }

    void OnTargetExecute(std::size_t target,
                         Ptr<const UbWqeSegment> request,
                         const UbTransactionRule&,
                         UbTargetCompletion completion)
    {
        if (request == nullptr) return completion(UbWorkExecutionResult{});
        const auto source_it = endpoint_by_node_.find(request->GetSrc());
        auto task = native_tasks_.find(request->GetTaskId());
        if (source_it == endpoint_by_node_.end() || task == native_tasks_.end())
            throw std::runtime_error("target execution cannot resolve external WQE");
        const std::size_t source = source_it->second;
        const std::uint16_t ta_ssn = static_cast<std::uint16_t>(request->GetRequestTassn());
        auto segment = task->second.segments.find(ta_ssn);
        if (segment == task->second.segments.end())
            throw std::runtime_error("target execution cannot resolve native segment");
        ubnet::UdmaWireHeader wire{};
        wire.magic = ubnet::kUdmaWireMagic;
        wire.version = ubnet::kUdmaWireVersion;
        wire.operation = task->second.wire.operation;
        wire.flags = ubnet::kUdmaWireCtpSegment |
                     ubnet::kUdmaWireLastFragment |
                     (task->second.wire.flags & ubnet::kUdmaWireOrderMask);
        wire.source_jetty = task->second.wire.source_jetty;
        wire.destination_jetty = task->second.wire.destination_jetty;
        wire.tpn = task->second.wire.tpn;
        wire.segment = task->second.wire.segment;
        wire.remote_address = task->second.wire.remote_address + segment->second.offset;
        wire.immediate = task->second.wire.immediate;
        wire.request_id = task->second.wire.request_id;
        wire.ta_ssn = ta_ssn;
        wire.transfer_length = segment->second.bytes;
        wire.payload_offset = segment->second.offset;
        std::vector<std::uint8_t> payload;
        if (request->HasExplicitPayload()) {
            Ptr<const Packet> packet = request->GetExplicitPayload();
            payload.resize(packet->GetSize());
            packet->CopyData(payload.data(), payload.size());
        }
        wire.payload_length = static_cast<std::uint32_t>(payload.size());
        ubnet::Frame frame{};
        frame.sequence = task->second.sequence;
        frame.source_eid = endpoints_[source].eid;
        frame.destination_eid = endpoints_[target].eid;
        frame.source_port = request->GetSport() < options_.ports ? request->GetSport() : 0;
        frame.destination_port = request->GetDport() < options_.ports ? request->GetDport() : 0;
        frame.traffic_class = static_cast<std::uint16_t>(request->GetPriority());
        QueueUdma(target, frame, wire, payload);
        ++delivered_;
        const auto operation = static_cast<ubnet::UdmaOperation>(wire.operation);
        if (operation == ubnet::UdmaOperation::Send ||
            operation == ubnet::UdmaOperation::SendImmediate) {
            completion(UbWorkExecutionResult{});
            return;
        }
        const TargetExecutionKey key{source, wire.request_id, ta_ssn};
        if (!pending_target_executions_.emplace(
                key, PendingTargetExecution{completion, request->GetTaskId(), ta_ssn}).second)
            throw std::runtime_error("duplicate external target DMA execution");
    }

    void CompleteTargetExecution(std::size_t target,
                                 const ubnet::Frame& frame,
                                 const ubnet::UdmaWireHeader& wire,
                                 std::vector<std::uint8_t> payload)
    {
        if (!IsTargetCompletion(wire.operation) ||
            wire.payload_length > UB_MTU_BYTE ||
            frame.destination_eid == endpoints_[target].eid)
            throw std::runtime_error("invalid external target DMA completion");
        const std::size_t source = route_.at(frame.destination_eid);
        const TargetExecutionKey key{source, wire.request_id,
                                     static_cast<std::uint16_t>(wire.ta_ssn)};
        auto pending = pending_target_executions_.find(key);
        if (pending == pending_target_executions_.end())
            throw std::runtime_error("unmatched external target DMA completion");
        auto task = native_tasks_.find(pending->second.task_id);
        if (task == native_tasks_.end())
            throw std::runtime_error("target DMA completion lost its native WQE");
        auto segment = task->second.segments.find(pending->second.ta_ssn);
        if (segment == task->second.segments.end())
            throw std::runtime_error("target DMA completion lost its native segment");
        if (static_cast<ubnet::UdmaOperation>(wire.operation) ==
            ubnet::UdmaOperation::ReadResponse) {
            if (payload.size() != segment->second.bytes)
                throw std::runtime_error("target READ payload length mismatch");
            segment->second.read_payload = std::move(payload);
        }
        UbTargetCompletion completion = pending->second.completion;
        pending_target_executions_.erase(pending);
        completion(UbWorkExecutionResult{});
    }

    void OnNativeSegmentComplete(std::uint32_t node_id, std::uint32_t task_id,
                                 std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t ta_ssn,
                                 std::uint32_t, std::uint32_t bytes,
                                 std::uint32_t opcode_value)
    {
        auto task = native_tasks_.find(task_id);
        if (task == native_tasks_.end() ||
            endpoint_nodes_[task->second.source]->GetId() != node_id)
            throw std::runtime_error("native completion trace has no external WQE");
        const std::uint16_t wire_ssn = static_cast<std::uint16_t>(ta_ssn);
        auto segment = task->second.segments.find(wire_ssn);
        if (segment == task->second.segments.end())
            throw std::runtime_error("native completion trace has no external segment");
        if (segment->second.bytes != bytes)
            ++native_trace_size_mismatches_;
        const auto opcode = static_cast<UbTransactionOpcode>(opcode_value);
        ++ctp_native_completions_;
        ++forwarded_;
        ++delivered_;
        ubnet::UdmaWireHeader wire{};
        wire.magic = ubnet::kUdmaWireMagic;
        wire.version = ubnet::kUdmaWireVersion;
        wire.flags = ubnet::kUdmaWireCtpSegment |
                     ubnet::kUdmaWireLastFragment |
                     (task->second.wire.flags & ubnet::kUdmaWireOrderMask);
        wire.source_jetty = task->second.wire.destination_jetty;
        wire.destination_jetty = task->second.wire.source_jetty;
        wire.tpn = task->second.wire.tpn;
        wire.request_id = task->second.wire.request_id;
        wire.ta_ssn = wire_ssn;
        wire.transfer_length = segment->second.bytes;
        wire.payload_offset = segment->second.offset;
        std::vector<std::uint8_t> payload;
        if (opcode == UbTransactionOpcode::WRITE) {
            wire.operation = static_cast<std::uint8_t>(ubnet::UdmaOperation::WriteAck);
            ++ctp_taacks_;
        } else if (opcode == UbTransactionOpcode::READ) {
            wire.operation = static_cast<std::uint8_t>(ubnet::UdmaOperation::ReadResponse);
            payload = std::move(segment->second.read_payload);
            wire.payload_length = static_cast<std::uint32_t>(payload.size());
            payload_bytes_ += payload.size();
            ++ctp_read_responses_;
        } else {
            ++ctp_native_send_delivery_completions_;
            return;
        }
        ubnet::Frame frame{};
        frame.sequence = task->second.sequence;
        frame.source_eid = endpoints_[task->second.destination].eid;
        frame.destination_eid = endpoints_[task->second.source].eid;
        frame.source_port = 0;
        frame.destination_port = 0;
        QueueUdma(task->second.source, frame, wire, payload);
    }

    void OnNativeWqeTerminal(UbWqeCompletion completion)
    {
        auto task = native_tasks_.find(completion.wqeId);
        if (task == native_tasks_.end())
            throw std::runtime_error("native WQE completion has no external WQE");
        if (completion.outcome != UbWorkOutcome::COMPLETED) {
            ubnet::UdmaWireHeader wire{};
            wire.magic = ubnet::kUdmaWireMagic;
            wire.version = ubnet::kUdmaWireVersion;
            wire.operation = static_cast<std::uint8_t>(ubnet::UdmaOperation::RmaError);
            wire.flags = ubnet::kUdmaWireCtpSegment |
                         ubnet::kUdmaWireLastFragment;
            wire.source_jetty = task->second.wire.destination_jetty;
            wire.destination_jetty = task->second.wire.source_jetty;
            wire.request_id = task->second.wire.request_id;
            ubnet::Frame frame{};
            frame.sequence = task->second.sequence;
            frame.source_eid = endpoints_[task->second.destination].eid;
            frame.destination_eid = endpoints_[task->second.source].eid;
            QueueUdma(task->second.source, frame, wire, {});
            ++native_wqes_failed_;
        } else {
            ++native_wqes_completed_;
        }
        native_tasks_.erase(task);
        NS_ABORT_MSG_IF(scheduled_packets_ == 0, "native WQE work counter underflow");
        --scheduled_packets_;
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

    bool SendLifecycleCommit(Endpoint& endpoint, std::uint64_t now,
                             std::uint64_t generation, bool enabled)
    {
        auto* output = ubnet::UbNetOutAlloc(&endpoint.interface, now);
        if (!output) return false;
        ZeroVolatile(output->lifecycle);
        output->lifecycle.generation = generation;
        output->lifecycle.action = static_cast<std::uint8_t>(
            ubnet::LifecycleAction::CommitSync);
        output->lifecycle.enabled = enabled ? 1 : 0;
        ubnet::UbNetOutSend(&endpoint.interface, output,
            static_cast<std::uint8_t>(ubnet::MessageType::Lifecycle));
        return true;
    }

    bool OutputsEmpty() const
    {
        return std::all_of(endpoints_.begin(), endpoints_.end(),
            [](const Endpoint& endpoint) { return endpoint.outgoing.empty(); });
    }

    static std::uint64_t AbsoluteNowPs()
    {
        return static_cast<std::uint64_t>(Simulator::Now().GetPicoSeconds());
    }

    std::uint64_t NowPs() const
    {
        const std::int64_t logical =
            static_cast<std::int64_t>(AbsoluteNowPs()) + time_offset_ps_;
        return logical > 0 ? static_cast<std::uint64_t>(logical) : 0;
    }

    void AdvanceTo(std::uint64_t target)
    {
        const std::uint64_t now = NowPs();
        if (target <= now) return;
        /* SYNC horizons are protocol promises, not ns-3 events.  When the
         * fabric is idle, advance the adapter's logical clock without running
         * the complete ns-3 scheduler once per synchronization quantum.  The
         * next real frame is scheduled relative to the then-current ns-3
         * clock, and the offset maps its callbacks back to protocol time. */
        if (scheduled_packets_ == 0) {
            time_offset_ps_ += static_cast<std::int64_t>(target - now);
            return;
        }
        Simulator::Schedule(PicoSeconds(target - now), [] { Simulator::Stop(); });
        Simulator::Run();
    }

    Options options_;
    std::vector<Endpoint> endpoints_;
    std::vector<ubnet::Intro> introductions_;
    std::unordered_map<std::uint32_t, std::size_t> route_;
    std::vector<Ptr<Node>> endpoint_nodes_;
    std::vector<std::vector<Ptr<UbPort>>> endpoint_ports_;
    std::vector<std::unordered_map<std::uint64_t, PartialWqe>> partial_wqes_;
    std::vector<std::optional<std::uint32_t>> last_native_tassn_;
    std::unordered_map<std::uint32_t, NativeTask> native_tasks_;
    std::map<TargetExecutionKey, PendingTargetExecution> pending_target_executions_;
    std::uint32_t next_native_task_id_{1};
    std::unordered_map<std::uint32_t, std::size_t> endpoint_by_node_;
    Ptr<Node> switch_node_;
    Ptr<UbSwitch> switch_;
    std::vector<Ptr<UbPort>> switch_ports_;
    std::uint64_t scheduled_packets_{};
    std::uint64_t forwarded_{};
    std::uint64_t delivered_{};
    std::uint64_t payload_bytes_{};
    std::uint64_t epoch_origin_ps_{};
    std::int64_t time_offset_ps_{};
    std::uint64_t loop_iterations_{};
    std::uint64_t idle_sleeps_{};
    std::uint64_t sync_steps_{};
    std::uint64_t sync_backpressure_{};
    std::uint64_t output_backpressure_{};
    std::uint64_t async_timestamp_jumps_{};
    std::uint64_t async_timestamp_jump_ps_{};
    std::uint64_t ctp_request_segments_{};
    std::uint64_t ctp_taacks_{};
    std::uint64_t ctp_read_responses_{};
    std::uint64_t first_native_segment_send_ps_{};
    Ptr<UbCtpQueueFeedback> ctp_feedback_;
    std::uint64_t second_native_segment_gap_ps_{};
    std::uint64_t last_native_segment_send_ps_{};
    bool ctp_cnp_injected_{};
    std::uint64_t ctp_max_payload_{};
    std::uint64_t ctp_native_admitted_{};
    std::uint64_t ctp_native_window_blocked_{};
    std::uint64_t ctp_native_max_outstanding_{};
    std::uint64_t ctp_native_completions_{};
    std::uint64_t ctp_native_send_delivery_completions_{};
    std::uint64_t ctp_tassn_discontinuities_{};
    std::uint64_t native_wqes_submitted_{};
    std::uint64_t native_wqes_completed_{};
    std::uint64_t native_wqes_failed_{};
    std::uint64_t native_order_no_{};
    std::uint64_t native_order_relax_{};
    std::uint64_t native_order_strong_{};
    std::uint64_t native_trace_size_mismatches_{};
    bool lifecycle_active_{false};
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
                     "[--sync-interval-ps N] [--lifecycle-sync] "
                     "[--ctp-retransmission on|off] [--ctp-rto-ps N] "
                     "[--ctp-max-retransmissions N] "
                     "[--drop-first-ctp-request] [--drop-first-ctp-taack] "
                     "[--drop-first-ctp-read-response] "
                     "[--inject-ctp-cnp-after-first-segment] "
                     "[--ctp-recovery-interval-ps N --ctp-recovery-step-bps N] "
                     "[--ctp-mark-threshold-bytes N] [--ctp-cnp-interval-ps N] "
                     "[--drop-all-ctp-requests]\n";
        return 1;
    }
}
