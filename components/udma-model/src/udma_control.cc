// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

std::uint32_t
UdmaModel::UbiosConfigRead(std::uint32_t address, bool endpoint) const
{
    address &= ~3U;
    const auto& config = endpoint ? ubios_endpoint_config_ : ubios_root_config_;
    const auto saved = config.find(address);
    if (saved != config.end()) return saved->second;
    switch (address) {
      case 0x04: return 0x00010001;
      case 0x38: return endpoint ? 0x00000002 : 0x00000001;
      case 0x40: return endpoint ? 0x11000000 : 0x12000000;
      case 0x44: return endpoint ? 0xcc08a001 : 0xcc080000;
      case 0x40004: return endpoint ? 0x00000008 : 0;
      case 0x40024: return endpoint ? 0x000001c0 : 0;
      case 0x40034:
      case 0x40038:
      case 0x4003c: return endpoint ? 0x00000100 : 0;
      case 0x40048: return endpoint ? 0x00100000 : 0;
      case 0x40050: return endpoint ? 0x00200000 : 0;
      case 0x40090: return endpoint ? 0 : 256;
      case 0x40404: return endpoint ? 0 : 0x00004040;
      case 0x40c08: return endpoint ? 0x00000003 : 0;
      default: return 0;
    }
}

void
UdmaModel::UbiosConfigWrite(std::uint32_t address,
                            std::uint32_t byte_enable,
                            std::uint32_t value, bool endpoint)
{
    address &= ~3U;
    std::uint32_t merged = UbiosConfigRead(address, endpoint);
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        if ((byte_enable & (1U << byte)) != 0) {
            const std::uint32_t mask = 0xffU << (byte * 8);
            merged = (merged & ~mask) | (value & mask);
        }
    }
    (endpoint ? ubios_endpoint_config_ : ubios_root_config_)[address] = merged;
}

void
UdmaModel::KickUbios(std::uint32_t producer)
{
    ubios_target_producer_ = producer;
    if (ubios_busy_) return;
    ubios_busy_ = true;
    ProcessNextUbios();
}

void
UdmaModel::FailUbios()
{
    ++ubios_errors_;
    ubios_busy_ = false;
}

void
UdmaModel::ProcessNextUbios()
{
    constexpr std::size_t SqLow = 0x00 / 4;
    constexpr std::size_t SqHigh = 0x04 / 4;
    constexpr std::size_t Consumer = 0x0c / 4;
    constexpr std::size_t Depth = 0x10 / 4;
    const std::uint32_t depth = ubios_message_queue_registers_[Depth];
    if (depth == 0) return FailUbios();
    std::uint32_t consumer = ubios_message_queue_registers_[Consumer] % depth;
    const std::uint32_t target = ubios_target_producer_ % depth;
    if (consumer == target) {
        ubios_busy_ = false;
        return;
    }
    const std::uint64_t base = ubios_message_queue_registers_[SqLow] |
        (std::uint64_t(ubios_message_queue_registers_[SqHigh]) << 32);
    if (base == 0) return FailUbios();
    host_.DmaRead(base + std::uint64_t(consumer) * 16, 16,
        [this](bool ok, std::vector<std::uint8_t> sqe) {
            if (!ok || sqe.size() != 16) return FailUbios();
            HandleUbiosSqe(std::move(sqe));
        });
}

void
UdmaModel::HandleUbiosSqe(std::vector<std::uint8_t> sqe)
{
    constexpr std::size_t SqLow = 0x00 / 4;
    constexpr std::size_t SqHigh = 0x04 / 4;
    const std::uint32_t word0 = LoadLe<std::uint32_t>(sqe.data());
    const std::uint32_t payload_offset = LoadLe<std::uint32_t>(sqe.data() + 8);
    const std::uint32_t payload_length = (word0 >> 16) & 0xfff;
    const std::uint64_t base = ubios_message_queue_registers_[SqLow] |
        (std::uint64_t(ubios_message_queue_registers_[SqHigh]) << 32);
    if (payload_length == 0 || base > std::numeric_limits<std::uint64_t>::max() -
                                      payload_offset)
        return FailUbios();
    host_.DmaRead(base + payload_offset, payload_length,
        [this, sqe = std::move(sqe)](
            bool ok, std::vector<std::uint8_t> request) mutable {
            if (!ok) return FailUbios();
            HandleUbiosPayload(std::move(sqe), std::move(request));
        });
}

