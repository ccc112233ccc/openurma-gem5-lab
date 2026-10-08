// SPDX-License-Identifier: Apache-2.0
// White-box DMA/network fault injection; not a full-system driver test.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <limits>
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
constexpr std::uint64_t Csq = 0x1000, Sq = 0x2000, Cq = 0x3000,
    Ci = 0x4000, Rq = 0x5000, Index = 0x6000, Pi = 0x7000, Payload = 0x8000;
struct Host final : d::HostInterface {
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory;
    std::uint64_t delayed_read{}, delayed_write{};
    std::deque<d::Completion> pending;
    void DmaRead(std::uint64_t address, std::size_t length,
                 d::ReadCompletion done) override {
        auto finish = [this, address, length, done = std::move(done)](bool ok) mutable {
            auto found = memory.find(address);
            ok = ok && found != memory.end() && found->second.size() == length;
            done(ok, ok ? found->second : std::vector<std::uint8_t>{});
        };
        if (address == delayed_read) pending.push_back(std::move(finish));
        else finish(true);
    }
    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> bytes,
                  d::Completion done) override {
        auto finish = [this, address, bytes = std::move(bytes), done = std::move(done)](bool ok) mutable {
            if (ok) memory[address] = std::move(bytes);
            done(ok);
        };
        if (address == delayed_write) pending.push_back(std::move(finish));
        else finish(true);
    }
    void Finish(bool ok = true) {
        assert(!pending.empty());
        auto done = std::move(pending.front()); pending.pop_front(); done(ok);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Network final : d::NetworkInterface {
    bool delayed{};
    std::vector<d::Frame> frames;
    std::deque<d::Completion> pending;
    void Send(d::Frame frame, d::Completion done) override {
        frames.push_back(std::move(frame));
        if (delayed) pending.push_back(std::move(done)); else done(true);
    }
    void Finish(bool ok = true) {
        assert(!pending.empty());
        auto done = std::move(pending.front()); pending.pop_front(); done(ok);
    }
};
struct Fixture {
    Host host;
    Network net;
    d::UdmaModel model{host, net};
    Fixture() {
        model.generic_iova_token_ = 0;
        for (std::uint64_t page = 1; page <= 8; ++page) {
            model.iotlb_[page] = page * 4096;
            model.iotlb_[(std::uint64_t{1} << 63) | page] = page * 4096;
        }
        model.ubase_command_queue_registers_[0] = Csq;
        model.ubase_command_queue_registers_[2] = 1;
        model.ubase_target_producer_ = 1;
        model.ubase_busy_ = true;
        auto& cq = model.jfc_contexts_[7];
        model.jfc_query_contexts_[7] = {};
        cq.depth = 8; cq.queue_iova = Cq; cq.index_iova = Ci;
        cq.moderation_count = 128;
        host.memory[Ci] = std::vector<std::uint8_t>(4);
        Recreate();
        model.tp_routes_[1] = {1, 1, 0x100, 0x200, 0, true};
        model.SetLinkState(0, true);
    }
    void Recreate() {
        auto& sq = model.jetty_contexts_[9];
        sq = {}; sq.depth = 8; sq.queue_iova = Sq;
        sq.completion_queue = 7; sq.receive_queue = 11;
        sq.receive_completion_queue = 7; sq.is_jetty = true;
        sq.device_page_offset = 0x20a000;
        auto& rq = model.jfr_contexts_[11];
        rq = {}; rq.depth = 8; rq.queue_iova = Rq; rq.index_iova = Index;
        rq.producer_iova = Pi; rq.completion_queue = 7;
        host.memory[Index] = std::vector<std::uint8_t>(4);
        host.memory[Pi] = {1, 0, 0, 0};
        host.memory[Rq] = std::vector<std::uint8_t>(16);
        d::abi::Store32(host.memory[Rq].data(), 16);
        d::abi::Store64(host.memory[Rq].data() + 8, Payload);
    }
    void Destroy(std::uint8_t command, std::uint32_t id) {
        std::vector<std::uint8_t> descriptor(32);
        descriptor[1] = 0x70; descriptor[3] = 1;
        d::abi::Store32(descriptor.data() + 16, command | (id << 8));
        model.HandleUbaseMailbox(std::move(descriptor), 1);
    }
    void AssertDestroyed(bool jfs) {
        assert(!model.pending_queue_destroy_ && !model.draining_queue_command_);
        assert(jfs ? model.jetty_count() == 0 : model.jfr_count() == 0);
        assert(host.memory.count(Csq));
        assert(host.memory.at(Csq)[2] & 2);
        assert(host.memory.at(Csq)[4] == 0);
    }
    void Send(std::uint8_t opcode = 0) {
        std::vector<std::uint8_t> wqe(64);
        d::abi::Store32(wqe.data(), 1U | (0x40U << 16) | (1U << 31));
        d::abi::Store32(wqe.data() + 4, (3U << 22) | (std::uint32_t(opcode) << 8));
        d::abi::Store32(wqe.data() + 8, 1);
        d::abi::Store32(wqe.data() + 12, 9);
        d::abi::Store64(wqe.data() + 40, Payload);
        wqe[48] = 'u'; wqe[49] = 'b'; wqe[50] = '!';
        model.ProcessSq(9, 1, std::move(wqe));
    }
    void Receive() {
        d::Frame frame{};
        frame.operation = d::Frame::Operation::Send;
        frame.destination_jetty = 9; frame.bytes = {'u', 'b', '!'};
        model.Receive(std::move(frame));
    }
};

int main() {
    // SQ DMA failure must wake a waiting destroy and preserve subsequent reuse.
    {
        Fixture f;
        f.host.delayed_read = Sq;
        f.model.ProcessSq(9, 1);
        assert(f.host.pending.size() == 1);
        f.Destroy(0x07, 9);
        assert(f.model.pending_queue_destroy_ && !f.host.memory.count(Csq));
        assert(!f.model.HandleJettyMmio(0x20a080, 4, 2));
        f.host.Finish(false);
        f.AssertDestroyed(true);
        f.Recreate();
        f.Send();
        assert(f.model.jetty_contexts_.at(9).consumer == 1);
    }
    // The network owns an accepted SEND until its completion callback.
    for (bool ok : {true, false}) {
        Fixture f;
        f.net.delayed = true; f.Send();
        assert(f.net.pending.size() == 1);
        f.Destroy(0x07, 9);
        assert(!f.host.memory.count(Csq));
        f.net.Finish(ok);
        f.AssertDestroyed(true);
    }
    // Successful WRITE injection is not completion: the peer ACK is required.
    {
        Fixture f;
        f.Send(3);
        assert(f.model.pending_rma_.size() == 1);
        f.Destroy(0x07, 9);
        assert(!f.host.memory.count(Csq));
        d::Frame ack{};
        ack.operation = d::Frame::Operation::WriteAck;
        ack.request_id = f.net.frames.back().request_id;
        ack.transfer_length = 3;
        f.model.Receive(std::move(ack));
        f.AssertDestroyed(true);
    }
    // JFR and its owning Jetty both remain alive through receive payload DMA.
    for (bool jfs : {false, true}) for (bool ok : {true, false}) {
        Fixture f;
        f.host.delayed_write = Payload;
        f.Receive();
        assert(f.model.receive_busy_ && f.host.pending.size() == 1);
        f.Destroy(jfs ? 0x07 : 0x57, jfs ? 9 : 11);
        f.Receive(); // This queued old packet must not target a rebuilt ID.
        assert(!f.host.memory.count(Csq));
        f.host.Finish(ok);
        f.AssertDestroyed(jfs);
        assert(!f.model.receive_busy_ && f.model.receive_frames_.empty());
        f.Recreate(); f.host.delayed_write = 0;
        f.Receive();
        assert(!f.model.receive_busy_);
        assert(f.model.jfr_contexts_.at(11).consumer == 1);
    }
    // Destroy must also wait after payload DMA, while the receive CQE is live.
    {
        Fixture f;
        f.host.delayed_write = Cq;
        f.Receive();
        assert(f.host.pending.size() == 1);
        f.Destroy(0x57, 11);
        assert(!f.host.memory.count(Csq));
        f.host.Finish();
        f.AssertDestroyed(false);
        assert(!f.model.receive_busy_);
    }
    // Distinct TA numbers are not enough: their byte intervals must partition
    // the destination, including when packets and DMA callbacks are reordered.
    {
        Fixture f;
        auto& request = f.model.pending_rma_[123];
        request.jetty_id = 9; request.producer = 1; request.wqebbs = 1;
        request.opcode = 6; request.byte_count = 12;
        request.local_address = Payload; request.completion = true;
        f.model.jetty_contexts_.at(9).busy = true;
        const auto response = [](unsigned ta, unsigned offset, unsigned length) {
            d::Frame frame{};
            frame.operation = d::Frame::Operation::ReadResponse;
            frame.request_id = 123; frame.ta_ssn = ta;
            frame.transaction_offset = offset; frame.transfer_length = length;
            frame.bytes.assign(length, static_cast<std::uint8_t>(ta));
            return frame;
        };
        f.host.delayed_write = Payload + 4;
        f.model.Receive(response(1, 4, 4));
        assert(f.host.pending.size() == 1);
        unsigned ta = 2;
        for (const auto& interval : std::vector<std::pair<unsigned, unsigned>>{
                 {4, 4}, {3, 3}, {6, 3}, {0, 8}, {0, 0}, {12, 1},
                 {std::numeric_limits<unsigned>::max(), 1}}) {
            const auto errors = f.model.ubase_errors();
            f.model.Receive(response(ta++, interval.first, interval.second));
            assert(f.model.ubase_errors() == errors + 1);
            assert(request.accepted_bytes == 4 && request.response_ranges.size() == 1);
            assert(request.completed_tassns.size() == 1 && f.host.pending.size() == 1);
        }
        auto mismatch = response(ta++, 0, 4); mismatch.bytes.pop_back();
        f.model.Receive(mismatch);
        f.model.Receive(response(1, 0, 4)); // same TA, otherwise disjoint
        assert(request.accepted_bytes == 4 && f.host.pending.size() == 1);
        f.host.delayed_write = Payload + 8;
        f.model.Receive(response(ta++, 8, 4));
        f.host.delayed_write = Payload;
        f.model.Receive(response(ta++, 0, 4));
        assert(request.accepted_bytes == 12 && f.host.pending.size() == 3);
        // Release last-accepted DMA first, leaving a hole until the others end.
        auto last = std::move(f.host.pending.back()); f.host.pending.pop_back();
        last(true);
        assert(f.model.pending_rma_.count(123) && !f.host.memory.count(Cq));
        f.host.Finish();
        assert(f.model.pending_rma_.count(123) && !f.host.memory.count(Cq));
        f.host.Finish();
        assert(f.model.pending_rma_.empty() && f.model.sq_completions() == 1);
        assert(f.host.memory.at(Cq)[3] == 0);
    }
    {
        Fixture f;
        auto& request = f.model.pending_rma_[123];
        request.opcode = 6; request.byte_count = 4;
        request.local_address = std::numeric_limits<std::uint64_t>::max() - 1;
        d::Frame frame{};
        frame.operation = d::Frame::Operation::ReadResponse;
        frame.request_id = 123; frame.transfer_length = 4; frame.bytes.resize(4);
        const auto errors = f.model.ubase_errors();
        f.model.Receive(frame);
        assert(f.model.ubase_errors() == errors + 1);
        assert(!request.accepted_bytes && request.response_ranges.empty());
        assert(f.host.pending.empty() && f.model.read_response_dma_.empty());
    }
    // The last valid byte may equal UINT64_MAX: only wraparound is invalid.
    {
        Fixture f;
        auto& request = f.model.pending_rma_[123];
        request.jetty_id = 9; request.producer = 1; request.wqebbs = 1;
        request.opcode = 6; request.byte_count = 4; request.completion = true;
        request.local_address = std::numeric_limits<std::uint64_t>::max() - 3;
        f.model.jetty_contexts_.at(9).busy = true;
        const auto page = request.local_address & ~std::uint64_t{4095};
        f.model.iotlb_[(std::uint64_t{1} << 63) | (page >> 12)] = page;
        d::Frame frame{};
        frame.operation = d::Frame::Operation::ReadResponse;
        frame.request_id = 123; frame.transfer_length = 4; frame.bytes.assign(4, 0xab);
        f.model.Receive(frame);
        assert(f.model.pending_rma_.empty() && f.model.sq_completions() == 1);
        assert(f.host.memory.at(std::numeric_limits<std::uint64_t>::max() - 3) ==
               std::vector<std::uint8_t>(4, 0xab));
    }
    // WRITE ACKs also carry offsets; distinct TA numbers must not acknowledge
    // the same bytes twice or wrap the completed-byte arithmetic.
    {
        Fixture f;
        auto& request = f.model.pending_rma_[123];
        request.jetty_id = 9; request.producer = 1; request.wqebbs = 1;
        request.opcode = 3; request.byte_count = 12; request.completion = true;
        f.model.jetty_contexts_.at(9).busy = true;
        d::Frame ack{};
        ack.operation = d::Frame::Operation::WriteAck;
        ack.request_id = 123; ack.transfer_length = 4; ack.transaction_offset = 4;
        f.model.Receive(ack);
        ack.ta_ssn = 1;
        const auto errors = f.model.ubase_errors();
        f.model.Receive(ack);
        assert(f.model.ubase_errors() == errors + 1 && request.completed_bytes == 4);
        ack.transfer_length = std::numeric_limits<std::uint32_t>::max();
        f.model.Receive(ack);
        assert(request.completed_bytes == 4 && request.response_ranges.size() == 1);
        ack.transfer_length = 4; ack.transaction_offset = 8;
        f.model.Receive(ack);
        assert(request.completed_bytes == 8 && !f.host.memory.count(Cq));
        ack.ta_ssn = 2; ack.transaction_offset = 0;
        f.model.Receive(ack);
        assert(f.model.pending_rma_.empty() && f.model.sq_completions() == 1);
    }
    // READ error retirement cannot release the destination while two response
    // DMAs are live. Exercise both a peer RmaError and a local DMA failure.
    for (bool peer_error : {true, false}) {
        Fixture f;
        auto& request = f.model.pending_rma_[123];
        request.jetty_id = 9; request.producer = 1; request.wqebbs = 1;
        request.opcode = 6; request.byte_count = 9;
        request.local_address = Payload;
        request.completion = true;
        f.model.jetty_contexts_.at(9).busy = true;
        f.host.delayed_write = Payload;
        d::Frame response{};
        response.operation = d::Frame::Operation::ReadResponse;
        response.request_id = 123; response.transfer_length = 3;
        response.bytes = {'a', 'b', 'c'};
        f.model.Receive(response);
        f.model.Receive(response); // Duplicate TA must not issue a second DMA.
        assert(f.host.pending.size() == 1);
        f.host.delayed_write = Payload + 3;
        response.ta_ssn = 1; response.transaction_offset = 3;
        f.model.Receive(response);
        assert(f.host.pending.size() == 2);
        if (peer_error) {
            d::Frame error{};
            error.operation = d::Frame::Operation::RmaError;
            error.request_id = 123;
            f.model.Receive(std::move(error));
        }
        f.Destroy(0x07, 9);
        assert(!f.host.memory.count(Csq) && !f.host.memory.count(Cq));
        f.host.Finish(peer_error);
        assert(!f.host.memory.count(Csq) && !f.host.memory.count(Cq));
        assert(f.model.pending_rma_.count(123));
        response.ta_ssn = 2; response.transaction_offset = 6;
        f.model.Receive(response); // valid final range, but request already failed
        assert(f.host.pending.size() == 1);
        assert(f.model.pending_rma_.at(123).accepted_bytes == 6);
        f.host.Finish();
        f.AssertDestroyed(true);
        assert(f.host.memory.at(Cq)[3] == 5); // Error CQE only after final DMA.
        assert(f.model.pending_rma_.empty() && f.model.read_response_dma_.empty());
    }
}
