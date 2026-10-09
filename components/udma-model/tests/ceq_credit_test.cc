// SPDX-License-Identifier: Apache-2.0
// White-box boundary/fault tests; this is not full-system driver acceptance.
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

namespace d = ubsim::device;
constexpr std::uint64_t Csq = 0x10000, Cq = 0x20000, Ci = 0x30000;
constexpr std::uint64_t Eq = 0x40000, Msi = 0x50000;
struct Host final : d::HostInterface {
    struct Write { std::uint64_t address; std::vector<std::uint8_t> bytes; d::Completion done; };
    std::deque<Write> pending;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory;
    bool delay_cq{}, delay_eq{}, delay_msi{};
    std::uint32_t cq_consumer{};
    std::size_t eq_writes{}, cq_writes{}, msi_writes{};
    void DmaRead(std::uint64_t address, std::size_t length, d::ReadCompletion done) override {
        assert(address == Ci && length == 4);
        std::vector<std::uint8_t> bytes(4);
        for (unsigned i = 0; i < 4; ++i) bytes[i] = cq_consumer >> (8 * i);
        done(true, std::move(bytes));
    }
    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> bytes, d::Completion done) override {
        const bool cq = address >= Cq && address < Cq + 8192;
        const bool eq = address >= Eq && address < Eq + 8192;
        cq_writes += cq; eq_writes += eq; msi_writes += address == Msi;
        if ((cq && delay_cq) || (eq && delay_eq) || (address == Msi && delay_msi)) {
            pending.push_back({address, std::move(bytes), std::move(done)});
            return;
        }
        memory[address] = std::move(bytes);
        done(true);
    }
    void Finish(bool ok = true) {
        assert(!pending.empty());
        auto write = std::move(pending.front()); pending.pop_front();
        if (ok) memory[write.address] = std::move(write.bytes);
        write.done(ok);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Network final : d::NetworkInterface {
    void Send(d::Frame, d::Completion done) override { done(true); }
};
struct Fixture {
    Host host; Network net; d::UdmaModel model;
    std::vector<bool> results;
    static d::UdmaModel::Config Config(std::uint32_t slots) {
        d::UdmaModel::Config config; config.completion_event_slots = slots;
        return config;
    }
    explicit Fixture(std::uint32_t slots = 64) : model(host, net, Config(slots)) {
        model.generic_iova_token_ = 0;
        for (const auto address : {Csq, Cq, Cq + 4096, Eq, Eq + 4096, Msi})
            model.iotlb_[(std::uint64_t{1} << 63) | (address >> 12)] = address;
        model.iotlb_[Ci >> 12] = Ci;
        model.ceqs_[2] = {Eq, 2, 0, 0};
        for (std::uint32_t id : {7U, 8U}) {
            auto& jfc = model.jfc_contexts_[id];
            model.jfc_query_contexts_[id] = {};
            jfc.queue_iova = Cq + (id - 7) * 4096;
            jfc.index_iova = Ci; jfc.depth = 64; jfc.moderation_count = 1;
            model.jfc_ceqs_[id] = 2;
        }
        model.ubios_endpoint_config_[0x40c04] = 1;
        model.ubios_endpoint_config_[0x40c0c] = 1;
        model.ubios_endpoint_config_[0x40c14] = Msi;
        model.msi_iova_ = Msi; model.msi_physical_ = Msi;
    }
    void Post(std::uint32_t id = 7) {
        model.WriteCqe(id, false, true, 0, 0, 9, 8, 0, 0,
            [this](bool ok) { results.push_back(ok); });
    }
    void AssertBusy() {
        assert(!model.IsQuiescent());
        std::ostringstream checkpoint; assert(!model.SaveState(checkpoint));
    }
    void Credit(std::uint32_t ci) {
        assert(model.WriteMmio(0x200000, 8,
            2U | (std::uint64_t{2} << 16) | (std::uint64_t(ci & 0xffffffU) << 32)));
    }
    void Destroy(std::uint8_t command, std::uint32_t id) {
        model.ubase_command_queue_registers_[0] = Csq;
        model.ubase_command_queue_registers_[2] = 1;
        model.ubase_target_producer_ = 1; model.ubase_busy_ = true;
        std::vector<std::uint8_t> descriptor(32);
        descriptor[1] = 0x70; descriptor[3] = 1;
        descriptor[16] = command; descriptor[17] = id;
        model.HandleUbaseMailbox(std::move(descriptor), 1);
    }
};

int main() {
    // Current JFC overflow is fail-closed, NOT CEQ-like credit backpressure.
    // Do not silently equate a held CQ DMA callback with a full hardware CQ.
    {
        Fixture f;
        f.model.ceqs_.clear();
        f.model.jfc_contexts_.at(7).depth = 2;
        auto& sq = f.model.jetty_contexts_[9];
        sq.depth = 8; sq.completion_queue = 7; sq.is_jetty = true;
        auto state = std::make_shared<d::UdmaModel::SqPipeline>();
        f.model.sq_pipelines_[9] = state;
        // Keep fetch ownership while injecting three already-executed WQEs.
        state->fetching = true;
        sq.busy = true;
        for (unsigned i = 0; i < 2; ++i) {
            f.model.ReserveSq(9, 1, false);
            f.model.CompleteSq(9, i + 1, 1, i, 0, 8, 0, true);
        }
        assert(f.host.cq_writes == 2 && sq.consumer == 2);
        const auto first = f.host.memory.at(Cq), second = f.host.memory.at(Cq + 64);
        const auto errors = f.model.ubase_errors();
        f.model.ReserveSq(9, 1, false);
        f.model.CompleteSq(9, 3, 1, 2, 0, 8, 0, true);
        state->fetching = false;
        assert(f.host.cq_writes == 2 && sq.consumer == 2);
        assert(f.model.jfc_contexts_.at(7).producer == 2);
        assert(f.host.memory.at(Cq) == first && f.host.memory.at(Cq + 64) == second);
        assert(state->publishing && state->completions.size() == 1);
        assert(f.model.ubase_errors() == errors + 1);
        assert(f.model.completion_event_slots_in_use_ == 0);
        f.AssertBusy();
        f.host.cq_consumer = 1;
        f.model.AdvanceTime(f.model.now_ps_ + 1000000);
        f.model.PumpCompletionWork();
        f.model.RetireSq(9);
        assert(f.host.cq_writes == 2 && sq.consumer == 2 && state->publishing);
        f.AssertBusy(); // No automatic resume merely because guest CI advanced.
    }
    // A quiet device may still have guest-unconsumed entries. Persist those
    // credits exactly; reject old checkpoints without the consumer field.
    {
        Fixture f; f.Post(); f.Post(); f.Credit(1);
        std::ostringstream out; assert(f.model.SaveState(out));
        Fixture restored;
        std::istringstream in(out.str()); assert(restored.model.LoadState(in));
        assert(restored.model.ceqs_.at(2).producer == 2);
        assert(restored.model.ceqs_.at(2).consumer == 1);
        restored.Credit(2);
        assert(restored.model.ceqs_.at(2).consumer == 2);
        auto old = out.str(); old.at(sizeof(std::uint64_t)) = 2;
        std::istringstream legacy(old); assert(!restored.model.LoadState(legacy));
    }
    // Hardware ring fullness is distinct from the software notification queue:
    // no old event may be overwritten while its guest consumer is delayed.
    {
        Fixture f;
        f.Post(7); f.Post(8); f.Post(7); f.Post(8);
        assert(f.results.size() == 4 && f.host.cq_writes == 4);
        assert(f.host.eq_writes == 2 && f.model.ceqs_.at(2).producer == 2);
        const auto first = f.host.memory.at(Eq), second = f.host.memory.at(Eq + 64);
        f.AssertBusy();
        const auto rejects = f.model.ceq_consumer_rejects();
        assert(f.model.WriteMmio(0x200000, 8, 99U | (std::uint64_t{2} << 16) |
            (std::uint64_t{1} << 32))); // unknown CEQ
        assert(f.model.WriteMmio(0x200000, 8, 2U | (std::uint64_t{2} << 16) |
            (std::uint64_t{1} << 32) | (std::uint64_t{1} << 60))); // reserved bits
        assert(f.model.ceq_consumer_rejects() == rejects + 2);
        assert(f.model.ceqs_.at(2).consumer == 0);
        f.Credit(3); // Never grant credit for an unpublished event.
        assert(f.model.ceqs_.at(2).consumer == 0 && f.host.eq_writes == 2);
        f.Credit(1);
        assert(f.model.ceqs_.at(2).consumer == 1 && f.host.eq_writes == 3);
        assert(f.host.memory.at(Eq + 64) == second);
        assert(f.host.memory.at(Eq) != first); // owner bit changes on ring wrap
        f.Credit(1); // duplicate is idempotent
        f.Credit(0); // stale consumer is not a giant credit grant
        assert(f.host.eq_writes == 3 && f.model.ceqs_.at(2).consumer == 1);
        f.Credit(2);
        assert(f.host.eq_writes == 4 && f.model.ceqs_.at(2).producer == 4);
        assert(f.model.IsQuiescent());
    }
    // Published producer/consumer straddle the official 24-bit doorbell wrap.
    {
        Fixture f;
        f.model.ceqs_.at(2).consumer = 0xfffffe;
        f.model.ceqs_.at(2).producer = 0xfffffe;
        f.Post(); f.Post(); f.Post();
        assert(f.host.eq_writes == 2);
        f.Credit(0xffffff);
        assert(f.host.eq_writes == 3);
        f.Credit(0);
        assert(f.model.ceqs_.at(2).consumer == 0x1000000);
        f.Credit(0xfffffe);
        assert(f.model.ceqs_.at(2).consumer == 0x1000000);
        f.Credit(2); // producer is 0x1000001
        assert(f.model.ceqs_.at(2).consumer == 0x1000000);
        f.Credit(1);
        assert(f.model.ceqs_.at(2).consumer == 0x1000001);
    }
    // An in-flight CEQ write has reserved a slot, but is not yet consumable.
    {
        Fixture f; f.host.delay_eq = true; f.Post();
        assert(f.model.ceqs_.at(2).producer == 0);
        f.Credit(1);
        assert(f.model.ceqs_.at(2).consumer == 0);
        f.AssertBusy(); f.host.Finish(); f.Credit(1);
        assert(f.model.ceqs_.at(2).consumer == 1 && f.model.IsQuiescent());
    }
    // Visibility can precede the host adapter's DMA acknowledgement. A guest
    // may consume that entry and ring CI once, before the callback arrives.
    // Do not lose this valid credit; provisional CI must commit only on success.
    for (const bool dma_ok : {true, false}) {
        Fixture f; f.host.delay_eq = true; f.Post();
        f.host.memory[Eq] = f.host.pending.front().bytes;
        f.Credit(1);
        f.host.Finish(dma_ok);
        assert(f.model.ceqs_.at(2).consumer == (dma_ok ? 1U : 0U));
        assert(f.model.ceqs_.at(2).producer == (dma_ok ? 1U : 0U));
        if (!dma_ok) {
            const auto writes = f.host.eq_writes;
            f.Post();
            assert(f.host.eq_writes == writes); // consumed failed DMA is fail-closed
            f.AssertBusy();
        }
    }
    // A stalled/full CEQ must not head-of-line block an unrelated ring.
    {
        Fixture f;
        f.model.ceqs_[3] = {Eq + 4096, 2, 0, 0};
        f.model.jfc_ceqs_[8] = 3;
        f.Post(7); f.Post(7); f.Post(7); // CEQ2 is full, one notification queued
        f.Post(8);
        assert(f.model.ceqs_.at(2).producer == 2);
        assert(f.model.ceqs_.at(3).producer == 1);
        assert(f.host.eq_writes == 3);
        f.AssertBusy(); f.Credit(1);
        assert(f.model.IsQuiescent());
    }
    // Queue teardown cannot acknowledge while a full-ring notification is
    // queued: the pending event still names the old JFC/CEQ memory.
    for (const bool destroy_ceq : {false, true}) {
        Fixture f;
        f.Post(); f.Post(); f.Post();
        f.Destroy(destroy_ceq ? 0x47 : 0x27, destroy_ceq ? 2 : 7);
        assert(!f.host.memory.count(Csq)); f.AssertBusy();
        f.Credit(1);
        assert(f.host.memory.count(Csq));
        assert(destroy_ceq ? !f.model.ceq_enabled(2) : f.model.jfc_count() == 1);
    }
    // The global budget applies before CQ DMA; delayed MSI cannot create an
    // unbounded committed-CQE notification backlog. Both JFCs make progress.
    {
        Fixture f(2); f.host.delay_msi = true;
        f.Post(7); f.Post(8); f.Post(7); f.Post(8);
        assert(f.host.cq_writes == 2 && f.results.size() == 2);
        f.AssertBusy();
        f.host.Finish();
        assert(f.host.cq_writes == 3 && f.results.size() == 3);
        f.host.Finish();
        assert(f.host.cq_writes == 4 && f.results.size() == 4);
        assert(f.model.jfc_contexts_.at(7).producer == 2);
        assert(f.model.jfc_contexts_.at(8).producer == 2);
        assert(f.host.eq_writes == 2); // still full despite MSI completions
        f.Credit(2);
        while (!f.host.pending.empty()) f.host.Finish();
        assert(f.host.eq_writes == 4 && f.model.IsQuiescent());
    }
    // CQ completion must not wait for a delayed MSI. Its outstanding ownership
    // must nevertheless block both checkpoint and JFC destruction.
    {
        Fixture f; f.host.delay_msi = true;
        f.Post(); f.Post();
        assert(f.results == std::vector<bool>({true, true}));
        assert(f.host.cq_writes == 2 && f.model.jfc_contexts_.at(7).producer == 2);
        f.AssertBusy(); f.Destroy(0x27, 7);
        assert(f.model.jfc_count() == 2 && !f.host.memory.count(Csq));
        while (!f.host.pending.empty()) f.host.Finish();
        assert(f.model.jfc_count() == 1 && f.host.memory.count(Csq));
        assert(f.model.IsQuiescent());
    }
    // One perpetually busy JFC must not monopolize every released global
    // reservation while another JFC already waits. Require bounded service.
    {
        Fixture f(1); f.host.delay_msi = true;
        f.model.ceqs_.at(2).depth = 64;
        f.Post(7); f.Post(8);
        for (int i = 0; i < 16; ++i) f.Post(7);
        f.host.Finish(); f.host.Finish();
        assert(f.model.jfc_contexts_.at(8).producer != 0);
        while (!f.host.pending.empty()) f.host.Finish();
        assert(f.model.IsQuiescent());
    }
    // A timer blocked on the finite notification budget is awaiting an
    // external completion, not a runnable past-due timer. Otherwise a process
    // adapter can spin forever without receiving the MSI acknowledgement.
    {
        Fixture f(1); f.host.delay_msi = true; f.Post(7);
        auto& jfc = f.model.jfc_contexts_.at(8);
        jfc.pending_completions = 1; jfc.moderation_deadline_ps = 1;
        f.model.AdvanceTime(1);
        assert(f.model.NextEventTime() > 1);
        assert(jfc.pending_completions == 1 && f.host.eq_writes == 1);
        f.host.Finish();
        assert(f.host.eq_writes == 2 && jfc.pending_completions == 0);
        assert(f.host.pending.size() == 1);
        f.host.Finish(); f.model.AdvanceTime(10);
        assert(f.host.eq_writes == 2 && f.model.IsQuiescent());
    }
    // Synchronous CQ callback may itself destroy the JFC. Notification
    // ownership must have been registered before handing control to caller.
    {
        Fixture f; f.host.delay_eq = true;
        f.model.WriteCqe(7, false, true, 0, 0, 9, 8, 0, 0,
            [&f](bool ok) {
                assert(ok); f.Destroy(0x27, 7);
                assert(f.model.jfc_count() == 2 && !f.host.memory.count(Csq));
            });
        f.AssertBusy(); f.host.Finish();
        assert(f.model.jfc_count() == 1 && f.host.memory.count(Csq));
        assert(f.model.IsQuiescent());
    }
    // Notification failure cannot undo/replay an already visible CQE.
    for (const bool fail_msi : {false, true}) {
        Fixture f;
        f.host.delay_eq = !fail_msi; f.host.delay_msi = fail_msi;
        f.Post();
        assert(f.results == std::vector<bool>({true}));
        f.AssertBusy(); f.host.Finish(false);
        assert(f.model.jfc_contexts_.at(7).producer == 1);
        assert(f.host.cq_writes == 1 && f.results.size() == 1);
        assert(f.model.IsQuiescent());
    }
    // Conversely failed CQ DMA must never publish a CEQ event or MSI.
    {
        Fixture f; f.host.delay_cq = true; f.Post();
        assert(f.results.empty()); f.AssertBusy(); f.host.Finish(false);
        assert(f.results == std::vector<bool>({false}));
        assert(f.host.eq_writes == 0 && f.host.msi_writes == 0);
        assert(f.model.jfc_contexts_.at(7).producer == 0);
        assert(f.model.IsQuiescent());
    }
    // CEQ destruction also waits on accepted CQ publication. When that CQ
    // produces no notification (failure or moderation), draining the CQ must
    // still wake the CEQ destroy command rather than wait for a nonexistent EQ.
    for (const bool cq_ok : {false, true}) {
        Fixture f; f.host.delay_cq = true;
        f.model.jfc_contexts_.at(7).moderation_count = 16;
        f.Post(); f.Destroy(0x47, 2);
        assert(f.model.ceq_enabled(2) && !f.host.memory.count(Csq));
        f.host.Finish(cq_ok);
        assert(f.results == std::vector<bool>({cq_ok}));
        assert(f.host.eq_writes == 0 && f.host.msi_writes == 0);
        assert(!f.model.ceq_enabled(2) && f.host.memory.count(Csq));
    }
}