bool
UdmaModel::BuildUbiosResponse(const std::vector<std::uint8_t>& request,
                              std::uint8_t task_type, std::uint8_t opcode,
                              std::vector<std::uint8_t>& response,
                              std::uint8_t& response_opcode)
{
    const bool config_request = task_type == 0 && request.size() == 48;
    const bool token_request = task_type == 0 && request.size() == 36;
    const bool header_request = task_type == 0 && request.size() == 32;
    const bool private_request = task_type == 2 && opcode == 2 &&
        (request.size() == 4 || request.size() == 12);
    const bool enum_request = task_type == 1 && opcode <= 2 && request.size() >= 44;
    if (!config_request && !token_request && !header_request &&
        !private_request && !enum_request) return false;
    response_opcode = opcode;
    if (private_request) {
        const std::uint32_t header = LoadLe<std::uint32_t>(request.data());
        const bool write = (header & 0xf) == 1;
        response.assign(write ? 4 : 12, 0);
        StoreLe<std::uint32_t>(response, 0, header | (1U << 15));
        if (!write && response.size() >= request.size())
            std::memcpy(response.data() + 4, request.data() + 4,
                        request.size() - 4);
        return true;
    }
    if (config_request || token_request || header_request) {
        if (config_request || header_request) response = request;
        else {
            response.assign(40, 0);
            std::memcpy(response.data(), request.data(), 32);
        }
        std::uint32_t nth = LoadLe<std::uint32_t>(request.data() + 4);
        StoreLe<std::uint32_t>(response, 4, (nth << 16) | (nth >> 16));
        std::uint32_t tph0 = LoadLe<std::uint32_t>(request.data() + 12);
        std::uint32_t tph1 = LoadLe<std::uint32_t>(request.data() + 16);
        const std::uint32_t source_eid = ((tph0 & 0xff) << 12) | (tph1 >> 20);
        const std::uint32_t destination_eid = tph1 & 0xfffff;
        tph0 = (tph0 & ~0xffU) | ((destination_eid >> 12) & 0xff);
        tph1 = (source_eid & 0xfffff) | ((destination_eid & 0xfff) << 20);
        StoreLe<std::uint32_t>(response, 12, tph0);
        StoreLe<std::uint32_t>(response, 16, tph1);
        response[28] = config_request ? 16 : (token_request ? 8 : 0);
        response[29] &= 0xf0;
        response[30] = 0;
        response_opcode = opcode | 1;
        response[31] = response_opcode;
        if (header_request && request[31] == 0x28) {
            response.resize(44, 0);
            response[28] = 12;
            response[31] = response_opcode = 0x29;
            response[36] = 1;
            response[38] = 1;
        }
        if (config_request) {
            const std::uint32_t address = LoadLe<std::uint32_t>(request.data() + 36);
            const std::uint32_t dcna = LoadLe<std::uint32_t>(request.data() + 4) & 0xffff;
            const bool endpoint = ubios_endpoint_cna_ && dcna == ubios_endpoint_cna_;
            const std::uint8_t subcode = (request[31] >> 4) & 0xf;
            if (subcode == 1 || subcode == 3) {
                const std::uint32_t control = LoadLe<std::uint32_t>(request.data() + 32);
                UbiosConfigWrite(address * 4, (control >> 4) & 0xf,
                                 LoadLe<std::uint32_t>(request.data() + 44), endpoint);
            }
            StoreLe<std::uint32_t>(response, 32,
                                   UbiosConfigRead(address * 4, endpoint));
        } else if (token_request) {
            StoreLe<std::uint32_t>(response, 32, 1);
            StoreLe<std::uint32_t>(response, 36, 0x4f55524d);
        }
        return true;
    }

    const std::uint32_t scan = LoadLe<std::uint32_t>(request.data() + 16);
    const std::uint32_t hops = (scan >> 8) & 0xff;
    const std::uint32_t hop_type = (scan >> 16) & 0xf;
    const std::uint32_t hop_bits = hop_type == 0 ? 4 : (hop_type == 1 ? 8 : 16);
    const std::uint32_t forward_bytes = ((hop_bits * hops + 31) / 32) * 4;
    const std::uint32_t request_scan_bytes =
        4 + forward_bytes * ((scan & (1U << 20)) ? 2 : 1);
    const std::uint32_t response_header_bytes = 20 + forward_bytes;
    constexpr std::uint32_t CommonBytes = 24;
    const std::uint32_t tlv_bytes = 12 + 28 * config_.port_count + 8;
    const std::uint32_t extra_bytes = opcode == 0 ? tlv_bytes : (opcode == 2 ? 4 : 0);
    const std::uint32_t request_common = 16 + request_scan_bytes;
    if (request_common + CommonBytes > request.size()) return false;
    response.assign(response_header_bytes + CommonBytes + extra_bytes, 0);
    std::memcpy(response.data(), request.data(), 16);
    StoreLe<std::uint32_t>(response, 16, scan & ~(1U << 20));
    if (forward_bytes)
        std::memcpy(response.data() + 20, request.data() + 20, forward_bytes);
    const std::uint32_t response_common = response_header_bytes;
    const bool endpoint = hops != 0;
    if (opcode == 1 && request[request_common + 1] == 2) {
        const std::uint32_t cna =
            LoadLe<std::uint32_t>(request.data() + request_common + 28) & 0xffffff;
        (endpoint ? ubios_endpoint_cna_ : ubios_root_cna_) = cna;
    }
    StoreLe<std::uint32_t>(response, response_common,
                           0x01000000 | (std::uint32_t(opcode) << 16));
    std::uint32_t common1 = LoadLe<std::uint32_t>(request.data() + request_common + 4);
    common1 = (common1 & 0xff00ffffU) | ((CommonBytes + extra_bytes) / 4 << 16);
    StoreLe<std::uint32_t>(response, response_common + 4, common1);
    std::memcpy(response.data() + response_common + 8,
                request.data() + request_common + 8, 16);
    std::uint8_t* extra = response.data() + response_common + CommonBytes;
    if (opcode == 0) {
        StoreLe<std::uint32_t>(response, extra - response.data(), 0x00040100);
        StoreLe<std::uint32_t>(response, extra - response.data() + 4,
                               0x01080000 | config_.port_count);
        StoreLe<std::uint32_t>(response, extra - response.data() + 8,
                               config_.port_count << 16);
        for (std::uint32_t port = 0; port < config_.port_count; ++port) {
            const std::size_t pos = extra - response.data() + 12 + 28 * port;
            StoreLe<std::uint32_t>(response, pos, 0x021c0100);
            StoreLe<std::uint32_t>(response, pos + 4, port | (port << 16));
            StoreLe<std::uint32_t>(response, pos + 12, endpoint ? 1 : 2);
            StoreLe<std::uint32_t>(response, pos + 20,
                                   endpoint ? 0x12000000 : 0x11000000);
            StoreLe<std::uint32_t>(response, pos + 24,
                                   endpoint ? 0xcc080000 : 0xcc08a001);
        }
        const std::size_t cap = extra - response.data() + 12 + 28 * config_.port_count;
        StoreLe<std::uint32_t>(response, cap, 0x04080000);
        StoreLe<std::uint32_t>(response, cap + 4, endpoint ? 0x0002 : 0);
    } else if (opcode == 2) {
        StoreLe<std::uint32_t>(response, extra - response.data(), endpoint ? 2 : 1);
    }
    return true;
}

