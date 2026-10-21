// SPDX-License-Identifier: Apache-2.0
// White-box fault injection: expose state only in this isolated test TU.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "ubsim/udma_model.h"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace d = ubsim::device;

struct Host final : d::HostInterface {
    struct Pending { std::uint64_t address; std::size_t length; d::Completion finish; };
    std::deque<Pending> pending;
    bool synchronous{};
    bool synchronous_ok{true};
    std::size_t issued{};
    std::unordered_map<std::uint64_t, std::uint64_t> page_table;
    void DmaRead(std::uint64_t address, std::size_t length,
                 d::ReadCompletion completion) override {
        ++issued;
        if (!page_table.empty()) {
            const auto found = page_table.find(address);
            if (length != 8 || found == page_table.end())
                return completion(false, {});
            std::vector<std::uint8_t> bytes(8);
            for (std::size_t byte = 0; byte < bytes.size(); ++byte)
                bytes[byte] = static_cast<std::uint8_t>(found->second >> (8 * byte));
            return completion(true, std::move(bytes));
        }
        auto finish = [length, completion = std::move(completion)](bool ok) mutable {
            completion(ok, std::vector<std::uint8_t>(ok ? length : 0, 7));
        };
        if (synchronous) finish(synchronous_ok);
        else pending.push_back({address, length, std::move(finish)});
    }
    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> bytes,
                  d::Completion completion) override {
        ++issued;
        if (synchronous) completion(synchronous_ok);
        else pending.push_back({address, bytes.size(), std::move(completion)});
    }
    void SetInterrupt(std::uint32_t, bool) override {}
    void Finish(std::size_t index, bool ok) {
        assert(index < pending.size());
        auto finish = std::move(pending[index].finish);
        pending.erase(pending.begin() + index);
        finish(ok);
    }
};
struct Network final : d::NetworkInterface {
    void Send(d::Frame, d::Completion completion) override { completion(true); }
};
struct Fixture {
    Host host;
    Network network;
    d::UdmaModel model{host, network};
    std::size_t completions{};
    bool success{};
    void Cache(bool write, std::size_t pages) {
        for (std::size_t page = 1; page <= pages; ++page)
            model.iotlb_[(write ? (std::uint64_t{1} << 63) : 0) | page] = page * 4096;
    }
    void Start(bool write, std::size_t length = 4 * 4096, std::size_t outstanding = 2) {
        if (write) model.WriteToken(0, 4096, std::vector<std::uint8_t>(length, 1),
            [this](bool ok) { ++completions; success = ok; }, outstanding);
        else model.ReadToken(0, 4096, length,
            [this, length](bool ok, std::vector<std::uint8_t> data) {
                ++completions; success = ok;
                assert(data.size() == (ok ? length : 0));
            }, outstanding);
    }
};

int main() {
    // The official TCT TTBA field preserves bits [11:4]. On 64-KiB Linux,
    // a 48-bit user's 512-byte PGD can begin inside a 4-KiB physical page.
    {
        Fixture f;
        std::vector<std::uint8_t> context(64, 0);
        constexpr std::uint64_t root = 0xa081bc00;
        context[0] = 1;
        for (std::size_t byte = 0; byte < 8; ++byte)
            context[16 + byte] = static_cast<std::uint8_t>(root >> (8 * byte));
        bool completed = false;
        f.model.CheckMapt(std::move(context), 0xfffff3950000ULL, false,
            [&](bool ok, std::uint64_t translated_root) {
                completed = true;
                assert(ok && translated_root == root);
            });
        assert(completed);
    }
    // A 64-KiB TCT granule selects three levels with 13-bit table indexes.
    {
        Fixture f;
        constexpr std::uint64_t address = 0x123456789abcULL;
        constexpr std::uint64_t root = 0x10000;
        constexpr std::uint64_t l2 = 0x20000;
        constexpr std::uint64_t l3 = 0x30000;
        constexpr std::uint64_t physical = 0xabcd0000;
        f.host.page_table[root + ((address >> 42) & 0x1fffU) * 8] = l2 | 3U;
        f.host.page_table[l2 + ((address >> 29) & 0x1fffU) * 8] = l3 | 3U;
        f.host.page_table[l3 + ((address >> 16) & 0x1fffU) * 8] = physical | 3U;
        bool completed = false;
        f.model.WalkTokenPageTable(root, address, 0, 1,
            [&](bool ok, std::uint64_t translated) {
                completed = true;
                assert(ok && translated == (physical | (address & 0xffffU)));
            });
        assert(completed && f.host.issued == 3);
    }
    for (bool write : {false, true}) {
        // A failed DMA must not complete while another accepted DMA is live.
        for (std::size_t failed_index : {0U, 1U}) {
            Fixture f;
            f.Cache(write, 4);
            f.Start(write);
            assert(f.host.pending.size() == 2);
            f.host.Finish(failed_index, false);
            assert(f.completions == 0 && f.host.issued == 2);
            f.host.Finish(0, true);
            assert(f.completions == 1 && !f.success && f.host.pending.empty());
            assert(f.host.issued == 2);
        }
        // Immediate translation failure still drains an earlier data DMA.
        {
            Fixture f;
            f.Cache(write, 1);
            f.Start(write);
            assert(f.completions == 0 && f.host.pending.size() == 1);
            f.host.Finish(0, true);
            assert(f.completions == 1 && !f.success && f.host.issued == 1);
        }
        // Out-of-order success refills the window, then completes once drained.
        {
            Fixture f;
            f.Cache(write, 4);
            f.Start(write);
            f.host.Finish(1, true);
            assert(f.completions == 0 && f.host.issued == 3);
            f.host.Finish(1, true);
            assert(f.completions == 0 && f.host.issued == 4);
            f.host.Finish(0, true);
            assert(f.completions == 0);
            f.host.Finish(0, true);
            assert(f.completions == 1 && f.success && f.host.pending.empty());
        }
        // Deferred translation failure drains both payload and other page walks.
        {
            Fixture f;
            f.Cache(write, 1);
            f.model.ummu_registers_[0x71] = 0x90; // TECT = 0x9000
            f.Start(write, 4 * 4096, 3);
            assert(f.host.pending.size() == 3 && f.host.pending[1].address == 0x9000);
            f.host.Finish(1, false);
            assert(f.completions == 0 && f.host.issued == 3);
            f.host.Finish(0, true);
            assert(f.completions == 0);
            f.host.Finish(0, false);
            assert(f.completions == 1 && !f.success && f.host.issued == 3);
        }
        // Inline callbacks must not recursively consume stack per DMA page.
        {
            Fixture f;
            f.Cache(write, 4096);
            f.host.synchronous = true;
            f.Start(write, 4096 * 4096, 512);
            assert(f.completions == 1 && f.success && f.host.issued == 4096);
        }
        {
            Fixture f;
            f.Cache(write, 4);
            f.host.synchronous = true;
            f.host.synchronous_ok = false;
            f.Start(write);
            assert(f.completions == 1 && !f.success && f.host.issued == 1);
        }
        {
            Fixture f;
            f.Start(write, 0);
            assert(f.completions == 1 && f.success && f.host.issued == 0);
        }
    }
}
