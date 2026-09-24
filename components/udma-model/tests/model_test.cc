// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_host/proto.h"
#include "protocol/ub_net/if.h"
#include "protocol/ub_net/proto.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace device = openurma::device;

class MockHost final : public device::HostInterface {
  public:
    void Store(std::uint64_t address, const std::vector<std::uint8_t>& bytes)
    {
        memory_[address] = bytes;
    }

    template <typename T>
    void StoreObject(std::uint64_t address, const T& object)
    {
        const auto* first = reinterpret_cast<const std::uint8_t*>(&object);
        Store(address, std::vector<std::uint8_t>(first, first + sizeof(object)));
    }

    void DmaRead(std::uint64_t address, std::size_t length,
                 device::ReadCompletion completion) override
    {
        events.emplace_back("dma-read");
        const auto found = memory_.find(address);
        if (found == memory_.end() || found->second.size() != length) {
            completion(false, {});
            return;
        }
        completion(true, found->second);
    }

    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> data,
                  device::Completion completion) override
    {
        events.emplace_back("dma-write");
        Store(address, data);
        completion(true);
    }

    void SetInterrupt(std::uint32_t vector, bool asserted) override
    {
        events.emplace_back("irq");
        irq_vector = vector;
        irq_asserted = asserted;
    }

    template <typename T>
    T LoadObject(std::uint64_t address) const
    {
        const auto& bytes = memory_.at(address);
        assert(bytes.size() == sizeof(T));
        T result{};
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    }

    const std::vector<std::uint8_t>& Load(std::uint64_t address) const
    {
        return memory_.at(address);
    }

    std::vector<std::string> events;
    std::uint32_t irq_vector{};
    bool irq_asserted{};

  private:
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory_;
};

class MockNetwork final : public device::NetworkInterface {
  public:
    explicit MockNetwork(MockHost& host) : host_(host) {}

    void Send(device::Frame frame, device::Completion completion) override
    {
        host_.events.emplace_back("frame");
        frames.push_back(std::move(frame));
        completion(true);
    }

    std::vector<device::Frame> frames;

  private:
    MockHost& host_;
};

