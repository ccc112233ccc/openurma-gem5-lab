// SPDX-License-Identifier: Apache-2.0
// Remote one-sided services use token/address ownership, not a local Jetty.
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
struct Host final : d::HostInterface {
    std::deque<d::Completion> pending;
    std::size_t issued{};
    void DmaRead(std::uint64_t, std::size_t length, d::ReadCompletion done) override {
        ++issued;
        pending.push_back([length, done = std::move(done)](bool ok) mutable {
            done(ok, std::vector<std::uint8_t>(ok ? length : 0, 0x42));
        });
    }
    void DmaWrite(std::uint64_t, std::vector<std::uint8_t>, d::Completion done) override {
        ++issued; pending.push_back(std::move(done));
    }
    void Finish(bool ok) {
        assert(!pending.empty());
        auto done = std::move(pending.front()); pending.pop_front(); done(ok);
    }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Network final : d::NetworkInterface {
    bool synchronous{};
    std::vector<d::Frame> frames;
    std::deque<d::Completion> pending;
    std::function<void()> before_completion;
    void Send(d::Frame frame, d::Completion done) override {
        frames.push_back(std::move(frame));
        if (synchronous) {
            if (before_completion) before_completion();
            done(true);
        } else pending.push_back(std::move(done));
    }
    void Finish(bool ok) {
        assert(!pending.empty());
        auto done = std::move(pending.front()); pending.pop_front();
        if (before_completion) before_completion();
        done(ok);
    }
};
struct Fixture {
    Host host;
    Network net;
    d::UdmaModel model{host, net};
    std::string checkpoint;
    Fixture() {
        for (std::uint64_t page = 1; page <= 2; ++page) {
            model.iotlb_[page] = page * 4096;
            model.iotlb_[(std::uint64_t{1} << 63) | page] = page * 4096;
        }
        std::ostringstream output;
        assert(model.SaveState(output)); checkpoint = output.str();
        net.before_completion = [this] { AssertBusy(); };
    }
    d::Frame Request(bool write, std::uint32_t length = 8) {
        d::Frame frame{};
        frame.operation = write ? d::Frame::Operation::Write : d::Frame::Operation::ReadRequest;
        frame.request_id = 17; frame.remote_address = 4096;
        frame.transfer_length = length; frame.source_eid = 123; frame.destination_eid = 456;
        frame.source_jetty = 987; frame.destination_jetty = 654;
        frame.source_port = 1; frame.destination_port = 2;
        frame.ta_ssn = 8; frame.transaction_offset = 4096;
        if (write) frame.bytes.resize(length, 0x42);
        return frame;
    }
    void AssertBusy() {
        assert(!model.IsQuiescent());
        std::ostringstream output;
        assert(!model.SaveState(output) && output.str().empty());
        std::istringstream input(checkpoint);
        assert(!model.LoadState(input));
        const auto before = model.now_ps_;
        model.RebaseTime(before + 123);
        assert(model.now_ps_ == before);
    }
    void AssertIdle() {
        assert(model.IsQuiescent() && model.remote_services_in_flight_ == 0);
        assert(model.jetty_count() == 0); // No fabricated Jetty ownership.
        std::ostringstream output;
        assert(model.SaveState(output));
        std::istringstream input(output.str());
        assert(model.LoadState(input));
    }
};

int main() {
    for (bool write : {false, true}) {
        for (bool dma_ok : {false, true}) for (bool send_ok : {false, true}) {
            Fixture f;
            f.model.Receive(f.Request(write));
            assert(f.host.pending.size() == 1 && f.net.pending.empty());
            f.AssertBusy(); f.host.Finish(dma_ok);
            assert(f.host.pending.empty() && f.net.pending.size() == 1);
            const auto& reply = f.net.frames.back();
            assert(reply.operation == (dma_ok ? (write ? d::Frame::Operation::WriteAck :
                d::Frame::Operation::ReadResponse) : d::Frame::Operation::RmaError));
            assert(reply.source_eid == 456 && reply.destination_eid == 123);
            assert(reply.source_jetty == 654 && reply.destination_jetty == 987);
            assert(reply.request_id == 17 && reply.ta_ssn == 8 && reply.transaction_offset == 4096);
            if (!dma_ok) assert(reply.bytes.empty());
            f.AssertBusy(); f.net.Finish(send_ok); f.AssertIdle();
        }
        // A failed first chunk still owns the service until other submitted
        // chunks finish, and subsequently until the error response completes.
        {
            Fixture f;
            f.model.Receive(f.Request(write, 8192));
            assert(f.host.pending.size() == 2);
            f.host.Finish(false);
            assert(f.net.frames.empty()); f.AssertBusy();
            f.host.Finish(true);
            assert(f.net.frames.back().operation == d::Frame::Operation::RmaError);
            f.AssertBusy(); f.net.Finish(true); f.AssertIdle();
        }
        // Concurrent services cannot release each other's model lease.
        {
            Fixture f;
            f.model.Receive(f.Request(write)); f.model.Receive(f.Request(write));
            assert(f.model.remote_services_in_flight_ == 2);
            f.host.Finish(true); f.net.Finish(true);
            assert(f.model.remote_services_in_flight_ == 1); f.AssertBusy();
            f.host.Finish(true); f.net.Finish(false); f.AssertIdle();
        }
        // Immediate translation failure and synchronous response completion.
        {
            Fixture f;
            f.model.iotlb_.clear(); f.net.synchronous = true;
            f.model.Receive(f.Request(write));
            assert(f.host.issued == 0 && f.net.frames.size() == 1);
            assert(f.net.frames.back().operation == d::Frame::Operation::RmaError);
            f.AssertIdle();
        }
        // Invalid intervals are rejected before acquiring a service lease.
        {
            Fixture f;
            auto request = f.Request(write);
            request.remote_address = UINT64_MAX - 3;
            f.model.Receive(std::move(request));
            assert(f.host.issued == 0 && f.net.frames.empty()); f.AssertIdle();
            request = f.Request(write, 0);
            f.model.Receive(std::move(request));
            assert(f.host.issued == 0 && f.net.frames.empty()); f.AssertIdle();
        }
    }
}