void
UdmaModel::HandleUbiosPayload(std::vector<std::uint8_t> sqe,
                              std::vector<std::uint8_t> request)
{
    constexpr std::size_t Depth = 0x10 / 4;
    constexpr std::size_t ReceiveLow = 0x40 / 4;
    constexpr std::size_t ReceiveHigh = 0x44 / 4;
    constexpr std::size_t ReceiveProducer = 0x48 / 4;
    constexpr std::size_t CompletionLow = 0x70 / 4;
    constexpr std::size_t CompletionHigh = 0x74 / 4;
    constexpr std::size_t CompletionProducer = 0x78 / 4;
    const std::uint32_t word0 = LoadLe<std::uint32_t>(sqe.data());
    const std::uint32_t word1 = LoadLe<std::uint32_t>(sqe.data() + 4);
    const std::uint8_t task_type = word0 & 0x3;
    const std::uint8_t opcode = (word0 >> 8) & 0xff;
    std::vector<std::uint8_t> response;
    std::uint8_t response_opcode{};
    if (!BuildUbiosResponse(request, task_type, opcode,
                            response, response_opcode)) return FailUbios();
    const std::uint32_t depth = ubios_message_queue_registers_[Depth];
    const std::uint32_t receive_index =
        ubios_message_queue_registers_[ReceiveProducer] % depth;
    const std::uint32_t completion_index =
        ubios_message_queue_registers_[CompletionProducer] % depth;
    const std::uint64_t receive_base = ubios_message_queue_registers_[ReceiveLow] |
        (std::uint64_t(ubios_message_queue_registers_[ReceiveHigh]) << 32);
    const std::uint64_t completion_base = ubios_message_queue_registers_[CompletionLow] |
        (std::uint64_t(ubios_message_queue_registers_[CompletionHigh]) << 32);
    std::vector<std::uint8_t> cqe(16, 0);
    StoreLe<std::uint32_t>(cqe, 0, std::uint32_t(task_type) |
        (std::uint32_t(response_opcode) << 8) |
        (std::uint32_t(response.size()) << 16));
    StoreLe<std::uint32_t>(cqe, 4, word1 & 0xffff);
    StoreLe<std::uint32_t>(cqe, 8, receive_index);
    host_.DmaWrite(receive_base + std::uint64_t(receive_index) * 0x800,
                   std::move(response),
        [this, cqe = std::move(cqe), completion_base, completion_index,
         receive_index, depth](bool ok) mutable {
            if (!ok) return FailUbios();
            host_.DmaWrite(completion_base + std::uint64_t(completion_index) * 16,
                           std::move(cqe),
                [this, completion_index, receive_index, depth](bool cqe_ok) {
                    if (!cqe_ok) return FailUbios();
                    constexpr std::size_t Consumer = 0x0c / 4;
                    constexpr std::size_t ReceiveProducer = 0x48 / 4;
                    constexpr std::size_t CompletionProducer = 0x78 / 4;
                    ubios_message_queue_registers_[ReceiveProducer] =
                        (receive_index + 1) % depth;
                    ubios_message_queue_registers_[CompletionProducer] =
                        (completion_index + 1) % depth;
                    ubios_message_queue_registers_[Consumer] =
                        (ubios_message_queue_registers_[Consumer] + 1) % depth;
                    ProcessNextUbios();
                });
        });
}

