// SPDX-License-Identifier: Apache-2.0
// Descriptor-level acceptance: 512 distinct WQEs, not 512 segments of one WQE.
// This fixture does not claim official-provider/full-system coverage.
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
#include <unordered_set>
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
constexpr std::uint64_t Sq = 0x10000, Cq = 0x30000, Ci = 0x50000,
    Payload = 0x60000, OtherSq = 0x70000, OtherCq = 0x80000;
struct Host final : d::HostInterface {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x100000);
    std::vector<std::uint16_t> entries;
    std::vector<std::uint8_t> statuses;
    unsigned payload_reads{};
    bool hold_payload = false, hold_cq = false;
    std::deque<d::Completion> payload, cq;
    void DmaRead(std::uint64_t a, std::size_t n, d::ReadCompletion done) override {
        assert(a + n <= memory.size());
        if (a >= Payload && a < Payload + 8192) ++payload_reads;
        done(true, {memory.begin() + a, memory.begin() + a + n});
    }
    void DmaWrite(std::uint64_t a, std::vector<std::uint8_t> b,
                  d::Completion done) override {
        assert(a + b.size() <= memory.size());
        const bool main_cq = a >= Cq && a < Cq + 65536;
        auto finish = [this, a, main_cq, b = std::move(b),
                       done = std::move(done)](bool ok) mutable {
            if (ok) {
                std::copy(b.begin(), b.end(), memory.begin() + a);
                if (main_cq) {
                    entries.push_back(d::abi::Load32(b.data() + 4) & 0xffff);
                    statuses.push_back(b[3]);
                }
            }
            done(ok);
        };
        if (hold_payload && a >= Payload && a < Payload + 4096)
            payload.push_back(std::move(finish));
        else if (hold_cq && main_cq) cq.push_back(std::move(finish));
        else finish(true);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Net final : d::NetworkInterface {
    std::vector<d::Frame> frames;
    void Send(d::Frame f, d::Completion done) override {
        frames.push_back(std::move(f)); done(true);
    }
};
struct Fixture {
    Host host;
    Net net;
    d::UdmaModel model{host, net};
    unsigned opcode, blocks, depth;
    explicit Fixture(unsigned op) : opcode(op), blocks(op == 3 ? 2 : 1), depth(512 * blocks) {
        model.generic_iova_token_ = 0;
        for (std::uint64_t p = 1; p < 256; ++p) {
            model.iotlb_[p] = p * 4096;
            model.iotlb_[(std::uint64_t{1} << 63) | p] = p * 4096;
        }
        for (unsigned id : {7U, 8U}) {
            auto& c = model.jfc_contexts_[id];
            model.jfc_query_contexts_[id] = {};
            c.depth = 1024; c.queue_iova = id == 7 ? Cq : OtherCq;
            c.index_iova = Ci + (id - 7) * 4; c.moderation_count = 4096;
        }
        for (unsigned id : {9U, 10U}) {
            auto& s = model.jetty_contexts_[id];
            s.depth = depth; s.queue_iova = id == 9 ? Sq : OtherSq;
            s.completion_queue = id == 9 ? 7 : 8; s.is_jetty = true;
        }
        model.tp_routes_[1] = {1, 1, 0x100, 0x200, 0, true};
        model.tp_routes_[2] = {2, 2, 0x100, 0x300, 1, true};
        model.SetLinkState(0, true); model.SetLinkState(1, true);
    }
    void Post(unsigned start, unsigned count, unsigned id = 9) {
        const auto base = id == 9 ? Sq : OtherSq;
        for (unsigned i = 0; i < count; ++i) {
            const auto pos = start + i * blocks;
            std::vector<std::uint8_t> w(blocks * 64);
            const auto flags = 0x20U | (opcode == 3 ? 0x40U : 0U);
            d::abi::Store32(w.data(), pos | (flags << 16) |
                (((pos / depth) & 1) ? 0 : (1U << 31)));
            d::abi::Store32(w.data() + 4, (opcode << 8) |
                (opcode == 3 ? 32U << 22 : 0));
            d::abi::Store32(w.data() + 8, (id == 9 ? 1 : 2) |
                (opcode == 6 ? 1U << 24 : 0));
            d::abi::Store32(w.data() + 12, id);
            d::abi::Store64(w.data() + 40, Payload);
            if (opcode == 3) std::fill(w.begin() + 48, w.begin() + 80, (pos / blocks) % 251 + 1);
            else {
                d::abi::Store32(w.data() + 48, 4);
                d::abi::Store64(w.data() + 56, Payload + (id == 9 ? 0 : 4096) +
                    ((pos / blocks) % 512) * 4);
            }
            for (unsigned b = 0; b < blocks; ++b)
                std::copy_n(w.begin() + b * 64, 64,
                    host.memory.begin() + base + ((pos + b) % depth) * 64);
        }
        model.ProcessSq(id, start + count * blocks);
    }
    void Reply(std::size_t i) {
        const auto sent = net.frames.at(i);
        d::Frame r{};
        r.request_id = sent.request_id; r.transfer_length = sent.transfer_length;
        r.operation = opcode == 3 ? d::Frame::Operation::WriteAck : d::Frame::Operation::ReadResponse;
        if (opcode == 6) r.bytes.assign(sent.transfer_length, i % 251 + 1);
        model.Receive(std::move(r));
    }
    void NoCheckpoint() {
        std::ostringstream s; assert(!model.SaveState(s));
    }
};

int main() {
    // Official CQE DW0 uses raw status at bits 31:24 and substatus at
    // 23:16. udma_u_get_cr_status() maps [2][1] to URMA_CR_LOC_LEN_ERR.
    // The oversized SEND must be rejected before payload DMA/network send,
    // then a later valid descriptor on the same SQ must still retire.
    for (unsigned opcode : {0U, 1U}) {
        Fixture f(opcode);
        std::fill_n(f.host.memory.begin() + Payload, 4096, 0x5a);
        auto post = [&](unsigned position, unsigned length) {
            std::array<std::uint8_t, 64> w{};
            d::abi::Store32(w.data(), position | (0x20U << 16) | (1U << 31));
            d::abi::Store32(w.data() + 4, opcode << 8);
            d::abi::Store32(w.data() + 8, 1U | (1U << 24));
            d::abi::Store32(w.data() + 12, 9);
            d::abi::Store32(w.data() + 48, length);
            // Invalid address proves no payload DMA occurs for length error.
            d::abi::Store64(w.data() + 56, length > 4096 ? 0xdead0000 : Payload);
            std::copy(w.begin(), w.end(), f.host.memory.begin() + Sq + position * 64);
            f.model.ProcessSq(9, position + 1);
        };
        post(0, 4096);
        assert(f.net.frames.size() == 1 && f.host.entries.size() == 1);
        assert(f.net.frames[0].bytes == std::vector<std::uint8_t>(4096, 0x5a));
        assert(f.host.memory[Cq + 3] == 0 && f.host.memory[Cq + 2] == 0);
        const auto reads = f.host.payload_reads;
        post(1, 4097);
        assert(f.net.frames.size() == 1 && f.host.payload_reads == reads);
        assert(f.host.entries.size() == 2);
        assert(f.host.memory[Cq + 64 + 3] == 2);
        assert(f.host.memory[Cq + 64 + 2] == 1);
        assert(f.model.jetty_contexts_.at(9).consumer == 2);
        assert(f.model.sq_pipelines_.at(9)->completions.empty());
        assert(f.model.pending_rma_.empty());
        post(2, 4096);
        assert(f.net.frames.size() == 2 && f.host.entries.size() == 3);
        assert(f.net.frames[1].bytes == std::vector<std::uint8_t>(4096, 0x5a));
        assert(f.host.memory[Cq + 128 + 3] == 0 && f.host.memory[Cq + 128 + 2] == 0);
        assert(f.model.jetty_contexts_.at(9).consumer == 3);
        assert(f.model.sq_pipelines_.at(9)->completions.empty());
        assert(f.model.completion_event_slots_in_use_ == 0);
        assert(f.model.sq_completions() == 3 && f.model.ubase_errors() == 0);
    }
    for (unsigned op : {3U, 6U}) {
        Fixture f(op);
        // 512 separate descriptors accepted in eight batches, without ACKs.
        for (unsigned b = 0; b < 8; ++b) f.Post(b * 64 * f.blocks, 64);
        assert(f.net.frames.size() == 512 && f.model.pending_rma_.size() == 512);
        assert(f.model.sq_pipelines_.at(9)->completions.size() == 512);
        assert(f.model.sq_wqes() == 512 && f.model.rma_pending_peak() == 512);
        std::unordered_set<std::uint64_t> requests;
        unsigned frame_index = 0;
        for (const auto& frame : f.net.frames) {
            assert(frame.source_jetty == 9 && frame.source_port == 0);
            assert(frame.destination_eid == 0x200);
            assert(frame.transfer_length == (op == 3 ? 32 : 4));
            assert(requests.insert(frame.request_id).second);
            if (op == 3) assert(frame.bytes == std::vector<std::uint8_t>(32, frame_index % 251 + 1));
            ++frame_index;
        }
        assert(f.host.entries.empty()); f.NoCheckpoint();
        // No duplicate issue when the same doorbell is rung on a full SQ.
        f.model.ProcessSq(9, f.depth);
        assert(f.net.frames.size() == 512);
        // A different JFC/Jetty/port progresses while the first is blocked.
        f.Post(0, 1, 10); f.Reply(512);
        assert(f.model.jetty_contexts_.at(10).consumer == f.blocks);
        assert(f.net.frames[512].source_port == 1);
        assert(f.net.frames[512].destination_eid == 0x300);
        assert(f.model.jetty_contexts_.at(9).consumer == 0 && f.host.entries.empty());
        if (op == 6) {
            for (unsigned b = 0; b < 4; ++b)
                assert(f.host.memory[Payload + 4096 + b] == 512 % 251 + 1);
            assert(std::all_of(f.host.memory.begin() + Payload,
                f.host.memory.begin() + Payload + 2048, [](auto b) { return b == 0; }));
        }
        // 511 out-of-order completions cannot pass the missing first WQE.
        for (unsigned i = 512; --i;) f.Reply(i);
        assert(f.host.entries.empty() && f.model.pending_rma_.size() == 1);
        f.host.hold_cq = true;
        f.Reply(0);
        assert(f.model.pending_rma_.empty() && f.host.cq.size() == 1);
        assert(f.model.jetty_contexts_.at(9).consumer == 0);
        f.NoCheckpoint();
        // CQ DMA backpressure releases exactly the selected batch, no early retirement.
        for (unsigned batch = 0; batch < 8; ++batch) {
            for (unsigned i = 0; i < 64; ++i) {
                assert(f.host.cq.size() == 1);
                auto done = std::move(f.host.cq.front()); f.host.cq.pop_front(); done(true);
            }
            assert(f.host.entries.size() == (batch + 1) * 64);
            assert(f.model.jetty_contexts_.at(9).consumer == (batch + 1) * 64 * f.blocks);
            if (batch != 7) f.NoCheckpoint();
        }
        assert(f.host.cq.empty());
        for (unsigned i = 0; i < 512; ++i) {
            assert(f.host.entries[i] == i * f.blocks + f.blocks - 1);
            assert(f.host.statuses[i] == 0);
            if (op == 6) for (unsigned b = 0; b < 4; ++b)
                assert(f.host.memory[Payload + i * 4 + b] == i % 251 + 1);
        }
        // Reuse the full ring at the next owner generation, again 512 WQEs in flight.
        d::abi::Store32(f.host.memory.data() + Ci, 512);
        f.host.hold_cq = false;
        f.Post(f.depth, 512);
        assert(f.model.pending_rma_.size() == 512);
        for (unsigned i = 513; i < 1025; ++i) f.Reply(i);
        assert(f.host.entries.size() == 1024);
        assert(f.model.jetty_contexts_.at(9).consumer == 2 * f.depth);
        assert(f.model.sq_consumer_wraps() == 2);
        assert(f.model.sq_decode_rejects() == 0 && f.model.ubase_errors() == 0);
    }
    // A producer beyond available WQEBB capacity is rejected, not accepted as
    // a 513th operation. This deliberately malformed doorbell is an error,
    // unlike legal CQ DMA backpressure above.
    for (unsigned op : {3U, 6U}) {
        Fixture f(op); f.Post(0, 512);
        f.model.ProcessSq(9, f.depth + f.blocks);
        assert(f.model.sq_depth_rejects() == 1);
        assert(f.net.frames.size() == 512 && f.model.pending_rma_.size() == 512);
        assert(f.model.sq_pipelines_.at(9)->completions.size() == 512);
        assert(f.host.entries.empty());
    }
    // READ responses are not completions until all 512 payload DMAs finish.
    {
        Fixture f(6); f.Post(0, 512); f.host.hold_payload = true;
        for (unsigned i = 0; i < 512; ++i) f.Reply(i);
        assert(f.host.payload.size() == 512 && f.host.entries.empty());
        assert(f.model.pending_rma_.size() == 512); f.NoCheckpoint();
        for (unsigned remaining = 512; remaining; --remaining) {
            auto done = std::move(f.host.payload.back()); f.host.payload.pop_back(); done(true);
            if (remaining > 1) assert(f.host.entries.empty());
        }
        assert(f.host.entries.size() == 512 && f.model.pending_rma_.empty());
        assert(f.model.ubase_errors() == 0);
    }
}
