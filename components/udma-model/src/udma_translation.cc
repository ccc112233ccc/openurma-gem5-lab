// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::TranslateToken(std::uint32_t token, std::uint64_t address, bool write,
                          TranslateCompletion completion)
{
    const std::uint64_t cache_key =
        (write ? (std::uint64_t{1} << 63) : 0) |
        (std::uint64_t(token) << 52) | (address >> 12);
    const auto cached = iotlb_.find(cache_key);
    if (cached != iotlb_.end())
        return completion(true, cached->second | (address & 0xfffU));
    constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
    constexpr std::uint32_t Entries = 1024;
    if (token >= Entries) return completion(false, 0);
    const std::uint64_t tect =
        LoadLe<std::uint64_t>(ummu_registers_.data() + 0x70) & AddressMask;
    if (!tect) return completion(false, 0);
    host_.DmaRead(tect, 64,
        [this, token, address, write, completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> entry) mutable {
            constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
            if (!ok || entry.size() != 64 ||
                !(LoadLe<std::uint64_t>(entry.data()) & 1U))
                return completion(false, 0);
            const std::uint64_t tct =
                LoadLe<std::uint64_t>(entry.data() + 8) & AddressMask;
            if (!tct) return completion(false, 0);
            host_.DmaRead(tct + std::uint64_t(token) * 64, 64,
                [this, token, address, write, completion = std::move(completion)]
                (bool tct_ok, std::vector<std::uint8_t> context) mutable {
                    if (!tct_ok || context.size() != 64 ||
                        !(LoadLe<std::uint64_t>(context.data()) & 1U))
                        return completion(false, 0);
                    const auto granule = static_cast<std::uint32_t>(
                        (LoadLe<std::uint64_t>(context.data() + 8) >> 6) & 3U);
                    CheckMapt(std::move(context), address, write,
                        [this, token, address, write, granule,
                         completion = std::move(completion)]
                        (bool permitted, std::uint64_t root) mutable {
                            if (!permitted || !root)
                                return completion(false, 0);
                            WalkTokenPageTable(root, address, 0, granule,
                                [this, token, address, write, root,
                                 completion = std::move(completion)]
                                (bool ok, std::uint64_t physical) mutable {
                                    if (!ok)
                                        std::cerr << "udma-model: token page walk failed"
                                                  << " token=" << token
                                                  << " address=0x" << std::hex
                                                  << address << " root=0x" << root
                                                  << std::dec << '\n';
                                    if (ok && config_.iotlb_entries) {
                                        if (iotlb_.size() >= config_.iotlb_entries)
                                            iotlb_.clear();
                                        const std::uint64_t key =
                                            (write ? (std::uint64_t{1} << 63) : 0) |
                                            (std::uint64_t(token) << 52) |
                                            (address >> 12);
                                        iotlb_[key] = physical & ~std::uint64_t{0xfff};
                                    }
                                    completion(ok, physical);
                                });
                        });
                });
        });
}

void
UdmaModel::TranslateIoVirtual(std::uint64_t address, bool write,
                              TranslateCompletion completion)
{
    constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
    if (generic_iova_token_) {
        const std::uint32_t cached = *generic_iova_token_;
        generic_iova_token_.reset();
        return TranslateToken(cached, address, write,
            [this, address, write, cached,
             completion = std::move(completion)]
            (bool ok, std::uint64_t physical) mutable {
                if (ok) {
                    generic_iova_token_ = cached;
                    return completion(true, physical);
                }
                TranslateIoVirtual(address, write, std::move(completion));
            });
    }
    const std::uint64_t tect =
        LoadLe<std::uint64_t>(ummu_registers_.data() + 0x70) & AddressMask;
    if (!tect) return completion(false, 0);
    host_.DmaRead(tect, 64,
        [this, address, write, completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> entry) mutable {
            constexpr std::uint64_t AddressMask = 0x0000fffffffff000ULL;
            if (!ok || entry.size() != 64 ||
                !(LoadLe<std::uint64_t>(entry.data()) & 1U))
                return completion(false, 0);
            const std::uint64_t tct =
                LoadLe<std::uint64_t>(entry.data() + 8) & AddressMask;
            if (!tct) return completion(false, 0);
            auto state = std::make_shared<IoVirtualState>();
            state->address = address;
            state->tct = tct;
            state->write = write;
            state->completion = std::move(completion);
            ContinueIoVirtual(std::move(state));
        });
}

