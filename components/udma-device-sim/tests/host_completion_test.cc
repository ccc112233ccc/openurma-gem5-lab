// SPDX-License-Identifier: Apache-2.0
// Exercise the actual adapter with in-memory protocol rings, without peers.
#define main udma_device_program_main
#include "../src/main.cc"
#undef main
#include <array>
#include <cassert>

namespace {
struct HostPortTestAccess {
    static void Complete(HostPort& host, host_proto::H2DMessage* message,
                         host_proto::H2DType type) {
        host.HandleDmaCompletion(message, type);
    }
    static std::size_t Pending(const HostPort& host) { return host.pending_.size(); }
};

struct Fixture {
    struct Entry { host_proto::H2DMessage message{}; std::array<std::uint8_t, 64> payload{}; };
    std::array<Entry, 8> outgoing{};
    Entry incoming{};
    host_proto::Interface interface{};
    std::uint64_t now{};
    HostPort host{interface, now};
    std::size_t callbacks{};
    bool success{};
    std::vector<std::uint8_t> result;
    Fixture() {
        interface.base.out_queue = outgoing.data();
        interface.base.out_enum = outgoing.size();
        interface.base.out_elen = sizeof(Entry);
        interface.base.in_queue = &incoming;
        interface.base.in_enum = 1;
        interface.base.in_elen = sizeof(Entry);
    }
    void Request(bool read, std::size_t length = 4) {
        if (read) host.DmaRead(0x1000, length,
            [this](bool ok, std::vector<std::uint8_t> bytes) {
                ++callbacks; success = ok; result = std::move(bytes);
            });
        else host.DmaWrite(0x1000, std::vector<std::uint8_t>(length),
            [this](bool ok) { ++callbacks; success = ok; });
        incoming.message.completion.request_id = 1;
        incoming.message.completion.length = length;
        incoming.payload.fill(0x5a);
    }
    void Complete(bool read) {
        HostPortTestAccess::Complete(host, &incoming.message,
            read ? host_proto::H2DType::DmaReadCompletion : host_proto::H2DType::DmaWriteCompletion);
    }
    void Invalid(bool read, std::size_t pending = 1) {
        bool threw{};
        try { Complete(read); } catch (const std::runtime_error&) { threw = true; }
        assert(threw && callbacks == 0 && HostPortTestAccess::Pending(host) == pending);
        // Production catches this exception only at process main and exits.
        // Never resume processing the device after the malformed completion.
    }
    void Prepare(std::uint64_t generation, std::uint8_t enabled) {
        incoming.message.lifecycle.generation = generation;
        incoming.message.lifecycle.enabled = enabled;
        incoming.message.lifecycle.action = static_cast<std::uint8_t>(host_proto::LifecycleAction::PrepareSync);
        incoming.message.lifecycle.own_type = SIMBRICKS_PROTO_MSG_OWN_CON |
            static_cast<std::uint8_t>(host_proto::H2DType::Lifecycle);
        assert(host.Poll());
    }
};
}

int main() {
    for (bool read : {false, true}) {
        {
            Fixture f; f.Request(read); f.Complete(read);
            assert(f.callbacks == 1 && f.success && f.host.PendingEmpty());
            assert(!read || f.result == std::vector<std::uint8_t>(4, 0x5a));
            f.callbacks = 0; f.Invalid(read, 0); // Duplicate completion.
        }
        { Fixture f; f.Request(read); f.Invalid(!read); }
        { Fixture f; f.Request(read); f.incoming.message.completion.request_id = 42; f.Invalid(read); }
        for (std::uint32_t length : {0U, 3U, 5U, 0xffffffffU}) {
            Fixture f; f.Request(read); f.incoming.message.completion.length = length; f.Invalid(read);
        }
        { Fixture f; f.Request(read); f.incoming.message.completion.status = 5; f.Invalid(read); }
        for (std::uint32_t length : {0U, 4U}) {
            Fixture f; f.Request(read); f.incoming.message.completion.status = 4;
            f.incoming.message.completion.length = length;
            f.Complete(read);
            assert(f.callbacks == 1 && !f.success && f.result.empty() && f.host.PendingEmpty());
        }
        { Fixture f; f.Request(read, 0); f.Complete(read); assert(f.callbacks == 1 && f.success); }
    }
    for (std::size_t capacity : {32U, 64U, 67U}) {
        Fixture f; f.Request(true); f.interface.base.in_elen = capacity; f.Invalid(true);
    }
    for (std::uint64_t generation : {1U, 2U}) {
        Fixture f; f.Prepare(1, 1);
        bool threw{};
        try { f.Prepare(generation, 1); } catch (const std::runtime_error&) { threw = true; }
        assert(threw && f.host.Generation() == 1);
    }
    for (std::uint64_t generation : {0U, 1U}) {
        Fixture f; f.Prepare(1, 1); f.host.FinishFence();
        bool threw{};
        try { f.Prepare(generation, 0); } catch (const std::runtime_error&) { threw = true; }
        assert(threw && !f.host.PrepareSeen() && f.host.Generation() == 1);
    }
    {
        Fixture f; f.Prepare(1, 1); f.host.FinishFence(); f.Prepare(2, 0);
        assert(f.host.Generation() == 2 && !f.host.TargetEnabled());
    }
    {
        Fixture f;
        bool threw{};
        try { f.Prepare(1, 2); } catch (const std::runtime_error&) { threw = true; }
        assert(threw && !f.host.PrepareSeen());
    }
}
