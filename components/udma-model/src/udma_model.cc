// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"
#include "openurma/udma_abi.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace openurma::device {

namespace {
constexpr std::uint8_t kOpcodeSend = 1;
constexpr std::uint32_t kStatusReady = 1U;
constexpr std::uint32_t kStatusSuccess = 0U;
constexpr std::uint32_t kStatusDmaError = 1U;
constexpr std::uint32_t kInterruptVector = 0U;

constexpr std::uint64_t kUbiosRootOffset = 0x10000;
constexpr std::uint64_t kUbiosUbcOffset = 0x11000;
constexpr std::uint64_t kUbiosUmmuOffset = 0x11c00;
constexpr std::uint64_t kUbiosMessageQueueOffset = 0x12000;
constexpr std::uint64_t kUbaseCommandSourceOffset = 0x00318004;
constexpr std::uint64_t kUbaseCommandQueueOffset = 0x00318400;
constexpr std::uint64_t kUmmuOffset = 0x00f00000;
constexpr std::uint64_t kUmmuBytes = 0x5000;
constexpr std::uint32_t kUmmuCr0 = 0x30;
constexpr std::uint32_t kUmmuCr0Ack = 0x34;
constexpr std::uint32_t kUmmuGbpa = 0x50;
constexpr std::uint32_t kUmmuCommandQueueOffset = 0x100;
constexpr std::uint32_t kUmmuCommandQueueStride = 0x10;
constexpr std::uint32_t kUmmuCommandQueueProducer = 0x8;
constexpr std::uint32_t kUmmuCommandQueueConsumer = 0xc;
constexpr std::uint32_t kUmmuCommandQueueEnable = 1U << 31;

template <typename T, typename Container>
void StoreLe(Container& bytes, std::size_t offset, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

template <typename T>
T LoadLe(const std::uint8_t* bytes)
{
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(bytes[i]) << (8 * i);
    return value;
}

std::vector<std::uint8_t> BuildUbiosRoot(std::uint64_t base)
{
    std::vector<std::uint8_t> table(56, 0);
    std::memcpy(table.data(), "ubios", 5);
    StoreLe<std::uint32_t>(table, 16, table.size());
    table[20] = 1;
    StoreLe<std::uint16_t>(table, 32, 2);
    StoreLe<std::uint64_t>(table, 40, base + kUbiosUmmuOffset);
    StoreLe<std::uint64_t>(table, 48, base + kUbiosUbcOffset);
    return table;
}

std::vector<std::uint8_t> BuildUbiosUbc(std::uint64_t base)
{
    std::vector<std::uint8_t> table(56 + 384, 0);
    std::memcpy(table.data(), "ubc", 3);
    StoreLe<std::uint32_t>(table, 16, table.size());
    table[20] = 1;
    StoreLe<std::uint32_t>(table, 32, 1);
    StoreLe<std::uint32_t>(table, 36, 0xffff);
    StoreLe<std::uint32_t>(table, 40, 1);
    StoreLe<std::uint32_t>(table, 44, 0xffff);
    StoreLe<std::uint16_t>(table, 54, 1);
    constexpr std::size_t node = 56;
    StoreLe<std::uint32_t>(table, node + 0, 160);
    StoreLe<std::uint32_t>(table, node + 4, 255);
    StoreLe<std::uint64_t>(table, node + 8, base + 0x20000);
    StoreLe<std::uint64_t>(table, node + 16, 0x00e00000);
    table[node + 24] = 48;
    table[node + 25] = 1;
    StoreLe<std::uint64_t>(table, node + 32, base + kUbiosMessageQueueOffset);
    StoreLe<std::uint64_t>(table, node + 40, 0x1000);
    StoreLe<std::uint16_t>(table, node + 48, 64);
    StoreLe<std::uint16_t>(table, node + 50, 104);
    StoreLe<std::uint64_t>(table, node + 112, 1);
    StoreLe<std::uint64_t>(table, node + 120, 0xcc08000000000001ULL);
    return table;
}

std::vector<std::uint8_t> BuildUbiosUmmu(std::uint64_t base)
{
    std::vector<std::uint8_t> table(40 + 160, 0);
    std::memcpy(table.data(), "ummu", 4);
    StoreLe<std::uint32_t>(table, 16, table.size());
    table[20] = 1;
    StoreLe<std::uint16_t>(table, 32, 1);
    constexpr std::size_t node = 40;
    StoreLe<std::uint64_t>(table, node + 0, base + kUmmuOffset);
    StoreLe<std::uint64_t>(table, node + 8, kUmmuBytes);
    StoreLe<std::uint16_t>(table, node + 20, 0xffff);
    StoreLe<std::uint32_t>(table, node + 44, 1);
    StoreLe<std::uint32_t>(table, node + 48, 2048);
    return table;
}

bool ReadTable(std::uint64_t base, std::uint64_t offset,
               std::uint32_t length, std::uint64_t& value)
{
    std::vector<std::uint8_t> table;
    std::uint64_t table_offset{};
    if (offset >= kUbiosRootOffset && offset < kUbiosRootOffset + 56) {
        table = BuildUbiosRoot(base);
        table_offset = kUbiosRootOffset;
    } else if (offset >= kUbiosUbcOffset && offset < kUbiosUbcOffset + 440) {
        table = BuildUbiosUbc(base);
        table_offset = kUbiosUbcOffset;
    } else if (offset >= kUbiosUmmuOffset && offset < kUbiosUmmuOffset + 200) {
        table = BuildUbiosUmmu(base);
        table_offset = kUbiosUmmuOffset;
    } else {
        return false;
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - length ||
        offset + length > table_offset + table.size()) return false;
    value = 0;
    for (std::uint32_t i = 0; i < length; ++i)
        value |= std::uint64_t(table[offset - table_offset + i]) << (8 * i);
    return true;
}
}

UdmaModel::UdmaModel(HostInterface& host, NetworkInterface& network)
    : UdmaModel(host, network, Config{})
{
}

UdmaModel::UdmaModel(HostInterface& host, NetworkInterface& network,
                     Config config)
    : host_(host), network_(network), config_(config)
{
    link_up_.assign(config_.port_count, true);
    StoreLe<std::uint32_t>(ummu_registers_, 0x10, 0x00000b08);
    StoreLe<std::uint32_t>(ummu_registers_, 0x14, 0x00042208);
    StoreLe<std::uint32_t>(ummu_registers_, 0x18, 0x00009056);
    StoreLe<std::uint32_t>(ummu_registers_, 0x1c, 0x00000010);
    StoreLe<std::uint32_t>(ummu_registers_, 0x24, 0x00000011);
}

std::uint64_t
UdmaModel::ReadMmio(std::uint64_t offset, std::uint32_t length) const
{
    std::uint64_t value{};
    ReadMmio(offset, length, value);
    return value;
}

bool
UdmaModel::ReadMmio(std::uint64_t offset, std::uint32_t length,
                    std::uint64_t& value) const
{
    value = 0;
    if (length == 0 || length > sizeof(value) ||
        offset > std::numeric_limits<std::uint64_t>::max() - length)
        return false;
    if (config_.extraction_test_abi) {
        if (length != sizeof(value)) return false;
        if (offset == kRegisterIdentity) value = kIdentity;
        else if (offset == kRegisterStatus) value = kStatusReady;
        return true;
    }
    if (ReadTable(config_.mmio_base, offset, length, value)) return true;
    const auto read_bytes = [&value, length](const std::uint8_t* bytes) {
        for (std::uint32_t i = 0; i < length; ++i)
            value |= std::uint64_t(bytes[i]) << (8 * i);
    };
    if (offset >= kUmmuOffset && offset + length <= kUmmuOffset + kUmmuBytes) {
        read_bytes(ummu_registers_.data() + offset - kUmmuOffset);
        return true;
    }
    if (offset >= kUbiosMessageQueueOffset &&
        offset + length <= kUbiosMessageQueueOffset + 0x120 &&
        length <= 4 && ((offset - kUbiosMessageQueueOffset) % 4) + length <= 4) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            ubios_message_queue_registers_.data());
        read_bytes(bytes + offset - kUbiosMessageQueueOffset);
        return true;
    }
    if (offset >= kUbaseCommandQueueOffset &&
        offset + length <= kUbaseCommandQueueOffset + 0x2c &&
        length <= 4 && ((offset - kUbaseCommandQueueOffset) % 4) + length <= 4) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            ubase_command_queue_registers_.data());
        read_bytes(bytes + offset - kUbaseCommandQueueOffset);
        return true;
    }
    if (offset == kUbaseCommandSourceOffset && length == 4) {
        value = ubase_command_source_;
        return true;
    }
    // Architected sparse apertures read as zero until a resource owns them.
    return offset + length <= kOfficialApertureBytes;
}