void
UdmaModel::ContinueIoVirtual(std::shared_ptr<IoVirtualState> state)
{
    constexpr std::uint32_t Entries = 1024;
    // 120 contexts (7680 bytes) fit in one 8-KiB UB-HOST message together
    // with its wire header. This mirrors the old in-process bulk TCT scan
    // without relying on direct guest-memory access.
    constexpr std::uint32_t EntriesPerBatch = 120;
    if (state->next_token >= Entries)
        return state->completion(false, 0);
    state->batch_start = state->next_token;
    state->batch_entries = std::min(EntriesPerBatch,
                                    Entries - state->next_token);
    state->batch_index = 0;
    state->next_token += state->batch_entries;
    host_.DmaRead(state->tct + std::uint64_t(state->batch_start) * 64,
                  std::size_t(state->batch_entries) * 64,
        [this, state](bool ok, std::vector<std::uint8_t> contexts) {
            if (!ok || contexts.size() !=
                    std::size_t(state->batch_entries) * 64)
                return state->completion(false, 0);
            state->contexts = std::move(contexts);
            TryIoVirtualContext(state);
        });
}

void
UdmaModel::TryIoVirtualContext(std::shared_ptr<IoVirtualState> state)
{
    while (state->batch_index < state->batch_entries) {
        const std::uint32_t local = state->batch_index++;
        const std::uint8_t* context = state->contexts.data() + local * 64;
        if (!(LoadLe<std::uint64_t>(context) & 1U)) continue;
        const std::uint32_t token = state->batch_start + local;
        const auto granule = static_cast<std::uint32_t>(
            (LoadLe<std::uint64_t>(context + 8) >> 6) & 3U);
        std::vector<std::uint8_t> copy(context, context + 64);
        return CheckMapt(std::move(copy), state->address, state->write,
            [this, state, token, granule](bool permitted, std::uint64_t root) {
                if (!permitted || !root) return TryIoVirtualContext(state);
                WalkTokenPageTable(root, state->address, 0, granule,
                    [this, state, token](bool ok, std::uint64_t physical) {
                        if (!ok) return TryIoVirtualContext(state);
                        generic_iova_token_ = token;
                        state->completion(true, physical);
                    });
            });
    }
    ContinueIoVirtual(std::move(state));
}

void
UdmaModel::ReadIoVirtual(std::uint64_t address, std::size_t length,
                         ReadCompletion completion)
{
    TranslateIoVirtual(address, false,
        [this, address, length, completion = std::move(completion)]
        (bool ok, std::uint64_t physical) mutable {
            if (!ok && config_.identity_iova_test_mode)
                return host_.DmaRead(address, length, std::move(completion));
            if (!ok) return completion(false, {});
            host_.DmaRead(physical, length, std::move(completion));
        });
}

void
UdmaModel::WriteIoVirtual(std::uint64_t address,
                          std::vector<std::uint8_t> data,
                          Completion completion)
{
    TranslateIoVirtual(address, true,
        [this, address, data = std::move(data),
         completion = std::move(completion)]
        (bool ok, std::uint64_t physical) mutable {
            if (!ok && config_.identity_iova_test_mode)
                return host_.DmaWrite(address, std::move(data),
                                      std::move(completion));
            if (!ok) return completion(false);
            host_.DmaWrite(physical, std::move(data), std::move(completion));
        });
}