void
UdmaModel::KickUbase(std::uint32_t producer)
{
    ubase_target_producer_ = producer;
    if (ubase_busy_) return;
    ubase_busy_ = true;
    ProcessNextUbase();
}

void
UdmaModel::FailUbase()
{
    ++ubase_errors_;
    ubase_busy_ = false;
}

void
UdmaModel::ProcessNextUbase()
{
    constexpr std::size_t BaseLow = 0x00 / 4;
    constexpr std::size_t BaseHigh = 0x04 / 4;
    constexpr std::size_t Depth = 0x08 / 4;
    constexpr std::size_t Head = 0x14 / 4;
    const std::uint32_t depth = ubase_command_queue_registers_[Depth] << 3;
    if (depth == 0) return FailUbase();
    const std::uint32_t head = ubase_command_queue_registers_[Head] % depth;
    if (head == ubase_target_producer_ % depth) {
        ubase_busy_ = false;
        return;
    }
    const std::uint64_t base = ubase_command_queue_registers_[BaseLow] |
        (std::uint64_t(ubase_command_queue_registers_[BaseHigh]) << 32);
    if (base == 0) return FailUbase();
    ReadIoVirtual(base + std::uint64_t(head) * 32, 32,
        [this](bool ok, std::vector<std::uint8_t> descriptor) {
            if (!ok || descriptor.size() != 32) return FailUbase();
            HandleUbaseDescriptor(std::move(descriptor));
        });
}

std::vector<std::uint8_t>
UdmaModel::BuildUbaseResponse(std::uint16_t opcode) const
{
    std::vector<std::uint8_t> response;
    if (opcode == 0x0030) {
        // UBASE reports SQ capacity in WQEBBs; the official UDMA driver
        // divides this value by MAX_WQEBB_IN_SQE (4) before advertising the
        // JFS depth to userspace. JFR/JFC depths are reported directly.
        const std::uint32_t jfs_depth_wqebbs = config_.max_jfs_depth * 4;
        response.resize(312, 0);
        StoreLe<std::uint16_t>(response, 26, 1);
        StoreLe<std::uint16_t>(response, 28, 1);
        StoreLe<std::uint16_t>(response, 30, 1);
        StoreLe<std::uint16_t>(response, 32, 64);
        StoreLe<std::uint16_t>(response, 34, 64);
        StoreLe<std::uint16_t>(response, 36, 64);
        StoreLe<std::uint32_t>(response, 40, 512);
        StoreLe<std::uint32_t>(response, 44, 4096);
        StoreLe<std::uint32_t>(response, 48,
                               config_.max_jettys - config_.public_jettys);
        StoreLe<std::uint32_t>(response, 56, jfs_depth_wqebbs);
        StoreLe<std::uint32_t>(response, 60, 1024);
        StoreLe<std::uint32_t>(response, 68, config_.max_jfr_depth);
        StoreLe<std::uint32_t>(response, 84, 1024);
        StoreLe<std::uint32_t>(response, 92, config_.max_jfc_depth);
        StoreLe<std::uint32_t>(response, 192, 1);
        StoreLe<std::uint32_t>(response, 224, 1024);
        response[239] = 1;
        StoreLe<std::uint32_t>(response, 256, config_.public_jettys);
        StoreLe<std::uint32_t>(response, 264, 64);
        StoreLe<std::uint32_t>(response, 268, 128);
    } else if (opcode == 0x0002) {
        response.resize(112, 0);
        StoreLe<std::uint16_t>(response, 0, 0xaaaa);
        StoreLe<std::uint16_t>(response, 2, 64);
        StoreLe<std::uint16_t>(response, 4, 0x6cac);
        StoreLe<std::uint16_t>(response, 6, 0x1084);
        StoreLe<std::uint16_t>(response, 8, 256);
        StoreLe<std::uint16_t>(response, 10, 256);
        StoreLe<std::uint16_t>(response, 16, 0x27);
        StoreLe<std::uint16_t>(response, 18, 1);
        StoreLe<std::uint16_t>(response, 24, 64);
        StoreLe<std::uint16_t>(response, 26, 1);
        StoreLe<std::uint16_t>(response, 28, 64);
        StoreLe<std::uint16_t>(response, 30, 1);
        StoreLe<std::uint16_t>(response, 32, 64);
        StoreLe<std::uint16_t>(response, 34, 4);
        response[44] = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(config_.port_count, 255));
        StoreLe<std::uint16_t>(response, 48, 128);
        StoreLe<std::uint16_t>(response, 50, 128);
        response[52] = 64;
        StoreLe<std::uint16_t>(response, 58,
                               static_cast<std::uint16_t>(config_.public_jettys));
        StoreLe<std::uint16_t>(response, 76,
                               static_cast<std::uint16_t>(config_.public_jettys));
        StoreLe<std::uint16_t>(response, 78, static_cast<std::uint16_t>(
                               config_.max_jettys - config_.public_jettys));
        StoreLe<std::uint32_t>(response, 80, 65536);
        StoreLe<std::uint32_t>(response, 84, 65536);
        StoreLe<std::uint32_t>(response, 88, 8);
        StoreLe<std::uint32_t>(response, 92, 8);
    } else if (opcode == 0x6200) {
        response.resize(24, 0);
        StoreLe<std::uint32_t>(response, 0, 400000);
        response[14] = 1;
    }
    return response;
}

