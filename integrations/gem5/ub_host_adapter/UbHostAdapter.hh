// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "dev/arm/base_gic.hh"
#include "dev/dma_device.hh"
#include "params/UbHostAdapter.hh"
#include "protocol/ub_host/if.h"
#include "sim/eventq.hh"

#include <array>
#include <chrono>
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
    void startup() override;
    AddrRangeList getAddrRanges() const override;
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;
    Tick read(PacketPtr packet) override;
    Tick write(PacketPtr packet) override;
    void toggleLifecycleSync();

  private:
    struct DmaOperation {
        UbHostAdapter &owner;
        uint64_t requestId;
        bool read;
        bool msi;
        std::vector<uint8_t> bytes;
        EventFunctionWrapper done;
        bool completed{false};

        DmaOperation(UbHostAdapter &owner, uint64_t request_id, bool is_read,
                     bool is_msi, size_t length);
    };

    void connectDevice();
    void pollDevice();
    bool serviceDevice(uint64_t deadline);
    void scheduleNextPoll();
    void handleDma(volatile openurma::proto::host::D2HMessage *message,
                   bool read);
    void completeDma(DmaOperation *operation);
    void handleInterrupt(
        const volatile openurma::proto::host::Interrupt &interrupt_message);
    uint64_t transactMmio(Addr offset, unsigned length, uint64_t value,
                          bool write);
    void sendDmaCompletion(const DmaOperation &operation, bool success);
    uint64_t protocolTime() const;
    void resetProtocolEpoch();

    const Addr pioAddr;
    const Addr pioSize;
    const Tick pioLatency;
    const Tick pollInterval;
    const bool syncEnabled;
    const bool lifecycleSync;
    const Tick linkLatency;
    const Tick syncInterval;
    const std::string socketPath;
    DmaPort msiPort;
    std::array<ArmInterruptPin *, 3> interrupts{};
    openurma::proto::host::Interface interface{};
    openurma::proto::host::DeviceIntro deviceIntro{};
    uint64_t nextRequest{1};
    uint64_t serviceTime{0};
    Tick epochOrigin{0};
    bool lifecycleSyncActive{false};
    bool lifecycleCommitSeen{false};
    bool lifecycleCommitEnabled{false};
    uint64_t lifecycleGeneration{0};
    uint64_t pollEvents{0};
    uint64_t servicePolls{0};
    uint64_t emptyServicePolls{0};
    uint64_t syncMessages{0};
    uint64_t syncBackpressure{0};
    uint64_t boundaryWaitYields{0};
    uint64_t mmioTransactions{0};
    uint64_t dmaTransactions{0};
    std::chrono::steady_clock::time_point wallStarted{
        std::chrono::steady_clock::now()};
    EventFunctionWrapper pollEvent;
    std::unordered_map<uint64_t, uint64_t> mmioCompletions;
    std::vector<std::unique_ptr<DmaOperation>> dmaOperations;
};

} // namespace gem5