void
UdmaModel::CheckMapt(std::vector<std::uint8_t> context,
                     std::uint64_t address, bool write,
                     TranslateCompletion completion)
{
    // Official UMMU TCT_ENT2_TTBA occupies bits [51:4]. With a 64-KiB
    // translation granule and 48-bit VA, Linux allocates the 512-byte PGD
    // from pgd_cache, so its physical base need not be 4-KiB aligned.
    constexpr std::uint64_t TctRootAddressMask =
        ((std::uint64_t{1} << 52) - 1U) & ~std::uint64_t{0xf};
    constexpr std::uint64_t MaptAddressMask = 0x0000ffffffffffe0ULL;
    const std::uint64_t control = LoadLe<std::uint64_t>(context.data());
    const std::uint64_t root =
        LoadLe<std::uint64_t>(context.data() + 16) & TctRootAddressMask;
    if (!(control & (std::uint64_t{1} << 19)))
        return completion(true, root);

    const std::uint64_t block =
        LoadLe<std::uint64_t>(context.data() + 24) & MaptAddressMask;
    if (!block) return completion(false, 0);
    if (!(control & (std::uint64_t{1} << 16))) {
        host_.DmaRead(block, 32,
            [root, address, write, completion = std::move(completion)]
            (bool ok, std::vector<std::uint8_t> node) mutable {
                if (!ok || node.size() != 32)
                    return completion(false, 0);
                const std::uint32_t word0 = LoadLe<std::uint32_t>(node.data());
                const std::uint64_t base =
                    LoadLe<std::uint32_t>(node.data() + 8) |
                    (std::uint64_t(LoadLe<std::uint32_t>(node.data() + 12) &
                                   0xffffU) << 32);
                const std::uint64_t limit =
                    LoadLe<std::uint32_t>(node.data() + 16) |
                    (std::uint64_t(LoadLe<std::uint32_t>(node.data() + 20) &
                                   0xffffU) << 32);
                const std::uint32_t permission = (word0 >> 4) & 0x3fU;
                const bool allowed = write ? (permission & 1U)
                                           : (permission & 2U);
                const bool token_check =
                    (LoadLe<std::uint32_t>(node.data() + 12) >> 31) != 0;
                completion((word0 & 1U) && !token_check && allowed &&
                           address >= base && address <= limit,
                           root);
            });
        return;
    }
    const std::uint64_t block_table =
        LoadLe<std::uint64_t>(context.data() + 32) & MaptAddressMask;
    WalkMaptTable(block_table, block, block, address, write, 0,
        [root, completion = std::move(completion)]
        (bool allowed, std::uint64_t) mutable {
            completion(allowed, root);
        });
}

void
UdmaModel::WalkMaptTable(std::uint64_t block_table, std::uint64_t block,
                         std::uint64_t level_base, std::uint64_t address, bool write,
                         std::uint32_t level,
                         TranslateCompletion completion)
{
    if (level >= 4 || !block)
        return completion(false, 0);
    CheckMaptNode(block_table, block, level_base, address, write, level,
                  std::move(completion));
}