void
UdmaModel::HandleUbaseDescriptor(std::vector<std::uint8_t> descriptor)
{
    const std::uint16_t opcode = LoadLe<std::uint16_t>(descriptor.data());
    const std::uint32_t count = std::max<std::uint32_t>(1, descriptor[3]);
    const auto depth = ubase_command_queue_registers_[0x08 / 4] << 3;
    const auto head = ubase_command_queue_registers_[0x14 / 4] % depth;
    const auto pending = (ubase_target_producer_ % depth + depth - head) % depth;
    if (count > pending) return RejectUbase(std::move(descriptor), 1, 22);
    // Official user_ctl diagnostics propagate failure before inspecting data.
    // No auxiliary CQE/AE hardware state is modeled: never fabricate success.
    if (opcode == 0x4213 || opcode == 0x4214)
        return RejectUbase(std::move(descriptor), count, 95);
    if (opcode == 0x7000) {
        HandleUbaseMailbox(std::move(descriptor), count);
        return;
    }
    if (opcode == 0xf00e) {
        HandleCtrlq(std::move(descriptor), count);
        return;
    }
    // Command-queue ownership is a hardware contract independent of whether
    // a particular command returns modeled data.  Queries with observable
    // results are populated by BuildUbaseResponse() (or the two inline cases
    // in FinishUbaseDescriptor); write-only control commands such as
    // NOTIFY_DRV_CAPS legitimately complete with an empty response.  Always
    // return ownership and advance Head here, matching the original in-process
    // device model and preventing an unchanged driver from waiting forever on
    // a command that the functional model has accepted.
    auto response = BuildUbaseResponse(opcode);
    if (descriptor[2] & (1U << 6)) { // UBASE_CMD_FLAG_GET_BD_NUM
        if (response.empty()) return RejectUbase(std::move(descriptor), count, 95);
        const auto blocks = static_cast<std::uint8_t>((response.size() + 39) / 32);
        return FinishUbaseDescriptor(std::move(descriptor), opcode, count, {blocks});
    }
    if (response.size() > 24 + (count - 1) * 32)
        return RejectUbase(std::move(descriptor), count, 90);
    FinishUbaseDescriptor(std::move(descriptor), opcode, count, std::move(response));
}

