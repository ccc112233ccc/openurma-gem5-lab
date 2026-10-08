// SPDX-License-Identifier: Apache-2.0
// White-box fault injection around the official 128-byte mailbox layout.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <sstream>
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
#include "ubsim/udma_abi.h"

namespace d = ubsim::device;
constexpr std::uint64_t Csq = 0x10000, Cq = 0x20000, Ci = 0x30000,
    Context = 0x40000, Output = 0x50000;
struct Host final : d::HostInterface {
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory;
    std::deque<d::Completion> pending;
    bool defer_cqe{}, fail_output{};
    std::size_t ci_reads{};
    void DmaRead(std::uint64_t address, std::size_t length, d::ReadCompletion done) override {
        if (address == Ci) ++ci_reads;
        auto found = memory.find(address);
        const bool ok = found != memory.end() && found->second.size() == length;
        done(ok, ok ? found->second : std::vector<std::uint8_t>{});
    }
    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> bytes,
                  d::Completion done) override {
        if (address == Output && fail_output) return done(false);
        auto finish = [this, address, bytes = std::move(bytes), done = std::move(done)](bool ok) mutable {
            if (ok) memory[address] = std::move(bytes);
            done(ok);
        };
        if (address == Cq && defer_cqe) pending.push_back(std::move(finish));
        else finish(true);
    }
    void Finish(bool ok = true) {
        assert(!pending.empty());
        auto done = std::move(pending.front()); pending.pop_front(); done(ok);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Network final : d::NetworkInterface {
    void Send(d::Frame, d::Completion done) override { done(true); }
};
struct Fixture {
    Host host;
    Network network;
    d::UdmaModel::Config config;
    d::UdmaModel model;
    Fixture() : config([] { d::UdmaModel::Config cfg; cfg.identity_iova_test_mode = true; return cfg; }()),
                model(host, network, config) {
        Cache();
        host.memory[Ci] = std::vector<std::uint8_t>(4);
    }
    void Cache() {
        model.iotlb_[Ci >> 12] = Ci;
        model.iotlb_[(std::uint64_t{1} << 63) | (Cq >> 12)] = Cq;
    }
    void Mailbox(std::uint8_t command, std::uint32_t id, std::uint64_t context = 0) {
        model.ubase_command_queue_registers_[0] = Csq;
        model.ubase_command_queue_registers_[2] = 1;
        model.ubase_command_queue_registers_[5] = 0;
        model.ubase_target_producer_ = 1;
        model.ubase_busy_ = true;
        host.memory.erase(Csq);
        std::vector<std::uint8_t> descriptor(32);
        descriptor[1] = 0x70; descriptor[3] = 1;
        d::abi::Store64(descriptor.data() + 8, context);
        d::abi::Store32(descriptor.data() + 16, command | (id << 8));
        model.HandleUbaseMailbox(std::move(descriptor), 1);
    }
    void Status(std::uint16_t status = 0) {
        const auto& descriptor = host.memory.at(Csq);
        assert((descriptor[2] & 2) && descriptor[4] == (status & 255) && descriptor[5] == (status >> 8));
    }
    void Create(std::uint32_t marker = 0x12345678) {
        std::vector<std::uint8_t> context(128);
        d::abi::Store32(context.data(), 1 | static_cast<std::uint32_t>(Cq));
        d::abi::Store32(context.data() + 16, 128U << 22);
        d::abi::Store32(context.data() + 24, Ci >> 6);
        d::abi::Store32(context.data() + 80, marker);
        host.memory[Context] = context;
        Mailbox(0x24, 7, Context); Status();
    }
    std::uint32_t Word(std::size_t index) const {
        return d::abi::Load32(host.memory.at(Output).data() + index * 4);
    }
    void Query() { Mailbox(0x26, 7, Output); Status(); }
};

int main() {
    Fixture f;
    f.Create();
    f.Query();
    assert((f.Word(4) & 0x3fffffU) == 0 && (f.Word(18) & 0x3fffffU) == 0);
    assert(f.Word(20) == 0x12345678 && (f.Word(0) & 3) == 1);
    f.host.defer_cqe = true;
    std::size_t completions{};
    f.model.WriteCqe(7, false, true, 0, 0, 9, 8, 0, 0,
                    [&](bool ok) { assert(ok); ++completions; });
    assert(f.host.pending.size() == 1 && !completions);
    f.Query();
    assert((f.Word(4) & 0x3fffffU) == 1 && (f.Word(18) & 0x3fffffU) == 0);
    f.Mailbox(0x27, 7);
    assert(!f.host.memory.count(Csq));
    f.host.Finish();
    f.Status();
    assert(completions == 1 && f.model.jfc_count() == 0);
    const auto reads = f.host.ci_reads;
    f.host.memory.erase(Ci);
    f.Query();
    assert(f.host.ci_reads == reads);
    assert((f.Word(4) & 0x3fffffU) == 1 && (f.Word(18) & 0x3fffffU) == 1);
    std::stringstream saved;
    assert(f.model.SaveState(saved));
    d::UdmaModel restored(f.host, f.network, f.config);
    assert(restored.LoadState(saved));
    bool restored_ok{};
    restored.QueryJfcContext(7, [&](bool ok, std::vector<std::uint8_t> bytes) {
        restored_ok = ok;
        assert(bytes.size() == 128 && (d::abi::Load32(bytes.data() + 72) & 0x3fffffU) == 1);
    });
    assert(restored_ok && f.host.ci_reads == reads);
    f.host.memory[Ci] = std::vector<std::uint8_t>(4);
    f.Create(0xabcdef01);
    f.Query();
    assert(f.Word(20) == 0xabcdef01 && (f.Word(4) & 0x3fffffU) == 0);
    assert(f.model.jfc_query_contexts_.size() == 1);
    f.Mailbox(0x26, 999, Output); f.Status(2);
    f.Mailbox(0x26, 7); f.Status(22);
    f.host.memory.erase(Ci);
    f.Mailbox(0x26, 7, Output); f.Status(5);
    f.host.memory[Ci] = {3, 0, 0, 0};
    f.Query(); assert((f.Word(5) & 0x3fffffU) == 3);
    f.host.fail_output = true;
    f.Mailbox(0x26, 7, Output); f.Status(5);
}