bool
UdmaModel::WriteMmio(std::uint64_t offset, std::uint32_t length,
                     std::uint64_t value)
{
    if (config_.extraction_test_abi) {
        if (offset != kRegisterDoorbell || length != sizeof(value) || value == 0)
            return false;
        FetchDescriptor(value);
        return true;
    }
    if (length == 0 || length > sizeof(value) ||
        offset > std::numeric_limits<std::uint64_t>::max() - length)
        return false;
    const auto write_bytes = [value, length](std::uint8_t* destination) {
        for (std::uint32_t i = 0; i < length; ++i)
            destination[i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    if (offset >= kUmmuOffset && offset + length <= kUmmuOffset + kUmmuBytes) {
        const std::uint32_t reg = static_cast<std::uint32_t>(offset - kUmmuOffset);
        write_bytes(ummu_registers_.data() + reg);
        if (reg <= kUmmuCr0 && reg + length >= kUmmuCr0 + 4)
            std::memcpy(ummu_registers_.data() + kUmmuCr0Ack,
                        ummu_registers_.data() + kUmmuCr0, 4);
        if (reg <= kUmmuGbpa && reg + length >= kUmmuGbpa + 4) {
            auto gbpa = LoadLe<std::uint32_t>(ummu_registers_.data() + kUmmuGbpa);
            gbpa &= ~(1U << 31);
            StoreLe<std::uint32_t>(ummu_registers_, kUmmuGbpa, gbpa);
        }
        if (reg >= kUmmuCommandQueueOffset && reg < kUmmuCommandQueueOffset + 0x1000) {
            const std::uint32_t local = (reg - kUmmuCommandQueueOffset) %
                                        kUmmuCommandQueueStride;
            if (local == kUmmuCommandQueueProducer && length == 4) {
                const auto producer = LoadLe<std::uint32_t>(
                    ummu_registers_.data() + reg);
                const std::uint32_t queue = reg - local;
                StoreLe<std::uint32_t>(ummu_registers_,
                    queue + kUmmuCommandQueueConsumer,
                    (producer & ~kUmmuCommandQueueEnable) |
                    (producer & kUmmuCommandQueueEnable));
            }
        }
        return true;
    }
    if (offset >= kUbiosMessageQueueOffset &&
        offset + length <= kUbiosMessageQueueOffset + 0x120) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(
            ubios_message_queue_registers_.data());
        write_bytes(bytes + offset - kUbiosMessageQueueOffset);
        const std::uint64_t local = offset - kUbiosMessageQueueOffset;
        if (local == 0 && length == 4) {
            ubios_message_queue_registers_[0x0c / 4] = 0;
            ubios_message_queue_registers_[0x48 / 4] = 0;
            ubios_message_queue_registers_[0x78 / 4] = 0;
        }
        if (local == 0x08 && length == 4)
            KickUbios(static_cast<std::uint32_t>(value));
        return true;
    }
    if (offset >= kUbaseCommandQueueOffset &&
        offset + length <= kUbaseCommandQueueOffset + 0x2c) {
        auto* bytes = reinterpret_cast<std::uint8_t*>(
            ubase_command_queue_registers_.data());
        write_bytes(bytes + offset - kUbaseCommandQueueOffset);
        const std::uint64_t local = offset - kUbaseCommandQueueOffset;
        if ((local == 0 || local == 4) && length == 4) {
            if (local == 0) ubase_command_queue_registers_[0x14 / 4] = 0;
        }
        if (local == 0x10 && length == 4)
            KickUbase(static_cast<std::uint32_t>(value));
        return true;
    }
    if (offset == kUbaseCommandSourceOffset && length == 4) {
        ubase_command_source_ &= ~static_cast<std::uint32_t>(value);
        return true;
    }
    if (HandleJettyMmio(offset, length, value)) return true;
    return offset + length <= kOfficialApertureBytes;
}

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
    host_.DmaReadIoVirtual(base + std::uint64_t(head) * 32, 32,
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
        response.resize(312, 0);
        StoreLe<std::uint16_t>(response, 26, 1);
        StoreLe<std::uint16_t>(response, 28, 1);
        StoreLe<std::uint16_t>(response, 30, 1);
        StoreLe<std::uint16_t>(response, 32, 64);
        StoreLe<std::uint16_t>(response, 34, 64);
        StoreLe<std::uint16_t>(response, 36, 64);
        StoreLe<std::uint32_t>(response, 40, 512);
        StoreLe<std::uint32_t>(response, 44, 4096);
        StoreLe<std::uint32_t>(response, 48, 1024);
        StoreLe<std::uint32_t>(response, 56, 4096);
        StoreLe<std::uint32_t>(response, 60, 1024);
        StoreLe<std::uint32_t>(response, 68, 1024);
        StoreLe<std::uint32_t>(response, 84, 1024);
        StoreLe<std::uint32_t>(response, 92, 4096);
        StoreLe<std::uint32_t>(response, 192, 1);
        StoreLe<std::uint32_t>(response, 224, 1024);
        response[239] = 1;
        StoreLe<std::uint32_t>(response, 256, 128);
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
        StoreLe<std::uint16_t>(response, 58, 128);
        StoreLe<std::uint16_t>(response, 76, 128);
        StoreLe<std::uint16_t>(response, 78, 896);
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
    if (opcode == 0x7000) {
        HandleUbaseMailbox(std::move(descriptor), count);
        return;
    }
    if (opcode == 0xf00e) {
        HandleCtrlq(std::move(descriptor), count);
        return;
    }
    if (opcode != 0x0030 && opcode != 0x0002 && opcode != 0x6200 &&
        opcode != 0x0001 && opcode != 0x7001)
        return FailUbase();
    FinishUbaseDescriptor(std::move(descriptor), opcode, count,
                          BuildUbaseResponse(opcode));
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
    *read_next = [this, descriptor = std::move(descriptor), descriptor_count,
                  depth, head, base, request, index, read_next]() mutable {
        if (*index >= descriptor_count) {
            ApplyCtrlq(std::move(descriptor), descriptor_count,
                       std::move(*request));
            return;
        }
        const std::uint64_t address =
            base + std::uint64_t((head + *index) % depth) * 32;
        ++*index;
        host_.DmaReadIoVirtual(address, 32,
            [this, request, read_next](bool ok,
                                      std::vector<std::uint8_t> continuation) {
                if (!ok || continuation.size() != 32) return FailUbase();
                request->insert(request->end(), continuation.begin(),
                                continuation.end());
                (*read_next)();
            });
    };
    (*read_next)();
}

bool
UdmaModel::BuildCtrlqResponse(const std::vector<std::uint8_t>& request,
                              std::vector<std::uint8_t>& event)
{
    constexpr std::size_t Outer = 16;
    constexpr std::size_t Ctrl = 12;
    if (request.size() < Outer + Ctrl ||
        LoadLe<std::uint16_t>(request.data() + 4) != 3) return false;
    const std::uint8_t service = request[Outer + 1];
    const std::uint8_t opcode = request[Outer + 3];
    const bool query_sl = service == 4 && opcode == 2;
    const bool query_vl = service == 4 && opcode == 1;
    const bool init = service == 2 && opcode == 0x15;
    const bool query_seid = service == 2 && opcode == 0x01;
    const bool get_tp = service == 1 && opcode == 0x21;
    const bool activate = service == 1 && opcode == 0x22;
    const bool deactivate = service == 1 && opcode == 0x23;
    if (!query_sl && !query_vl && !init && !query_seid && !get_tp &&
        !activate && !deactivate) return false;
    const std::size_t output = query_sl || query_vl ? 20 :
        LoadLe<std::uint16_t>(request.data() + 12);
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
        std::uint32_t id = next_tp_id_;
        for (std::uint32_t attempts = 0; attempts < 1023; ++attempts) {
            if (++next_tp_id_ >= 1024) next_tp_id_ = 1;
            if (!tp_routes_.count(id)) break;
            id = next_tp_id_;
        }
        const std::uint8_t* cfg = request.data() + Outer + Ctrl;
        std::uint32_t port = 0;
        bool found_port = false;
        for (std::uint32_t attempt = 0; attempt < config_.port_count; ++attempt) {
            const std::uint32_t candidate = config_.port_count ?
                next_tp_port_++ % config_.port_count : 0;
            if (candidate < link_up_.size() && link_up_[candidate]) {
                port = candidate; found_port = true; break;
            }
        }
        if (!found_port) return false;
        TpRoute route{id, id, LoadLe<std::uint32_t>(cfg) & 0xfffffU,
                      LoadLe<std::uint32_t>(cfg + 16) & 0xfffffU,
                      port,
                      false};
        if (!route.local_eid || !route.remote_eid || tp_routes_.count(id))
            return false;
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
        if (found == tp_routes_.end()) return false;
        found->second.active = activate;
        if (activate && output >= 8) std::memcpy(response, cfg, 8);
    }
    return true;
}