int main()
{
    static_assert(sizeof(openurma::proto::host::H2DMessage) == 64);
    static_assert(sizeof(openurma::proto::host::D2HMessage) == 64);
    static_assert(sizeof(openurma::proto::net::Message) == 64);
    static_assert(sizeof(openurma::proto::host::Interface) ==
                  sizeof(SimbricksBaseIf));
    static_assert(sizeof(openurma::proto::net::Interface) ==
                  sizeof(SimbricksBaseIf));

    constexpr std::uint64_t descriptor_address = 0x10000;
    constexpr std::uint64_t payload_address = 0x20000;
    constexpr std::uint64_t completion_address = 0x30000;
    const std::vector<std::uint8_t> payload{'u', 'b', '-', 'f', 'r', 'a', 'm', 'e'};

    MockHost host;
    MockNetwork network(host);
    device::UdmaModel::Config config{};
    config.extraction_test_abi = true;
    device::UdmaModel model(host, network, config);
    const device::Descriptor descriptor{
        1, 1, 0, static_cast<std::uint32_t>(payload.size()),
        0x101, 0x202, payload_address, completion_address};
    host.StoreObject(descriptor_address, descriptor);
    host.Store(payload_address, payload);

    assert(model.ReadMmio(device::UdmaModel::kRegisterIdentity, 8) ==
           device::UdmaModel::kIdentity);
    assert(model.WriteMmio(device::UdmaModel::kRegisterDoorbell, 8,
                           descriptor_address));
    assert(model.submitted() == 1);
    assert(model.completed() == 1);
    assert(network.frames.size() == 1);
    assert(network.frames[0].source_eid == 0x101);
    assert(network.frames[0].destination_eid == 0x202);
    assert(network.frames[0].source_port == 1);
    assert(network.frames[0].bytes == payload);

    const auto cqe = host.LoadObject<device::CompletionEntry>(completion_address);
    assert(cqe.sequence == 1);
    assert(cqe.status == 0);
    assert(cqe.bytes == payload.size());
    assert(host.irq_asserted && host.irq_vector == 0);
    const std::vector<std::string> expected{
        "dma-read", "dma-read", "frame", "dma-write", "irq"};
    assert(host.events == expected);

    // Production-mode control-plane contract copied from the official-driver
    // path: firmware discovery publishes absolute resources, UMMU capability
    // and handshake registers behave architecturally, and queue registers
    // retain driver programming.
    device::UdmaModel::Config official_config{};
    official_config.mmio_base = 0x2d000000;
    official_config.port_count = 2;
    device::UdmaModel official_model(host, network, official_config);
    std::uint64_t value{};
    const std::string ubios("ubios");
    for (std::size_t i = 0; i < ubios.size(); ++i) {
        assert(official_model.ReadMmio(0x10000 + i, 1, value));
        assert(value == static_cast<std::uint8_t>(ubios[i]));
    }
    assert(official_model.ReadMmio(0x10010, 4, value) && value == 56);
    assert(official_model.ReadMmio(0x10028, 8, value) &&
           value == official_config.mmio_base + 0x11c00);
    assert(official_model.ReadMmio(0x10030, 8, value) &&
           value == official_config.mmio_base + 0x11000);
    assert(official_model.ReadMmio(0x11000 + 56 + 32, 8, value) &&
           value == official_config.mmio_base + 0x12000);
    assert(official_model.ReadMmio(0xf00010, 4, value) && value == 0x00000b08);
    assert(official_model.WriteMmio(0xf00030, 4, 0x55aa));
    assert(official_model.ReadMmio(0xf00034, 4, value) && value == 0x55aa);
    assert(official_model.WriteMmio(0xf00050, 4, 0x80000003));
    assert(official_model.ReadMmio(0xf00050, 4, value) && value == 3);
    assert(official_model.WriteMmio(0xf00108, 4, 0x80000007));
    assert(official_model.ReadMmio(0xf0010c, 4, value) &&
           value == 0x80000007);
    assert(official_model.WriteMmio(0x12010, 4, 64));
    assert(official_model.ReadMmio(0x12010, 4, value) && value == 64);
    assert(official_model.WriteMmio(0x318408, 4, 8));
    assert(official_model.ReadMmio(0x318408, 4, value) && value == 8);

    constexpr std::uint64_t ubios_sq = 0x40000;
    constexpr std::uint64_t ubios_rq = 0x50000;
    constexpr std::uint64_t ubios_cq = 0x60000;
    std::vector<std::uint8_t> ubios_sqe(16, 0);
    const auto store32 = [](std::vector<std::uint8_t>& bytes,
                            std::size_t offset, std::uint32_t word) {
        for (std::size_t i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(word >> (8 * i));
    };
    store32(ubios_sqe, 0, (36U << 16) | (0x10U << 8));
    store32(ubios_sqe, 4, 7);
    store32(ubios_sqe, 8, 0x100);
    std::vector<std::uint8_t> token_request(36, 0);
    token_request[31] = 0x10;
    host.Store(ubios_sq, ubios_sqe);
    host.Store(ubios_sq + 0x100, token_request);
    assert(official_model.WriteMmio(0x12000, 4, ubios_sq));
    assert(official_model.WriteMmio(0x12004, 4, ubios_sq >> 32));
    assert(official_model.WriteMmio(0x12010, 4, 8));
    assert(official_model.WriteMmio(0x12040, 4, ubios_rq));
    assert(official_model.WriteMmio(0x12044, 4, ubios_rq >> 32));
    assert(official_model.WriteMmio(0x12070, 4, ubios_cq));
    assert(official_model.WriteMmio(0x12074, 4, ubios_cq >> 32));
    assert(official_model.WriteMmio(0x12008, 4, 1));
    assert(official_model.ReadMmio(0x1200c, 4, value) && value == 1);
    assert(official_model.ReadMmio(0x12048, 4, value) && value == 1);
    assert(official_model.ReadMmio(0x12078, 4, value) && value == 1);
    const auto& token_response = host.Load(ubios_rq);
    assert(token_response.size() == 40);
    assert(token_response[31] == 0x11);
    assert(token_response[32] == 1);
    assert(token_response[36] == 'M' && token_response[37] == 'R' &&
           token_response[38] == 'U' && token_response[39] == 'O');
    const auto ubios_cqe = host.LoadObject<std::array<std::uint8_t, 16>>(ubios_cq);
    assert(ubios_cqe[0] == 0 && ubios_cqe[1] == 0x11);
    assert(ubios_cqe[2] == 40 && ubios_cqe[4] == 7);
    assert(!official_model.ReadMmio(
        device::UdmaModel::kOfficialApertureBytes, 1, value));

    std::cout << "udma-model host/device/network separation test: PASS\n";
    return 0;
}
