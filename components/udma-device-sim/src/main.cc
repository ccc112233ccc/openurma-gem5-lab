// SPDX-License-Identifier: Apache-2.0
#include "ubsim/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_net/if.h"
#include "protocol/ub_net/udma_wire.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace device = ubsim::device;
namespace host_proto = ubsim::proto::host;
namespace net_proto = ubsim::proto::net;

namespace {

std::atomic<bool> running{true};
constexpr std::size_t kPollBatch = 256;

void Stop(int) { running.store(false); }

struct Options {
    std::string host_socket;
    std::string net_socket;
    std::string shm_path;
    std::uint64_t host_link_latency_ps{100000};
    std::uint64_t net_link_latency_ps{100000};
    std::uint64_t host_sync_interval_ps{100000};
    std::uint64_t net_sync_interval_ps{100000};
    std::uint64_t endpoint_eid{0x100};
    std::uint64_t port_count{2};
    SimbricksBaseIfSyncMode sync_mode{kSimbricksBaseIfSyncOptional};
    bool lifecycle_sync{false};
    bool extraction_test_abi{false};
    std::string state_in;
    std::string state_out;
};

bool ParseUnsigned(const char* value, std::uint64_t& output)
{
    try {
        std::size_t consumed = 0;
        output = std::stoull(value, &consumed, 0);
        return value[consumed] == '\0' && output != 0;
    } catch (...) {
        return false;
    }
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--host-socket" && i + 1 < argc) options.host_socket = argv[++i];
        else if (arg == "--net-socket" && i + 1 < argc) options.net_socket = argv[++i];
        else if (arg == "--shm" && i + 1 < argc) options.shm_path = argv[++i];
        else if (arg == "--link-latency-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.host_link_latency_ps)) return false;
            options.net_link_latency_ps = options.host_link_latency_ps;
        } else if (arg == "--host-link-latency-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.host_link_latency_ps)) return false;
        } else if (arg == "--net-link-latency-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.net_link_latency_ps)) return false;
        } else if (arg == "--sync-interval-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.host_sync_interval_ps)) return false;
            options.net_sync_interval_ps = options.host_sync_interval_ps;
        } else if (arg == "--host-sync-interval-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.host_sync_interval_ps)) return false;
        } else if (arg == "--net-sync-interval-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.net_sync_interval_ps)) return false;
        } else if (arg == "--sync" && i + 1 < argc) {
            const std::string mode(argv[++i]);
            if (mode == "off") options.sync_mode = kSimbricksBaseIfSyncDisabled;
            else if (mode == "optional") options.sync_mode = kSimbricksBaseIfSyncOptional;
            else if (mode == "required") options.sync_mode = kSimbricksBaseIfSyncRequired;
            else return false;
        } else if (arg == "--eid" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.endpoint_eid) ||
                options.endpoint_eid > 0xcffff) return false;
        } else if (arg == "--ports" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.port_count) ||
                options.port_count > 255) return false;
        } else if (arg == "--test-abi") {
            options.extraction_test_abi = true;
        } else if (arg == "--lifecycle-sync") {
            options.lifecycle_sync = true;
        } else if (arg == "--state-in" && i + 1 < argc) {
            options.state_in = argv[++i];
        } else if (arg == "--state-out" && i + 1 < argc) {
            options.state_out = argv[++i];
        } else {
            return false;
        }
    }
    return !options.host_socket.empty() && !options.net_socket.empty() &&
           !options.shm_path.empty() &&
           (options.sync_mode == kSimbricksBaseIfSyncDisabled ||
            (options.host_sync_interval_ps <= options.host_link_latency_ps &&
             options.net_sync_interval_ps <= options.net_link_latency_ps));
}

template <typename T>
void ZeroVolatile(volatile T& object)
{
    const std::uint64_t timestamp = object.timestamp;
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
    object.timestamp = timestamp;
}

class HostPort final : public device::HostInterface {
  public:
    HostPort(host_proto::Interface& interface, std::uint64_t& now)
        : interface_(interface), now_(now) {}

    void Attach(device::UdmaModel* model) { model_ = model; }
    bool PrepareSeen() const { return prepare_seen_; }
    std::uint64_t Generation() const { return generation_; }
    bool TargetEnabled() const { return target_enabled_; }
    std::uint64_t AsyncTimestampJumps() const { return async_timestamp_jumps_; }
    std::uint64_t AsyncTimestampJumpPs() const { return async_timestamp_jump_ps_; }
    bool PendingEmpty() const { return pending_.empty(); }

