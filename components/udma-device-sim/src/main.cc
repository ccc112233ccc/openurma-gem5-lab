// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_net/if.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace device = openurma::device;
namespace host_proto = openurma::proto::host;
namespace net_proto = openurma::proto::net;

namespace {

std::atomic<bool> running{true};

void Stop(int) { running.store(false); }

struct Options {
    std::string host_socket;
    std::string net_socket;
    std::string shm_path;
    std::uint64_t link_latency_ps{100000};
    std::uint64_t sync_interval_ps{100000};
    std::uint64_t endpoint_eid{0x100};
    std::uint64_t port_count{2};
    SimbricksBaseIfSyncMode sync_mode{kSimbricksBaseIfSyncOptional};
    bool extraction_test_abi{false};
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
            if (!ParseUnsigned(argv[++i], options.link_latency_ps)) return false;
        } else if (arg == "--sync-interval-ps" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], options.sync_interval_ps)) return false;
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
        } else {
            return false;
        }
    }
    return !options.host_socket.empty() && !options.net_socket.empty() &&
           !options.shm_path.empty();
}

template <typename T>
void ZeroVolatile(volatile T& object)
{
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
}

class HostPort final : public device::HostInterface {
  public:
    HostPort(host_proto::Interface& interface, std::uint64_t& now)
        : interface_(interface), now_(now) {}

    void Attach(device::UdmaModel* model) { model_ = model; }

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

    void SetInterrupt(std::uint32_t vector, bool asserted) override
    {
        auto* message = host_proto::UbHostD2HOutAlloc(&interface_, now_);
        if (message == nullptr) return;
        ZeroVolatile(message->interrupt);
        message->interrupt.sequence = next_request_++;
        message->interrupt.vector = vector;
        message->interrupt.action = static_cast<std::uint8_t>(
            asserted ? host_proto::InterruptAction::Raise
                     : host_proto::InterruptAction::Lower);
        host_proto::UbHostD2HOutSend(
            &interface_, message,
            static_cast<std::uint8_t>(host_proto::D2HType::Interrupt));
    }

    bool Poll()
    {
        auto* message = host_proto::UbHostH2DInPoll(&interface_, now_);
        if (message == nullptr) return false;
        const auto type = static_cast<host_proto::H2DType>(
            host_proto::UbHostH2DInType(&interface_, message));
        if (type == host_proto::H2DType::MmioRead ||
            type == host_proto::H2DType::MmioWrite) {
            HandleMmio(message->mmio, type == host_proto::H2DType::MmioWrite);
        } else if (type == host_proto::H2DType::DmaReadCompletion ||
                   type == host_proto::H2DType::DmaWriteCompletion) {
            HandleDmaCompletion(message, type);
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
                     host_proto::AddressKind::GuestPhysical)
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
};

class NetworkPort final : public device::NetworkInterface {
  public:
    NetworkPort(net_proto::Interface& interface, std::uint64_t& now)
        : interface_(interface), now_(now) {}
    void Attach(device::UdmaModel* model) { model_ = model; }

    void Send(device::Frame frame, device::Completion completion) override
    {
        const std::size_t capacity = net_proto::UbNetOutMsgLen(&interface_) -
                                     sizeof(net_proto::Message);
        auto* message = frame.bytes.size() <= capacity
                            ? net_proto::UbNetOutAlloc(&interface_, now_)
                            : nullptr;
        if (message == nullptr) {
            completion(false);
            return;
        }
        ZeroVolatile(message->frame);
        message->frame.sequence = frame.sequence;
        message->frame.length = static_cast<std::uint32_t>(frame.bytes.size());
        message->frame.source_eid = frame.source_eid;
        message->frame.destination_eid = frame.destination_eid;
        message->frame.source_port = frame.source_port;
        message->frame.destination_port = frame.destination_port;
        auto* destination = reinterpret_cast<volatile std::uint8_t*>(message) +
                            sizeof(net_proto::Message);
        for (std::size_t i = 0; i < frame.bytes.size(); ++i) destination[i] = frame.bytes[i];
        net_proto::UbNetOutSend(&interface_, message,
            static_cast<std::uint8_t>(net_proto::MessageType::Frame));
        completion(true);
    }

    bool Poll()
    {
        auto* message = net_proto::UbNetInPoll(&interface_, now_);
        if (message == nullptr) return false;
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
            model_->Receive(std::move(frame));
        }
        net_proto::UbNetInDone(&interface_, message);
        return true;
    }

  private:
    net_proto::Interface& interface_;
    std::uint64_t& now_;
    device::UdmaModel* model_{nullptr};
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
    host_params.link_latency = net_params.link_latency = options.link_latency_ps;
    host_params.sync_interval = net_params.sync_interval = options.sync_interval_ps;
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
    host.Attach(&model);
    network.Attach(&model);
    std::cout << "udma-device-sim: connected host=" << options.host_socket
              << " net=" << options.net_socket << '\n';

    while (running.load() && !SimbricksBaseIfInTerminated(&host_if.base) &&
           !SimbricksBaseIfInTerminated(&net_if.base)) {
        bool progress = false;
        while (host.Poll()) progress = true;
        while (network.Poll()) progress = true;
        host_proto::UbHostD2HOutSync(&host_if, now);
        net_proto::UbNetOutSync(&net_if, now);
        now += options.sync_interval_ps;
        if (!progress) std::this_thread::yield();
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
                     "[--link-latency-ps N] [--sync-interval-ps N] "
                     "[--eid N] [--ports N] "
                     "[--test-abi]\n";
        return 2;
    }
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    return Run(options);
}
