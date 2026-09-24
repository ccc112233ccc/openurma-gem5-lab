// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"

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
    if (opcode != 0x0030 && opcode != 0x0002 && opcode != 0x6200 &&
        opcode != 0x0001 && opcode != 0x7001)
        return FailUbase();
    FinishUbaseDescriptor(std::move(descriptor), opcode, count,
                          BuildUbaseResponse(opcode));
}

void
UdmaModel::FinishUbaseDescriptor(std::vector<std::uint8_t> descriptor,
                                 std::uint16_t opcode,
                                 std::uint32_t descriptor_count,
                                 std::vector<std::uint8_t> response)
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
    *issue = [this, writes, cursor, issue, head, descriptor_count, depth]() {
        if (*cursor == writes->size()) {
            constexpr std::size_t Tail = 0x10 / 4;
            constexpr std::size_t Head = 0x14 / 4;
            ubase_command_queue_registers_[Head] =
                (head + descriptor_count) % depth;
            ubase_command_queue_registers_[Tail] =
                ubase_target_producer_ % depth;
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
    (void)frame;
    // Receive queues are deliberately a separate extraction milestone. Keeping
    // this entry point in the stable core API prevents the host adapters from
    // acquiring receive-side device semantics in the meantime.
}

} // namespace openurma::device