void
UdmaModel::ApplyCtrlq(std::vector<std::uint8_t> descriptor,
                      std::uint32_t descriptor_count,
                      std::vector<std::uint8_t> request)
{
    std::vector<std::uint8_t> response;
    if (!BuildCtrlqResponse(request, response)) return FailUbase();
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
    if (!base || !depth) return completion(false);
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
    *issue = [this, writes, cursor, issue, tail, done]() mutable {
        if (*cursor == writes->size()) {
            ubase_command_queue_registers_[0x24 / 4] = tail;
            ubase_command_source_ |= 1U << 1;
            RaiseInterrupt(0, *done);
            return;
        }
        auto& write = writes->at((*cursor)++);
        host_.DmaWriteIoVirtual(write.first, std::move(write.second),
            [issue, done](bool ok) {
                if (!ok) {
                    (*done)(false);
                    return;
                }
                (*issue)();
            });
    };
    (*issue)();
}

void
UdmaModel::HandleUbaseMailbox(std::vector<std::uint8_t> descriptor,
                              std::uint32_t descriptor_count)
{
    const std::uint8_t command = descriptor[16];
    const std::uint64_t context_iova =
        LoadLe<std::uint32_t>(descriptor.data() + 8) |
        (std::uint64_t(LoadLe<std::uint32_t>(descriptor.data() + 12)) << 32);
    constexpr std::uint8_t QueryJfsContext = 0x06;
    constexpr std::uint8_t DestroyJfsContext = 0x07;
    constexpr std::uint8_t DestroyJfcContext = 0x27;
    constexpr std::uint8_t DestroyJfrContext = 0x57;
    const std::uint32_t tag =
        LoadLe<std::uint32_t>(descriptor.data() + 16) >> 8;

    if (command == DestroyJfsContext) jetty_contexts_.erase(tag);
    if (command == DestroyJfcContext) jfc_contexts_.erase(tag);
    if (command == DestroyJfrContext) jfr_contexts_.erase(tag);

    if (command == QueryJfsContext && context_iova) {
        std::vector<std::uint8_t> context(128, 0);
        StoreLe<std::uint32_t>(context, 0, 1U << 16);
        StoreLe<std::uint32_t>(context, 26 * sizeof(std::uint32_t), 1U << 26);
        host_.DmaWriteIoVirtual(context_iova, std::move(context),
            [this, descriptor = std::move(descriptor), descriptor_count](bool ok) mutable {
                if (!ok) return FailUbase();
                ApplyUbaseMailbox(std::move(descriptor), descriptor_count, {});
            });
        return;
    }

    const bool creates_context = command == 0x34 || command == 0x44 ||
        command == 0x04 || command == 0x24 || command == 0x54;
    if (creates_context && context_iova) {
        const std::size_t bytes = (command == 0x34 || command == 0x44) ? 64 : 128;
        host_.DmaReadIoVirtual(context_iova, bytes,
            [this, descriptor = std::move(descriptor), descriptor_count, bytes]
            (bool ok, std::vector<std::uint8_t> context) mutable {
                if (!ok || context.size() != bytes) return FailUbase();
                ApplyUbaseMailbox(std::move(descriptor), descriptor_count,
                                  std::move(context));
            });
        return;
    }
    ApplyUbaseMailbox(std::move(descriptor), descriptor_count, {});
}

void
UdmaModel::ApplyUbaseMailbox(std::vector<std::uint8_t> descriptor,
                             std::uint32_t descriptor_count,
                             std::vector<std::uint8_t> context)
{
    const std::uint8_t command = descriptor[16];
    const std::uint32_t tag =
        LoadLe<std::uint32_t>(descriptor.data() + 16) >> 8;
    const std::uint32_t control =
        LoadLe<std::uint32_t>(descriptor.data() + 20);
    const std::uint16_t sequence = static_cast<std::uint16_t>(control);
    const bool event_enabled = (control & (1U << 16)) != 0;
    const auto dw = [&context](std::size_t index) {
        return LoadLe<std::uint32_t>(context.data() + index * 4);
    };

    if ((command == 0x34 || command == 0x44) && context.size() >= 64) {
        const std::uint32_t shift = dw(1) & 0x1f;
        if (shift >= 26) return FailUbase();
        const std::uint64_t iova =
            ((std::uint64_t(dw(3)) << 20) | (dw(2) >> 12)) << 12;
        if (!iova) return FailUbase();
        if (command == 0x34) {
            aeq_iova_ = iova;
            aeq_depth_ = 1U << (shift + 6);
            aeq_producer_ = 0;
        } else {
            ceq_iova_ = iova;
            ceq_depth_ = 1U << (shift + 6);
            ceq_producer_ = 0;
        }
    } else if (command == 0x24 && context.size() >= 128) {
        const std::uint32_t shift = ((dw(0) >> 4) & 0xfU) + 6U;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(1)) << 20) | (dw(0) >> 12)) << 12;
        queue.index_iova =
            ((std::uint64_t(dw(7) & 0x03ffffffU) << 32) | dw(6)) << 6;
        queue.depth = shift < 31 ? 1U << shift : 0;
        queue.token = dw(2) & 0x000fffffU;
        queue.moderation_count = dw(4) >> 22;
        if (!queue.moderation_count) queue.moderation_count = 1;
        queue.moderation_period = (dw(5) >> 22) & 0x7U;
        if (!queue.queue_iova || !queue.index_iova || !queue.depth)
            return FailUbase();
        jfc_contexts_[tag] = queue;
    } else if (command == 0x54 && context.size() >= 128) {
        const std::uint32_t rqe_shift = (dw(0) >> 8) & 0xfU;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(2)) << 20) | (dw(1) >> 12)) << 12;
        queue.index_iova =
            ((std::uint64_t(dw(9) & 0x000fffffU) << 32) | dw(8)) << 12;
        queue.completion_queue = ((dw(10) & 0xffU) << 12) | (dw(9) >> 20);
        queue.producer_iova =
            ((std::uint64_t(dw(12) & 3U) << 56) |
             (std::uint64_t(dw(11)) << 24) | (dw(10) >> 8)) << 6;
        queue.depth = rqe_shift < 31 ? 1U << rqe_shift : 0;
        const std::uint32_t sge_shift = (dw(0) >> 4) & 0x7U;
        queue.max_sge = 1U << sge_shift;
        queue.entry_stride = queue.max_sge * abi::kSgeBytes;
        queue.token = ((dw(1) & 0x3fU) << 14) | ((dw(0) >> 18) & 0x3fffU);
        queue.payload_token = (dw(3) >> 2) & 0x000fffffU;
        if (!queue.queue_iova || !queue.index_iova ||
            !queue.producer_iova || !queue.depth) return FailUbase();
        jfr_contexts_[tag] = queue;
    } else if (command == 0x04 && context.size() >= 128) {
        const std::uint32_t sq_shift = (dw(0) >> 8) & 0xfU;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(2)) << 20) | (dw(1) >> 12)) << 12;
        queue.depth = sq_shift < 31 ? 1U << sq_shift : 0;
        queue.completion_queue = dw(4) & 0x000fffffU;
        queue.token = ((dw(1) & 0xffU) << 12) | ((dw(0) >> 20) & 0xfffU);
        queue.payload_token = (dw(9) >> 2) & 0x000fffffU;
        queue.receive_queue = ((dw(5) & 0xffU) << 12) | (dw(4) >> 20);
        queue.receive_completion_queue = dw(5) >> 12;
        queue.eid_index = dw(6) & 0x3ffU;
        queue.user_queue = std::uint64_t(dw(7)) | (std::uint64_t(dw(8)) << 32);
        queue.device_page_offset = abi::kUdmaMemoryOffset +
            abi::kHardwarePageBytes + std::uint64_t(tag) * abi::kHardwarePageBytes;
        queue.is_jetty = ((dw(0) >> 19) & 1U) != 0;
        if (!queue.queue_iova || !queue.depth) return FailUbase();
        jetty_contexts_[tag] = queue;
    }

    auto after = event_enabled ? std::function<void()>([this, sequence]() {
        EmitMailboxEvent(sequence, [this](bool ok) {
            if (!ok) ++ubase_errors_;
        });
    }) : std::function<void()>{};
    FinishUbaseDescriptor(std::move(descriptor), 0x7000, descriptor_count, {},
                          std::move(after));
}

