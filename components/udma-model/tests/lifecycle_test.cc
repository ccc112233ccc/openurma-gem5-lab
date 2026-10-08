// SPDX-License-Identifier: Apache-2.0
// Independent white-box lifecycle fault injection; no guest-driver claim.
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
#include "ubsim/udma_abi.h"

namespace d = ubsim::device;
constexpr std::uint64_t Csq = 0x10000, Cq = 0x20000, Ci = 0x30000;
struct Host final : d::HostInterface {
    struct Write { std::uint64_t address; std::vector<std::uint8_t> bytes; d::Completion done; };
    std::deque<Write> pending;
    std::vector<std::uint64_t> addresses;
    std::vector<std::uint32_t> vectors;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory;
    void DmaRead(std::uint64_t address, std::size_t length, d::ReadCompletion done) override {
        assert(address == Ci && length == 4);
        done(true, std::vector<std::uint8_t>(4));
    }
    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> bytes, d::Completion done) override {
        addresses.push_back(address);
        if (address >= Cq && address < Cq + 8192) {
            pending.push_back({address, std::move(bytes), std::move(done)});
            return;
        }
        memory[address] = std::move(bytes);
        done(true);
    }
    void Finish(bool ok) {
        assert(!pending.empty());
        auto write = std::move(pending.front());
        pending.pop_front();
        if (ok) memory[write.address] = std::move(write.bytes);
        write.done(ok);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
    void PulseInterrupt(std::uint32_t vector) override { vectors.push_back(vector); }
};
struct Network final : d::NetworkInterface {
    void Send(d::Frame, d::Completion done) override { done(true); }
};
struct Fixture {
    Host host;
    Network net;
    d::UdmaModel model{host, net};
    std::vector<bool> results;
    Fixture() {
        model.generic_iova_token_ = 0;
        model.iotlb_[(std::uint64_t{1} << 63) | (Csq >> 12)] = Csq;
        model.iotlb_[(std::uint64_t{1} << 63) | (Cq >> 12)] = Cq;
        model.iotlb_[Ci >> 12] = Ci;
        model.ubase_command_queue_registers_[0] = Csq;
        model.ubase_command_queue_registers_[2] = 1;
        model.ubase_target_producer_ = 1;
        model.ubase_busy_ = true;
        auto& jfc = model.jfc_contexts_[7];
        model.jfc_query_contexts_[7] = {};
        jfc.queue_iova = Cq;
        jfc.index_iova = Ci;
        jfc.depth = 64;
        jfc.moderation_count = 128;
    }
    void Post(std::uint32_t local_id) {
        model.WriteCqe(7, false, true, 0, 0, local_id, 8, 0, 0,
            [this](bool ok) { results.push_back(ok); });
    }
    void Destroy(std::uint8_t opcode = 0x27, std::uint8_t id = 7) {
        std::vector<std::uint8_t> descriptor(32);
        descriptor[1] = 0x70;
        descriptor[3] = 1;
        descriptor[16] = opcode;
        descriptor[17] = id;
        model.HandleUbaseMailbox(std::move(descriptor), 1);
    }
};

int main() {
    // Timer-triggered notifications outlive the CQ publication callback.
    // JFC teardown must drain these too before the driver can reuse its ID.
    {
        Fixture f;
        f.model.iotlb_[(std::uint64_t{1} << 63) | ((Cq + 4096) >> 12)] = Cq + 4096;
        f.model.ceqs_[2] = {Cq + 4096, 64, 0, 5};
        f.model.jfc_ceqs_[7] = 2;
        f.model.jfc_contexts_[7].pending_completions = 1;
        f.model.jfc_contexts_[7].moderation_deadline_ps = 1;
        f.model.AdvanceTime(1);
        assert(f.host.pending.size() == 1 && f.model.jfc_publications_.empty());
        f.Destroy();
        assert(!f.host.memory.count(Csq) && f.model.jfc_count() == 1);
        f.host.Finish(true);
        assert(f.host.memory.at(Csq)[2] & 2);
        assert(f.model.jfc_count() == 0 && !f.model.draining_jfc_id_);
        assert(f.host.pending.empty());
    }
    // CEQ A destruction is independent of a delayed event on CEQ B.
    for (bool first_ok : {true, false}) {
        Fixture f;
        f.model.iotlb_[(std::uint64_t{1} << 63) | ((Cq + 4096) >> 12)] = Cq + 4096;
        f.model.ceqs_[1] = {Cq, 64, 0, 3};
        f.model.ceqs_[2] = {Cq + 4096, 64, 0, 5};
        f.model.jfc_ceqs_[101] = 1;
        f.model.jfc_ceqs_[202] = 2;
        f.model.EmitCompletionEvent(101, [&f](bool ok) { f.results.push_back(ok); });
        f.model.EmitCompletionEvent(202, [&f](bool ok) { f.results.push_back(ok); });
        assert(f.host.pending.size() == 1);
        f.Destroy(0x47, 1);
        assert(f.model.ceq_enabled(1) && !f.host.memory.count(Csq));
        f.host.Finish(first_ok);
        assert(f.results.size() == 1 && f.results[0] == first_ok);
        assert(!f.model.ceq_enabled(1) && f.model.ceq_enabled(2));
        assert(f.host.memory.at(Csq)[2] & 2);
        assert(f.host.pending.size() == 1 && f.host.pending.front().address == Cq + 4096);
        assert(f.model.CeqBusy(2));
        f.host.Finish(true);
        assert(f.results.size() == 2 && f.results[1]);
        assert(!f.model.CeqBusy(2) && f.model.ceqs_.at(2).producer == 1);
        assert(f.host.vectors.back() == 5);
        if (first_ok) assert(f.host.vectors.front() == 3);
    }
    for (bool first_ok : {true, false}) {
        Fixture f;
        f.Post(101);
        f.Post(202);
        assert(f.host.pending.size() == 1 && f.host.pending.front().address == Cq);
        assert(f.model.JfcPublicationBusy(7));
        f.Destroy();
        assert(f.model.draining_jfc_id_ == 7 && f.model.jfc_count() == 1);
        assert(!f.host.memory.count(Csq));
        f.host.Finish(first_ok);
        assert(f.results.size() == 1 && f.results[0] == first_ok);
        assert(f.host.pending.size() == 1);
        assert(f.host.pending.front().address == Cq + (first_ok ? 64 : 0));
        assert(!f.host.memory.count(Csq) && f.model.jfc_count() == 1);
        f.host.Finish(true);
        assert(f.results.size() == 2 && f.results[1]);
        assert(f.host.pending.empty() && !f.model.JfcPublicationBusy(7));
        assert(f.model.jfc_count() == 0 && !f.model.draining_jfc_id_);
        assert(f.host.memory.at(Csq)[2] & 2);
        assert(f.host.memory.at(Csq)[4] == 0);
        assert(f.model.ubase_command_queue_registers_[5] == 1);
        // The second distinct Jetty CQE is retained, not overwritten by an old callback.
        const auto& last = f.host.memory.at(Cq + (first_ok ? 64 : 0));
        const auto expected = d::abi::MakeCqe(false, true, true, 0, 0, 202, 8, 0, 0);
        assert(last == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    }
}