    void FinishFence() { prepare_seen_ = false; }

    bool SendCommit(std::uint64_t generation, bool enabled)
    {
        auto* message = host_proto::UbHostD2HOutAlloc(&interface_, now_);
        if (message == nullptr) return false;
        ZeroVolatile(message->lifecycle);
        message->lifecycle.generation = generation;
        message->lifecycle.action = static_cast<std::uint8_t>(
            host_proto::LifecycleAction::CommitSync);
        message->lifecycle.enabled = enabled ? 1 : 0;
        host_proto::UbHostD2HOutSend(&interface_, message,
            static_cast<std::uint8_t>(host_proto::D2HType::Lifecycle));
        return true;
    }

    void DmaRead(std::uint64_t address, std::size_t length,
                 device::ReadCompletion completion) override
    {
        if (length > PayloadCapacity()) return completion(false, {});
        SendDma(address, length, true, std::move(completion), {});
    }

    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> data,
                  device::Completion completion) override
    {
        if (data.size() > PayloadCapacity()) return completion(false);
        SendDma(address, data.size(), false, {}, std::move(completion), &data);
    }

    void DmaReadIoVirtual(std::uint64_t address, std::size_t length,
                          device::ReadCompletion completion) override
    {
        if (length > PayloadCapacity()) return completion(false, {});
        SendDma(address, length, true, std::move(completion), {}, nullptr,
                host_proto::AddressKind::IoVirtual);
    }

    void DmaWriteIoVirtual(std::uint64_t address,
                           std::vector<std::uint8_t> data,
                           device::Completion completion) override
    {
        if (data.size() > PayloadCapacity()) return completion(false);
        SendDma(address, data.size(), false, {}, std::move(completion), &data,
                host_proto::AddressKind::IoVirtual);
    }

    void DmaReadToken(std::uint32_t token, std::uint64_t address,
                      std::size_t length,
                      device::ReadCompletion completion) override
    {
        if (length > PayloadCapacity()) return completion(false, {});
        SendDma(address, length, true, std::move(completion), {}, nullptr,
                host_proto::AddressKind::IoVirtual, token);
    }

    void DmaWriteToken(std::uint32_t token, std::uint64_t address,
                       std::vector<std::uint8_t> data,
                       device::Completion completion) override
    {
        if (data.size() > PayloadCapacity()) return completion(false);
        SendDma(address, data.size(), false, {}, std::move(completion), &data,
                host_proto::AddressKind::IoVirtual, token);
    }

    void MsiWrite(std::uint64_t physical_address, std::uint32_t data,
                  device::Completion completion) override
    {
        std::vector<std::uint8_t> bytes(4);
        for (std::uint32_t i = 0; i < 4; ++i)
            bytes[i] = static_cast<std::uint8_t>(data >> (8 * i));
        SendDma(physical_address, bytes.size(), false, {},
                std::move(completion), &bytes,
                host_proto::AddressKind::Msi);
    }

    void SetInterrupt(std::uint32_t vector, bool asserted) override
    {
        SendInterrupt(vector, asserted ? host_proto::InterruptAction::Raise
                                       : host_proto::InterruptAction::Lower);
    }

    void PulseInterrupt(std::uint32_t vector) override
    {
        SendInterrupt(vector, host_proto::InterruptAction::Pulse);
    }

    void SendInterrupt(std::uint32_t vector,
                       host_proto::InterruptAction action)
    {
        auto* message = host_proto::UbHostD2HOutAlloc(&interface_, now_);
        if (message == nullptr) return;
        ZeroVolatile(message->interrupt);
        message->interrupt.sequence = next_request_++;
        message->interrupt.vector = vector;
        message->interrupt.action = static_cast<std::uint8_t>(action);
        host_proto::UbHostD2HOutSend(
            &interface_, message,
            static_cast<std::uint8_t>(host_proto::D2HType::Interrupt));
    }

    bool Poll()
    {
        auto* message = host_proto::UbHostH2DInPoll(&interface_, now_);
        if (message == nullptr) return false;
        // With synchronization disabled SimBricks deliberately makes every
        // queued message immediately visible.  Preserve causality without a
        // fixed-step polling loop by adopting the sender timestamp before the
        // hardware action is evaluated.
        if (!SimbricksBaseIfSyncEnabled(&interface_.base)) {
            const std::uint64_t message_time = message->base.header.timestamp;
            if (message_time > now_) {
                ++async_timestamp_jumps_;
                async_timestamp_jump_ps_ += message_time - now_;
            }
            now_ = std::max(now_, message_time);
            if (model_ != nullptr) model_->AdvanceTime(now_);
        }
        const auto type = static_cast<host_proto::H2DType>(
            host_proto::UbHostH2DInType(&interface_, message));
        if (type == host_proto::H2DType::MmioRead ||
            type == host_proto::H2DType::MmioWrite) {
            HandleMmio(message->mmio, type == host_proto::H2DType::MmioWrite);
        } else if (type == host_proto::H2DType::DmaReadCompletion ||
                   type == host_proto::H2DType::DmaWriteCompletion) {
            HandleDmaCompletion(message, type);
        } else if (type == host_proto::H2DType::Lifecycle &&
                   message->lifecycle.action == static_cast<std::uint8_t>(
                       host_proto::LifecycleAction::PrepareSync)) {
            prepare_seen_ = true;
            generation_ = message->lifecycle.generation;
            target_enabled_ = message->lifecycle.enabled != 0;
        }
        host_proto::UbHostH2DInDone(&interface_, message);
        return true;
    }

  private:
    struct Pending {
        device::ReadCompletion read;
        device::Completion write;
    };

    std::size_t PayloadCapacity() const
    {
        return host_proto::UbHostD2HOutMsgLen(
                   const_cast<host_proto::Interface*>(&interface_)) -
               sizeof(host_proto::D2HMessage);
    }

    bool SendDma(std::uint64_t address, std::size_t length, bool read,
                 device::ReadCompletion read_completion,
                 device::Completion write_completion,
                 const std::vector<std::uint8_t>* payload = nullptr,
                 host_proto::AddressKind address_kind =
                     host_proto::AddressKind::GuestPhysical,
                 std::uint32_t pasid = 0)
    {
        auto* message = host_proto::UbHostD2HOutAlloc(&interface_, now_);
        if (message == nullptr) {
            if (read_completion) read_completion(false, {});
            if (write_completion) write_completion(false);
            return false;
        }
        ZeroVolatile(message->dma);
        const std::uint64_t id = next_request_++;
        message->dma.request_id = id;
        message->dma.address = address;
        message->dma.length = static_cast<std::uint32_t>(length);
        message->dma.address_kind = static_cast<std::uint8_t>(address_kind);
        message->dma.pasid = pasid;
        if (payload != nullptr) {
            auto* destination = reinterpret_cast<volatile std::uint8_t*>(message) +
                                sizeof(host_proto::D2HMessage);
            for (std::size_t i = 0; i < payload->size(); ++i) destination[i] = (*payload)[i];
        }
        pending_.emplace(id, Pending{std::move(read_completion),
                                     std::move(write_completion)});
        host_proto::UbHostD2HOutSend(
            &interface_, message,
            static_cast<std::uint8_t>(read ? host_proto::D2HType::DmaRead
                                           : host_proto::D2HType::DmaWrite));
        return true;
    }

    void HandleMmio(const volatile host_proto::MmioRequest& request, bool write)
    {
        auto* response = host_proto::UbHostD2HOutAlloc(&interface_, now_);
        if (response == nullptr || model_ == nullptr) return;
        ZeroVolatile(response->completion);
        response->completion.request_id = request.request_id;
        response->completion.length = request.length;
        bool ok = true;
        if (write) {
            ok = model_->WriteMmio(request.offset, request.length, request.value);
        } else {
            std::uint64_t value{};
            ok = model_->ReadMmio(request.offset, request.length, value);
            response->completion.value = value;
        }
        response->completion.status = static_cast<std::uint16_t>(
            ok ? host_proto::Status::Success : host_proto::Status::InvalidAddress);
        host_proto::UbHostD2HOutSend(
            &interface_, response,
            static_cast<std::uint8_t>(host_proto::D2HType::MmioCompletion));
    }

    void HandleDmaCompletion(volatile host_proto::H2DMessage* message,
                             host_proto::H2DType type)
    {
        const std::uint64_t id = message->completion.request_id;
        auto found = pending_.find(id);
        if (found == pending_.end()) return;
        Pending pending = std::move(found->second);
        pending_.erase(found);
        const bool ok = message->completion.status ==
            static_cast<std::uint16_t>(host_proto::Status::Success);
        if (type == host_proto::H2DType::DmaReadCompletion) {
            std::vector<std::uint8_t> bytes(message->completion.length);
            const auto* source = reinterpret_cast<volatile std::uint8_t*>(message) +
                                 sizeof(host_proto::H2DMessage);
            for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = source[i];
            if (pending.read) pending.read(ok, std::move(bytes));
        } else if (pending.write) {
            pending.write(ok);
        }
    }

    host_proto::Interface& interface_;
    std::uint64_t& now_;
    device::UdmaModel* model_{nullptr};
    std::uint64_t next_request_{1};
    std::unordered_map<std::uint64_t, Pending> pending_;
    bool prepare_seen_{false};
    bool target_enabled_{false};
    std::uint64_t generation_{};
    std::uint64_t async_timestamp_jumps_{};
    std::uint64_t async_timestamp_jump_ps_{};
};

