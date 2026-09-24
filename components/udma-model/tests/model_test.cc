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
    device::UdmaModel model(host, network);
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

    std::cout << "udma-model host/device/network separation test: PASS\n";
    return 0;
}
