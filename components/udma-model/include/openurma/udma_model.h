// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
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
    virtual void DmaReadToken(std::uint32_t token, std::uint64_t address,
                              std::size_t length, ReadCompletion completion)
    {
        (void)token;
        DmaReadIoVirtual(address, length, std::move(completion));
    }
    virtual void DmaWriteToken(std::uint32_t token, std::uint64_t address,
                               std::vector<std::uint8_t> data,
                               Completion completion)
    {
        (void)token;
        DmaWriteIoVirtual(address, std::move(data), std::move(completion));
    }
    virtual void SetInterrupt(std::uint32_t vector, bool asserted) = 0;
    virtual void PulseInterrupt(std::uint32_t vector)
    {
        SetInterrupt(vector, true);
        SetInterrupt(vector, false);
    }
    virtual void MsiWrite(std::uint64_t physical_address, std::uint32_t data,
                          Completion completion)
    {
        std::vector<std::uint8_t> bytes(4);
        for (std::uint32_t i = 0; i < 4; ++i)
            bytes[i] = static_cast<std::uint8_t>(data >> (8 * i));
        DmaWrite(physical_address, std::move(bytes), std::move(completion));
    }
};

struct Frame {
    enum class Operation : std::uint8_t {
        Raw = 0xff,
        Send = 0,
        SendImmediate = 1,
        Write = 3,
        ReadRequest = 6,
        WriteAck = 0x83,
        ReadResponse = 0x85,
    };
    std::uint64_t sequence{};
    std::uint32_t source_eid{};
    std::uint32_t destination_eid{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    Operation operation{Operation::Raw};
    std::uint32_t source_jetty{};
    std::uint32_t destination_jetty{};
    std::uint32_t tpn{};
    std::uint32_t segment{};
    std::uint64_t remote_address{};
    std::uint64_t immediate{};
    std::uint64_t request_id{};
    std::uint32_t transfer_length{};
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
        std::uint32_t endpoint_eid{0x100};
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
    void SetLinkState(std::uint32_t port, bool up);

    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t completed() const { return completed_; }
    std::size_t jfc_count() const { return jfc_contexts_.size(); }
    std::size_t jfr_count() const { return jfr_contexts_.size(); }
    std::size_t jetty_count() const { return jetty_contexts_.size(); }
    std::size_t tp_count() const { return tp_routes_.size(); }
    bool tp_active(std::uint32_t id) const
    {
        const auto found = tp_routes_.find(id);
        return found != tp_routes_.end() && found->second.active;
    }
    std::uint32_t tp_port(std::uint32_t id) const
    {
        const auto found = tp_routes_.find(id);
        return found == tp_routes_.end() ? UINT32_MAX : found->second.port;
    }

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
    void HandleCtrlq(std::vector<std::uint8_t> descriptor,
                     std::uint32_t descriptor_count);
    void ApplyCtrlq(std::vector<std::uint8_t> descriptor,
                    std::uint32_t descriptor_count,
                    std::vector<std::uint8_t> request);
    bool BuildCtrlqResponse(const std::vector<std::uint8_t>& request,
                            std::vector<std::uint8_t>& event);
    void EmitCtrlqResponse(std::vector<std::uint8_t> event,
                           Completion completion);
    bool HandleJettyMmio(std::uint64_t offset, std::uint32_t length,
                         std::uint64_t value);
    void ProcessSq(std::uint32_t jetty_id, std::uint32_t producer,
                   std::vector<std::uint8_t> direct = {});
    void HandleSqWqe(std::uint32_t jetty_id, std::uint32_t producer,
                     std::vector<std::uint8_t> raw);
    void SubmitSqPayload(std::uint32_t jetty_id, std::uint32_t producer,
                         std::vector<std::uint8_t> raw,
                         std::vector<std::uint8_t> payload);
    void CompleteSq(std::uint32_t jetty_id, std::uint32_t producer,
                    std::uint32_t wqebbs, std::uint16_t completed_index,
                    std::uint8_t opcode, std::uint32_t byte_count,
                    std::uint64_t immediate, bool completion_enabled);
    void WriteCqe(std::uint32_t jfc_id, bool receive, bool jetty,
                  std::uint8_t opcode, std::uint16_t entry_index,
                  std::uint32_t local_id, std::uint32_t byte_count,
                  std::uint64_t user_data, std::uint64_t immediate,
                  Completion completion, std::uint32_t remote_id = 0,
                  std::uint32_t remote_eid = 0, std::uint32_t tpn = 0);
    void EmitCompletionEvent(std::uint32_t jfc_id, Completion completion);
    void RaiseInterrupt(std::uint32_t vector, Completion completion);
    struct MsiState {
        std::uint32_t vector{};
        std::uint64_t address{};
        std::uint32_t data{};
        std::vector<std::uint32_t> tokens;
        std::size_t index{};
        Completion completion;
    };
    void ContinueMsi(std::shared_ptr<MsiState> state);
    void ProcessReceiveQueue();
    void ReceiveSend(std::shared_ptr<Frame> frame);
    void ReceiveSge(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                    std::uint32_t rqe_index, std::uint32_t sge_index,
                    std::uint32_t copied);
    void FinishReceive(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                       std::uint32_t rqe_index);
    void ReceiveWrite(Frame frame);
    void ReceiveReadRequest(Frame frame);
    void ReceiveWriteAck(Frame frame);
    void ReceiveReadResponse(Frame frame);
    using TranslateCompletion = std::function<void(bool, std::uint64_t)>;
    void TranslateToken(std::uint32_t token, std::uint64_t address,
                        TranslateCompletion completion);
    void WalkTokenPageTable(std::uint64_t table, std::uint64_t address,
                            std::uint32_t level,
                            TranslateCompletion completion);
    void ReadToken(std::uint32_t token, std::uint64_t address,
                   std::size_t length, ReadCompletion completion);
    void WriteToken(std::uint32_t token, std::uint64_t address,
                    std::vector<std::uint8_t> data, Completion completion);
    struct TokenReadState {
        std::uint32_t token{};
        std::uint64_t address{};
        std::size_t length{};
        std::size_t offset{};
        std::vector<std::uint8_t> bytes;
        ReadCompletion completion;
    };
    struct TokenWriteState {
        std::uint32_t token{};
        std::uint64_t address{};
        std::size_t offset{};
        std::vector<std::uint8_t> bytes;
        Completion completion;
    };
    void ContinueTokenRead(std::shared_ptr<TokenReadState> state);
    void ContinueTokenWrite(std::shared_ptr<TokenWriteState> state);
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
    std::uint64_t msi_iova_{};
    std::uint64_t msi_physical_{};
    struct QueueContext {
        std::uint64_t queue_iova{};
        std::uint64_t index_iova{};
        std::uint64_t producer_iova{};
        std::uint32_t depth{};
        std::uint32_t completion_queue{};
        std::uint32_t token{};
        std::uint32_t payload_token{};
        std::uint32_t receive_queue{};
        std::uint32_t receive_completion_queue{};
        std::uint32_t eid_index{};
        std::uint64_t user_queue{};
        std::uint64_t device_page_offset{};
        std::uint64_t producer{};
        std::uint64_t consumer{};
        bool is_jetty{true};
        bool busy{};
        bool direct_pending{};
        std::uint32_t target_producer{};
        std::uint32_t max_sge{1};
        std::uint32_t entry_stride{16};
        std::uint32_t moderation_count{1};
        std::uint32_t moderation_period{};
        std::uint32_t pending_completions{};
        std::array<std::uint8_t, 64> direct_wqe{};
        std::uint64_t direct_valid{};
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
    struct TpRoute {
        std::uint32_t id{};
        std::uint32_t tpn{};
        std::uint32_t local_eid{};
        std::uint32_t remote_eid{};
        std::uint32_t port{};
        bool active{};
    };
    std::unordered_map<std::uint32_t, TpRoute> tp_routes_;
    std::uint32_t next_tp_id_{1};
    std::uint32_t next_tp_port_{};
    std::vector<bool> link_up_;
    std::uint64_t next_sequence_{1};
    std::uint64_t next_rma_request_{1};
    std::uint64_t submitted_{0};
    std::uint64_t completed_{0};
    std::unordered_map<std::uint64_t, Descriptor> pending_;
    std::deque<Frame> receive_frames_;
    bool receive_busy_{};
    struct PendingRma {
        std::uint32_t jetty_id{};
        std::uint32_t producer{};
        std::uint16_t completed_index{};
        std::uint8_t opcode{};
        std::uint32_t byte_count{};
        std::uint64_t immediate{};
        bool completion{};
        std::uint32_t local_token{};
        std::uint64_t local_address{};
    };
    std::unordered_map<std::uint64_t, PendingRma> pending_rma_;
};

} // namespace openurma::device