void
UdmaModel::HandleCtrlq(std::vector<std::uint8_t> descriptor,
                       std::uint32_t descriptor_count)
{
    constexpr std::size_t BaseLow = 0x00 / 4;
    constexpr std::size_t BaseHigh = 0x04 / 4;
    constexpr std::size_t Depth = 0x08 / 4;
    constexpr std::size_t Head = 0x14 / 4;
    const std::uint32_t depth = ubase_command_queue_registers_[Depth] << 3;
    const std::uint32_t head = ubase_command_queue_registers_[Head] % depth;
    const std::uint64_t base = ubase_command_queue_registers_[BaseLow] |
        (std::uint64_t(ubase_command_queue_registers_[BaseHigh]) << 32);
    auto request = std::make_shared<std::vector<std::uint8_t>>(
        descriptor.begin() + 8, descriptor.end());
    auto index = std::make_shared<std::uint32_t>(1);
    auto read_next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_next = read_next;
    *read_next = [this, descriptor = std::move(descriptor), descriptor_count,
                  depth, head, base, request, index, weak_next]() mutable {
        if (*index >= descriptor_count) {
            ApplyCtrlq(std::move(descriptor), descriptor_count,
                       std::move(*request));
            return;
        }
        const std::uint64_t address =
            base + std::uint64_t((head + *index) % depth) * 32;
        ++*index;
        ReadIoVirtual(address, 32,
            [this, request, read_next = weak_next.lock(), descriptor, descriptor_count]
            (bool ok, std::vector<std::uint8_t> continuation) mutable {
                if (!ok || continuation.size() != 32)
                    return RejectUbase(std::move(descriptor), descriptor_count, 5);
                request->insert(request->end(), continuation.begin(),
                                continuation.end());
                (*read_next)();
            });
    };
    (*read_next)();
}

std::uint16_t
UdmaModel::BuildCtrlqResponse(const std::vector<std::uint8_t>& request,
                              std::vector<std::uint8_t>& event)
{
    constexpr std::size_t Outer = 16;
    constexpr std::size_t Ctrl = 12;
    if (request.size() < Outer + Ctrl ||
        LoadLe<std::uint16_t>(request.data() + 4) != 3) return 22;
    // Linux errno values are part of the guest ABI, not the host errno ABI.
    const std::size_t input = LoadLe<std::uint16_t>(request.data() + 10);
    const std::size_t output = LoadLe<std::uint16_t>(request.data() + 12);
    if (input > request.size() - Outer - Ctrl) return 22;
    const std::uint8_t service = request[Outer + 1];
    const std::uint8_t opcode = request[Outer + 3];
    const bool query_sl = service == 4 && opcode == 2;
    const bool query_vl = service == 4 && opcode == 1;
    const bool init = service == 2 && opcode == 0x15;
    const bool query_seid = service == 2 && opcode == 0x01;
    const bool get_tp = service == 1 && opcode == 0x21;
    const bool activate = service == 1 && opcode == 0x22;
    const bool deactivate = service == 1 && opcode == 0x23;
    const bool remove = service == 1 && opcode == 0x13;
    if (!query_sl && !query_vl && !init && !query_seid && !get_tp &&
        !activate && !deactivate && !remove) return 95;
    const std::size_t min_input = get_tp ? 36 : (activate ? 24 : (deactivate ? 12 : (remove ? 4 : 0)));
    const std::size_t min_output = query_sl || query_vl ? 20 :
        (query_seid ? 100 : (get_tp ? 44 : (activate ? 8 : (remove ? 4 : 0))));
    if (input < min_input || output < min_output) return 22;
    const std::uint32_t crq_depth = ubase_command_queue_registers_[0x20 / 4] << 3;
    if (Outer + Ctrl + output + 8 >= std::uint64_t(crq_depth) * 32) return 90;
    event.assign(Outer + Ctrl + output, 0);
    std::memcpy(event.data(), request.data(), Outer + Ctrl);
    StoreLe<std::uint16_t>(event, 10, static_cast<std::uint16_t>(output));
    StoreLe<std::uint16_t>(event, 12, 0);
    event[14] = 1U << 1;
    std::uint8_t* response = event.data() + Outer + Ctrl;
    if (query_sl) {
        response[0] = response[4] = response[6] = 1;
        response[3] = 4;
    } else if (query_vl) {
        response[0] = 1;
    } else if (query_seid && output >= 100) {
        response[0] = 4;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint32_t eid = config_.endpoint_eid + i * 0x10000;
            std::uint8_t* entry = response + 4 + i * 24;
            StoreLe<std::uint32_t>(event, entry - event.data(), i);
            entry[4] = static_cast<std::uint8_t>(eid);
            entry[5] = static_cast<std::uint8_t>(eid >> 8);
            entry[6] = static_cast<std::uint8_t>(eid >> 16);
            StoreLe<std::uint32_t>(event, entry - event.data() + 20, 0x7fff);
        }
    } else if (get_tp && output >= 44 && request.size() >= Outer + Ctrl + 36) {
        if (tp_routes_.size() >= 1023) return 28;
        const std::uint8_t* cfg = request.data() + Outer + Ctrl;
        if (!(LoadLe<std::uint32_t>(cfg) & 0xfffffU) ||
            !(LoadLe<std::uint32_t>(cfg + 16) & 0xfffffU)) return 22;
        std::uint32_t id = next_tp_id_;
        for (std::uint32_t attempts = 0; attempts < 1023; ++attempts) {
            if (++next_tp_id_ >= 1024) next_tp_id_ = 1;
            if (!tp_routes_.count(id)) break;
            id = next_tp_id_;
        }
        std::uint32_t port = 0;
        bool found_port = false;
        for (std::uint32_t attempt = 0; attempt < config_.port_count; ++attempt) {
            const std::uint32_t candidate = config_.port_count ?
                next_tp_port_++ % config_.port_count : 0;
            if (candidate < link_up_.size() && link_up_[candidate]) {
                port = candidate; found_port = true; break;
            }
        }
        if (!found_port) return 100;
        TpRoute route{id, id, LoadLe<std::uint32_t>(cfg) & 0xfffffU,
                      LoadLe<std::uint32_t>(cfg + 16) & 0xfffffU,
                      port,
                      false};
        if (!route.local_eid || !route.remote_eid || tp_routes_.count(id))
            return 28;
        tp_routes_[id] = route;
        StoreLe<std::uint32_t>(event, response - event.data(), 1);
        StoreLe<std::uint32_t>(event, response - event.data() + 4,
                               id | (1U << 24));
        StoreLe<std::uint32_t>(event, response - event.data() + 8, id);
    } else if ((activate || deactivate) &&
               request.size() >= Outer + Ctrl + 8) {
        const std::uint8_t* cfg = request.data() + Outer + Ctrl;
        const std::uint32_t id = LoadLe<std::uint32_t>(cfg) & 0xffffffU;
        auto found = tp_routes_.find(id);
        if (found == tp_routes_.end()) return 2;
        if (activate && (found->second.port >= link_up_.size() || !link_up_[found->second.port]))
            return 100;
        found->second.active = activate;
        if (activate && output >= 8) std::memcpy(response, cfg, 8);
    } else if (remove) {
        const auto tpn = LoadLe<std::uint32_t>(request.data() + Outer + Ctrl) & 0xffffffU;
        auto found = tp_routes_.find(tpn); // current model allocates one TPN per TP ID
        if (found == tp_routes_.end()) return 2;
        if (found->second.active || !pending_rma_.empty()) return 16;
        tp_routes_.erase(found);
    }
    return 0;
}

