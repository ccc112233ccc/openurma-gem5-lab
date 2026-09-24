// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_net/if.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace device = openurma::device;
namespace host_proto = openurma::proto::host;
namespace net_proto = openurma::proto::net;

namespace {

template <typename T>
void ZeroVolatile(volatile T& object)
{
    auto* bytes = reinterpret_cast<volatile std::uint8_t*>(&object);
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[i] = 0;
}

template <typename Message>
void CopyPayload(volatile Message* message, const std::vector<std::uint8_t>& bytes)
{
    auto* destination = reinterpret_cast<volatile std::uint8_t*>(message) + sizeof(Message);
    for (std::size_t i = 0; i < bytes.size(); ++i) destination[i] = bytes[i];
}

bool Connect(SimbricksBaseIf& interface, SimbricksBaseIfParams& params,
             const std::string& socket, const void* tx_intro, std::size_t tx_len,
             void* rx_intro, std::size_t rx_len)
{
    params.sock_path = socket.c_str();
    params.sync_mode = kSimbricksBaseIfSyncDisabled;
    if (SimbricksBaseIfInit(&interface, &params) != 0 ||
        SimbricksBaseIfConnect(&interface) != 0) return false;
    SimBricksBaseIfEstablishData establish{
        &interface, tx_intro, tx_len, rx_intro, rx_len};
    return SimBricksBaseIfEstablish(&establish, 1) == 0;
}

int RunHost(const std::string& socket)
{
    host_proto::Interface interface{};
    SimbricksBaseIfParams params{};
    host_proto::DefaultParams(&params);
    host_proto::HostIntro host_intro{
        host_proto::kVersion, 64, 0, 0x2d000000, 0x01000000};
    host_proto::DeviceIntro device_intro{};
    if (!Connect(interface.base, params, socket, &host_intro, sizeof(host_intro),
                 &device_intro, sizeof(device_intro))) return 10;
    if (device_intro.version != host_proto::kVersion || device_intro.port_count != 2) return 11;

    constexpr std::uint64_t descriptor_address = 0x10000;
    constexpr std::uint64_t payload_address = 0x20000;
    constexpr std::uint64_t completion_address = 0x30000;
    const std::vector<std::uint8_t> payload{'u', 'b', '-', 'i', 'p', 'c'};
    const device::Descriptor descriptor{1, 1, 0,
        static_cast<std::uint32_t>(payload.size()), 0x101, 0x202,
        payload_address, completion_address};
    std::vector<std::uint8_t> descriptor_bytes(sizeof(descriptor));
    std::memcpy(descriptor_bytes.data(), &descriptor, sizeof(descriptor));

    auto* doorbell = host_proto::UbHostH2DOutAlloc(&interface, 0);
    if (doorbell == nullptr) return 12;
    ZeroVolatile(doorbell->mmio);
    doorbell->mmio.request_id = 1;
    doorbell->mmio.offset = device::UdmaModel::kRegisterDoorbell;
    doorbell->mmio.value = descriptor_address;
    doorbell->mmio.length = 8;
    host_proto::UbHostH2DOutSend(&interface, doorbell,
        static_cast<std::uint8_t>(host_proto::H2DType::MmioWrite));

    bool mmio_done = false;
    bool completion_written = false;
    bool interrupt_seen = false;
    for (std::uint64_t spins = 0; spins < 10000000 && !interrupt_seen; ++spins) {
        auto* message = host_proto::UbHostD2HInPoll(&interface, UINT64_MAX);
        if (message == nullptr) {
            std::this_thread::yield();
            continue;
        }
        const auto type = static_cast<host_proto::D2HType>(
            host_proto::UbHostD2HInType(&interface, message));
        if (type == host_proto::D2HType::MmioCompletion) {
            mmio_done = message->completion.status == 0;
        } else if (type == host_proto::D2HType::DmaRead) {
            const std::uint64_t address = message->dma.address;
            const std::uint64_t request_id = message->dma.request_id;
            const auto& bytes = address == descriptor_address ? descriptor_bytes : payload;
            auto* response = host_proto::UbHostH2DOutAlloc(&interface, 0);
            if (response == nullptr || bytes.size() != message->dma.length) return 13;
            ZeroVolatile(response->completion);
            response->completion.request_id = request_id;
            response->completion.length = static_cast<std::uint32_t>(bytes.size());
            response->completion.status = 0;
            CopyPayload(response, bytes);
            host_proto::UbHostH2DOutSend(&interface, response,
                static_cast<std::uint8_t>(host_proto::H2DType::DmaReadCompletion));
        } else if (type == host_proto::D2HType::DmaWrite) {
            if (message->dma.address != completion_address ||
                message->dma.length != sizeof(device::CompletionEntry)) return 14;
            device::CompletionEntry cqe{};
            auto* source = reinterpret_cast<volatile std::uint8_t*>(message) +
                           sizeof(host_proto::D2HMessage);
            auto* destination = reinterpret_cast<std::uint8_t*>(&cqe);
            for (std::size_t i = 0; i < sizeof(cqe); ++i) destination[i] = source[i];
            completion_written = cqe.status == 0 && cqe.bytes == payload.size();
            auto* response = host_proto::UbHostH2DOutAlloc(&interface, 0);
            if (response == nullptr) return 15;
            ZeroVolatile(response->completion);
            response->completion.request_id = message->dma.request_id;
            response->completion.status = 0;
            host_proto::UbHostH2DOutSend(&interface, response,
                static_cast<std::uint8_t>(host_proto::H2DType::DmaWriteCompletion));
        } else if (type == host_proto::D2HType::Interrupt) {
            interrupt_seen = message->interrupt.action ==
                static_cast<std::uint8_t>(host_proto::InterruptAction::Raise);
        }
        host_proto::UbHostD2HInDone(&interface, message);
    }
    SimbricksBaseIfClose(&interface.base);
    if (!mmio_done || !completion_written || !interrupt_seen) return 16;
    std::cout << "mock-host: MMIO/DMA/CQE/IRQ contract PASS\n";
    return 0;
}

int RunNetwork(const std::string& socket)
{
    net_proto::Interface interface{};
    SimbricksBaseIfParams params{};
    net_proto::DefaultParams(&params);
    net_proto::Intro intro{net_proto::kVersion, 64, 16384, 32, 0};
    net_proto::Intro device_intro{};
    if (!Connect(interface.base, params, socket, &intro, sizeof(intro),
                 &device_intro, sizeof(device_intro))) return 20;
    if (device_intro.version != net_proto::kVersion) return 21;
    for (std::uint64_t spins = 0; spins < 10000000; ++spins) {
        auto* message = net_proto::UbNetInPoll(&interface, UINT64_MAX);
        if (message == nullptr) {
            std::this_thread::yield();
            continue;
        }
        const auto type = static_cast<net_proto::MessageType>(
            net_proto::UbNetInType(&interface, message));
        bool valid = false;
        if (type == net_proto::MessageType::Frame) {
            const std::vector<std::uint8_t> expected{'u', 'b', '-', 'i', 'p', 'c'};
            const auto* source = reinterpret_cast<volatile std::uint8_t*>(message) +
                                 sizeof(net_proto::Message);
            valid = message->frame.source_eid == 0x101 &&
                    message->frame.destination_eid == 0x202 &&
                    message->frame.source_port == 1 &&
                    message->frame.length == expected.size();
            for (std::size_t i = 0; valid && i < expected.size(); ++i)
                valid = source[i] == expected[i];
        }
        net_proto::UbNetInDone(&interface, message);
        SimbricksBaseIfClose(&interface.base);
        if (!valid) return 22;
        std::cout << "mock-network: UB-NET frame contract PASS\n";
        return 0;
    }
    return 23;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: udma-transport-peer host|net SOCKET\n";
        return 2;
    }
    const std::string role(argv[1]);
    if (role == "host") return RunHost(argv[2]);
    if (role == "net") return RunNetwork(argv[2]);
    return 2;
}