void
UdmaModel::CheckMaptNode(std::uint64_t block_table, std::uint64_t block,
                         std::uint64_t level_base, std::uint64_t address, bool write,
                         std::uint32_t level,
                         TranslateCompletion completion)
{
    constexpr std::array<std::uint32_t, 4> Shift{39, 30, 21, 12};
    const std::uint64_t index = (address >> Shift[level]) & 0x1ffU;
    const std::uint32_t shift = Shift[level];
    host_.DmaRead(level_base + index * 32, 32,
        [this, block_table, block, address, write, level, shift,
         completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> node) mutable {
            if (!ok || node.size() != 32)
                return completion(false, 0);
            const std::uint32_t word0 = LoadLe<std::uint32_t>(node.data());
            const std::uint32_t word1 = LoadLe<std::uint32_t>(node.data() + 4);
            if (!(word0 & 1U)) return completion(false, 0);
            if (word0 & 2U) {
                const std::uint64_t mask =
                    (std::uint64_t{1} << shift) - 1U;
                const std::uint64_t base =
                    LoadLe<std::uint32_t>(node.data() + 8) |
                    (std::uint64_t(LoadLe<std::uint32_t>(node.data() + 12) &
                                   0x7fU) << 32);
                const std::uint64_t limit =
                    LoadLe<std::uint32_t>(node.data() + 16) |
                    (std::uint64_t(LoadLe<std::uint32_t>(node.data() + 20) &
                                   0x7fU) << 32);
                const std::uint32_t permission = (word0 >> 4) & 0x3fU;
                const bool allowed = write ? (permission & 1U)
                                           : (permission & 2U);
                const bool token_check =
                    (LoadLe<std::uint32_t>(node.data() + 12) >> 31) != 0;
                const std::uint64_t within = address & mask;
                return completion(!token_check && allowed && within >= base &&
                                  within <= limit, 0);
            }
            const std::uint32_t next_offset =
                ((word1 & 0x3ffU) << 20) | (word0 >> 12);
            const bool crosses_block = (word0 & (1U << 2)) != 0;
            const std::uint32_t next_block_index = word1 >> 16;
            auto continue_walk =
                [this, block_table, address, write, level, next_offset,
                 completion = std::move(completion)]
                (bool valid, std::uint64_t next_base) mutable {
                    if (!valid || !next_base)
                        return completion(false, 0);
                    WalkMaptTable(block_table, next_base,
                                  next_base + std::uint64_t(next_offset) * 32,
                                  address, write, level + 1,
                                  std::move(completion));
                };
            if (!crosses_block)
                return continue_walk(true, block);
            if (!block_table) return continue_walk(false, 0);
            host_.DmaRead(block_table + std::uint64_t(next_block_index) * 8, 8,
                [continue_walk = std::move(continue_walk)]
                (bool table_ok, std::vector<std::uint8_t> descriptor) mutable {
                    if (!table_ok || descriptor.size() != 8)
                        return continue_walk(false, 0);
                    const std::uint64_t value =
                        LoadLe<std::uint64_t>(descriptor.data());
                    continue_walk(value & 1U,
                                  value & 0x0000ffffffffffe0ULL);
                });
        });
}

void
UdmaModel::WalkTokenPageTable(std::uint64_t table, std::uint64_t address,
                              std::uint32_t level, std::uint32_t granule,
                              TranslateCompletion completion)
{
    constexpr std::array<std::uint32_t, 4> Shift4k{39, 30, 21, 12};
    constexpr std::array<std::uint32_t, 3> Shift64k{42, 29, 16};
    if (granule > 1) return completion(false, 0);
    const std::uint32_t last_level = granule == 1 ? 2 : 3;
    if (level > last_level) return completion(false, 0);
    const std::uint32_t shift = granule == 1 ? Shift64k[level] : Shift4k[level];
    const std::uint64_t index = (address >> shift) &
                                (granule == 1 ? 0x1fffU : 0x1ffU);
    host_.DmaRead(table + index * 8, 8,
        [this, table, index, address, level, granule, shift, last_level,
         completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> bytes) mutable {
            const std::uint64_t address_mask = granule == 1 ?
                0x0000ffffffff0000ULL : 0x0000fffffffff000ULL;
            if (!ok || bytes.size() != 8) return completion(false, 0);
            const std::uint64_t descriptor = LoadLe<std::uint64_t>(bytes.data());
            if (!(descriptor & 1U)) {
                std::cerr << "udma-model: invalid page descriptor"
                          << " level=" << level << " granule=" << granule
                          << " table=0x" << std::hex << table
                          << " index=0x" << index
                          << " descriptor=0x" << descriptor
                          << " address=0x" << address << std::dec << '\n';
                return completion(false, 0);
            }
            const std::uint64_t output = descriptor & address_mask;
            if (level == last_level)
                return completion(true, output |
                    (address & ((std::uint64_t{1} << shift) - 1U)));
            if ((descriptor & 3U) == 1U && level != 0) {
                const std::uint64_t block = std::uint64_t{1} << shift;
                return completion(true, (output & ~(block - 1U)) |
                                         (address & (block - 1U)));
            }
            WalkTokenPageTable(output, address, level + 1, granule,
                               std::move(completion));
        });
}

void
UdmaModel::ReadToken(std::uint32_t token, std::uint64_t address,
                     std::size_t length, ReadCompletion completion,
                     std::size_t max_outstanding)
{
    auto state = std::make_shared<TokenReadState>();
    state->token = token;
    state->address = address;
    state->length = length;
    state->max_outstanding = std::max<std::size_t>(1, max_outstanding);
    state->bytes.resize(length);
    state->completion = std::move(completion);
    ContinueTokenRead(std::move(state));
}