void
UdmaModel::ApplyCtrlq(std::vector<std::uint8_t> descriptor,
                      std::uint32_t descriptor_count,
                      std::vector<std::uint8_t> request)
{
    std::vector<std::uint8_t> response;
    const auto status = BuildCtrlqResponse(request, response);
    if (status) {
        std::cerr << "udma-model: rejected CtrlQ request bytes="
                  << request.size() << '\n';
        return RejectUbase(std::move(descriptor), descriptor_count, status);
    }
    FinishUbaseDescriptor(std::move(descriptor), 0xf00e, descriptor_count, {},
        [this, response = std::move(response)]() mutable {
            EmitCtrlqResponse(std::move(response), [this](bool ok) {
                if (!ok) ++ubase_errors_;
            });
        });
}

void
UdmaModel::EmitCtrlqResponse(std::vector<std::uint8_t> event,
                             Completion completion)
{
    constexpr std::size_t BaseLow = 0x18 / 4;
    constexpr std::size_t BaseHigh = 0x1c / 4;
    constexpr std::size_t Depth = 0x20 / 4;
    constexpr std::size_t Tail = 0x24 / 4;
    const std::uint64_t base = ubase_command_queue_registers_[BaseLow] |
        (std::uint64_t(ubase_command_queue_registers_[BaseHigh]) << 32);
    const std::uint32_t depth = ubase_command_queue_registers_[Depth] << 3;
    if (!base || !depth) {
        std::cerr << "udma-model: invalid CRQ base/depth base=" << base
                  << " depth=" << depth << '\n';
        return completion(false);
    }
    const std::uint32_t count = static_cast<std::uint32_t>((event.size() + 39) / 32);
    auto writes = std::make_shared<std::vector<std::pair<std::uint64_t,
                                                         std::vector<std::uint8_t>>>>();
    std::uint32_t tail = ubase_command_queue_registers_[Tail] % depth;
    std::size_t offset = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::vector<std::uint8_t> desc(32, 0);
        std::size_t start = 0;
        if (i == 0) {
            desc[0] = 0x0e; desc[1] = 0xf0; desc[2] = 1U << 1;
            desc[3] = static_cast<std::uint8_t>(count); start = 8;
        }
        const std::size_t chunk = std::min(desc.size() - start,
                                           event.size() - offset);
        std::memcpy(desc.data() + start, event.data() + offset, chunk);
        offset += chunk;
        writes->push_back({base + std::uint64_t(tail) * 32, std::move(desc)});
        tail = (tail + 1) % depth;
    }
    auto cursor = std::make_shared<std::size_t>(0);
    auto done = std::make_shared<Completion>(std::move(completion));
    auto issue = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_issue = issue;
    *issue = [this, writes, cursor, weak_issue, tail, done]() mutable {
        if (*cursor == writes->size()) {
            ubase_command_queue_registers_[0x24 / 4] = tail;
            ubase_command_source_ |= 1U << 1;
            RaiseInterrupt(0, *done);
            return;
        }
        auto& write = writes->at((*cursor)++);
        WriteIoVirtual(write.first, std::move(write.second),
            [issue = weak_issue.lock(), done](bool ok) {
                if (!ok) {
                    std::cerr << "udma-model: CRQ IOVA write failed\n";
                    (*done)(false);
                    return;
                }
                (*issue)();
            });
    };
    (*issue)();
}