class NetworkPort final : public device::NetworkInterface {
  public:
    NetworkPort(net_proto::Interface& interface, std::uint64_t& now)
        : interface_(interface), now_(now) {}
    void Attach(device::UdmaModel* model) { model_ = model; }
    bool CommitSeen() const { return commit_seen_; }
    bool CommitEnabled() const { return commit_enabled_; }
    void FinishFence() { commit_seen_ = false; }
    bool PendingEmpty() const { return outgoing_.empty(); }
    std::uint64_t FragmentsQueued() const { return fragments_queued_; }
    std::uint64_t FragmentsSent() const { return fragments_sent_; }
    std::uint64_t SendBackpressure() const { return send_backpressure_; }
    std::uint64_t AsyncTimestampJumps() const { return async_timestamp_jumps_; }
    std::uint64_t AsyncTimestampJumpPs() const { return async_timestamp_jump_ps_; }

    bool SendPrepare(std::uint64_t generation, bool enabled)
    {
        auto* message = net_proto::UbNetOutAlloc(&interface_, now_);
        if (message == nullptr) return false;
        ZeroVolatile(message->lifecycle);
        message->lifecycle.generation = generation;
        message->lifecycle.action = static_cast<std::uint8_t>(
            net_proto::LifecycleAction::PrepareSync);
        message->lifecycle.enabled = enabled ? 1 : 0;
        net_proto::UbNetOutSend(&interface_, message,
            static_cast<std::uint8_t>(net_proto::MessageType::Lifecycle));
        return true;
    }

