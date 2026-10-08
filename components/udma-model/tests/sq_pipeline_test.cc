// SPDX-License-Identifier: Apache-2.0
// Independent descriptor-level pipeline counterexamples, not a guest test.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <limits>
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
constexpr std::uint64_t Sq = 0x2000, Cq = 0x3000, Ci = 0x4000,
    Payload = 0x8000, Csq = 0x1000;
struct Host final : d::HostInterface {
    std::array<std::uint8_t, 65536> memory{};
    std::vector<std::uint16_t> entries;
    std::vector<std::uint8_t> statuses;
    bool hold_payload{};
    bool hold_cq{};
    std::deque<d::Completion> pending;
    void DmaRead(std::uint64_t addr, std::size_t len,
                 d::ReadCompletion done) override {
        assert(addr + len <= memory.size());
        done(true, {memory.begin() + addr, memory.begin() + addr + len});
    }
    void DmaWrite(std::uint64_t addr, std::vector<std::uint8_t> bytes,
                  d::Completion done) override {
        auto finish = [this, addr, bytes = std::move(bytes),
                       done = std::move(done)](bool ok) mutable {
            assert(addr + bytes.size() <= memory.size());
            if (ok) {
                std::copy(bytes.begin(), bytes.end(), memory.begin() + addr);
                if (addr >= Cq && addr < Cq + 1024 && bytes.size() == 64) {
                    entries.push_back(d::abi::Load32(bytes.data() + 4) & 0xffff);
                    statuses.push_back(bytes[3]);
                }
            }
            done(ok);
        };
        if ((hold_payload && addr >= Payload) ||
            (hold_cq && addr >= Cq && addr < Cq + 1024))
            pending.push_back(std::move(finish));
        else finish(true);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Net final : d::NetworkInterface {
    std::vector<d::Frame> frames;
    void Send(d::Frame frame, d::Completion done) override {
        frames.push_back(std::move(frame)); done(true);
    }
};
struct Fixture {
    Host host;
    Net net;
    d::UdmaModel model{host, net};
    Fixture() {
        model.generic_iova_token_ = 0;
        for (std::uint64_t page = 1; page < 16; ++page) {
            model.iotlb_[page] = page * 4096;
            model.iotlb_[(std::uint64_t{1} << 63) | page] = page * 4096;
        }
        auto& cq = model.jfc_contexts_[7];
        model.jfc_query_contexts_[7] = {};
        cq.depth = 16; cq.queue_iova = Cq; cq.index_iova = Ci;
        cq.moderation_count = 128;
        auto& sq = model.jetty_contexts_[9];
        sq.depth = 8; sq.queue_iova = Sq; sq.completion_queue = 7;
        sq.is_jetty = true;
        model.tp_routes_[1] = {1, 1, 0x100, 0x200, 0, true};
        model.SetLinkState(0, true);
    }
    void Post(unsigned start, unsigned opcode, unsigned count,
              unsigned ordered_index = std::numeric_limits<unsigned>::max(),
              unsigned expected_issued = 0, unsigned order_flags = 2) {
        const unsigned blocks = opcode == 3 ? 2 : 1;
        for (unsigned i = 0; i < count; ++i) {
            unsigned pos = start + i * blocks;
            std::vector<std::uint8_t> wqe(blocks * 64);
            unsigned flags = 0x20 | (opcode == 3 ? 0x40 : 0) |
                (i == ordered_index ? order_flags : 0);
            d::abi::Store32(wqe.data(), pos | (flags << 16) |
                (((pos / 8) & 1) ? 0 : (1U << 31)));
            d::abi::Store32(wqe.data() + 4, (opcode << 8) |
                (opcode == 3 ? (32U << 22) : 0));
            d::abi::Store32(wqe.data() + 8, 1 | (opcode == 6 ? 1U << 24 : 0));
            d::abi::Store32(wqe.data() + 12, 9);
            d::abi::Store64(wqe.data() + 40, Payload);
            if (opcode == 3) std::fill(wqe.begin() + 48, wqe.begin() + 80, i + 1);
            else {
                d::abi::Store32(wqe.data() + 48, 4);
                d::abi::Store64(wqe.data() + 56, Payload + i * 4);
            }
            for (unsigned b = 0; b < blocks; ++b)
                std::copy_n(wqe.begin() + b * 64, 64,
                    host.memory.begin() + Sq + ((pos + b) % 8) * 64);
        }
        model.ProcessSq(9, start + count * blocks);
        const unsigned issued = expected_issued ? expected_issued : count;
        assert(net.frames.size() == issued);
        assert(model.pending_rma_.size() == issued);
        assert(model.rma_pending_peak() >= issued);
        assert(!model.IsQuiescent());
        std::ostringstream state;
        assert(!model.SaveState(state));
    }
    void Reply(unsigned index) {
        const auto& sent = net.frames.at(index);
        d::Frame reply{};
        reply.request_id = sent.request_id;
        reply.transfer_length = sent.transfer_length;
        if (sent.operation == d::Frame::Operation::Write)
            reply.operation = d::Frame::Operation::WriteAck;
        else {
            reply.operation = d::Frame::Operation::ReadResponse;
            reply.bytes.assign(sent.transfer_length, index + 1);
        }
        model.Receive(std::move(reply));
    }
    void Destroy() {
        model.ubase_command_queue_registers_[0] = Csq;
        model.ubase_command_queue_registers_[2] = 1;
        model.ubase_target_producer_ = 1;
        model.ubase_busy_ = true;
        std::vector<std::uint8_t> cmd(32);
        cmd[1] = 0x70; cmd[3] = 1;
        d::abi::Store32(cmd.data() + 16, 0x07 | (9 << 8));
        model.HandleUbaseMailbox(std::move(cmd), 1);
        assert(model.pending_queue_destroy_ && model.jetty_count() == 1);
    }
};

int main() {
    // A later success cannot pass an older error; both release their slots.
    {
        Fixture f;
        f.Post(0, 6, 2);
        f.Reply(1);
        assert(f.host.entries.empty());
        d::Frame error{};
        error.operation = d::Frame::Operation::RmaError;
        error.request_id = f.net.frames[0].request_id;
        f.model.Receive(std::move(error));
        assert((f.host.entries == std::vector<std::uint16_t>{0, 1}));
        assert((f.host.statuses == std::vector<std::uint8_t>{5, 0}));
        assert(f.model.pending_rma_.empty());
    }
    // Failed CQ publication must not silently free accepted work/resources.
    {
        Fixture f;
        f.host.hold_cq = true;
        f.Post(0, 6, 2);
        f.Reply(1); f.Reply(0);
        assert(f.host.pending.size() == 1);
        std::ostringstream progress;
        f.model.DumpProgress(progress);
        assert(progress.str().find("publishing=1") != std::string::npos);
        assert(progress.str().find("[UDMA_JFC_PROGRESS]") != std::string::npos);
        assert(f.host.pending.size() == 1 && f.host.entries.empty());
        f.Destroy();
        auto done = std::move(f.host.pending.front()); f.host.pending.pop_front();
        done(false);
        assert(f.host.entries.empty());
        assert(f.model.pending_queue_destroy_ && f.model.jetty_count() == 1);
        std::ostringstream state; assert(!f.model.SaveState(state));
    }
    // Strong placement before or after another READ conservatively fences
    // issue. Ordinary READs overlap in the independent eight-request case.
    for (unsigned flags : {2U, 8U}) for (unsigned ordered : {0U, 1U}) {
        Fixture f;
        f.Post(0, 6, 2, ordered, 1, flags);
        assert(f.net.frames.size() == 1);
        f.Reply(0);
        assert(f.net.frames.size() == 2);
        assert(f.host.entries == std::vector<std::uint16_t>{0});
        f.Reply(1);
        assert((f.host.entries == std::vector<std::uint16_t>{0, 1}));
    }
    // A single two-block WQE itself crosses the last-to-first SQ slot.
    {
        Fixture f;
        f.model.jetty_contexts_.at(9).consumer = 7;
        f.model.jetty_contexts_.at(9).target_producer = 7;
        f.Post(7, 3, 3);
        f.Reply(2); f.Reply(1);
        assert(f.host.entries.empty());
        f.Reply(0);
        assert((f.host.entries == std::vector<std::uint16_t>{0, 2, 4}));
        assert(f.model.jetty_contexts_.at(9).consumer == 13);
        assert(f.model.sq_consumer_wraps() == 1);
    }
    // Four two-block WRITEs, reverse ACK order, three ring generations.
    {
        Fixture f;
        for (unsigned round = 0; round < 3; ++round) {
            f.Post(round * 8, 3, 4);
            for (unsigned i = 4; i-- > 1;) {
                f.Reply(i);
                assert(f.host.entries.size() == round * 4);
                assert(f.model.jetty_contexts_.at(9).consumer == round * 8);
            }
            f.Reply(0);
            assert(f.model.pending_rma_.empty());
            assert(f.model.jetty_contexts_.at(9).consumer == (round + 1) * 8);
            assert(f.host.entries.size() == (round + 1) * 4);
            for (unsigned i = 0; i < 4; ++i)
                assert(f.host.entries[round * 4 + i] == 2 * i + 1);
            d::abi::Store32(f.host.memory.data() + Ci, (round + 1) * 4);
            f.net.frames.clear();
        }
        assert(f.model.sq_consumer_wraps() == 3);
        assert(f.model.sq_wqebbs_retired() == 24);
    }
    // READ response reception is not completion: DMA can finish out of order.
    {
        Fixture f;
        f.host.hold_payload = true;
        f.Post(0, 6, 8);
        for (unsigned i = 0; i < 8; ++i) f.Reply(i);
        assert(f.host.pending.size() == 8 && f.host.entries.empty());
        std::ostringstream progress, repeated;
        f.model.DumpProgress(progress);
        f.model.DumpProgress(repeated);
        assert(progress.str() == repeated.str());
        assert(progress.str().find("entries=8 ready=0 head_ready=0") != std::string::npos);
        assert(f.host.pending.size() == 8 && f.host.entries.empty());
        f.Destroy();
        for (unsigned i = 8; i-- > 0;) {
            auto done = std::move(f.host.pending.back()); f.host.pending.pop_back();
            done(true);
            if (i) {
                assert(f.host.entries.empty() && f.model.jetty_count() == 1);
                std::ostringstream state; assert(!f.model.SaveState(state));
            }
        }
        assert(!f.model.pending_queue_destroy_ && f.model.jetty_count() == 0);
        assert(f.host.entries.size() == 8);
        for (unsigned i = 0; i < 8; ++i) {
            assert(f.host.entries[i] == i);
            for (unsigned b = 0; b < 4; ++b)
                assert(f.host.memory[Payload + 4 * i + b] == i + 1);
        }
    }
}