void
UdmaModel::EmitMailboxEvent(std::uint16_t sequence, Completion completion)
{
    if (!aeq_iova_ || !aeq_depth_ || (aeq_depth_ & (aeq_depth_ - 1))) {
        completion(false);
        return;
    }
    std::vector<std::uint8_t> event(64, 0);
    std::uint32_t header = 0x13;
    if (!(aeq_producer_ & aeq_depth_)) header |= 1U << 31;
    StoreLe<std::uint32_t>(event, 0, header);
    event[12] = static_cast<std::uint8_t>(sequence);
    event[13] = static_cast<std::uint8_t>(sequence >> 8);
    const std::uint32_t slot = aeq_producer_ & (aeq_depth_ - 1);
    host_.DmaWriteIoVirtual(aeq_iova_ + std::uint64_t(slot) * 64,
                            std::move(event),
        [this, completion = std::move(completion)](bool ok) mutable {
            if (ok) {
                ++aeq_producer_;
                ubase_command_source_ |= 1U << 1;
                RaiseInterrupt(1, std::move(completion));
                return;
            }
            completion(false);
        });
}

bool
UdmaModel::HandleJettyMmio(std::uint64_t offset, std::uint32_t length,
                           std::uint64_t value)
{
    if (!length || length > 8) return false;
    for (auto& [id, jetty] : jetty_contexts_) {
        if (offset >= jetty.device_page_offset &&
            offset + length <= jetty.device_page_offset + abi::kWqebbBytes) {
            const std::uint32_t begin =
                static_cast<std::uint32_t>(offset - jetty.device_page_offset);
            for (std::uint32_t i = 0; i < length; ++i) {
                jetty.direct_wqe[begin + i] =
                    static_cast<std::uint8_t>(value >> (8 * i));
                jetty.direct_valid |= std::uint64_t{1} << (begin + i);
            }
            if (jetty.direct_valid == ~std::uint64_t{0}) {
                jetty.direct_valid = 0;
                jetty.direct_pending = true;
                const std::uint32_t low = abi::Load32(jetty.direct_wqe.data()) & 0xffffU;
                std::uint32_t producer =
                    (static_cast<std::uint32_t>(jetty.consumer) & ~0xffffU) | low;
                if (producer < jetty.consumer) producer += 0x10000U;
                ProcessSq(id, producer,
                    std::vector<std::uint8_t>(jetty.direct_wqe.begin(),
                                              jetty.direct_wqe.end()));
            }
            return true;
        }
        if (offset == jetty.device_page_offset + abi::kDoorbellOffset &&
            length == 4) {
            ProcessSq(id, static_cast<std::uint32_t>(value));
            return true;
        }
    }
    return false;
}

void
UdmaModel::ProcessSq(std::uint32_t jetty_id, std::uint32_t producer,
                     std::vector<std::uint8_t> direct)
{
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    QueueContext& jetty = found->second;
    jetty.target_producer = producer;
    if (!direct.empty()) {
        if (direct.size() != abi::kWqebbBytes) return;
        std::copy(direct.begin(), direct.end(), jetty.direct_wqe.begin());
        jetty.direct_pending = true;
    }
    if (jetty.busy) return;
    if (jetty.consumer == jetty.target_producer) {
        jetty.direct_pending = false;
        return;
    }
    const std::uint32_t outstanding =
        jetty.target_producer - static_cast<std::uint32_t>(jetty.consumer);
    if (!jetty.depth || outstanding > jetty.depth) return;
    jetty.busy = true;
    if (jetty.direct_pending && jetty.consumer + 1 == jetty.target_producer) {
        jetty.direct_pending = false;
        HandleSqWqe(jetty_id, producer,
            std::vector<std::uint8_t>(jetty.direct_wqe.begin(),
                                      jetty.direct_wqe.end()));
        return;
    }
    const std::uint64_t address = jetty.queue_iova +
        (jetty.consumer & (jetty.depth - 1U)) * abi::kWqebbBytes;
    ReadToken(jetty.token, address, abi::kWqebbBytes,
        [this, jetty_id, producer](bool ok, std::vector<std::uint8_t> raw) {
            if (!ok || raw.size() != abi::kWqebbBytes) {
                auto found = jetty_contexts_.find(jetty_id);
                if (found != jetty_contexts_.end()) found->second.busy = false;
                ++ubase_errors_;
                return;
            }
            HandleSqWqe(jetty_id, producer, std::move(raw));
        });
}