    void Send(device::Frame frame, device::Completion completion) override
    {
        PendingSend pending{};
        pending.frame = frame;
        pending.completion = std::move(completion);
        if (frame.operation == device::Frame::Operation::Raw) {
            pending.fragments.push_back(std::move(frame.bytes));
            pending.frame.bytes.clear();
            ++fragments_queued_;
            outgoing_.push_back(std::move(pending));
            return;
        }
        const std::size_t capacity = net_proto::UbNetOutMsgLen(&interface_) -
                                     sizeof(net_proto::Message) -
                                     sizeof(net_proto::UdmaWireHeader);
        if (capacity == 0) return pending.completion(false);
        const std::size_t total = frame.bytes.size();
        std::size_t offset = 0;
        do {
            net_proto::UdmaWireHeader header{};
            header.magic = net_proto::kUdmaWireMagic;
            header.version = 1;
            header.operation = static_cast<std::uint8_t>(frame.operation);
            header.source_jetty = frame.source_jetty;
            header.destination_jetty = frame.destination_jetty;
            header.tpn = frame.tpn;
            header.segment = frame.segment;
            header.remote_address = frame.remote_address;
            header.immediate = frame.immediate;
            header.request_id = frame.request_id;
            header.transfer_length = frame.transfer_length;
            const std::size_t chunk = std::min(capacity, total - offset);
            header.payload_length = static_cast<std::uint32_t>(chunk);
            header.payload_offset = static_cast<std::uint32_t>(offset);
            if (total > capacity) header.flags |= net_proto::kUdmaWireFragmented;
            if (offset + chunk == total)
                header.flags |= net_proto::kUdmaWireLastFragment;
            const auto* first = reinterpret_cast<const std::uint8_t*>(&header);
            std::vector<std::uint8_t> wire(first, first + sizeof(header));
            wire.insert(wire.end(), frame.bytes.begin() + offset,
                        frame.bytes.begin() + offset + chunk);
            pending.fragments.push_back(std::move(wire));
            ++fragments_queued_;
            offset += chunk;
        } while (offset < total);
        pending.frame.bytes.clear();
        outgoing_.push_back(std::move(pending));
    }

