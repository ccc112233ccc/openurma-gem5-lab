// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "dev/arm/base_gic.hh"
#include "dev/dma_device.hh"
#include "params/UbHostAdapter.hh"
#include "protocol/ub_host/if.h"
#include "sim/eventq.hh"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gem5
{

class UbHostAdapter final : public DmaDevice
{
  public:
    PARAMS(UbHostAdapter);
    explicit UbHostAdapter(const Params &params);
    ~UbHostAdapter() override;

    void init() override;
    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr packet) override;
    Tick write(PacketPtr packet) override;

  private:
    struct DmaOperation {
        UbHostAdapter &owner;
        uint64_t requestId;
        bool read;
        std::vector<uint8_t> bytes;
        EventFunctionWrapper done;
        bool completed{false};

        DmaOperation(UbHostAdapter &owner, uint64_t request_id, bool is_read,
                     size_t length);
    };

    void connectDevice();
    void pollDevice();
    void handleDma(volatile openurma::proto::host::D2HMessage *message,
                   bool read);
    void completeDma(DmaOperation *operation);
    void handleInterrupt(
        const volatile openurma::proto::host::Interrupt &interrupt_message);
    uint64_t transactMmio(Addr offset, unsigned length, uint64_t value,
                          bool write);
    void sendDmaCompletion(const DmaOperation &operation, bool success);
    uint64_t protocolTime() const;

    const Addr pioAddr;
    const Addr pioSize;
    const Tick pioLatency;
    const Tick pollInterval;
    const std::string socketPath;
    std::array<ArmInterruptPin *, 3> interrupts{};
    openurma::proto::host::Interface interface{};
    openurma::proto::host::DeviceIntro deviceIntro{};
    uint64_t nextRequest{1};
    EventFunctionWrapper pollEvent;
    std::unordered_map<uint64_t, uint64_t> mmioCompletions;
    std::vector<std::unique_ptr<DmaOperation>> dmaOperations;
};

} // namespace gem5