void
UdmaModel::HandleSqWqe(std::uint32_t jetty_id, std::uint32_t producer,
                       std::vector<std::uint8_t> raw)
{
    auto fail = [this, jetty_id]() {
        auto found = jetty_contexts_.find(jetty_id);
        if (found != jetty_contexts_.end()) found->second.busy = false;
        ++ubase_errors_;
    };
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end() || raw.size() != abi::kWqebbBytes)
        return fail();
    std::array<std::uint8_t, abi::kWqebbBytes> bytes{};
    std::copy(raw.begin(), raw.end(), bytes.begin());
    const abi::SqWqe wqe(bytes);
    QueueContext& jetty = found->second;
    const bool expected_owner = ((jetty.consumer / jetty.depth) & 1U) == 0;
    if (wqe.owner() != expected_owner || wqe.wqebb_count() != 1 ||
        (wqe.opcode() != 0 && wqe.opcode() != 1 &&
         wqe.opcode() != 3 && wqe.opcode() != 6)) return fail();
    const auto route = tp_routes_.find(wqe.tpn());
    if (route == tp_routes_.end() || !route->second.active ||
        route->second.port >= link_up_.size() ||
        !link_up_[route->second.port]) return fail();
    if (wqe.opcode() == 6) {
        if (wqe.inline_payload() || wqe.sge_count() != 1) return fail();
        const abi::Sge local = wqe.first_sge();
        if (!local.address || !local.length || !wqe.remote_address()) return fail();
        const std::uint64_t request_id = next_rma_request_++;
        const std::uint16_t completed_index = static_cast<std::uint16_t>(
            jetty.consumer & (jetty.depth - 1U));
        pending_rma_[request_id] = PendingRma{
            jetty_id, producer, completed_index, wqe.opcode(), local.length,
            0, wqe.completion(), jetty.payload_token, local.address};
        Frame frame{};
        frame.sequence = next_sequence_++;
        frame.source_eid = config_.endpoint_eid + jetty.eid_index * 0x10000U;
        frame.destination_eid = route->second.remote_eid;
        frame.source_port = static_cast<std::uint16_t>(route->second.port);
        frame.destination_port = UINT16_MAX;
        frame.operation = Frame::Operation::ReadRequest;
        frame.source_jetty = jetty_id;
        frame.destination_jetty = wqe.remote_jetty();
        frame.tpn = wqe.tpn();
        frame.segment = wqe.remote_segment();
        frame.remote_address = wqe.remote_address();
        frame.request_id = request_id;
        frame.transfer_length = local.length;
        network_.Send(std::move(frame), [this, request_id, jetty_id](bool ok) {
            if (ok) return;
            pending_rma_.erase(request_id);
            auto found = jetty_contexts_.find(jetty_id);
            if (found != jetty_contexts_.end()) found->second.busy = false;
            ++ubase_errors_;
        });
        return;
    }
    if (wqe.inline_payload()) {
        if (wqe.inline_length() > 16) return fail();
        SubmitSqPayload(jetty_id, producer, std::move(raw),
            std::vector<std::uint8_t>(bytes.begin() + 48,
                                      bytes.begin() + 48 + wqe.inline_length()));
        return;
    }
    if (wqe.sge_count() != 1) return fail();
    const abi::Sge sge = wqe.first_sge();
    if (!sge.address || !sge.length) return fail();
    ReadToken(jetty.payload_token, sge.address, sge.length,
        [this, jetty_id, producer, raw = std::move(raw), expected = sge.length]
        (bool ok, std::vector<std::uint8_t> payload) mutable {
            if (!ok || payload.size() != expected) {
                auto found = jetty_contexts_.find(jetty_id);
                if (found != jetty_contexts_.end()) found->second.busy = false;
                ++ubase_errors_;
                return;
            }
            SubmitSqPayload(jetty_id, producer, std::move(raw),
                            std::move(payload));
        });
}

void
UdmaModel::SubmitSqPayload(std::uint32_t jetty_id, std::uint32_t producer,
                           std::vector<std::uint8_t> raw,
                           std::vector<std::uint8_t> payload)
{
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    std::array<std::uint8_t, abi::kWqebbBytes> bytes{};
    std::copy(raw.begin(), raw.end(), bytes.begin());
    const abi::SqWqe wqe(bytes);
    const QueueContext jetty = found->second;
    const auto route = tp_routes_.find(wqe.tpn());
    if (route == tp_routes_.end()) return;
    Frame frame{};
    frame.sequence = next_sequence_++;
    frame.source_eid = config_.endpoint_eid + jetty.eid_index * 0x10000U;
    frame.destination_eid = route->second.remote_eid;
    frame.source_port = static_cast<std::uint16_t>(route->second.port);
    frame.destination_port = UINT16_MAX;
    frame.operation = wqe.opcode() == 0 ? Frame::Operation::Send :
        (wqe.opcode() == 1 ? Frame::Operation::SendImmediate :
                             Frame::Operation::Write);
    frame.source_jetty = jetty_id;
    frame.destination_jetty = wqe.remote_jetty();
    frame.tpn = wqe.tpn();
    frame.immediate = wqe.immediate();
    frame.segment = wqe.remote_segment();
    frame.remote_address = wqe.remote_address();
    frame.bytes = std::move(payload);
    const std::uint32_t count = static_cast<std::uint32_t>(frame.bytes.size());
    const std::uint16_t completed_index = static_cast<std::uint16_t>(
        jetty.consumer & (jetty.depth - 1U));
    std::uint64_t request_id = 0;
    if (wqe.opcode() == 3) {
        request_id = next_rma_request_++;
        frame.request_id = request_id;
        frame.transfer_length = count;
        pending_rma_[request_id] = PendingRma{
            jetty_id, producer, completed_index, wqe.opcode(), count,
            wqe.immediate(), wqe.completion(), 0, 0};
    }
    network_.Send(std::move(frame),
        [this, jetty_id, producer, completed_index, opcode = wqe.opcode(),
         count, immediate = wqe.immediate(), completion = wqe.completion(),
         request_id]
        (bool ok) {
            if (!ok) {
                if (request_id) pending_rma_.erase(request_id);
                auto found = jetty_contexts_.find(jetty_id);
                if (found != jetty_contexts_.end()) found->second.busy = false;
                ++ubase_errors_;
                return;
            }
            if (opcode == 3) return;
            CompleteSq(jetty_id, producer, 1, completed_index, opcode, count,
                       immediate, completion);
        });
}

void
UdmaModel::CompleteSq(std::uint32_t jetty_id, std::uint32_t producer,
                      std::uint32_t wqebbs, std::uint16_t completed_index,
                      std::uint8_t opcode, std::uint32_t byte_count,
                      std::uint64_t immediate, bool completion_enabled)
{
    auto finish = [this, jetty_id, producer, wqebbs](bool ok) {
        auto found = jetty_contexts_.find(jetty_id);
        if (found == jetty_contexts_.end()) return;
        if (!ok) { found->second.busy = false; ++ubase_errors_; return; }
        found->second.consumer += wqebbs;
        found->second.busy = false;
        ProcessSq(jetty_id, producer);
    };
    const auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    if (!completion_enabled) return finish(true);
    WriteCqe(found->second.completion_queue, false, found->second.is_jetty,
             opcode, completed_index, jetty_id, byte_count,
             found->second.user_queue, immediate, std::move(finish));
}

void
UdmaModel::WriteCqe(std::uint32_t jfc_id, bool receive, bool jetty,
                    std::uint8_t opcode, std::uint16_t entry_index,
                    std::uint32_t local_id, std::uint32_t byte_count,
                    std::uint64_t user_data, std::uint64_t immediate,
                    Completion completion, std::uint32_t remote_id,
                    std::uint32_t remote_eid, std::uint32_t tpn)
{
    auto found = jfc_contexts_.find(jfc_id);
    if (found == jfc_contexts_.end()) return completion(false);
    const std::uint64_t ci = found->second.index_iova;
    ReadToken(found->second.token, ci, 4,
        [this, jfc_id, receive, jetty, opcode, entry_index, local_id,
         byte_count, user_data, immediate, remote_id, remote_eid, tpn,
         completion = std::move(completion)]
        // remote identity is captured separately because it is produced by
        // the peer packet rather than the local queue context.
        (bool ok, std::vector<std::uint8_t> consumer_bytes) mutable {
            auto found = jfc_contexts_.find(jfc_id);
            if (!ok || consumer_bytes.size() != 4 ||
                found == jfc_contexts_.end()) return completion(false);
            QueueContext& jfc = found->second;
            const std::uint32_t consumer = abi::Load32(consumer_bytes.data()) & 0x3fffffU;
            const std::uint32_t producer = static_cast<std::uint32_t>(jfc.producer) & 0x3fffffU;
            if (((producer - consumer) & 0x3fffffU) >= jfc.depth)
                return completion(false);
            const bool owner = ((jfc.producer / jfc.depth) & 1U) == 0;
            const auto cqe = abi::MakeCqe(receive, jetty, owner, opcode,
                entry_index, local_id, byte_count, user_data, immediate,
                remote_id, remote_eid, tpn);
            const std::uint64_t address = jfc.queue_iova +
                (jfc.producer & (jfc.depth - 1U)) * abi::kCqeBytes;
            WriteToken(jfc.token, address,
                std::vector<std::uint8_t>(cqe.begin(), cqe.end()),
                [this, jfc_id, completion = std::move(completion)](bool write_ok) mutable {
                    auto found = jfc_contexts_.find(jfc_id);
                    if (!write_ok || found == jfc_contexts_.end())
                        return completion(false);
                    ++found->second.producer;
                    ++found->second.pending_completions;
                    if (found->second.pending_completions <
                        found->second.moderation_count) {
                        completion(true);
                        return;
                    }
                    found->second.pending_completions = 0;
                    EmitCompletionEvent(jfc_id, std::move(completion));
                });
        });
}