    // Attempt one queued fragment.  Unlike the former busy-waiting Send(),
    // this returns to the main loop on ring backpressure so the same process
    // can drain inbound traffic.  That is required for symmetric large RMA
    // transfers where both endpoints can fill their outbound rings at once.
    bool FlushOne()
    {
        if (outgoing_.empty()) return false;
        PendingSend& pending = outgoing_.front();
        if (!SendWire(pending.frame, pending.fragments[pending.next])) {
            ++send_backpressure_;
            return false;
        }
        ++fragments_sent_;
        if (++pending.next != pending.fragments.size()) return true;
        auto completion = std::move(pending.completion);
        outgoing_.pop_front();
        completion(true);
        return true;
    }

    bool SendWire(const device::Frame& frame,
                  const std::vector<std::uint8_t>& wire)
    {
        const std::size_t capacity = net_proto::UbNetOutMsgLen(&interface_) -
                                     sizeof(net_proto::Message);
        auto* message = wire.size() <= capacity
                            ? net_proto::UbNetOutAlloc(&interface_, now_)
                            : nullptr;
        if (message == nullptr) {
            return false;
        }
        ZeroVolatile(message->frame);
        message->frame.sequence = frame.sequence;
        message->frame.length = static_cast<std::uint32_t>(wire.size());
        message->frame.source_eid = frame.source_eid;
        message->frame.destination_eid = frame.destination_eid;
        message->frame.source_port = frame.source_port;
        message->frame.destination_port = frame.destination_port;
        auto* destination = reinterpret_cast<volatile std::uint8_t*>(message) +
                            sizeof(net_proto::Message);
        for (std::size_t i = 0; i < wire.size(); ++i) destination[i] = wire[i];
        net_proto::UbNetOutSend(&interface_, message,
            static_cast<std::uint8_t>(net_proto::MessageType::Frame));
        return true;
    }

    bool Poll()
    {
        auto* message = net_proto::UbNetInPoll(&interface_, now_);
        if (message == nullptr) return false;
        if (!SimbricksBaseIfSyncEnabled(&interface_.base)) {
            const std::uint64_t message_time = message->base.header.timestamp;
            if (message_time > now_) {
                ++async_timestamp_jumps_;
                async_timestamp_jump_ps_ += message_time - now_;
            }
            now_ = std::max(now_, message_time);
            if (model_ != nullptr) model_->AdvanceTime(now_);
        }
        const auto type = static_cast<net_proto::MessageType>(
            net_proto::UbNetInType(&interface_, message));
        if (type == net_proto::MessageType::Frame && model_ != nullptr) {
            device::Frame frame{};
            frame.sequence = message->frame.sequence;
            frame.source_eid = message->frame.source_eid;
            frame.destination_eid = message->frame.destination_eid;
            frame.source_port = message->frame.source_port;
            frame.destination_port = message->frame.destination_port;
            frame.bytes.resize(message->frame.length);
            const auto* source = reinterpret_cast<volatile std::uint8_t*>(message) +
                                 sizeof(net_proto::Message);
            for (std::size_t i = 0; i < frame.bytes.size(); ++i) frame.bytes[i] = source[i];
            if (frame.bytes.size() >= sizeof(net_proto::UdmaWireHeader)) {
                net_proto::UdmaWireHeader header{};
                std::memcpy(&header, frame.bytes.data(), sizeof(header));
                if (header.magic == net_proto::kUdmaWireMagic &&
                    header.version == 1 &&
                    frame.bytes.size() == sizeof(header) + header.payload_length) {
                    frame.operation = static_cast<device::Frame::Operation>(
                        header.operation);
                    frame.source_jetty = header.source_jetty;
                    frame.destination_jetty = header.destination_jetty;
                    frame.tpn = header.tpn;
                    frame.segment = header.segment;
                    frame.remote_address = header.remote_address;
                    frame.immediate = header.immediate;
                    frame.request_id = header.request_id;
                    frame.transfer_length = header.transfer_length;
                    frame.bytes.erase(frame.bytes.begin(),
                                      frame.bytes.begin() + sizeof(header));
                    if (header.flags & net_proto::kUdmaWireFragmented) {
                        auto& partial = fragments_[frame.sequence];
                        if (header.payload_offset == 0) {
                            partial.frame = frame;
                            partial.frame.bytes.clear();
                            partial.frame.bytes.reserve(header.transfer_length);
                            partial.next_offset = 0;
                        }
                        if (header.payload_offset != partial.next_offset) {
                            fragments_.erase(frame.sequence);
                            net_proto::UbNetInDone(&interface_, message);
                            return true;
                        }
                        partial.frame.bytes.insert(partial.frame.bytes.end(),
                                                   frame.bytes.begin(),
                                                   frame.bytes.end());
                        partial.next_offset += frame.bytes.size();
                        if (!(header.flags & net_proto::kUdmaWireLastFragment)) {
                            net_proto::UbNetInDone(&interface_, message);
                            return true;
                        }
                        frame = std::move(partial.frame);
                        fragments_.erase(frame.sequence);
                    }
                }
            }
            model_->Receive(std::move(frame));
        } else if (type == net_proto::MessageType::LinkState &&
                   model_ != nullptr) {
            model_->SetLinkState(message->link.port,
                message->link.state ==
                    static_cast<std::uint8_t>(net_proto::LinkState::Up));
        } else if (type == net_proto::MessageType::Lifecycle &&
                   message->lifecycle.action == static_cast<std::uint8_t>(
                       net_proto::LifecycleAction::CommitSync)) {
            commit_seen_ = true;
            commit_generation_ = message->lifecycle.generation;
            commit_enabled_ = message->lifecycle.enabled != 0;
        }
        net_proto::UbNetInDone(&interface_, message);
        return true;
    }

