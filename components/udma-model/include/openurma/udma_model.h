// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openurma::device {

using Completion = std::function<void(bool)>;
using ReadCompletion = std::function<void(bool, std::vector<std::uint8_t>)>;

class HostInterface {
  public:
    virtual ~HostInterface() = default;
    virtual void DmaRead(std::uint64_t address, std::size_t length,
                         ReadCompletion completion) = 0;
    virtual void DmaWrite(std::uint64_t address,
                          std::vector<std::uint8_t> data,
                          Completion completion) = 0;
    virtual void DmaReadIoVirtual(std::uint64_t address, std::size_t length,
                                  ReadCompletion completion)
    {
        DmaRead(address, length, std::move(completion));
    }
    virtual void DmaWriteIoVirtual(std::uint64_t address,
                                   std::vector<std::uint8_t> data,
                                   Completion completion)
    {
        DmaWrite(address, std::move(data), std::move(completion));
    }
    virtual void SetInterrupt(std::uint32_t vector, bool asserted) = 0;
};

struct Frame {
    std::uint64_t sequence{};
    std::uint32_t source_eid{};
    std::uint32_t destination_eid{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    std::vector<std::uint8_t> bytes;
};

class NetworkInterface {
  public:
    virtual ~NetworkInterface() = default;
    virtual void Send(Frame frame, Completion completion) = 0;
};

// First simulator-independent execution slice. The descriptor format is an
// internal model contract used only by the extraction tests; official UDMA WQE
// decoders will replace/extend it as they move out of NICTopologySC.
struct Descriptor {
    std::uint8_t opcode;
    std::uint8_t source_port;
    std::uint16_t flags;
    std::uint32_t length;
    std::uint32_t source_eid;
    std::uint32_t destination_eid;
    std::uint64_t payload_address;
    std::uint64_t completion_address;
};
static_assert(sizeof(Descriptor) == 32);

struct CompletionEntry {
    std::uint64_t sequence;
    std::uint32_t status;
    std::uint32_t bytes;
};
static_assert(sizeof(CompletionEntry) == 16);

class UdmaModel {
  public:
    struct Config {
        std::uint64_t mmio_base{};
        std::uint32_t port_count{2};
        // Temporary descriptor ABI used only by the extraction contract test.
        // Production device processes leave this false.
        bool extraction_test_abi{false};
    };

    static constexpr std::uint64_t kRegisterIdentity = 0x0000;
    static constexpr std::uint64_t kRegisterStatus = 0x0008;
    static constexpr std::uint64_t kRegisterDoorbell = 0x0100;
    static constexpr std::uint64_t kIdentity = 0x4f50454e55444d41ULL;

    static constexpr std::uint64_t kOfficialApertureBytes = 0x01000000;

    UdmaModel(HostInterface& host, NetworkInterface& network);
    UdmaModel(HostInterface& host, NetworkInterface& network, Config config);

    std::uint64_t ReadMmio(std::uint64_t offset, std::uint32_t length) const;
    bool ReadMmio(std::uint64_t offset, std::uint32_t length,
                  std::uint64_t& value) const;
    bool WriteMmio(std::uint64_t offset, std::uint32_t length,
                   std::uint64_t value);
    void Receive(Frame frame);

    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t completed() const { return completed_; }
    std::size_t jfc_count() const { return jfc_contexts_.size(); }
    std::size_t jfr_count() const { return jfr_contexts_.size(); }
    std::size_t jetty_count() const { return jetty_contexts_.size(); }

  private:
    void FetchDescriptor(std::uint64_t address);
    void FetchPayload(std::uint64_t sequence, Descriptor descriptor);
    void Finish(std::uint64_t sequence, const Descriptor& descriptor,
                std::uint32_t status);
    void KickUbios(std::uint32_t producer);
    void ProcessNextUbios();
    void HandleUbiosSqe(std::vector<std::uint8_t> sqe);
    void HandleUbiosPayload(std::vector<std::uint8_t> sqe,
                            std::vector<std::uint8_t> request);
    bool BuildUbiosResponse(const std::vector<std::uint8_t>& request,
                            std::uint8_t task_type, std::uint8_t opcode,
                            std::vector<std::uint8_t>& response,
                            std::uint8_t& response_opcode);
    std::uint32_t UbiosConfigRead(std::uint32_t address,
                                  bool endpoint) const;
    void UbiosConfigWrite(std::uint32_t address, std::uint32_t byte_enable,
                          std::uint32_t value, bool endpoint);
    void FailUbios();
    void KickUbase(std::uint32_t producer);
    void ProcessNextUbase();
    void HandleUbaseDescriptor(std::vector<std::uint8_t> descriptor);
    void HandleUbaseMailbox(std::vector<std::uint8_t> descriptor,
                            std::uint32_t descriptor_count);
    void ApplyUbaseMailbox(std::vector<std::uint8_t> descriptor,
                           std::uint32_t descriptor_count,
                           std::vector<std::uint8_t> context);
    void EmitMailboxEvent(std::uint16_t sequence, Completion completion);
    std::vector<std::uint8_t> BuildUbaseResponse(std::uint16_t opcode) const;
    void FinishUbaseDescriptor(std::vector<std::uint8_t> descriptor,
                               std::uint16_t opcode,
                               std::uint32_t descriptor_count,
                               std::vector<std::uint8_t> response,
                               std::function<void()> after_completion = {});
    void FailUbase();

    HostInterface& host_;
    NetworkInterface& network_;
    Config config_;
    std::array<std::uint8_t, 0x5000> ummu_registers_{};
    std::array<std::uint32_t, 0x120 / sizeof(std::uint32_t)>
        ubios_message_queue_registers_{};
    std::array<std::uint32_t, 0x2c / sizeof(std::uint32_t)>
        ubase_command_queue_registers_{};
    std::uint32_t ubase_command_source_{};
    std::unordered_map<std::uint32_t, std::uint32_t> ubios_root_config_;
    std::unordered_map<std::uint32_t, std::uint32_t> ubios_endpoint_config_;
    std::uint32_t ubios_root_cna_{};
    std::uint32_t ubios_endpoint_cna_{};
    std::uint32_t ubios_target_producer_{};
    bool ubios_busy_{};
    std::uint64_t ubios_errors_{};
    std::uint32_t ubase_target_producer_{};
    bool ubase_busy_{};
    std::uint64_t ubase_errors_{};
    struct QueueContext {
        std::uint64_t queue_iova{};
        std::uint64_t index_iova{};
        std::uint64_t producer_iova{};
        std::uint32_t depth{};
        std::uint32_t completion_queue{};
        std::uint32_t token{};
    };
    std::uint64_t aeq_iova_{};
    std::uint32_t aeq_depth_{};
    std::uint32_t aeq_producer_{};
    std::uint64_t ceq_iova_{};
    std::uint32_t ceq_depth_{};
    std::uint32_t ceq_producer_{};
    std::unordered_map<std::uint32_t, QueueContext> jfc_contexts_;
    std::unordered_map<std::uint32_t, QueueContext> jfr_contexts_;
    std::unordered_map<std::uint32_t, QueueContext> jetty_contexts_;
    std::uint64_t next_sequence_{1};
    std::uint64_t submitted_{0};
    std::uint64_t completed_{0};
    std::unordered_map<std::uint64_t, Descriptor> pending_;
};

} // namespace openurma::device