void
UdmaModel::EmitCompletionEvent(std::uint32_t jfc_id, Completion completion)
{
    if (!ceq_iova_ || !ceq_depth_ || (ceq_depth_ & (ceq_depth_ - 1)))
        return completion(false);
    std::vector<std::uint8_t> event(64, 0);
    std::uint32_t word = jfc_id & 0xfffffU;
    if (!(ceq_producer_ & ceq_depth_)) word |= 1U << 31;
    StoreLe<std::uint32_t>(event, 0, word);
    const std::uint32_t slot = ceq_producer_ & (ceq_depth_ - 1);
    host_.DmaWriteIoVirtual(ceq_iova_ + std::uint64_t(slot) * 64,
                            std::move(event),
        [this, completion = std::move(completion)](bool ok) mutable {
            if (ok) {
                ++ceq_producer_;
                RaiseInterrupt(2, std::move(completion));
                return;
            }
            completion(false);
        });
}

void
UdmaModel::RaiseInterrupt(std::uint32_t vector, Completion completion)
{
    constexpr std::uint32_t Enable = 0x40c04;
    constexpr std::uint32_t EnabledVectors = 0x40c0c;
    constexpr std::uint32_t Data = 0x40c10;
    constexpr std::uint32_t AddressLow = 0x40c14;
    constexpr std::uint32_t AddressHigh = 0x40c18;
    constexpr std::uint32_t Mask = 0x40c20;
    const auto reg = [this](std::uint32_t address) {
        const auto found = ubios_endpoint_config_.find(address);
        return found == ubios_endpoint_config_.end() ? 0U : found->second;
    };
    const std::uint32_t enabled_log2 = reg(EnabledVectors);
    const std::uint32_t count = enabled_log2 < 31 ? 1U << enabled_log2 : 0;
    const std::uint64_t address = reg(AddressLow) |
        (std::uint64_t(reg(AddressHigh)) << 32);
    if (!(reg(Enable) & 1U) || !address) {
        // Pre-MSI bootstrap compatibility: the host adapter exposes the same
        // logical vector until the official UBUS code programs Type-1 MSI.
        host_.PulseInterrupt(vector);
        completion(true);
        return;
    }
    if (vector >= count || (vector < 32 && (reg(Mask) & (1U << vector))))
        return completion(false);
    auto state = std::make_shared<MsiState>();
    state->vector = vector;
    state->address = address;
    state->data = reg(Data) + vector;
    state->completion = std::move(completion);
    if (msi_iova_ == address && msi_physical_) {
        host_.MsiWrite(msi_physical_, state->data,
                       std::move(state->completion));
        return;
    }
    state->tokens.push_back(0);
    const auto add_token = [&state](const auto& contexts) {
        for (const auto& [id, context] : contexts) {
            (void)id;
            if (std::find(state->tokens.begin(), state->tokens.end(),
                          context.token) == state->tokens.end())
                state->tokens.push_back(context.token);
        }
    };
    add_token(jfc_contexts_);
    add_token(jfr_contexts_);
    add_token(jetty_contexts_);
    ContinueMsi(std::move(state));
}

void
UdmaModel::ContinueMsi(std::shared_ptr<MsiState> state)
{
    if (state->index >= state->tokens.size())
        return state->completion(false);
    const std::uint32_t token = state->tokens[state->index++];
    TranslateToken(token, state->address,
        [this, state = std::move(state)](bool ok, std::uint64_t physical) mutable {
            if (!ok || !physical) return ContinueMsi(std::move(state));
            msi_iova_ = state->address;
            msi_physical_ = physical;
            host_.MsiWrite(physical, state->data,
                           std::move(state->completion));
        });
}

void
UdmaModel::TranslateToken(std::uint32_t token, std::uint64_t address,
                          TranslateCompletion completion)
{
    constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
    constexpr std::uint32_t Entries = 1024;
    if (token >= Entries) return completion(false, 0);
    const std::uint64_t tect =
        LoadLe<std::uint64_t>(ummu_registers_.data() + 0x70) & AddressMask;
    if (!tect) return completion(false, 0);
    host_.DmaRead(tect, 64,
        [this, token, address, completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> entry) mutable {
            constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
            if (!ok || entry.size() != 64 ||
                !(LoadLe<std::uint64_t>(entry.data()) & 1U))
                return completion(false, 0);
            const std::uint64_t tct =
                LoadLe<std::uint64_t>(entry.data() + 8) & AddressMask;
            if (!tct) return completion(false, 0);
            host_.DmaRead(tct + std::uint64_t(token) * 64, 64,
                [this, address, completion = std::move(completion)]
                (bool tct_ok, std::vector<std::uint8_t> context) mutable {
                    constexpr std::uint64_t AddressMask =
                        0x0000fffffffff000ULL;
                    if (!tct_ok || context.size() != 64 ||
                        !(LoadLe<std::uint64_t>(context.data()) & 1U))
                        return completion(false, 0);
                    const std::uint64_t root =
                        LoadLe<std::uint64_t>(context.data() + 16) & AddressMask;
                    if (!root) return completion(false, 0);
                    WalkTokenPageTable(root, address, 0,
                                       std::move(completion));
                });
        });
}

void
UdmaModel::WalkTokenPageTable(std::uint64_t table, std::uint64_t address,
                              std::uint32_t level,
                              TranslateCompletion completion)
{
    constexpr std::array<std::uint32_t, 4> Shift{39, 30, 21, 12};
    if (level >= Shift.size()) return completion(false, 0);
    const std::uint64_t index = (address >> Shift[level]) & 0x1ffU;
    host_.DmaRead(table + index * 8, 8,
        [this, address, level, completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> bytes) mutable {
            constexpr std::array<std::uint32_t, 4> Shift{39, 30, 21, 12};
            constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
            if (!ok || bytes.size() != 8) return completion(false, 0);
            const std::uint64_t descriptor = LoadLe<std::uint64_t>(bytes.data());
            if (!(descriptor & 1U)) return completion(false, 0);
            const std::uint64_t output = descriptor & AddressMask;
            if (level == 3)
                return completion(true, output | (address & 0xfffU));
            if ((descriptor & 3U) == 1U && level != 0) {
                const std::uint64_t block = std::uint64_t{1} << Shift[level];
                return completion(true, (output & ~(block - 1U)) |
                                         (address & (block - 1U)));
            }
            WalkTokenPageTable(output, address, level + 1,
                               std::move(completion));
        });
}

void
UdmaModel::ReadToken(std::uint32_t token, std::uint64_t address,
                     std::size_t length, ReadCompletion completion)
{
    auto state = std::make_shared<TokenReadState>();
    state->token = token;
    state->address = address;
    state->length = length;
    state->bytes.resize(length);
    state->completion = std::move(completion);
    ContinueTokenRead(std::move(state));
}