void
UdmaModel::ContinueTokenRead(std::shared_ptr<TokenReadState> state)
{
    if (state->done || state->advancing) return;
    // Synchronous host callbacks update state; the outer pump resumes issuing.
    // On failure, drain all accepted translations/DMA before releasing buffers.
    state->advancing = true;
    const std::size_t limit = state->max_outstanding;
    while (!state->failed && state->in_flight < limit &&
           state->next_offset < state->length) {
        const std::size_t offset = state->next_offset;
        const std::uint64_t current = state->address + offset;
        const std::size_t chunk = std::min<std::size_t>(
            state->length - offset, 4096 - (current & 0xfffU));
        state->next_offset += chunk;
        ++state->in_flight;
        TranslateToken(state->token, current, false,
            [this, state, offset, chunk]
            (bool ok, std::uint64_t physical) mutable {
                if (state->failed || !ok || !physical) {
                    state->failed = true;
                    --state->in_flight;
                    return ContinueTokenRead(std::move(state));
                }
                host_.DmaRead(physical, chunk,
                    [this, state, offset, chunk]
                    (bool read_ok, std::vector<std::uint8_t> bytes) mutable {
                        if (!read_ok || bytes.size() != chunk) {
                            state->failed = true;
                        } else if (!state->failed) {
                            std::copy(bytes.begin(), bytes.end(),
                                      state->bytes.begin() + offset);
                            state->completed_bytes += chunk;
                        }
                        --state->in_flight;
                        ContinueTokenRead(std::move(state));
                    });
            });
    }
    state->advancing = false;
    if (!state->in_flight &&
        (state->failed || state->completed_bytes == state->length)) {
        state->done = true;
        auto completion = std::move(state->completion);
        if (state->failed) return completion(false, {});
        completion(true, std::move(state->bytes));
    }
}

void
UdmaModel::WriteToken(std::uint32_t token, std::uint64_t address,
                      std::vector<std::uint8_t> data, Completion completion,
                      std::size_t max_outstanding)
{
    auto state = std::make_shared<TokenWriteState>();
    state->token = token;
    state->address = address;
    state->bytes = std::move(data);
    state->max_outstanding = std::max<std::size_t>(1, max_outstanding);
    state->completion = std::move(completion);
    ContinueTokenWrite(std::move(state));
}

void
UdmaModel::ContinueTokenWrite(std::shared_ptr<TokenWriteState> state)
{
    if (state->done || state->advancing) return;
    state->advancing = true;
    const std::size_t limit = state->max_outstanding;
    while (!state->failed && state->in_flight < limit &&
           state->next_offset < state->bytes.size()) {
        const std::size_t offset = state->next_offset;
        const std::uint64_t current = state->address + offset;
        const std::size_t chunk = std::min<std::size_t>(
            state->bytes.size() - offset, 4096 - (current & 0xfffU));
        state->next_offset += chunk;
        ++state->in_flight;
        TranslateToken(state->token, current, true,
            [this, state, offset, chunk]
            (bool ok, std::uint64_t physical) mutable {
                if (state->failed || !ok || !physical) {
                    state->failed = true;
                    --state->in_flight;
                    return ContinueTokenWrite(std::move(state));
                }
                std::vector<std::uint8_t> bytes(
                    state->bytes.begin() + offset,
                    state->bytes.begin() + offset + chunk);
                host_.DmaWrite(physical, std::move(bytes),
                    [this, state, chunk](bool write_ok) mutable {
                        if (!write_ok) {
                            state->failed = true;
                        } else {
                            state->completed_bytes += chunk;
                        }
                        --state->in_flight;
                        ContinueTokenWrite(std::move(state));
                    });
            });
    }
    state->advancing = false;
    if (!state->in_flight &&
        (state->failed || state->completed_bytes == state->bytes.size())) {
        state->done = true;
        auto completion = std::move(state->completion);
        completion(!state->failed);
    }
}

} // namespace ubsim::device