void
UdmaModel::RejectUbase(std::vector<std::uint8_t> descriptor,
                       std::uint32_t descriptor_count, std::uint16_t status)
{
    ++ubase_errors_;
    const auto opcode = LoadLe<std::uint16_t>(descriptor.data());
    FinishUbaseDescriptor(std::move(descriptor), opcode, descriptor_count, {}, {}, status);
}

void
UdmaModel::FinishUbaseDescriptor(std::vector<std::uint8_t> descriptor,
                                 std::uint16_t opcode,
                                 std::uint32_t descriptor_count,
                                 std::vector<std::uint8_t> response,
                                 std::function<void()> after_completion,
                                 std::uint16_t status)
{
    constexpr std::size_t BaseLow = 0x00 / 4;
    constexpr std::size_t BaseHigh = 0x04 / 4;
    constexpr std::size_t Depth = 0x08 / 4;
    constexpr std::size_t Head = 0x14 / 4;
    const std::uint32_t depth = ubase_command_queue_registers_[Depth] << 3;
    const std::uint32_t head = ubase_command_queue_registers_[Head] % depth;
    const std::uint64_t base = ubase_command_queue_registers_[BaseLow] |
        (std::uint64_t(ubase_command_queue_registers_[BaseHigh]) << 32);
    const std::size_t first = std::min<std::size_t>(response.size(), 24);
    if (first) std::memcpy(descriptor.data() + 8, response.data(), first);
    descriptor[2] |= 1U << 1;
    StoreLe<std::uint16_t>(descriptor, 4, status);
    if (opcode == 0x0001) StoreLe<std::uint32_t>(descriptor, 8, 0x01000000);
    if (opcode == 0x7001) StoreLe<std::uint32_t>(descriptor, 8, 1);

    struct WriteRecord {
        std::uint64_t address;
        std::vector<std::uint8_t> bytes;
    };
    auto writes = std::make_shared<std::vector<WriteRecord>>();
    std::size_t response_offset = first;
    for (std::uint32_t index = 1;
         index < descriptor_count && response_offset < response.size(); ++index) {
        std::vector<std::uint8_t> continuation(32, 0);
        const std::size_t chunk = std::min<std::size_t>(
            continuation.size(), response.size() - response_offset);
        std::memcpy(continuation.data(), response.data() + response_offset, chunk);
        writes->push_back({base + std::uint64_t((head + index) % depth) * 32,
                           std::move(continuation)});
        response_offset += chunk;
    }
    writes->push_back({base + std::uint64_t(head) * 32, std::move(descriptor)});
    auto cursor = std::make_shared<std::size_t>(0);
    auto issue = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_issue = issue;
    *issue = [this, writes, cursor, weak_issue, head, descriptor_count, depth,
              after_completion = std::move(after_completion)]() mutable {
        if (*cursor == writes->size()) {
            constexpr std::size_t Tail = 0x10 / 4;
            constexpr std::size_t Head = 0x14 / 4;
            ubase_command_queue_registers_[Head] =
                (head + descriptor_count) % depth;
            ubase_command_queue_registers_[Tail] =
                ubase_target_producer_ % depth;
            if (after_completion) after_completion();
            ProcessNextUbase();
            return;
        }
        WriteRecord& record = writes->at((*cursor)++);
        WriteIoVirtual(record.address, std::move(record.bytes),
            [this, issue = weak_issue.lock()](bool ok) {
                if (!ok) return FailUbase();
                (*issue)();
            });
    };
    (*issue)();
}

} // namespace ubsim::device