void
UdmaModel::ContinueTokenRead(std::shared_ptr<TokenReadState> state)
{
    if (state->offset == state->length)
        return state->completion(true, std::move(state->bytes));
    const std::uint64_t current = state->address + state->offset;
    const std::size_t chunk = std::min<std::size_t>(
        state->length - state->offset, 4096 - (current & 0xfffU));
    TranslateToken(state->token, current,
        [this, state = std::move(state), chunk]
        (bool ok, std::uint64_t physical) mutable {
            if (!ok || !physical) return state->completion(false, {});
            host_.DmaRead(physical, chunk,
                [this, state = std::move(state), chunk]
                (bool read_ok, std::vector<std::uint8_t> bytes) mutable {
                    if (!read_ok || bytes.size() != chunk)
                        return state->completion(false, {});
                    std::copy(bytes.begin(), bytes.end(),
                              state->bytes.begin() + state->offset);
                    state->offset += chunk;
                    ContinueTokenRead(std::move(state));
                });
        });
}

void
UdmaModel::WriteToken(std::uint32_t token, std::uint64_t address,
                      std::vector<std::uint8_t> data, Completion completion)
{
    auto state = std::make_shared<TokenWriteState>();
    state->token = token;
    state->address = address;
    state->bytes = std::move(data);
    state->completion = std::move(completion);
    ContinueTokenWrite(std::move(state));
}

void
UdmaModel::ContinueTokenWrite(std::shared_ptr<TokenWriteState> state)
{
    if (state->offset == state->bytes.size()) return state->completion(true);
    const std::uint64_t current = state->address + state->offset;
    const std::size_t chunk = std::min<std::size_t>(
        state->bytes.size() - state->offset, 4096 - (current & 0xfffU));
    TranslateToken(state->token, current,
        [this, state = std::move(state), chunk]
        (bool ok, std::uint64_t physical) mutable {
            if (!ok || !physical) return state->completion(false);
            std::vector<std::uint8_t> bytes(
                state->bytes.begin() + state->offset,
                state->bytes.begin() + state->offset + chunk);
            host_.DmaWrite(physical, std::move(bytes),
                [this, state = std::move(state), chunk](bool write_ok) mutable {
                    if (!write_ok) return state->completion(false);
                    state->offset += chunk;
                    ContinueTokenWrite(std::move(state));
                });
        });
}

void
UdmaModel::FinishUbaseDescriptor(std::vector<std::uint8_t> descriptor,
                                 std::uint16_t opcode,
                                 std::uint32_t descriptor_count,
                                 std::vector<std::uint8_t> response,
                                 std::function<void()> after_completion)
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
    descriptor[4] = descriptor[5] = 0;
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
    *issue = [this, writes, cursor, issue, head, descriptor_count, depth,
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
        host_.DmaWriteIoVirtual(record.address, std::move(record.bytes),
            [this, issue](bool ok) {
                if (!ok) return FailUbase();
                (*issue)();
            });
    };
    (*issue)();
}

void
UdmaModel::FetchDescriptor(std::uint64_t address)
{
    host_.DmaRead(address, sizeof(Descriptor),
        [this](bool ok, std::vector<std::uint8_t> bytes) {
            if (!ok || bytes.size() != sizeof(Descriptor)) {
                return;
            }
            Descriptor descriptor{};
            std::memcpy(&descriptor, bytes.data(), sizeof(descriptor));
            const std::uint64_t sequence = next_sequence_++;
            ++submitted_;
            if (descriptor.opcode != kOpcodeSend || descriptor.length == 0) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            pending_.emplace(sequence, descriptor);
            FetchPayload(sequence, descriptor);
        });
}

void
UdmaModel::FetchPayload(std::uint64_t sequence, Descriptor descriptor)
{
    host_.DmaRead(descriptor.payload_address, descriptor.length,
        [this, sequence, descriptor](bool ok, std::vector<std::uint8_t> bytes) mutable {
            if (!ok || bytes.size() != descriptor.length) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            Frame frame{};
            frame.sequence = sequence;
            frame.source_eid = descriptor.source_eid;
            frame.destination_eid = descriptor.destination_eid;
            frame.source_port = descriptor.source_port;
            frame.destination_port = descriptor.source_port;
            frame.bytes = std::move(bytes);
            network_.Send(std::move(frame),
                [this, sequence, descriptor](bool sent) {
                    Finish(sequence, descriptor,
                           sent ? kStatusSuccess : kStatusDmaError);
                });
        });
}

void
UdmaModel::Finish(std::uint64_t sequence, const Descriptor& descriptor,
                  std::uint32_t status)
{
    CompletionEntry entry{sequence, status, descriptor.length};
    const auto* first = reinterpret_cast<const std::uint8_t*>(&entry);
    host_.DmaWrite(descriptor.completion_address,
                   std::vector<std::uint8_t>(first, first + sizeof(entry)),
        [this, sequence](bool ok) {
            pending_.erase(sequence);
            if (!ok) {
                return;
            }
            ++completed_;
            host_.SetInterrupt(kInterruptVector, true);
        });
}

void
UdmaModel::Receive(Frame frame)
{
    switch (frame.operation) {
      case Frame::Operation::Send:
      case Frame::Operation::SendImmediate:
        receive_frames_.push_back(std::move(frame));
        ProcessReceiveQueue();
        return;
      case Frame::Operation::Write: return ReceiveWrite(std::move(frame));
      case Frame::Operation::ReadRequest:
        return ReceiveReadRequest(std::move(frame));
      case Frame::Operation::WriteAck: return ReceiveWriteAck(std::move(frame));
      case Frame::Operation::ReadResponse:
        return ReceiveReadResponse(std::move(frame));
      default: ++ubase_errors_; return;
    }
}

void
UdmaModel::SetLinkState(std::uint32_t port, bool up)
{
    if (port >= link_up_.size()) { ++ubase_errors_; return; }
    link_up_[port] = up;
    if (up) return;
    for (auto& [id, route] : tp_routes_) {
        (void)id;
        if (route.port != port) continue;
        bool rebound = false;
        for (std::uint32_t candidate = 0; candidate < link_up_.size(); ++candidate) {
            if (link_up_[candidate]) {
                route.port = candidate;
                rebound = true;
                break;
            }
        }
        if (!rebound) route.active = false;
    }
}

void
UdmaModel::ReceiveWrite(Frame frame)
{
    if (!frame.remote_address || frame.bytes.size() != frame.transfer_length) {
        ++ubase_errors_;
        return;
    }
    const Frame reply_basis = frame;
    WriteToken(frame.segment, frame.remote_address,
                        std::move(frame.bytes),
        [this, reply_basis](bool ok) mutable {
            if (!ok) { ++ubase_errors_; return; }
            Frame ack{};
            ack.sequence = next_sequence_++;
            ack.source_eid = reply_basis.destination_eid;
            ack.destination_eid = reply_basis.source_eid;
            ack.source_port = reply_basis.destination_port;
            ack.destination_port = reply_basis.source_port;
            ack.operation = Frame::Operation::WriteAck;
            ack.source_jetty = reply_basis.destination_jetty;
            ack.destination_jetty = reply_basis.source_jetty;
            ack.tpn = reply_basis.tpn;
            ack.request_id = reply_basis.request_id;
            network_.Send(std::move(ack), [this](bool sent) {
                if (!sent) ++ubase_errors_;
            });
        });
}

void
UdmaModel::ReceiveReadRequest(Frame frame)
{
    if (!frame.remote_address || !frame.transfer_length) {
        ++ubase_errors_;
        return;
    }
    const Frame reply_basis = frame;
    ReadToken(frame.segment, frame.remote_address,
                       frame.transfer_length,
        [this, reply_basis](bool ok, std::vector<std::uint8_t> payload) mutable {
            if (!ok || payload.size() != reply_basis.transfer_length) {
                ++ubase_errors_;
                return;
            }
            Frame response{};
            response.sequence = next_sequence_++;
            response.source_eid = reply_basis.destination_eid;
            response.destination_eid = reply_basis.source_eid;
            response.source_port = reply_basis.destination_port;
            response.destination_port = reply_basis.source_port;
            response.operation = Frame::Operation::ReadResponse;
            response.source_jetty = reply_basis.destination_jetty;
            response.destination_jetty = reply_basis.source_jetty;
            response.tpn = reply_basis.tpn;
            response.request_id = reply_basis.request_id;
            response.transfer_length = reply_basis.transfer_length;
            response.bytes = std::move(payload);
            network_.Send(std::move(response), [this](bool sent) {
                if (!sent) ++ubase_errors_;
            });
        });
}

