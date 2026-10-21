// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

namespace {
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
    if (!config_.sq_prefetch_wqebbs || !config_.sq_fetch_burst_wqebbs ||
        !config_.sq_fetch_max_outstanding || config_.sq_prefetch_wqebbs > 65536 ||
        config_.sq_fetch_burst_wqebbs > 65536 || config_.sq_fetch_max_outstanding > 4096)
        throw std::invalid_argument("invalid SQ prefetch bounds");
    if (!config_.write_payload_read_window ||
        config_.write_payload_read_window > 4096)
        throw std::invalid_argument("invalid WRITE payload DMA window");
    if (!config_.completion_event_slots || config_.completion_event_slots > 4096)
        throw std::invalid_argument("invalid completion event slot budget");
    if (config_.max_jettys < 2 || config_.max_jettys > 65535 ||
        !config_.public_jettys || config_.public_jettys >= config_.max_jettys)
        throw std::invalid_argument("invalid Jetty identity capacity");
    const auto power_of_two = [](std::uint32_t depth) {
        return depth && !(depth & (depth - 1));
    };
    if (!power_of_two(config_.max_jfs_depth) || config_.max_jfs_depth > 8192 ||
        !power_of_two(config_.max_jfr_depth) || config_.max_jfr_depth > 32768 ||
        !power_of_two(config_.max_jfc_depth) || config_.max_jfc_depth > 2097152)
        throw std::invalid_argument("invalid guest queue capacity");
    StoreLe<std::uint32_t>(ummu_registers_, 0x10, 0x00000b08);
    StoreLe<std::uint32_t>(ummu_registers_, 0x14, 0x00042208);
    // CAP2.OAS=48 is required by the stock OLK UMMU driver's 48-bit S1
    // identity domain. Advertising 40-bit output addresses makes probe fail
    // with -ERANGE before SVA can be enabled. The SVA driver also requires
    // the CPU's base-page granule in CAP2; expose both 4 KiB and 64 KiB.
    StoreLe<std::uint32_t>(ummu_registers_, 0x18, 0x0000916e);
    // CAP3.STALL_MODEL=1 selects terminate-on-fault.  This model implements
    // SVA/MAPT translation, but not an EVENTQ-backed IOPF replay engine, so it
    // must not advertise the stall model (STALL_MODEL=0/2) to the stock UMMU
    // driver.
    StoreLe<std::uint32_t>(ummu_registers_, 0x1c, 0x00001010);
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
    ++mmio_writes_;
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
        // UMMU programming and command-queue doorbells may change token or
        // page-table mappings. A real device invalidates matching IOTLB
        // entries; clearing this compact model is conservative and correct.
        iotlb_.clear();
        // RELEASE_PERMQ is a command/status register.  The modeled queue is
        // drained synchronously, so acknowledge completion by clearing the
        // command bit exactly as hardware does before the driver's poll.
        if (reg == kUmmuReleasePermissionQueue && length == 4 &&
            (LoadLe<std::uint32_t>(ummu_registers_.data() + reg) &
             kUmmuReleaseCompletion)) {
            StoreLe<std::uint32_t>(ummu_registers_, reg, 0);
        }
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
                const std::uint32_t queue = reg - local;
                ProcessUmmuCommandQueue(queue);
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
    if (HandleCeqConsumerMmio(offset, length, value)) return true;
    if (config_.one_shot_jfc_notifications &&
        offset == abi::kUdmaMemoryOffset + 0x40 && length == 8) {
        // udma_jfc_db: CI[23:0], notify[24], arm_sn[26:25],
        // type[27] == UDMA_CQ_ARM_DB, jfcn[51:32].
        const auto id = static_cast<std::uint32_t>((value >> 32) & 0xfffffU);
        if ((value & (1ULL << 27)) && !(value & 0xfff00000f0000000ULL) &&
            jfc_contexts_.count(id)) armed_jfcs_[id] = 1;
        else ++unknown_queue_writes_;
        return true;
    }
    if (HandleJettyMmio(offset, length, value)) return true;
    if (offset >= abi::kUdmaMemoryOffset &&
        offset < abi::kUdmaMemoryOffset + 0x00100000) {
        ++unknown_queue_writes_;
        last_unknown_queue_offset_ = offset;
    }
    return offset + length <= kOfficialApertureBytes;
}

void
UdmaModel::ProcessUmmuCommandQueue(std::uint32_t queue)
{
    if (ummu_command_queue_busy_[queue]) return;
    const auto producer = LoadLe<std::uint32_t>(
        ummu_registers_.data() + queue + kUmmuCommandQueueProducer);
    const auto base_word = LoadLe<std::uint64_t>(ummu_registers_.data() + queue);
    const auto log2size = static_cast<std::uint32_t>(base_word & 0x1fU);
    const auto base = base_word & 0x000fffffffffffe0ULL;
    const auto consumer_offset = queue + kUmmuCommandQueueConsumer;
    auto consumer = LoadLe<std::uint32_t>(
        ummu_registers_.data() + consumer_offset);

    // An enable/disable write contains no new command. Preserve the boot-time
    // register handshake even before the driver has installed a queue base.
    if (!base || !log2size || log2size > 16 ||
        !(producer & kUmmuCommandQueueEnable)) {
        StoreLe<std::uint32_t>(ummu_registers_, consumer_offset, producer);
        return;
    }
    const std::uint32_t depth = 1U << log2size;
    const std::uint32_t ring_mask = 2U * depth - 1U;
    if ((consumer & ring_mask) == (producer & ring_mask)) {
        StoreLe<std::uint32_t>(ummu_registers_, consumer_offset,
                              (consumer & ~kUmmuCommandQueueEnable) |
                              (producer & kUmmuCommandQueueEnable));
        return;
    }

    ummu_command_queue_busy_[queue] = true;
    const auto address = base + (consumer & (depth - 1U)) * 32U;
    host_.DmaRead(address, 32,
        [this, queue, consumer_offset, ring_mask](
            bool ok, std::vector<std::uint8_t> command) {
            ummu_command_queue_busy_[queue] = false;
            if (!ok || command.size() != 32) {
                std::cerr << "udma-model: UMMU command fetch failed"
                          << " queue=0x" << std::hex << queue << std::dec
                          << '\n';
                return;
            }
            const auto producer = LoadLe<std::uint32_t>(
                ummu_registers_.data() + queue + kUmmuCommandQueueProducer);
            const auto old_consumer = LoadLe<std::uint32_t>(
                ummu_registers_.data() + consumer_offset);
            const auto next = ((old_consumer + 1U) & ring_mask) |
                              (producer & kUmmuCommandQueueEnable);
            StoreLe<std::uint32_t>(ummu_registers_, consumer_offset, next);
            ProcessUmmuCommandQueue(queue);
        });
}

} // namespace ubsim::device