  private:
    struct PendingSend {
        device::Frame frame;
        std::vector<std::vector<std::uint8_t>> fragments;
        std::size_t next{};
        device::Completion completion;
    };

    struct PartialFrame {
        device::Frame frame;
        std::size_t next_offset{};
    };
    net_proto::Interface& interface_;
    std::uint64_t& now_;
    device::UdmaModel* model_{nullptr};
    bool commit_seen_{false};
    bool commit_enabled_{false};
    std::uint64_t commit_generation_{};
    std::deque<PendingSend> outgoing_;
    std::unordered_map<std::uint64_t, PartialFrame> fragments_;
    std::uint64_t fragments_queued_{};
    std::uint64_t fragments_sent_{};
    std::uint64_t send_backpressure_{};
    std::uint64_t async_timestamp_jumps_{};
    std::uint64_t async_timestamp_jump_ps_{};
};

int Run(const Options& options)
{
    host_proto::Interface host_if{};
    net_proto::Interface net_if{};
    SimbricksBaseIfParams host_params{};
    SimbricksBaseIfParams net_params{};
    host_proto::DefaultParams(&host_params);
    net_proto::DefaultParams(&net_params);
    host_params.sock_path = options.host_socket.c_str();
    net_params.sock_path = options.net_socket.c_str();
    host_params.link_latency = options.host_link_latency_ps;
    net_params.link_latency = options.net_link_latency_ps;
    host_params.sync_interval = options.host_sync_interval_ps;
    net_params.sync_interval = options.net_sync_interval_ps;
    host_params.sync_mode = net_params.sync_mode = options.sync_mode;

    SimbricksBaseIfSHMPool pool{};
    const std::size_t pool_size = SimbricksBaseIfSHMSize(&host_params) +
                                  SimbricksBaseIfSHMSize(&net_params);
    if (SimbricksBaseIfSHMPoolCreate(&pool, options.shm_path.c_str(), pool_size) != 0 ||
        SimbricksBaseIfInit(&host_if.base, &host_params) != 0 ||
        SimbricksBaseIfInit(&net_if.base, &net_params) != 0 ||
        SimbricksBaseIfListen(&host_if.base, &pool) != 0 ||
        SimbricksBaseIfListen(&net_if.base, &pool) != 0) {
        return 1;
    }

    host_proto::DeviceIntro device_intro{};
    device_intro.version = host_proto::kVersion;
    device_intro.region_count = 1;
    device_intro.irq_count = 3;
    device_intro.port_count = static_cast<std::uint32_t>(options.port_count);
    device_intro.device_id = device::UdmaModel::kIdentity;
    device_intro.regions[0].size = device::UdmaModel::kOfficialApertureBytes;
    host_proto::HostIntro host_intro{};
    net_proto::Intro net_intro{net_proto::kVersion,
        static_cast<std::uint32_t>(options.port_count), 16384, 32, 0};
    net_proto::Intro peer_net_intro{};
    SimBricksBaseIfEstablishData establish[] = {
        {&host_if.base, &device_intro, sizeof(device_intro), &host_intro, sizeof(host_intro)},
        {&net_if.base, &net_intro, sizeof(net_intro), &peer_net_intro, sizeof(peer_net_intro)},
    };
    if (SimBricksBaseIfEstablish(establish, 2) != 0) return 1;
    if (host_intro.version != host_proto::kVersion ||
        peer_net_intro.version != net_proto::kVersion) return 2;

    std::uint64_t now = 0;
    HostPort host(host_if, now);
    NetworkPort network(net_if, now);
    device::UdmaModel::Config model_config{};
    model_config.mmio_base = host_intro.mmio_base;
    model_config.port_count = device_intro.port_count;
    model_config.endpoint_eid = static_cast<std::uint32_t>(options.endpoint_eid);
    model_config.extraction_test_abi = options.extraction_test_abi;
    device::UdmaModel model(host, network, model_config);
    if (!options.state_in.empty()) {
        std::ifstream input(options.state_in, std::ios::binary);
        if (!input || !model.LoadState(input)) {
            std::cerr << "udma-device-sim: failed to restore "
                      << options.state_in << '\n';
            return 3;
        }
        std::cerr << "[UDMA_CHECKPOINT] restored=" << options.state_in << '\n';
    }
    host.Attach(&model);
    network.Attach(&model);
    std::cout << "udma-device-sim: connected host=" << options.host_socket
              << " net=" << options.net_socket << '\n';

    const auto wall_started = std::chrono::steady_clock::now();
    std::uint64_t loop_iterations = 0;
    std::uint64_t idle_sleeps = 0;
    std::uint64_t sync_backpressure = 0;
    std::uint64_t sync_steps = 0;
    bool prepare_forwarded = false;
    bool lifecycle_active = false;
    bool host_sync_primed = false;
    bool net_sync_primed = false;
    std::uint64_t epoch_origin_ps = 0;

    while (running.load() && !SimbricksBaseIfInTerminated(&host_if.base) &&
           !SimbricksBaseIfInTerminated(&net_if.base)) {
        ++loop_iterations;
        model.AdvanceTime(now);
        bool progress = false;
        // Bound each drain pass.  In synchronized mode a peer can keep the
        // ring continuously populated with SYNC messages; an unbounded drain
        // then starves the outer-loop signal check and the other interface.
        for (std::size_t count = 0;
             count < kPollBatch && running.load() && host.Poll(); ++count)
            progress = true;
        for (std::size_t count = 0;
             count < kPollBatch && running.load() && network.Poll(); ++count)
            progress = true;
        while (network.FlushOne()) progress = true;
        if (options.lifecycle_sync && host.PrepareSeen() &&
            !prepare_forwarded && host.PendingEmpty() &&
            network.PendingEmpty() && model.IsQuiescent()) {
            prepare_forwarded = network.SendPrepare(host.Generation(),
                                                    host.TargetEnabled());
            progress = prepare_forwarded || progress;
        }
        if (options.lifecycle_sync && prepare_forwarded &&
            network.CommitSeen() &&
            host.PendingEmpty() && network.PendingEmpty() &&
            model.IsQuiescent()) {
            const bool enable = host.TargetEnabled();
            if (network.CommitEnabled() != enable || enable == lifecycle_active) {
                std::cerr << "udma-device-sim: lifecycle target mismatch\n";
                return 4;
            }
            if (!host.SendCommit(host.Generation(), enable)) {
                std::this_thread::yield();
                continue;
            }
            if (enable) {
                epoch_origin_ps = now;
                host_if.base.in_timestamp = host_if.base.out_timestamp = 0;
                net_if.base.in_timestamp = net_if.base.out_timestamp = 0;
                host_if.base.sync = net_if.base.sync = true;
                host_sync_primed = net_sync_primed = false;
                now = 0;
                model.RebaseTime(0);
            } else {
                now += epoch_origin_ps;
                model.RebaseTime(now);
                host_if.base.in_timestamp = host_if.base.out_timestamp = now;
                net_if.base.in_timestamp = net_if.base.out_timestamp = now;
                host_if.base.sync = net_if.base.sync = false;
                host_sync_primed = net_sync_primed = false;
                epoch_origin_ps = 0;
            }
            lifecycle_active = enable;
            host.FinishFence();
            network.FinishFence();
            prepare_forwarded = false;
            std::cerr << "[UDMA_FENCE] generation=" << host.Generation()
                      << " eid=0x" << std::hex << options.endpoint_eid
                      << std::dec << " active=" << (enable ? 1 : 0) << '\n';
        }
        const auto send_sync = [now](auto& interface, bool& primed,
                                     auto out_sync, auto next_sync) {
            if (!SimbricksBaseIfSyncEnabled(&interface.base)) return 0;
            if (primed && now < next_sync(&interface)) return 0;
            const int result = out_sync(&interface, now);
            if (result == 0) primed = true;
            return result;
        };
        const bool sync_blocked =
            send_sync(host_if, host_sync_primed,
                      host_proto::UbHostD2HOutSync,
                      host_proto::UbHostD2HOutNextSync) != 0 ||
            send_sync(net_if, net_sync_primed,
                      net_proto::UbNetOutSync,
                      net_proto::UbNetOutNextSync) != 0;
        if (sync_blocked) {
            ++sync_backpressure;
            continue;
        }
        bool synchronized = false;
        std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
        const auto constrain = [&synchronized, &next]
            (SimbricksBaseIf& interface) {
                if (!SimbricksBaseIfSyncEnabled(&interface)) return;
                synchronized = true;
                // The last received SYNC remains the conservative horizon
                // until the peer publishes a newer message.
                next = std::min(next,
                    SimbricksBaseIfInTimestamp(&interface));
                next = std::min(next,
                    SimbricksBaseIfOutNextSync(&interface));
            };
        constrain(host_if.base);
        constrain(net_if.base);
        if (!synchronized) {
            // In functional/asynchronous mode external messages carry their
            // own timestamps (adopted by Poll()).  Only autonomous device
            // deadlines require local time advancement, and those can be
            // reached in one event-driven jump.
            const std::uint64_t deadline = model.NextEventTime();
            if (deadline > now &&
                deadline != std::numeric_limits<std::uint64_t>::max()) {
                now = deadline;
                model.AdvanceTime(now);
                progress = true;
            }
        } else if (next > now && next != std::numeric_limits<std::uint64_t>::max()) {
            now = next;
            ++sync_steps;
            progress = true;
        }
        if (!progress) {
            ++idle_sleeps;
            if (!synchronized)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_started).count();
    std::cerr << "[UDMA_PROFILE] eid=0x" << std::hex << options.endpoint_eid
              << std::dec << " wall_ns=" << wall_ns
              << " virtual_ps=" << now
              << " loops=" << loop_iterations
              << " idle_sleeps=" << idle_sleeps
              << " sync_steps=" << sync_steps
              << " sync_backpressure=" << sync_backpressure
              << " async_timestamp_jumps="
              << host.AsyncTimestampJumps() + network.AsyncTimestampJumps()
              << " async_timestamp_jump_ps="
              << host.AsyncTimestampJumpPs() + network.AsyncTimestampJumpPs()
              << " net_fragments_queued=" << network.FragmentsQueued()
              << " net_fragments_sent=" << network.FragmentsSent()
              << " net_send_backpressure=" << network.SendBackpressure()
              << " submitted=" << model.submitted()
              << " completed=" << model.completed()
              << " contexts=" << model.jetty_count()
              << " mmio_writes=" << model.mmio_writes()
              << " jetty_mmio_writes=" << model.jetty_mmio_writes()
              << " sq_doorbells=" << model.sq_doorbells()
              << " sq_dma_reads=" << model.sq_dma_reads()
              << " sq_wqes=" << model.sq_wqes()
              << " sq_completions=" << model.sq_completions()
              << " sq_depth_rejects=" << model.sq_depth_rejects()
              << " sq_decode_rejects=" << model.sq_decode_rejects()
              << " unknown_queue_writes=" << model.unknown_queue_writes()
              << " last_unknown_queue_offset=0x" << std::hex
              << model.last_unknown_queue_offset() << std::dec
              << " ubase_errors=" << model.ubase_errors() << '\n';
    if (!options.state_out.empty()) {
        std::ofstream output(options.state_out,
                             std::ios::binary | std::ios::trunc);
        if (!output || !model.SaveState(output)) {
            std::cerr << "udma-device-sim: device was not quiescent; state not saved\n";
            return 4;
        }
        std::cerr << "[UDMA_CHECKPOINT] saved=" << options.state_out << '\n';
    }
    SimbricksBaseIfClose(&host_if.base);
    SimbricksBaseIfClose(&net_if.base);
    SimbricksBaseIfSHMPoolUnmap(&pool);
    SimbricksBaseIfSHMPoolUnlink(&pool);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        std::cerr << "usage: udma-device-sim --host-socket PATH --net-socket PATH "
                     "--shm PATH [--sync off|optional|required] "
                     "[--link-latency-ps N | --host-link-latency-ps N "
                     "--net-link-latency-ps N] [--sync-interval-ps N] "
                     "[--eid N] [--ports N] "
                     "[--lifecycle-sync] [--state-in PATH] [--state-out PATH] "
                     "[--test-abi]\n";
        return 2;
    }
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    return Run(options);
}