void
UdmaModel::ReceiveWriteAck(Frame frame)
{
    auto found = pending_rma_.find(frame.request_id);
    if (found == pending_rma_.end() || found->second.opcode != 3) {
        ++ubase_errors_;
        return;
    }
    const PendingRma pending = found->second;
    pending_rma_.erase(found);
    CompleteSq(pending.jetty_id, pending.producer, 1,
               pending.completed_index, pending.opcode, pending.byte_count,
               pending.immediate, pending.completion);
}

void
UdmaModel::ReceiveReadResponse(Frame frame)
{
    auto found = pending_rma_.find(frame.request_id);
    if (found == pending_rma_.end() || found->second.opcode != 6 ||
        frame.bytes.size() != found->second.byte_count) {
        ++ubase_errors_;
        return;
    }
    const PendingRma pending = found->second;
    pending_rma_.erase(found);
    WriteToken(pending.local_token, pending.local_address,
                        std::move(frame.bytes),
        [this, pending](bool ok) {
            if (!ok) {
                auto jetty = jetty_contexts_.find(pending.jetty_id);
                if (jetty != jetty_contexts_.end()) jetty->second.busy = false;
                ++ubase_errors_;
                return;
            }
            CompleteSq(pending.jetty_id, pending.producer, 1,
                       pending.completed_index, pending.opcode,
                       pending.byte_count, pending.immediate,
                       pending.completion);
        });
}

void
UdmaModel::ProcessReceiveQueue()
{
    if (receive_busy_ || receive_frames_.empty()) return;
    receive_busy_ = true;
    auto frame = std::make_shared<Frame>(std::move(receive_frames_.front()));
    receive_frames_.pop_front();
    ReceiveSend(std::move(frame));
}

void
UdmaModel::ReceiveSend(std::shared_ptr<Frame> frame)
{
    auto fail = [this]() {
        ++ubase_errors_;
        receive_busy_ = false;
        ProcessReceiveQueue();
    };
    auto jetty = jetty_contexts_.find(frame->destination_jetty);
    if (jetty == jetty_contexts_.end()) return fail();
    auto jfr = jfr_contexts_.find(jetty->second.receive_queue);
    if (jfr == jfr_contexts_.end() || !jfr->second.producer_iova)
        return fail();
    const std::uint32_t jfr_id = jfr->first;
    const std::uint64_t pi = jfr->second.producer_iova;
    ReadToken(jfr->second.token, pi, 4,
        [this, frame = std::move(frame), jfr_id]
        (bool ok, std::vector<std::uint8_t> producer_bytes) mutable {
            auto fail = [this]() {
                ++ubase_errors_; receive_busy_ = false; ProcessReceiveQueue();
            };
            auto jfr = jfr_contexts_.find(jfr_id);
            if (!ok || producer_bytes.size() != 4 || jfr == jfr_contexts_.end())
                return fail();
            const std::uint16_t producer =
                static_cast<std::uint16_t>(abi::Load32(producer_bytes.data()));
            if (static_cast<std::uint16_t>(jfr->second.consumer) == producer)
                return fail();
            const std::uint64_t address = jfr->second.index_iova +
                (jfr->second.consumer & (jfr->second.depth - 1U)) * 4;
            ReadToken(jfr->second.token, address, 4,
                [this, frame = std::move(frame), jfr_id]
                (bool index_ok, std::vector<std::uint8_t> index_bytes) mutable {
                    auto jfr = jfr_contexts_.find(jfr_id);
                    if (!index_ok || index_bytes.size() != 4 ||
                        jfr == jfr_contexts_.end()) {
                        ++ubase_errors_; receive_busy_ = false;
                        ProcessReceiveQueue(); return;
                    }
                    const std::uint32_t rqe = abi::Load32(index_bytes.data());
                    if (rqe >= jfr->second.depth) {
                        ++ubase_errors_; receive_busy_ = false;
                        ProcessReceiveQueue(); return;
                    }
                    ReceiveSge(std::move(frame), jfr_id, rqe, 0, 0);
                });
        });
}

void
UdmaModel::ReceiveSge(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                      std::uint32_t rqe_index, std::uint32_t sge_index,
                      std::uint32_t copied)
{
    auto jfr = jfr_contexts_.find(jfr_id);
    if (jfr == jfr_contexts_.end()) return;
    if (copied == frame->bytes.size())
        return FinishReceive(std::move(frame), jfr_id, rqe_index);
    if (sge_index >= jfr->second.max_sge) {
        ++ubase_errors_; receive_busy_ = false; ProcessReceiveQueue(); return;
    }
    const std::uint64_t address = jfr->second.queue_iova +
        std::uint64_t(rqe_index) * jfr->second.entry_stride +
        std::uint64_t(sge_index) * abi::kSgeBytes;
    ReadToken(jfr->second.token, address, abi::kSgeBytes,
        [this, frame = std::move(frame), jfr_id, rqe_index, sge_index, copied]
        (bool ok, std::vector<std::uint8_t> bytes) mutable {
            if (!ok || bytes.size() != abi::kSgeBytes) {
                ++ubase_errors_; receive_busy_ = false;
                ProcessReceiveQueue(); return;
            }
            const abi::Sge sge{abi::Load32(bytes.data()),
                               abi::Load32(bytes.data() + 4),
                               abi::Load64(bytes.data() + 8)};
            const std::uint32_t chunk = std::min<std::uint32_t>(
                sge.length, static_cast<std::uint32_t>(frame->bytes.size() - copied));
            if (!chunk || !sge.address) {
                ++ubase_errors_; receive_busy_ = false;
                ProcessReceiveQueue(); return;
            }
            std::vector<std::uint8_t> payload(frame->bytes.begin() + copied,
                                               frame->bytes.begin() + copied + chunk);
            auto jfr = jfr_contexts_.find(jfr_id);
            if (jfr == jfr_contexts_.end()) {
                ++ubase_errors_; receive_busy_ = false;
                ProcessReceiveQueue(); return;
            }
            WriteToken(jfr->second.payload_token, sge.address,
                       std::move(payload),
                [this, frame = std::move(frame), jfr_id, rqe_index,
                 sge_index, copied, chunk](bool write_ok) mutable {
                    if (!write_ok) {
                        ++ubase_errors_; receive_busy_ = false;
                        ProcessReceiveQueue(); return;
                    }
                    ReceiveSge(std::move(frame), jfr_id, rqe_index,
                               sge_index + 1, copied + chunk);
                });
        });
}

void
UdmaModel::FinishReceive(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                         std::uint32_t rqe_index)
{
    auto jfr = jfr_contexts_.find(jfr_id);
    auto jetty = jetty_contexts_.find(frame->destination_jetty);
    if (jfr == jfr_contexts_.end() || jetty == jetty_contexts_.end()) return;
    const std::uint32_t jfc_id = jetty->second.receive_completion_queue ?
        jetty->second.receive_completion_queue : jfr->second.completion_queue;
    WriteCqe(jfc_id, true, true, static_cast<std::uint8_t>(frame->operation),
             static_cast<std::uint16_t>(rqe_index), frame->destination_jetty,
             static_cast<std::uint32_t>(frame->bytes.size()), 0,
             frame->immediate,
        [this, jfr_id](bool ok) {
            auto jfr = jfr_contexts_.find(jfr_id);
            if (ok && jfr != jfr_contexts_.end()) ++jfr->second.consumer;
            if (!ok) ++ubase_errors_;
            receive_busy_ = false;
            ProcessReceiveQueue();
        }, frame->source_jetty, frame->source_eid, frame->tpn);
}

} // namespace openurma::device
